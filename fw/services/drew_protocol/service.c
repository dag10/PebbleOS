/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include <inttypes.h>
#include <string.h>

#include <pbl/kernel/mutex.h>
#include <pbl/logging/logging.h>
#include <pbl/services/bluetooth/bluetooth_persistent_storage.h>
#include <pbl/services/comm_session/session.h>
#include <pbl/services/drew_protocol.h>
#include <pbl/services/drew_updates.h>
#include <pbl/services/new_timer/new_timer.h>
#include <pbl/services/settings/settings_file.h>
#include <pbl/services/system_task.h>
#include <pbl/util/math.h>

#include <applib/event_service_client.h>
#include <kernel/events.h>
#include <kernel/pbl_malloc.h>
#include <system/version.h>

PBL_LOG_MODULE_DEFINE(service_drew_protocol, CONFIG_SERVICE_DREW_PROTOCOL_LOG_LEVEL);

//! @file
//! The Drew endpoint (docs/drew-os-extensions.md in pebble-workspace). Multi-byte integers are
//! little-endian. The phone's features are cached in the "drewproto" settings file, tied to the
//! bonding of the phone that sent them, so the Ping Phone row outlives a dropped session.

#define HELLO_FLAG_REPLY_REQUESTED (1 << 0)
//! The bitmap the watch sends: 64 bits.
#define HELLO_FEATURES_LENGTH     (8)
#define HELLO_MAX_FEATURES_LENGTH (32)
#define HELLO_MAX_BUILD_LENGTH    (63)

#define HELLO_RETRY_MS  (5000)
#define HELLO_MAX_SENDS (3)

#define CACHE_FILE         "drewproto"
#define CACHE_FILE_SIZE    (512)
#define CACHE_KEY          "phone"
#define CACHE_KEY_LENGTH   (5)
#define CACHE_VERSION      (1)
#define CACHE_RECORD_BYTES (12)

typedef struct Outbound {
  size_t length;
  uint8_t data[];
} Outbound;

typedef struct PhoneCache {
  pbl_bt_bonding_id_t bonding;
  uint64_t features;
} PhoneCache;

static PBL_MUTEX_DEFINE(s_mutex);
//! Guarded by s_mutex.
static PhoneCache s_cache = {.bonding = PBL_BT_BONDING_ID_INVALID};

//! Handshake state, touched only on KernelBG.
static TimerID s_hello_timer = TIMER_INVALID_ID;
static bool s_awaiting_phone_hello;
static uint8_t s_hello_sends;

// Features
////////////

uint64_t drew_protocol_local_features(void) {
  uint64_t features = 0;
#ifdef CONFIG_SERVICE_DREW_UPDATES
  features |= DREW_FEATURE_BIT(DrewFeatureUpdates);
#endif
  features |= DREW_FEATURE_BIT(DrewFeaturePrefs);
  return features;
}

//! The phone the cache is for: the active gateway, or PBL_BT_BONDING_ID_INVALID without one (as
//! in QEMU).
static pbl_bt_bonding_id_t prv_active_phone(void) {
  pbl_bt_bonding_id_t bonding = PBL_BT_BONDING_ID_INVALID;
  if (!bt_persistent_storage_get_active_gateway(&bonding, nullptr)) {
    bonding = PBL_BT_BONDING_ID_INVALID;
  }
  return bonding;
}

uint64_t drew_protocol_phone_features(void) {
  const pbl_bt_bonding_id_t active = prv_active_phone();
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const uint64_t features = (s_cache.bonding == active) ? s_cache.features : 0;
  pbl_mutex_unlock(&s_mutex);
  return features;
}

bool drew_protocol_has_feature(DrewFeature feature) {
  const uint64_t bit = DREW_FEATURE_BIT(feature);
  return (drew_protocol_local_features() & bit) && (drew_protocol_phone_features() & bit);
}

// Cache
/////////

static void prv_put_le64(uint8_t *out, uint64_t value) {
  for (int i = 0; i < 8; i++) {
    out[i] = (uint8_t)(value >> (8 * i));
  }
}

static uint64_t prv_get_le64(const uint8_t *in, size_t length) {
  uint64_t value = 0;
  for (size_t i = 0; i < length && i < 8; i++) {
    value |= (uint64_t)in[i] << (8 * i);
  }
  return value;
}

static void prv_load_cache(void) {
  PhoneCache cache = {.bonding = PBL_BT_BONDING_ID_INVALID};
  SettingsFile file;
  if (settings_file_open(&file, CACHE_FILE, CACHE_FILE_SIZE) == S_SUCCESS) {
    uint8_t record[CACHE_RECORD_BYTES];
    if (settings_file_get_len(&file, CACHE_KEY, CACHE_KEY_LENGTH) == CACHE_RECORD_BYTES &&
        settings_file_get(&file, CACHE_KEY, CACHE_KEY_LENGTH, record, sizeof(record)) ==
            S_SUCCESS &&
        record[0] == CACHE_VERSION) {
      cache = (PhoneCache){
        .bonding = record[1],
        .features = prv_get_le64(&record[4], 8),
      };
    }
    settings_file_close(&file);
  }
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  s_cache = cache;
  pbl_mutex_unlock(&s_mutex);
}

//! Stores @p features for @p bonding; true if that changed what the active phone has.
static bool prv_store_cache(pbl_bt_bonding_id_t bonding, uint64_t features) {
  const uint64_t old_features = drew_protocol_phone_features();
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const bool changed = (s_cache.bonding != bonding) || (s_cache.features != features);
  s_cache = (PhoneCache){
    .bonding = bonding,
    .features = features,
  };
  pbl_mutex_unlock(&s_mutex);
  if (!changed) {
    return false;
  }

  uint8_t record[CACHE_RECORD_BYTES] = {CACHE_VERSION, bonding};
  prv_put_le64(&record[4], features);
  SettingsFile file;
  if (settings_file_open(&file, CACHE_FILE, CACHE_FILE_SIZE) == S_SUCCESS) {
    settings_file_set(&file, CACHE_KEY, CACHE_KEY_LENGTH, record, sizeof(record));
    settings_file_close(&file);
  }
  return drew_protocol_phone_features() != old_features;
}

static void prv_post_features_event(void) {
  PebbleEvent e = {
    .type = PEBBLE_DREW_EVENT,
    .drew.type = DrewEventPhoneFeatures,
  };
  event_put(&e);
}

static void prv_forget_bonding_cb(void *data) {
  const pbl_bt_bonding_id_t bonding = (pbl_bt_bonding_id_t)(uintptr_t)data;
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const bool cached = (s_cache.bonding == bonding);
  pbl_mutex_unlock(&s_mutex);
  if (cached && prv_store_cache(PBL_BT_BONDING_ID_INVALID, 0)) {
    prv_post_features_event();
  }
}

void drew_protocol_handle_bonding_change(pbl_bt_bonding_id_t bonding, BtPersistBondingOp op) {
  if (op == BtPersistBondingOpWillDelete) {
    system_task_add_callback(prv_forget_bonding_cb, (void *)(uintptr_t)bonding);
  }
}

// Outbound
////////////

static void prv_send_cb(void *data) {
  Outbound *message = data;
  CommSession *session = comm_session_get_system_session();
  if (session) {
    comm_session_send_data(session, DREW_PROTOCOL_ENDPOINT_ID, message->data, message->length,
                           COMM_SESSION_DEFAULT_TIMEOUT);
  }
  kernel_free(message);
}

bool drew_protocol_send(DrewService service, uint8_t command, const uint8_t *payload,
                        size_t length) {
  const size_t total = DREW_PROTOCOL_HEADER_LENGTH + length;
  if (total > DREW_PROTOCOL_MAX_SEND_LENGTH || !comm_session_get_system_session()) {
    return false;
  }
  Outbound *message = kernel_malloc(sizeof(Outbound) + total);
  if (!message) {
    return false;
  }
  message->length = total;
  memcpy(message->data, DREW_PROTOCOL_MAGIC, DREW_PROTOCOL_MAGIC_LENGTH);
  message->data[4] = DREW_PROTOCOL_FRAME_VERSION;
  message->data[5] = service;
  message->data[6] = command;
  if (length) {
    memcpy(&message->data[DREW_PROTOCOL_HEADER_LENGTH], payload, length);
  }
  if (!system_task_add_callback(prv_send_cb, message)) {
    kernel_free(message);
    return false;
  }
  return true;
}

static void prv_send_hello(uint8_t flags) {
  const char *build = TINTIN_METADATA.version_tag;
  const size_t build_length =
      strnlen(build, MIN(sizeof(TINTIN_METADATA.version_tag), (size_t)HELLO_MAX_BUILD_LENGTH));
  uint8_t payload[3 + HELLO_FEATURES_LENGTH + HELLO_MAX_BUILD_LENGTH];
  size_t n = 0;
  payload[n++] = flags;
  payload[n++] = HELLO_FEATURES_LENGTH;
  prv_put_le64(&payload[n], drew_protocol_local_features());
  n += HELLO_FEATURES_LENGTH;
  payload[n++] = build_length;
  memcpy(&payload[n], build, build_length);
  n += build_length;
  drew_protocol_send(DrewServiceCore, DrewCoreCommandWatchHello, payload, n);
}

// Handshake
/////////////

static void prv_hello_timer_cb(void *unused);

static void prv_send_hello_and_wait(void) {
  s_hello_sends++;
  prv_send_hello(HELLO_FLAG_REPLY_REQUESTED);
  new_timer_start(s_hello_timer, HELLO_RETRY_MS, prv_hello_timer_cb, nullptr, 0 /* flags */);
}

static void prv_hello_timeout_kernel_bg_cb(void *unused) {
  if (!s_awaiting_phone_hello || !comm_session_get_system_session()) {
    return;
  }
  if (s_hello_sends < HELLO_MAX_SENDS) {
    prv_send_hello_and_wait();
    return;
  }
  // Not DrewMobileApp: nothing on this phone has Drew features.
  PBL_LOG_DBG("No PhoneHello after %u hellos", s_hello_sends);
  s_awaiting_phone_hello = false;
  if (prv_store_cache(prv_active_phone(), 0)) {
    prv_post_features_event();
  }
}

static void prv_hello_timer_cb(void *unused) {
  system_task_add_callback(prv_hello_timeout_kernel_bg_cb, nullptr);
}

static void prv_session_opened_kernel_bg_cb(void *unused) {
  if (!comm_session_get_system_session()) {
    return;
  }
  s_awaiting_phone_hello = true;
  s_hello_sends = 0;
  prv_send_hello_and_wait();
}

static void prv_session_closed_kernel_bg_cb(void *unused) {
  s_awaiting_phone_hello = false;
  new_timer_stop(s_hello_timer);
}

static void prv_comm_session_event_handler(PebbleEvent *e, void *context) {
  const PebbleCommSessionEvent *event = &e->bluetooth.comm_session_event;
  if (!event->is_system) {
    return;
  }
  system_task_add_callback(
      event->is_open ? prv_session_opened_kernel_bg_cb : prv_session_closed_kernel_bg_cb, nullptr);
}

// Inbound
///////////

static void prv_handle_phone_hello(const uint8_t *payload, size_t length) {
  // flags, features_len, features, build_len, build
  if (length < 2) {
    PBL_LOG_WRN("Short PhoneHello");
    return;
  }
  const uint8_t flags = payload[0];
  const size_t features_length = payload[1];
  if (features_length > HELLO_MAX_FEATURES_LENGTH || length < 3 + features_length) {
    PBL_LOG_WRN("Bad PhoneHello features");
    return;
  }
  const uint8_t *features_bytes = &payload[2];
  const size_t build_length = payload[2 + features_length];
  if (length < 3 + features_length + build_length) {
    PBL_LOG_WRN("Bad PhoneHello build");
    return;
  }
  char build[HELLO_MAX_BUILD_LENGTH + 1];
  const size_t build_copy = MIN(build_length, (size_t)HELLO_MAX_BUILD_LENGTH);
  memcpy(build, &payload[3 + features_length], build_copy);
  build[build_copy] = '\0';

  // Bits 64 and up aren't used yet.
  const uint64_t features = prv_get_le64(features_bytes, features_length);
  PBL_LOG_DBG("PhoneHello from %s: features 0x%08" PRIx32 "%08" PRIx32 ", flags 0x%x", build,
              (uint32_t)(features >> 32), (uint32_t)features, flags);
  const uint64_t unknown = features & ~drew_protocol_local_features();
  if (unknown) {
    PBL_LOG_DBG("Phone features this firmware lacks: 0x%08" PRIx32 "%08" PRIx32,
                (uint32_t)(unknown >> 32), (uint32_t)unknown);
  }

  s_awaiting_phone_hello = false;
  new_timer_stop(s_hello_timer);
  prv_store_cache(prv_active_phone(), features);
  // Posted on every hello, so a page waiting on the phone can stop waiting.
  prv_post_features_event();
  if (flags & HELLO_FLAG_REPLY_REQUESTED) {
    prv_send_hello(0 /* flags */);
  }
}

static void prv_handle_core(uint8_t command, const uint8_t *payload, size_t length) {
  switch (command) {
    case DrewCoreCommandPhoneHello:
      prv_handle_phone_hello(payload, length);
      return;
    default:
      PBL_LOG_WRN("Unknown Core command 0x%x", command);
      return;
  }
}

void drew_protocol_msg_callback(CommSession *session, const uint8_t *data, size_t length) {
  if (length < DREW_PROTOCOL_HEADER_LENGTH ||
      memcmp(data, DREW_PROTOCOL_MAGIC, DREW_PROTOCOL_MAGIC_LENGTH) != 0) {
    PBL_LOG_WRN("0xde01 message without the DREW magic, dropped (an endpoint collision?)");
    return;
  }
  const uint8_t version = data[4];
  const uint8_t service = data[5];
  const uint8_t command = data[6];
  if (version != DREW_PROTOCOL_FRAME_VERSION) {
    PBL_LOG_WRN("Unknown frame version %u, dropped", version);
    return;
  }
  if (!(command & DREW_PROTOCOL_COMMAND_FROM_PHONE)) {
    PBL_LOG_WRN("Watch-to-phone command 0x%x of service 0x%x from the phone, dropped", command,
                service);
    return;
  }
  const uint8_t *payload = &data[DREW_PROTOCOL_HEADER_LENGTH];
  const size_t payload_length = length - DREW_PROTOCOL_HEADER_LENGTH;
  switch (service) {
    case DrewServiceCore:
      prv_handle_core(command, payload, payload_length);
      return;
#ifdef CONFIG_SERVICE_DREW_UPDATES
    case DrewServiceUpdates:
      drew_updates_handle_message(command, payload, payload_length);
      return;
#endif
    default:
      PBL_LOG_WRN("Unknown service 0x%x, dropped", service);
      return;
  }
}

// Init
////////

void drew_protocol_init(void) {
  s_hello_timer = new_timer_create();
  prv_load_cache();

  static EventServiceInfo s_session_event_info;
  s_session_event_info = (EventServiceInfo){
    .type = PEBBLE_COMM_SESSION_EVENT,
    .handler = prv_comm_session_event_handler,
  };
  event_service_client_subscribe(&s_session_event_info);

  // The session may have opened before this subscribed.
  if (comm_session_get_system_session()) {
    system_task_add_callback(prv_session_opened_kernel_bg_cb, nullptr);
  }
}

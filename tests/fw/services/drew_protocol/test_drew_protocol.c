/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <pbl/services/comm_session/session.h>
#include <pbl/services/drew_protocol.h>

#include <applib/event_service_client.h>
#include <clar.h>
#include <fake_new_timer.h>
#include <fake_pbl_malloc.h>
#include <fake_regular_timer.h>
#include <fake_settings_file.h>
#include <fake_system_task.h>
#include <kernel/events.h>
#include <stubs_logging.h>
#include <system/version.h>

extern void drew_protocol_msg_callback(CommSession *session, const uint8_t *data, size_t length);

const FirmwareMetadata TINTIN_METADATA = {
  .version_tag = "v4.44.1-drew.6",
};

// The features this firmware announces.
#define LOCAL_FEATURES                                                             \
  (DREW_FEATURE_BIT(DrewFeatureUpdates) | DREW_FEATURE_BIT(DrewFeatureHeartbeat) | \
   DREW_FEATURE_BIT(DrewFeatureFindPhone) | DREW_FEATURE_BIT(DrewFeaturePrefs))

static CommSession *s_session;
static bool s_has_gateway;
static pbl_bt_bonding_id_t s_gateway;
static EventServiceInfo *s_session_event_info;
static int s_num_feature_events;

#define MAX_SENT (8)
typedef struct Sent {
  uint8_t data[128];
  size_t length;
} Sent;
static Sent s_sent[MAX_SENT];
static int s_num_sent;

static int s_num_updates_messages;
static uint8_t s_updates_command;
static Sent s_updates_payload;

void drew_updates_handle_message(uint8_t command, const uint8_t *payload, size_t length) {
  s_num_updates_messages++;
  s_updates_command = command;
  cl_assert(length <= sizeof(s_updates_payload.data));
  memcpy(s_updates_payload.data, payload, length);
  s_updates_payload.length = length;
}

CommSession *comm_session_get_system_session(void) {
  return s_session;
}

bool comm_session_send_data(CommSession *session, uint16_t endpoint_id, const uint8_t *data,
                            size_t length, uint32_t timeout_ms) {
  cl_assert_equal_i(endpoint_id, DREW_PROTOCOL_ENDPOINT_ID);
  cl_assert(s_num_sent < MAX_SENT);
  cl_assert(length <= sizeof(s_sent[0].data));
  memcpy(s_sent[s_num_sent].data, data, length);
  s_sent[s_num_sent].length = length;
  s_num_sent++;
  return true;
}

bool bt_persistent_storage_get_active_gateway(pbl_bt_bonding_id_t *bonding_out,
                                              BtPersistBondingType *type_out) {
  if (s_has_gateway && bonding_out) {
    *bonding_out = s_gateway;
  }
  return s_has_gateway;
}

void event_service_client_subscribe(EventServiceInfo *service_info) {
  cl_assert_equal_i(service_info->type, PEBBLE_COMM_SESSION_EVENT);
  s_session_event_info = service_info;
}

void event_put(PebbleEvent *event) {
  cl_assert_equal_i(event->type, PEBBLE_DREW_EVENT);
  cl_assert_equal_i(event->drew.type, DrewEventPhoneFeatures);
  s_num_feature_events++;
}

// Helpers
///////////

static void prv_deliver(const uint8_t *data, size_t length) {
  drew_protocol_msg_callback(s_session, data, length);
  fake_system_task_callbacks_invoke_pending();
}

#define DELIVER(...)                         \
  do {                                       \
    const uint8_t message[] = {__VA_ARGS__}; \
    prv_deliver(message, sizeof(message));   \
  } while (0)

#define HEADER(service, command) 0x44, 0x52, 0x45, 0x57, 0x01, (service), (command)

static void prv_phone_hello(uint8_t flags, uint64_t features) {
  DELIVER(HEADER(0x00, 0x81), flags, 8, (uint8_t)features, (uint8_t)(features >> 8),
          (uint8_t)(features >> 16), (uint8_t)(features >> 24), (uint8_t)(features >> 32),
          (uint8_t)(features >> 40), (uint8_t)(features >> 48), (uint8_t)(features >> 56), 0);
}

static void prv_session_event(bool is_open) {
  PebbleEvent e = {
    .type = PEBBLE_COMM_SESSION_EVENT,
    .bluetooth.comm_session_event = {.is_open = is_open, .is_system = true},
  };
  s_session_event_info->handler(&e, s_session_event_info->context);
  fake_system_task_callbacks_invoke_pending();
}

static void prv_fire_hello_timer(void) {
  cl_assert(stub_new_timer_fire(s_new_timer_start_param_timer_id));
  fake_system_task_callbacks_invoke_pending();
}

//! The WatchHello this firmware sends, as in the spec's example but with its own features.
static void prv_assert_watch_hello(const Sent *sent, uint8_t flags) {
  const uint8_t expected[] = {
    0x44, 0x52, 0x45, 0x57, 0x01, 0x00, 0x01, flags, 0x08, (uint8_t)LOCAL_FEATURES,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e,  0x76, 0x34,
    0x2e, 0x34, 0x34, 0x2e, 0x31, 0x2d, 0x64, 0x72,  0x65, 0x77,
    0x2e, 0x36,
  };
  cl_assert_equal_i(sent->length, sizeof(expected));
  cl_assert_equal_m(sent->data, expected, sizeof(expected));
}

// Setup
/////////

void test_drew_protocol__initialize(void) {
  fake_settings_file_reset();
  s_session = (CommSession *)0x1;
  s_has_gateway = true;
  s_gateway = 3;
  s_num_sent = 0;
  s_num_feature_events = 0;
  s_num_updates_messages = 0;
  s_session_event_info = nullptr;
  drew_protocol_init();
  fake_system_task_callbacks_invoke_pending();
  // The session was already open: init sent the first hello.
  cl_assert_equal_i(s_num_sent, 1);
  s_num_sent = 0;
}

void test_drew_protocol__cleanup(void) {
  fake_system_task_callbacks_cleanup();
  stub_new_timer_cleanup();
}

// Framing
///////////

void test_drew_protocol__send_frames_the_payload(void) {
  cl_assert(drew_protocol_send(DrewServiceFindPhone, DrewFindPhoneCommandStart, nullptr, 0));
  cl_assert(drew_protocol_send(DrewServiceFindPhone, DrewFindPhoneCommandStop, nullptr, 0));
  const uint8_t cookie[] = {0x07, 0x00, 0x00, 0x00};
  cl_assert(drew_protocol_send(DrewServiceCore, DrewCoreCommandHeartbeat, cookie, sizeof(cookie)));
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_num_sent, 3);

  const uint8_t ping[] = {0x44, 0x52, 0x45, 0x57, 0x01, 0x02, 0x01};
  const uint8_t stop[] = {0x44, 0x52, 0x45, 0x57, 0x01, 0x02, 0x02};
  const uint8_t heartbeat[] = {0x44, 0x52, 0x45, 0x57, 0x01, 0x00, 0x02, 0x07, 0x00, 0x00, 0x00};
  cl_assert_equal_i(s_sent[0].length, sizeof(ping));
  cl_assert_equal_m(s_sent[0].data, ping, sizeof(ping));
  cl_assert_equal_i(s_sent[1].length, sizeof(stop));
  cl_assert_equal_m(s_sent[1].data, stop, sizeof(stop));
  cl_assert_equal_i(s_sent[2].length, sizeof(heartbeat));
  cl_assert_equal_m(s_sent[2].data, heartbeat, sizeof(heartbeat));
}

void test_drew_protocol__updates_messages_from_the_spec(void) {
  // ListRequest with RECHECK.
  const uint8_t recheck[] = {0x01};
  cl_assert(drew_protocol_send(DrewServiceUpdates, 0x01, recheck, sizeof(recheck)));
  fake_system_task_callbacks_invoke_pending();
  const uint8_t list_request[] = {0x44, 0x52, 0x45, 0x57, 0x01, 0x01, 0x01, 0x01};
  cl_assert_equal_i(s_sent[0].length, sizeof(list_request));
  cl_assert_equal_m(s_sent[0].data, list_request, sizeof(list_request));

  // InstallStatus for "fw:drew", Transferring, 42 %, no message.
  DELIVER(0x44, 0x52, 0x45, 0x57, 0x01, 0x01, 0x83, 0x07, 0x66, 0x77, 0x3a, 0x64, 0x72, 0x65, 0x77,
          0x02, 0x2a, 0x00);
  cl_assert_equal_i(s_num_updates_messages, 1);
  cl_assert_equal_i(s_updates_command, 0x83);
  const uint8_t install_status[] = {0x07, 0x66, 0x77, 0x3a, 0x64, 0x72,
                                    0x65, 0x77, 0x02, 0x2a, 0x00};
  cl_assert_equal_i(s_updates_payload.length, sizeof(install_status));
  cl_assert_equal_m(s_updates_payload.data, install_status, sizeof(install_status));

  // An Updates command with the watch's direction bit isn't passed on.
  DELIVER(0x44, 0x52, 0x45, 0x57, 0x01, 0x01, 0x01, 0x01);
  cl_assert_equal_i(s_num_updates_messages, 1);
}

void test_drew_protocol__send_needs_a_session_and_fits_1024_bytes(void) {
  static uint8_t big[DREW_PROTOCOL_MAX_SEND_LENGTH];
  cl_assert(!drew_protocol_send(DrewServiceCore, 0x02, big, sizeof(big)));
  cl_assert(drew_protocol_send(DrewServiceCore, 0x02, big,
                               DREW_PROTOCOL_MAX_SEND_LENGTH - DREW_PROTOCOL_HEADER_LENGTH));
  s_session = nullptr;
  cl_assert(!drew_protocol_send(DrewServiceCore, 0x02, nullptr, 0));
}

void test_drew_protocol__bad_headers_are_dropped(void) {
  // Short, bad magic, bad version, a watch-to-phone command, an unknown service, an unknown
  // Core command.
  DELIVER(0x44, 0x52, 0x45, 0x57, 0x01, 0x00);
  DELIVER(0x44, 0x52, 0x45, 0x58, 0x01, 0x00, 0x81, 0x00, 0x08, 0x1f, 0, 0, 0, 0, 0, 0, 0, 0);
  DELIVER(0x44, 0x52, 0x45, 0x57, 0x02, 0x00, 0x81, 0x00, 0x08, 0x1f, 0, 0, 0, 0, 0, 0, 0, 0);
  DELIVER(HEADER(0x00, 0x01), 0x00, 0x08, 0x1f, 0, 0, 0, 0, 0, 0, 0, 0);
  DELIVER(HEADER(0x7f, 0x81), 0x00, 0x08, 0x1f, 0, 0, 0, 0, 0, 0, 0, 0);
  DELIVER(HEADER(0x00, 0x8f), 0x00, 0x08, 0x1f, 0, 0, 0, 0, 0, 0, 0, 0);
  cl_assert_equal_i(drew_protocol_phone_features(), 0);
  cl_assert_equal_i(s_num_feature_events, 0);
  cl_assert_equal_i(s_num_sent, 0);
}

// Hellos
//////////

void test_drew_protocol__phone_hello_from_the_spec(void) {
  // PhoneHello, answer, features 0-4, build "1.16.0.4-drew.3".
  DELIVER(0x44, 0x52, 0x45, 0x57, 0x01, 0x00, 0x81, 0x00, 0x08, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00,
          0x00, 0x00, 0x0f, 0x31, 0x2e, 0x31, 0x36, 0x2e, 0x30, 0x2e, 0x34, 0x2d, 0x64, 0x72, 0x65,
          0x77, 0x2e, 0x33);
  cl_assert_equal_i(drew_protocol_phone_features(), 0x1f);
  cl_assert_equal_i(s_num_feature_events, 1);
  // An answer never asks for one back.
  cl_assert_equal_i(s_num_sent, 0);
  // The retries stop.
  cl_assert(!stub_new_timer_is_scheduled(s_new_timer_start_param_timer_id));
}

void test_drew_protocol__hello_reply_only_on_request(void) {
  prv_phone_hello(0x01, 0x1f);
  cl_assert_equal_i(s_num_sent, 1);
  prv_assert_watch_hello(&s_sent[0], 0x00);
  prv_phone_hello(0x00, 0x1f);
  cl_assert_equal_i(s_num_sent, 1);
}

void test_drew_protocol__hello_bitmap_lengths(void) {
  // No bitmap at all.
  DELIVER(HEADER(0x00, 0x81), 0x00, 0x00, 0x00);
  cl_assert_equal_i(drew_protocol_phone_features(), 0);
  cl_assert_equal_i(s_num_feature_events, 1);
  // One byte.
  DELIVER(HEADER(0x00, 0x81), 0x00, 0x01, 0x14, 0x00);
  cl_assert_equal_i(drew_protocol_phone_features(), 0x14);
  // 32 bytes: bits 64 and up are ignored.
  uint8_t hello[DREW_PROTOCOL_HEADER_LENGTH + 2 + 32 + 1] = {HEADER(0x00, 0x81), 0x00, 32};
  memset(&hello[DREW_PROTOCOL_HEADER_LENGTH + 2], 0xff, 32);
  memset(&hello[DREW_PROTOCOL_HEADER_LENGTH + 2], 0x00, 8);
  hello[DREW_PROTOCOL_HEADER_LENGTH + 2] = 0x01;
  hello[DREW_PROTOCOL_HEADER_LENGTH + 2 + 7] = 0x80;
  hello[sizeof(hello) - 1] = 0;
  prv_deliver(hello, sizeof(hello));
  cl_assert(drew_protocol_phone_features() == 0x8000000000000001ULL);
  // 33 bytes is too many.
  uint8_t too_long[DREW_PROTOCOL_HEADER_LENGTH + 2 + 33 + 1] = {HEADER(0x00, 0x81), 0x00, 33};
  too_long[DREW_PROTOCOL_HEADER_LENGTH + 2] = 0x1f;
  prv_deliver(too_long, sizeof(too_long));
  cl_assert(drew_protocol_phone_features() == 0x8000000000000001ULL);
}

void test_drew_protocol__truncated_hellos_are_dropped(void) {
  // Ends inside the bitmap, ends after the bitmap (no build_len), ends inside the build.
  DELIVER(HEADER(0x00, 0x81), 0x00, 0x08, 0x1f, 0, 0);
  DELIVER(HEADER(0x00, 0x81), 0x00, 0x08, 0x1f, 0, 0, 0, 0, 0, 0, 0);
  DELIVER(HEADER(0x00, 0x81), 0x00, 0x08, 0x1f, 0, 0, 0, 0, 0, 0, 0, 0x03, 'a', 'b');
  DELIVER(HEADER(0x00, 0x81), 0x00);
  cl_assert_equal_i(drew_protocol_phone_features(), 0);
  cl_assert_equal_i(s_num_feature_events, 0);
}

void test_drew_protocol__trailing_bytes_are_ignored(void) {
  DELIVER(HEADER(0x00, 0x81), 0x01, 0x01, 0x1f, 0x02, 'o', 'k', 0xaa, 0xbb, 0xcc);
  cl_assert_equal_i(drew_protocol_phone_features(), 0x1f);
  cl_assert_equal_i(s_num_sent, 1);
}

void test_drew_protocol__unasked_hello_replaces_features(void) {
  prv_phone_hello(0x00, 0x1f);
  cl_assert(drew_protocol_has_feature(DrewFeaturePrefs));
  prv_phone_hello(0x00, 0x0f);
  cl_assert_equal_i(drew_protocol_phone_features(), 0x0f);
  cl_assert(!drew_protocol_has_feature(DrewFeaturePrefs));
  cl_assert_equal_i(s_num_feature_events, 2);
}

void test_drew_protocol__has_feature_is_the_and(void) {
  cl_assert_equal_i(drew_protocol_local_features(), LOCAL_FEATURES);
  // The phone announces everything, including bits this firmware doesn't know.
  prv_phone_hello(0x00, UINT64_MAX);
  for (int bit = 0; bit < 64; bit++) {
    cl_assert_equal_b(drew_protocol_has_feature((DrewFeature)bit),
                      (LOCAL_FEATURES & DREW_FEATURE_BIT(bit)) != 0);
  }
  prv_phone_hello(0x00, 0);
  cl_assert(!drew_protocol_has_feature(DrewFeaturePrefs));
}

// Handshake
/////////////

void test_drew_protocol__session_open_sends_hello_and_retries(void) {
  prv_session_event(true /* is_open */);
  cl_assert_equal_i(s_num_sent, 1);
  prv_assert_watch_hello(&s_sent[0], 0x01);
  cl_assert_equal_i(s_new_timer_start_param_timeout_ms, 5000);

  prv_fire_hello_timer();
  prv_fire_hello_timer();
  cl_assert_equal_i(s_num_sent, 3);
  prv_assert_watch_hello(&s_sent[2], 0x01);

  // The phone answers the third.
  prv_phone_hello(0x00, 0x1f);
  cl_assert_equal_i(drew_protocol_phone_features(), 0x1f);
  cl_assert(!stub_new_timer_is_scheduled(s_new_timer_start_param_timer_id));
  cl_assert_equal_i(s_num_sent, 3);
}

void test_drew_protocol__three_unanswered_hellos_clear_the_cache(void) {
  prv_phone_hello(0x00, 0x1f);
  s_num_feature_events = 0;

  prv_session_event(true /* is_open */);
  prv_fire_hello_timer();
  prv_fire_hello_timer();
  cl_assert_equal_i(s_num_sent, 3);
  // Still the cached features while waiting.
  cl_assert_equal_i(drew_protocol_phone_features(), 0x1f);
  prv_fire_hello_timer();
  cl_assert_equal_i(s_num_sent, 3);
  cl_assert_equal_i(drew_protocol_phone_features(), 0);
  cl_assert_equal_i(s_num_feature_events, 1);
  cl_assert(!stub_new_timer_is_scheduled(s_new_timer_start_param_timer_id));
}

void test_drew_protocol__session_close_stops_the_retries(void) {
  prv_session_event(true /* is_open */);
  prv_session_event(false /* is_open */);
  cl_assert(!stub_new_timer_is_scheduled(s_new_timer_start_param_timer_id));
  cl_assert_equal_i(s_num_sent, 1);
}

// Heartbeat
/////////////

static void prv_fire_heartbeat_timer(void) {
  RegularTimerInfo *timer = (RegularTimerInfo *)list_get_next(&s_minutes_callbacks);
  cl_assert(timer);
  fake_regular_timer_trigger(timer);
  fake_system_task_callbacks_invoke_pending();
}

void test_drew_protocol__heartbeat_only_when_active(void) {
  prv_fire_heartbeat_timer();
  cl_assert_equal_i(s_num_sent, 0);
  // Every feature but Heartbeat.
  prv_phone_hello(0x00, 0x1d);
  prv_fire_heartbeat_timer();
  cl_assert_equal_i(s_num_sent, 0);

  prv_phone_hello(0x00, 0x1f);
  for (int i = 0; i < 7; i++) {
    prv_fire_heartbeat_timer();
  }
  cl_assert_equal_i(s_num_sent, 7);
  // The seventh heartbeat since boot has cookie 7.
  const uint8_t heartbeat[] = {0x44, 0x52, 0x45, 0x57, 0x01, 0x00, 0x02, 0x07, 0x00, 0x00, 0x00};
  cl_assert_equal_i(s_sent[6].length, sizeof(heartbeat));
  cl_assert_equal_m(s_sent[6].data, heartbeat, sizeof(heartbeat));

  // Its ack is only logged.
  DELIVER(0x44, 0x52, 0x45, 0x57, 0x01, 0x00, 0x82, 0x07, 0x00, 0x00, 0x00);
  DELIVER(0x44, 0x52, 0x45, 0x57, 0x01, 0x00, 0x82, 0x07);
  cl_assert_equal_i(s_num_sent, 7);
  cl_assert_equal_i(s_num_feature_events, 2);

  s_session = nullptr;
  prv_fire_heartbeat_timer();
  cl_assert_equal_i(s_num_sent, 7);
}

// Cache
/////////

void test_drew_protocol__cache_belongs_to_the_active_phone(void) {
  prv_phone_hello(0x00, 0x1f);
  cl_assert_equal_i(drew_protocol_phone_features(), 0x1f);
  s_gateway = 4;
  cl_assert_equal_i(drew_protocol_phone_features(), 0);
  cl_assert(!drew_protocol_has_feature(DrewFeaturePrefs));
  s_has_gateway = false;
  cl_assert_equal_i(drew_protocol_phone_features(), 0);
  s_has_gateway = true;
  s_gateway = 3;
  cl_assert_equal_i(drew_protocol_phone_features(), 0x1f);
}

void test_drew_protocol__cache_without_a_gateway(void) {
  // QEMU has no bonding: the cache is kept for "no phone".
  s_has_gateway = false;
  prv_phone_hello(0x00, 0x1f);
  cl_assert_equal_i(drew_protocol_phone_features(), 0x1f);
  s_has_gateway = true;
  cl_assert_equal_i(drew_protocol_phone_features(), 0);
}

void test_drew_protocol__cache_survives_a_reboot(void) {
  prv_phone_hello(0x00, 0x1f);
  drew_protocol_init();
  cl_assert_equal_i(drew_protocol_phone_features(), 0x1f);
  fake_settings_file_reset();
  drew_protocol_init();
  cl_assert_equal_i(drew_protocol_phone_features(), 0);
}

void test_drew_protocol__deleting_the_bonding_clears_the_cache(void) {
  prv_phone_hello(0x00, 0x1f);
  s_num_feature_events = 0;
  drew_protocol_handle_bonding_change(2, BtPersistBondingOpWillDelete);
  drew_protocol_handle_bonding_change(3, BtPersistBondingOpDidChange);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(drew_protocol_phone_features(), 0x1f);
  drew_protocol_handle_bonding_change(3, BtPersistBondingOpWillDelete);
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(drew_protocol_phone_features(), 0);
  cl_assert_equal_i(s_num_feature_events, 1);
  // A new phone that gets the same bonding id starts without features.
  drew_protocol_init();
  cl_assert_equal_i(drew_protocol_phone_features(), 0);
}

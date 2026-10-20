/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <pbl/drivers/rtc.h>
#include <pbl/kernel/mutex.h>
#include <pbl/logging/logging.h>
#include <pbl/services/drew_pin_lock.h>
#include <pbl/services/settings/settings_file.h>
#include <pbl/util/math.h>
#include <pbl/util/rand32.h>

#include <apps/system_app_ids.h>
#include <process_management/app_install_manager.h>

PBL_LOG_MODULE_DEFINE(service_drew_pin_lock, CONFIG_SERVICE_DREW_PIN_LOCK_LOG_LEVEL);

//! @file
//! The PIN record and the locked apps share the "drewpin" settings file: the record under the key
//! "pin", and one 1-byte record per locked app keyed by its 16-byte UUID. It isn't a shell pref,
//! so the phone can neither read nor sync it.

#define PIN_FILE      "drewpin"
#define PIN_FILE_SIZE (2048)
#define PIN_KEY       "pin"
#define PIN_VERSION   (1)
#define SALT_LENGTH   (16)
#define HASH_LENGTH   (32)

typedef struct PBL_PACKED PinRecord {
  uint8_t version;
  uint8_t length;
  //! Wrong tries in a row.
  uint8_t fails;
  uint8_t reserved;
  //! RTC time the current lockout ends, 0 if none.
  uint32_t lockout_until;
  uint8_t salt[SALT_LENGTH];
  uint8_t hash[HASH_LENGTH];
} PinRecord;

typedef enum {
  PinStateUnknown,
  PinStateNone,
  PinStateSet,
} PinState;

static PBL_MUTEX_DEFINE(s_mutex);
//! Cached so launches don't read flash while no PIN is set.
static PinState s_state;

// SHA-256 (FIPS 180-4)
////////////////////////

typedef struct Sha256 {
  uint32_t h[8];
  uint8_t block[64];
  uint32_t block_length;
  uint64_t total_length;
} Sha256;

static const uint32_t s_k[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t prv_ror(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32 - n));
}

static void prv_sha256_compress(Sha256 *ctx) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) {
    const uint8_t *p = &ctx->block[i * 4];
    w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
  }
  for (int i = 16; i < 64; i++) {
    const uint32_t s0 = prv_ror(w[i - 15], 7) ^ prv_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = prv_ror(w[i - 2], 17) ^ prv_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t v[8];
  memcpy(v, ctx->h, sizeof(v));
  for (int i = 0; i < 64; i++) {
    const uint32_t s1 = prv_ror(v[4], 6) ^ prv_ror(v[4], 11) ^ prv_ror(v[4], 25);
    const uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
    const uint32_t t1 = v[7] + s1 + ch + s_k[i] + w[i];
    const uint32_t s0 = prv_ror(v[0], 2) ^ prv_ror(v[0], 13) ^ prv_ror(v[0], 22);
    const uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
    memmove(&v[1], &v[0], 7 * sizeof(uint32_t));
    v[4] += t1;
    v[0] = t1 + s0 + maj;
  }
  for (int i = 0; i < 8; i++) {
    ctx->h[i] += v[i];
  }
}

static void prv_sha256_init(Sha256 *ctx) {
  static const uint32_t s_h0[8] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  };
  *ctx = (Sha256){};
  memcpy(ctx->h, s_h0, sizeof(s_h0));
}

static void prv_sha256_update(Sha256 *ctx, const void *data, size_t length) {
  const uint8_t *bytes = data;
  ctx->total_length += length;
  while (length--) {
    ctx->block[ctx->block_length++] = *bytes++;
    if (ctx->block_length == sizeof(ctx->block)) {
      prv_sha256_compress(ctx);
      ctx->block_length = 0;
    }
  }
}

static void prv_sha256_final(Sha256 *ctx, uint8_t digest[HASH_LENGTH]) {
  const uint64_t bits = ctx->total_length * 8;
  const uint8_t pad = 0x80;
  prv_sha256_update(ctx, &pad, 1);
  const uint8_t zero = 0;
  while (ctx->block_length != 56) {
    prv_sha256_update(ctx, &zero, 1);
  }
  for (int i = 7; i >= 0; i--) {
    const uint8_t b = (uint8_t)(bits >> (i * 8));
    prv_sha256_update(ctx, &b, 1);
  }
  for (int i = 0; i < 8; i++) {
    digest[i * 4] = (uint8_t)(ctx->h[i] >> 24);
    digest[i * 4 + 1] = (uint8_t)(ctx->h[i] >> 16);
    digest[i * 4 + 2] = (uint8_t)(ctx->h[i] >> 8);
    digest[i * 4 + 3] = (uint8_t)ctx->h[i];
  }
}

void drew_pin_lock_sha256(const void *data, size_t length, uint8_t digest[HASH_LENGTH]) {
  Sha256 ctx;
  prv_sha256_init(&ctx);
  prv_sha256_update(&ctx, data, length);
  prv_sha256_final(&ctx, digest);
}

//! SHA-256(salt || pin), then DREW_PIN_LOCK_HASH_ITERATIONS - 1 rounds of SHA-256(hash || salt).
static void prv_hash_pin(const char *pin, const uint8_t salt[SALT_LENGTH],
                         uint8_t hash[HASH_LENGTH]) {
  Sha256 ctx;
  prv_sha256_init(&ctx);
  prv_sha256_update(&ctx, salt, SALT_LENGTH);
  prv_sha256_update(&ctx, pin, strlen(pin));
  prv_sha256_final(&ctx, hash);
  for (int i = 1; i < DREW_PIN_LOCK_HASH_ITERATIONS; i++) {
    prv_sha256_init(&ctx);
    prv_sha256_update(&ctx, hash, HASH_LENGTH);
    prv_sha256_update(&ctx, salt, SALT_LENGTH);
    prv_sha256_final(&ctx, hash);
  }
}

// Storage
///////////

static bool prv_open(SettingsFile *file) {
  return settings_file_open(file, PIN_FILE, PIN_FILE_SIZE) == S_SUCCESS;
}

static bool prv_read_record(SettingsFile *file, PinRecord *record) {
  if ((settings_file_get_len(file, PIN_KEY, sizeof(PIN_KEY)) != sizeof(*record)) ||
      (settings_file_get(file, PIN_KEY, sizeof(PIN_KEY), record, sizeof(*record)) != S_SUCCESS)) {
    return false;
  }
  return (record->version == PIN_VERSION) && (record->length >= DREW_PIN_LOCK_MIN_DIGITS) &&
         (record->length <= DREW_PIN_LOCK_MAX_DIGITS);
}

static void prv_write_record(SettingsFile *file, const PinRecord *record) {
  settings_file_set(file, PIN_KEY, sizeof(PIN_KEY), record, sizeof(*record));
}

//! Load the record with the mutex held. @return false if there's no PIN (or no file).
static bool prv_load(PinRecord *record) {
  if (s_state == PinStateNone) {
    return false;
  }
  SettingsFile file;
  if (!prv_open(&file)) {
    return false;
  }
  const bool found = prv_read_record(&file, record);
  settings_file_close(&file);
  s_state = found ? PinStateSet : PinStateNone;
  return found;
}

static void prv_store(const PinRecord *record) {
  SettingsFile file;
  if (prv_open(&file)) {
    prv_write_record(&file, record);
    settings_file_close(&file);
  }
}

//! Seconds left of @p record's lockout. A clock set back can't stretch it past the maximum.
static uint32_t prv_lockout_remaining(const PinRecord *record) {
  const uint32_t now = (uint32_t)rtc_get_time();
  if (record->lockout_until <= now) {
    return 0;
  }
  return MIN(record->lockout_until - now, (uint32_t)DREW_PIN_LOCK_LOCKOUT_MAX_S);
}

static uint32_t prv_lockout_duration(uint8_t fails) {
  const unsigned doublings = MIN(fails - DREW_PIN_LOCK_FREE_ATTEMPTS, 5);
  return MIN((uint32_t)DREW_PIN_LOCK_LOCKOUT_BASE_S << doublings,
             (uint32_t)DREW_PIN_LOCK_LOCKOUT_MAX_S);
}

// API
///////

bool drew_pin_lock_is_valid_pin(const char *pin) {
  if (!pin) {
    return false;
  }
  const size_t length = strlen(pin);
  if ((length < DREW_PIN_LOCK_MIN_DIGITS) || (length > DREW_PIN_LOCK_MAX_DIGITS)) {
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    if ((pin[i] < '0') || (pin[i] > '9')) {
      return false;
    }
  }
  return true;
}

bool drew_pin_lock_has_pin(void) {
  return drew_pin_lock_get_length() != 0;
}

uint8_t drew_pin_lock_get_length(void) {
  PinRecord record;
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const bool found = prv_load(&record);
  pbl_mutex_unlock(&s_mutex);
  return found ? record.length : 0;
}

bool drew_pin_lock_set_pin(const char *pin) {
  if (!drew_pin_lock_is_valid_pin(pin)) {
    return false;
  }
  PinRecord record = {
    .version = PIN_VERSION,
    .length = (uint8_t)strlen(pin),
  };
  for (int i = 0; i < SALT_LENGTH; i += 4) {
    const uint32_t r = pbl_rand32() ^ (uint32_t)rtc_get_ticks();
    memcpy(&record.salt[i], &r, sizeof(r));
  }
  prv_hash_pin(pin, record.salt, record.hash);

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  SettingsFile file;
  bool stored = false;
  if (prv_open(&file)) {
    prv_write_record(&file, &record);
    PinRecord check;
    stored = prv_read_record(&file, &check) && (memcmp(&check, &record, sizeof(record)) == 0);
    settings_file_close(&file);
  }
  s_state = stored ? PinStateSet : PinStateUnknown;
  pbl_mutex_unlock(&s_mutex);
  PBL_LOG_INFO("PIN set: %s", stored ? "ok" : "failed");
  return stored;
}

static bool prv_drop_all_cb(void *key, size_t key_len, void *value, size_t value_len,
                            void *context) {
  return false;
}

void drew_pin_lock_remove(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  SettingsFile file;
  if (prv_open(&file)) {
    settings_file_rewrite_filtered(&file, prv_drop_all_cb, nullptr);
    settings_file_close(&file);
  }
  s_state = PinStateNone;
  pbl_mutex_unlock(&s_mutex);
  PBL_LOG_INFO("PIN removed");
}

DrewPinResult drew_pin_lock_check(const char *pin) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  PinRecord record;
  DrewPinResult result;
  if (!prv_load(&record)) {
    result = DrewPinResult_NoPin;
  } else if (prv_lockout_remaining(&record) > 0) {
    result = DrewPinResult_LockedOut;
  } else {
    uint8_t hash[HASH_LENGTH];
    uint8_t diff = 0;
    if (drew_pin_lock_is_valid_pin(pin)) {
      prv_hash_pin(pin, record.salt, hash);
      for (int i = 0; i < HASH_LENGTH; i++) {
        diff |= hash[i] ^ record.hash[i];
      }
    } else {
      diff = 1;
    }
    if (diff == 0) {
      result = DrewPinResult_Ok;
      if (record.fails || record.lockout_until) {
        record.fails = 0;
        record.lockout_until = 0;
        prv_store(&record);
      }
    } else {
      result = DrewPinResult_Wrong;
      if (record.fails < UINT8_MAX) {
        record.fails++;
      }
      if (record.fails >= DREW_PIN_LOCK_FREE_ATTEMPTS) {
        record.lockout_until = (uint32_t)rtc_get_time() + prv_lockout_duration(record.fails);
      }
      prv_store(&record);
    }
  }
  pbl_mutex_unlock(&s_mutex);
  if (result == DrewPinResult_Wrong) {
    PBL_LOG_INFO("Wrong PIN");
  }
  return result;
}

uint32_t drew_pin_lock_get_lockout_remaining(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  PinRecord record;
  const uint32_t remaining = prv_load(&record) ? prv_lockout_remaining(&record) : 0;
  pbl_mutex_unlock(&s_mutex);
  return remaining;
}

uint8_t drew_pin_lock_get_attempts_left(void) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  PinRecord record;
  uint8_t left = 0;
  if (prv_load(&record)) {
    left = (record.fails < DREW_PIN_LOCK_FREE_ATTEMPTS) ? DREW_PIN_LOCK_FREE_ATTEMPTS - record.fails
                                                        : 1;
  }
  pbl_mutex_unlock(&s_mutex);
  return left;
}

bool drew_pin_lock_is_app_locked(const Uuid *uuid) {
  if (!uuid || uuid_is_invalid(uuid)) {
    return false;
  }
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  bool locked = false;
  SettingsFile file;
  if ((s_state != PinStateNone) && prv_open(&file)) {
    PinRecord record;
    const bool has_pin = prv_read_record(&file, &record);
    s_state = has_pin ? PinStateSet : PinStateNone;
    locked = has_pin && settings_file_exists(&file, uuid, sizeof(*uuid));
    settings_file_close(&file);
  }
  pbl_mutex_unlock(&s_mutex);
  return locked;
}

void drew_pin_lock_set_app_locked(const Uuid *uuid, bool locked) {
  if (!uuid || uuid_is_invalid(uuid)) {
    return;
  }
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  SettingsFile file;
  if (prv_open(&file)) {
    if (locked) {
      const uint8_t value = 1;
      settings_file_set(&file, uuid, sizeof(*uuid), &value, sizeof(value));
    } else {
      settings_file_delete(&file, uuid, sizeof(*uuid));
    }
    settings_file_close(&file);
  }
  pbl_mutex_unlock(&s_mutex);
}

static bool prv_count_cb(SettingsFile *file, SettingsRecordInfo *info, void *context) {
  if (info->key_len == sizeof(Uuid)) {
    Uuid uuid;
    info->get_key(file, &uuid, sizeof(uuid));
    if (app_install_get_id_for_uuid(&uuid) != INSTALL_ID_INVALID) {
      (*(uint16_t *)context)++;
    }
  }
  return true;
}

uint16_t drew_pin_lock_get_num_locked_apps(void) {
  uint16_t count = 0;
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  SettingsFile file;
  if (prv_open(&file)) {
    settings_file_each(&file, prv_count_cb, &count);
    settings_file_close(&file);
  }
  pbl_mutex_unlock(&s_mutex);
  return count;
}

bool drew_pin_lock_launch_needs_pin(int32_t id) {
  // Read without the mutex: a stale value only costs a lookup, and launches run on KernelMain.
  if ((s_state == PinStateNone) || (id == INSTALL_ID_INVALID) || (id == APP_ID_SETTINGS) ||
      (id == APP_ID_LAUNCHER_MENU)) {
    return false;
  }
  AppInstallEntry entry;
  if (!app_install_get_entry_for_install_id(id, &entry) || app_install_entry_is_watchface(&entry)) {
    return false;
  }
  return drew_pin_lock_is_app_locked(&entry.uuid);
}

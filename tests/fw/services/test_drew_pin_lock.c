/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <pbl/services/drew_pin_lock.h>

#include <apps/system_app_ids.h>
#include <clar.h>
#include <fake_rtc.h>
#include <fake_settings_file.h>
#include <process_management/app_install_manager.h>
#include <stubs_logging.h>
#include <stubs_mutex.h>
#include <stubs_passert.h>

// App install
///////////////

#define WATCHFACE_ID   (5)
#define UNINSTALLED_ID (6)

static Uuid prv_uuid(uint8_t n) {
  return (Uuid){n, n, n, n, n, n, n, n, n, n, n, n, n, n, n, n};
}

static AppInstallId prv_id_for(const Uuid *uuid) {
  return uuid->byte0;
}

AppInstallId app_install_get_id_for_uuid(const Uuid *uuid) {
  return (prv_id_for(uuid) == UNINSTALLED_ID) ? INSTALL_ID_INVALID : prv_id_for(uuid);
}

bool app_install_get_entry_for_install_id(AppInstallId install_id, AppInstallEntry *entry) {
  *entry = (AppInstallEntry){.install_id = install_id};
  if (install_id > 0) {
    entry->uuid = prv_uuid((uint8_t)install_id);
  }
  return true;
}

bool app_install_entry_is_watchface(const AppInstallEntry *entry) {
  return entry->install_id == WATCHFACE_ID;
}

uint32_t pbl_rand32(void) {
  static uint32_t s_next = 1;
  s_next = s_next * 1103515245u + 12345u;
  return s_next;
}

// Tests
/////////

void test_drew_pin_lock__initialize(void) {
  fake_settings_file_reset();
  fake_rtc_init(0, 1000000);
  // Forget the cached "no PIN" state between tests.
  drew_pin_lock_remove();
}

void test_drew_pin_lock__sha256_vectors(void) {
  uint8_t digest[32];
  static const uint8_t s_abc[32] = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
  };
  drew_pin_lock_sha256("abc", 3, digest);
  cl_assert_equal_m(digest, s_abc, sizeof(s_abc));

  static const uint8_t s_empty[32] = {
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
    0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
  };
  drew_pin_lock_sha256("", 0, digest);
  cl_assert_equal_m(digest, s_empty, sizeof(s_empty));

  // Two blocks: the padding spills into a second one.
  static const char *s_long = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  static const uint8_t s_two_blocks[32] = {
    0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
    0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1,
  };
  drew_pin_lock_sha256(s_long, strlen(s_long), digest);
  cl_assert_equal_m(digest, s_two_blocks, sizeof(s_two_blocks));
}

void test_drew_pin_lock__valid_pins(void) {
  cl_assert(drew_pin_lock_is_valid_pin("0000"));
  cl_assert(drew_pin_lock_is_valid_pin("123456"));
  cl_assert(!drew_pin_lock_is_valid_pin("123"));
  cl_assert(!drew_pin_lock_is_valid_pin("1234567"));
  cl_assert(!drew_pin_lock_is_valid_pin("12a4"));
  cl_assert(!drew_pin_lock_is_valid_pin(""));
  cl_assert(!drew_pin_lock_is_valid_pin(NULL));
  cl_assert(!drew_pin_lock_set_pin("123"));
  cl_assert(!drew_pin_lock_has_pin());
}

void test_drew_pin_lock__set_and_check(void) {
  cl_assert(!drew_pin_lock_has_pin());
  cl_assert_equal_i(drew_pin_lock_check("1234"), DrewPinResult_NoPin);

  cl_assert(drew_pin_lock_set_pin("2580"));
  cl_assert(drew_pin_lock_has_pin());
  cl_assert_equal_i(drew_pin_lock_get_length(), 4);
  cl_assert_equal_i(drew_pin_lock_check("2580"), DrewPinResult_Ok);
  cl_assert_equal_i(drew_pin_lock_check("2581"), DrewPinResult_Wrong);
  cl_assert_equal_i(drew_pin_lock_check("25800"), DrewPinResult_Wrong);
  cl_assert_equal_i(drew_pin_lock_check("2580"), DrewPinResult_Ok);

  // Changing it keeps only the new one.
  cl_assert(drew_pin_lock_set_pin("123456"));
  cl_assert_equal_i(drew_pin_lock_get_length(), 6);
  cl_assert_equal_i(drew_pin_lock_check("2580"), DrewPinResult_Wrong);
  cl_assert_equal_i(drew_pin_lock_check("123456"), DrewPinResult_Ok);

  drew_pin_lock_remove();
  cl_assert(!drew_pin_lock_has_pin());
  cl_assert_equal_i(drew_pin_lock_check("123456"), DrewPinResult_NoPin);
}

static bool prv_find_value_cb(SettingsFile *file, SettingsRecordInfo *info, void *context) {
  uint8_t value[128];
  if (info->val_len <= sizeof(value)) {
    info->get_val(file, value, info->val_len);
    for (int i = 0; i + 4 <= info->val_len; i++) {
      if (memcmp(&value[i], context, 4) == 0) {
        cl_fail("The PIN is stored in plain text");
      }
    }
  }
  return true;
}

void test_drew_pin_lock__stores_a_salted_hash(void) {
  cl_assert(drew_pin_lock_set_pin("2580"));
  SettingsFile file;
  cl_assert_equal_i(settings_file_open(&file, "drewpin", 2048), S_SUCCESS);
  settings_file_each(&file, prv_find_value_cb, "2580");

  uint8_t first[64];
  const int length = settings_file_get_len(&file, "pin", sizeof("pin"));
  cl_assert(length > 0 && length <= (int)sizeof(first));
  settings_file_get(&file, "pin", sizeof("pin"), first, length);
  settings_file_close(&file);

  // The same PIN again gets a new salt, so a different record.
  cl_assert(drew_pin_lock_set_pin("2580"));
  uint8_t second[64];
  cl_assert_equal_i(settings_file_open(&file, "drewpin", 2048), S_SUCCESS);
  settings_file_get(&file, "pin", sizeof("pin"), second, length);
  settings_file_close(&file);
  cl_assert(memcmp(first, second, length) != 0);
  cl_assert_equal_i(drew_pin_lock_check("2580"), DrewPinResult_Ok);
}

void test_drew_pin_lock__lockout(void) {
  cl_assert(drew_pin_lock_set_pin("2580"));
  for (int i = 0; i < DREW_PIN_LOCK_FREE_ATTEMPTS - 1; i++) {
    cl_assert_equal_i(drew_pin_lock_get_attempts_left(), DREW_PIN_LOCK_FREE_ATTEMPTS - i);
    cl_assert_equal_i(drew_pin_lock_check("0000"), DrewPinResult_Wrong);
    cl_assert_equal_i(drew_pin_lock_get_lockout_remaining(), 0);
  }
  cl_assert_equal_i(drew_pin_lock_get_attempts_left(), 1);

  // The fifth wrong try locks entry out for 30 s, even for the right PIN.
  cl_assert_equal_i(drew_pin_lock_check("0000"), DrewPinResult_Wrong);
  cl_assert_equal_i(drew_pin_lock_get_lockout_remaining(), 30);
  cl_assert_equal_i(drew_pin_lock_check("2580"), DrewPinResult_LockedOut);
  fake_rtc_increment_time(29);
  cl_assert_equal_i(drew_pin_lock_check("2580"), DrewPinResult_LockedOut);
  fake_rtc_increment_time(1);

  // Then each wrong try doubles it.
  cl_assert_equal_i(drew_pin_lock_check("0000"), DrewPinResult_Wrong);
  cl_assert_equal_i(drew_pin_lock_get_lockout_remaining(), 60);
  fake_rtc_increment_time(60);
  cl_assert_equal_i(drew_pin_lock_check("0000"), DrewPinResult_Wrong);
  cl_assert_equal_i(drew_pin_lock_get_lockout_remaining(), 120);

  // Up to 16 minutes.
  for (int i = 0; i < 6; i++) {
    fake_rtc_increment_time(DREW_PIN_LOCK_LOCKOUT_MAX_S);
    cl_assert_equal_i(drew_pin_lock_check("0000"), DrewPinResult_Wrong);
  }
  cl_assert_equal_i(drew_pin_lock_get_lockout_remaining(), DREW_PIN_LOCK_LOCKOUT_MAX_S);

  // The right PIN after the lockout resets the count.
  fake_rtc_increment_time(DREW_PIN_LOCK_LOCKOUT_MAX_S);
  cl_assert_equal_i(drew_pin_lock_check("2580"), DrewPinResult_Ok);
  cl_assert_equal_i(drew_pin_lock_get_attempts_left(), DREW_PIN_LOCK_FREE_ATTEMPTS);
}

void test_drew_pin_lock__lockout_survives_reboot_and_clock_changes(void) {
  cl_assert(drew_pin_lock_set_pin("2580"));
  for (int i = 0; i < DREW_PIN_LOCK_FREE_ATTEMPTS; i++) {
    drew_pin_lock_check("1111");
  }
  // Nothing but the settings file holds the state, so a reboot keeps it.
  cl_assert_equal_i(drew_pin_lock_get_lockout_remaining(), 30);
  // A clock set back can't stretch the lockout past the maximum.
  fake_rtc_init(0, 1000000 - 100000);
  cl_assert_equal_i(drew_pin_lock_get_lockout_remaining(), DREW_PIN_LOCK_LOCKOUT_MAX_S);
}

void test_drew_pin_lock__changing_the_pin_clears_the_lockout(void) {
  cl_assert(drew_pin_lock_set_pin("2580"));
  for (int i = 0; i < DREW_PIN_LOCK_FREE_ATTEMPTS; i++) {
    drew_pin_lock_check("1111");
  }
  cl_assert(drew_pin_lock_get_lockout_remaining() > 0);
  cl_assert(drew_pin_lock_set_pin("1357"));
  cl_assert_equal_i(drew_pin_lock_get_lockout_remaining(), 0);
  cl_assert_equal_i(drew_pin_lock_check("1357"), DrewPinResult_Ok);
}

void test_drew_pin_lock__locked_apps(void) {
  const Uuid music = prv_uuid(3);
  const Uuid timer = prv_uuid(4);
  const Uuid gone = prv_uuid(UNINSTALLED_ID);

  // Without a PIN nothing is locked, even if marked.
  drew_pin_lock_set_app_locked(&music, true);
  cl_assert(!drew_pin_lock_is_app_locked(&music));
  cl_assert(!drew_pin_lock_launch_needs_pin(3));

  cl_assert(drew_pin_lock_set_pin("2580"));
  cl_assert(drew_pin_lock_is_app_locked(&music));
  cl_assert(!drew_pin_lock_is_app_locked(&timer));
  drew_pin_lock_set_app_locked(&timer, true);
  drew_pin_lock_set_app_locked(&gone, true);
  cl_assert(drew_pin_lock_is_app_locked(&timer));
  // Only installed apps count.
  cl_assert_equal_i(drew_pin_lock_get_num_locked_apps(), 2);

  cl_assert(drew_pin_lock_launch_needs_pin(3));
  cl_assert(drew_pin_lock_launch_needs_pin(4));
  cl_assert(!drew_pin_lock_launch_needs_pin(7));

  drew_pin_lock_set_app_locked(&music, false);
  cl_assert(!drew_pin_lock_is_app_locked(&music));
  cl_assert(!drew_pin_lock_launch_needs_pin(3));
  cl_assert_equal_i(drew_pin_lock_get_num_locked_apps(), 1);

  // Changing the PIN keeps the locks; removing it drops them all.
  cl_assert(drew_pin_lock_set_pin("1357"));
  cl_assert(drew_pin_lock_is_app_locked(&timer));
  drew_pin_lock_remove();
  cl_assert(!drew_pin_lock_is_app_locked(&timer));
  cl_assert(drew_pin_lock_set_pin("2580"));
  cl_assert(!drew_pin_lock_is_app_locked(&timer));
  cl_assert_equal_i(drew_pin_lock_get_num_locked_apps(), 0);
}

void test_drew_pin_lock__never_locks_watchfaces_settings_or_the_launcher(void) {
  cl_assert(drew_pin_lock_set_pin("2580"));
  const Uuid face = prv_uuid(WATCHFACE_ID);
  drew_pin_lock_set_app_locked(&face, true);
  cl_assert(!drew_pin_lock_launch_needs_pin(WATCHFACE_ID));
  cl_assert(!drew_pin_lock_launch_needs_pin(APP_ID_SETTINGS));
  cl_assert(!drew_pin_lock_launch_needs_pin(APP_ID_LAUNCHER_MENU));
  cl_assert(!drew_pin_lock_launch_needs_pin(INSTALL_ID_INVALID));
}

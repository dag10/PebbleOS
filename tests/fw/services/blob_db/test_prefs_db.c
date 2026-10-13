/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/blob_db/prefs_db.h>
#include <pbl/services/filesystem/pfs.h>
#include <pbl/util/uuid.h>

#include <apps/system/quick_launch_watchface.h>
#include <clar.h>
#include <shell/normal/quick_launch.h>
#include <shell/prefs.h>
#include <shell/prefs_private.h>

// Fixture
////////////////////////////////////////////////////////////////

// Fakes
////////////////////////////////////////////////////////////////
#include <fake_kernel_services_notifications.h>
#include <fake_spi_flash.h>
#include <fake_system_task.h>

// Stubs
////////////////////////////////////////////////////////////////
#include <stubs_activity.h>
#include <stubs_ambient_light.h>
#include <stubs_analytics.h>
#include <stubs_app_install_manager.h>
#include <stubs_event_loop.h>
#include <stubs_hexdump.h>
#include <stubs_logging.h>
#include <stubs_mfg_info.h>
#include <stubs_mutex.h>
#include <stubs_passert.h>
#include <stubs_pbl_malloc.h>
#include <stubs_rand_ptr.h>
#include <stubs_sleep.h>
#include <stubs_system_theme.h>
#include <stubs_task_wdt.h>
#include <stubs_timeline_peek.h>

void prefs_sync_init(void) {
}

void event_put(PebbleEvent *event) {
}

void i18n_enable(bool enable) {
}

void test_prefs_db__initialize(void) {
  fake_spi_flash_init(0, 0x1000000);
  pfs_init(false);
}

void test_prefs_db__cleanup(void) {
}

void test_prefs_db__get_length(void) {
  Uuid uuid = {0, 1, 2, 3};
  const char *key = "workerId";
  int key_len = strlen(key);
  cl_assert_equal_i(prefs_db_insert((uint8_t *)key, key_len, (uint8_t *)&uuid, sizeof(uuid)), 0);
  cl_assert_equal_i(prefs_db_get_len((uint8_t *)key, key_len), sizeof(uuid));
}

void test_prefs_db__insert_and_read(void) {
  uint32_t set_value = 42;

  // NOTE: We intentionally put one garbage character after the key to catch errors
  // that assume the key is 0 terminated
  const char *key = "lightTimeoutMsX";
  int key_len = strlen(key) - 1;

  // Set initial value
  backlight_set_timeout_ms(set_value + 1);

  // Insert and check the length
  cl_assert_equal_i(
      prefs_db_insert((uint8_t *)key, key_len, (uint8_t *)&set_value, sizeof(set_value)), 0);
  cl_assert_equal_i(prefs_db_get_len((uint8_t *)key, key_len), sizeof(set_value));

  // Read it back
  uint32_t get_value;
  cl_assert_equal_i(
      prefs_db_read((uint8_t *)key, key_len, (uint8_t *)&get_value, sizeof(get_value)), 0);
  cl_assert_equal_i(set_value, get_value);

  // If we get the pref setting now, it should still be the old value because we haven't
  // issued the blob_db update event yet
  uint32_t get_pref = backlight_get_timeout_ms();
  cl_assert_equal_i(get_pref, set_value + 1);

  // Issue the blob_db update event
  PebbleBlobDBEvent event = (PebbleBlobDBEvent){
    .db_id = BlobDBIdPrefs,
    .type = BlobDBEventTypeInsert,
    .key = (uint8_t *)key,
    .key_len = key_len,
  };
  prefs_private_handle_blob_db_event(&event);
  get_pref = backlight_get_timeout_ms();
  cl_assert(get_pref == get_value);

  // Set new value using the set call and read it back using prefs_db
  uint32_t new_set_value = 4242;
  backlight_set_timeout_ms(new_set_value);
  cl_assert_equal_i(
      prefs_db_read((uint8_t *)key, key_len, (uint8_t *)&get_value, sizeof(get_value)), 0);
  cl_assert_equal_i(new_set_value, get_value);

  // Try and insert an unknown key. It should fail
  const char *bad_key = "bad_key";
  int bad_key_len = strlen(bad_key);
  cl_assert(prefs_db_insert((uint8_t *)bad_key, bad_key_len, (uint8_t *)&set_value,
                            sizeof(set_value)) < 0);
  cl_assert(prefs_db_get_len((uint8_t *)bad_key, bad_key_len) < 0);
  cl_assert(
      prefs_db_read((uint8_t *)bad_key, bad_key_len, (uint8_t *)&get_value, sizeof(get_value)) < 0);

  // Try and insert the wrong size for a known key, it should fail
  cl_assert(prefs_db_insert((uint8_t *)key, key_len, (uint8_t *)&set_value, sizeof(set_value) + 1) <
            0);
  // Read it back
  cl_assert(prefs_db_read((uint8_t *)key, key_len, (uint8_t *)&get_value, sizeof(get_value) + 1) <
            0);
}

// Mirrors QuickLaunchPreference in shell/normal/prefs.c, the record behind "qlSingleClick*".
typedef struct {
  bool enabled;
  Uuid uuid;
} TestQuickLaunchPreference;

static void prv_insert_tap_pref(const char *key, bool enabled, const Uuid *uuid) {
  const TestQuickLaunchPreference pref = {
    .enabled = enabled,
    .uuid = *uuid,
  };
  const int key_len = strlen(key);
  cl_assert_equal_i(prefs_db_insert((uint8_t *)key, key_len, (uint8_t *)&pref, sizeof(pref)), 0);
  PebbleBlobDBEvent event = (PebbleBlobDBEvent){
    .db_id = BlobDBIdPrefs,
    .type = BlobDBEventTypeInsert,
    .key = (uint8_t *)key,
    .key_len = key_len,
  };
  prefs_private_handle_blob_db_event(&event);
}

void test_prefs_db__quick_launch_tap_sent_to_watchface(void) {
  const Uuid watchface_uuid = QUICK_LAUNCH_WATCHFACE_UUID;
  const Uuid other_uuid = {0, 1, 2, 3};

  // The defaults (Health / Timeline) keep both taps for the system.
  cl_assert(!quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_UP));
  cl_assert(!quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_DOWN));

  // Up and Down are independent.
  prv_insert_tap_pref("qlSingleClickUp", true, &watchface_uuid);
  cl_assert(quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_UP));
  cl_assert(!quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_DOWN));

  prv_insert_tap_pref("qlSingleClickDown", true, &watchface_uuid);
  cl_assert(quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_DOWN));

  // A disabled binding, or one to anything else, keeps the tap for the system.
  prv_insert_tap_pref("qlSingleClickUp", false, &watchface_uuid);
  cl_assert(!quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_UP));
  prv_insert_tap_pref("qlSingleClickDown", true, &other_uuid);
  cl_assert(!quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_DOWN));

  // Select has no tap binding, and Back's is unassigned by default.
  cl_assert(!quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_SELECT));
  cl_assert(!quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_BACK));
}

void test_prefs_db__quick_launch_tap_back_sent_to_watchface(void) {
  const Uuid watchface_uuid = QUICK_LAUNCH_WATCHFACE_UUID;

  // Unassigned by default, which keeps Back's stock tap.
  cl_assert(!quick_launch_single_click_is_enabled(BUTTON_ID_BACK));
  cl_assert(!quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_BACK));

  prv_insert_tap_pref("qlSingleClickBack", true, &watchface_uuid);
  cl_assert(quick_launch_single_click_is_enabled(BUTTON_ID_BACK));
  cl_assert(quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_BACK));
  cl_assert(!quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_SELECT));

  // The prefs outlive the test.
  const Uuid invalid_uuid = UUID_INVALID_INIT;
  prv_insert_tap_pref("qlSingleClickBack", false, &invalid_uuid);
}

void test_prefs_db__quick_launch_hold_sent_to_watchface(void) {
  const Uuid watchface_uuid = QUICK_LAUNCH_WATCHFACE_UUID;

  // The defaults (unassigned, Back toggles Quiet Time) keep every hold for the system.
  for (ButtonId button = 0; button < NUM_BUTTONS; button++) {
    cl_assert(!quick_launch_is_sent_to_watchface(button));
  }

  prv_insert_tap_pref("qlSelect", true, &watchface_uuid);
  prv_insert_tap_pref("qlBack", true, &watchface_uuid);
  cl_assert(quick_launch_is_sent_to_watchface(BUTTON_ID_SELECT));
  cl_assert(quick_launch_is_sent_to_watchface(BUTTON_ID_BACK));
  cl_assert(!quick_launch_is_sent_to_watchface(BUTTON_ID_UP));
  // A hold binding is independent of the tap binding of the same button.
  cl_assert(!quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_BACK));

  // The prefs outlive the test.
  const Uuid invalid_uuid = UUID_INVALID_INIT;
  prv_insert_tap_pref("qlSelect", false, &invalid_uuid);
  prv_insert_tap_pref("qlBack", false, &invalid_uuid);
}

void test_prefs_db__quick_launch_squeeze(void) {
  const Uuid other_uuid = {0, 1, 2, 3};

  cl_assert(!quick_launch_squeeze_is_enabled(BUTTON_ID_UP));
  cl_assert(!quick_launch_squeeze_is_enabled(BUTTON_ID_DOWN));

  prv_insert_tap_pref("qlSqueezeBackDown", true, &other_uuid);
  cl_assert(!quick_launch_squeeze_is_enabled(BUTTON_ID_UP));
  cl_assert(quick_launch_squeeze_is_enabled(BUTTON_ID_DOWN));

  // An invalid UUID is never enabled.
  const Uuid invalid_uuid = UUID_INVALID_INIT;
  prv_insert_tap_pref("qlSqueezeBackDown", true, &invalid_uuid);
  cl_assert(!quick_launch_squeeze_is_enabled(BUTTON_ID_DOWN));
}

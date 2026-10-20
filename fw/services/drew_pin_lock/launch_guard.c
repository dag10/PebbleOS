/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/drivers/rtc.h>
#include <pbl/logging/logging.h>
#include <pbl/services/drew_pin_lock.h>

#include <applib/ui/window_stack.h>
#include <apps/system/app_fetch_ui.h>
#include <kernel/events.h>
#include <kernel/pbl_malloc.h>
#include <kernel/pebble_tasks.h>
#include <popups/pin_prompt.h>
#include <process_management/app_install_manager.h>
#include <process_management/app_manager.h>
#include <system/passert.h>

PBL_LOG_MODULE_DECLARE(service_drew_pin_lock, CONFIG_SERVICE_DREW_PIN_LOCK_LOG_LEVEL);

//! @file
//! Every launch of an app goes through PEBBLE_APP_LAUNCH_EVENT on KernelMain, whether it comes
//! from the launcher, Quick Launch, a notification or timeline action, another app, a wakeup or
//! the phone. For a locked app this keeps the event's data and shows the PIN prompt as a modal
//! over whatever is on screen, so cancelling leaves the user where they were. The right PIN posts
//! the launch again, which is let through once.

//! How long an approved launch may take to come back around the event queue.
#define APPROVAL_TIMEOUT_MS (5000)

static Window *s_prompt;
static void *s_pending_data;
static AppInstallId s_pending_id = INSTALL_ID_INVALID;
//! Tells a cancelled prompt's late callback from the current one's.
static uintptr_t s_generation;
static AppInstallId s_approved_id = INSTALL_ID_INVALID;
static RtcTicks s_approved_ticks;

static void prv_drop_pending(void) {
  kernel_free(s_pending_data);
  s_pending_data = nullptr;
  s_pending_id = INSTALL_ID_INVALID;
  s_prompt = nullptr;
}

static void prv_prompt_done(bool success, const char *pin, void *context) {
  if ((uintptr_t)context != s_generation) {
    return;
  }
  s_prompt = nullptr;
  if (!success) {
    PBL_LOG_DBG("PIN prompt cancelled");
    prv_drop_pending();
    return;
  }
  s_approved_id = s_pending_id;
  s_approved_ticks = rtc_get_ticks();
  PebbleEvent e = {
    .type = PEBBLE_APP_LAUNCH_EVENT,
    .launch_app = {.id = s_pending_id, .data = s_pending_data},
  };
  s_pending_data = nullptr;
  s_pending_id = INSTALL_ID_INVALID;
  event_put(&e);
}

static bool prv_take_approval(AppInstallId id) {
  const bool approved =
      (s_approved_id == id) &&
      ((rtc_get_ticks() - s_approved_ticks) * 1000 / RTC_TICKS_HZ < APPROVAL_TIMEOUT_MS);
  s_approved_id = INSTALL_ID_INVALID;
  return approved;
}

bool drew_pin_lock_handle_launch(int32_t id, void *data) {
  PBL_ASSERT_TASK(PebbleTask_KernelMain);
  if (prv_take_approval(id)) {
    return false;
  }
  // The App Fetch UI relaunches the app it fetched, which was already let through.
  const PebbleProcessMd *current = app_manager_get_current_app_md();
  if (current && uuid_equal(&current->uuid, &app_fetch_ui_get_app_info()->uuid)) {
    return false;
  }

  // Any other launch replaces one still waiting for its PIN.
  if (s_prompt) {
    Window *prompt = s_prompt;
    s_generation++;
    prv_drop_pending();
    window_stack_remove(prompt, false /* animated */);
  }

  if (!drew_pin_lock_launch_needs_pin(id)) {
    return false;
  }

  AppInstallEntry entry;
  if (!app_install_get_entry_for_install_id(id, &entry)) {
    return false;
  }
  PBL_LOG_INFO("Launch of a locked app, asking for the PIN");
  s_pending_data = data;
  s_pending_id = id;
  s_generation++;
  s_prompt =
      pin_prompt_create(PinPromptMode_Verify, entry.name, prv_prompt_done, (void *)s_generation);
  pin_prompt_push(s_prompt);
  return true;
}

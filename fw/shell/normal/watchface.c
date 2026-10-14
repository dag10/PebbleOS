/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "watchface.h"

#include <pbl/logging/logging.h>
#include <pbl/services/compositor/compositor_transitions.h>
#include <pbl/services/music.h>
#include <pbl/services/notifications/do_not_disturb.h>

#include <applib/app_launch_reason.h>
#include <applib/app_timer.h>
#include <applib/ui/click_internal.h>
#include <apps/system/launcher/launcher.h>
#include <apps/system/settings/quick_launch_setup_menu.h>
#include <apps/system/timeline/timeline.h>
#include <apps/system_app_ids.h>
#include <kernel/event_loop.h>
#include <kernel/low_power.h>
#include <popups/timeline/peek.h>
#include <process_management/app_manager.h>
#include <process_management/pebble_process_md.h>
#include <process_management/process_manager.h>
#include <syscall/syscall_internal.h>
#include <system/passert.h>

#define QUICK_LAUNCH_HOLD_MS (400)
//! How long after a Back+Up/Down/Select tap is released a second one may start and count as a
//! double tap. Matches the click recognizer's default multi-click timeout.
#define COMBO_DOUBLE_TAP_WINDOW_MS (300)
#define BIT_SET                    (1)
#define BIT_CLEAR                  (0)
// Button events are remapped at the driver level when the display is rotated
// (left-hand mode), so these masks are correct in either orientation.
#define COMBO_BACK_UP_BUTTONS ((BIT_SET << BUTTON_ID_BACK) | (BIT_SET << BUTTON_ID_UP))
#define COMBO_UP_DOWN_BUTTONS ((BIT_SET << BUTTON_ID_UP) | (BIT_SET << BUTTON_ID_DOWN))

static ClickManager s_click_manager;
static uint8_t s_buttons_pressed = BIT_CLEAR;
static AppTimer *s_combo_back_hold_timer = nullptr;
static uint8_t s_active_combo_buttons = BIT_CLEAR;
//! Buttons whose hold was sent to the watchface as a button down, owing it a button up.
static uint8_t s_watchface_held_buttons = BIT_CLEAR;

typedef enum {
  ComboTapStateIdle,
  //! Back and s_combo_tap_button are down for a first tap whose double tap is assigned.
  ComboTapStatePressed,
  //! The first tap was released; a second one within the window makes it a double tap.
  ComboTapStateWaiting,
} ComboTapState;

static ComboTapState s_combo_tap_state = ComboTapStateIdle;
//! BUTTON_ID_UP, BUTTON_ID_DOWN or BUTTON_ID_SELECT, the button tapped together with Back.
static ButtonId s_combo_tap_button;
static AppTimer *s_combo_tap_timer = NULL;
//! Set once the current Back press has been part of a Back combo; its hold then does nothing.
static bool s_back_press_had_combo = false;

static bool prv_launch_quick_launch_app(AppInstallId app_id, ButtonId button,
                                        AppLaunchReason timeline_reason,
                                        AppQuickLaunchAction action);

static bool prv_should_ignore_button_click(void) {
  if (app_manager_get_task_context()->closing_state != ProcessRunState_Running) {
    // Ignore if the app is not running (such as if it is in the process of closing)
    return true;
  }
  if (low_power_is_active()) {
    // If we're in low power mode we dont allow any interaction
    return true;
  }
  return false;
}

static void prv_launch_app_via_button(AppLaunchEventConfig *config, ClickRecognizerRef recognizer) {
  config->common.button = click_recognizer_get_button_id(recognizer);
  app_manager_put_launch_app_event(config);
}

static bool prv_is_combo_pressed(uint8_t combo_buttons) {
  return (s_buttons_pressed & combo_buttons) == combo_buttons;
}

static bool prv_combo_is_enabled(uint8_t combo_buttons) {
  if (combo_buttons == COMBO_BACK_UP_BUTTONS) {
    return quick_launch_combo_back_up_is_enabled();
  } else if (combo_buttons == COMBO_UP_DOWN_BUTTONS) {
    return quick_launch_combo_up_down_is_enabled();
  }
  return false;
}

static AppInstallId prv_combo_get_app(uint8_t combo_buttons) {
  if (combo_buttons == COMBO_BACK_UP_BUTTONS) {
    return quick_launch_combo_back_up_get_app();
  } else if (combo_buttons == COMBO_UP_DOWN_BUTTONS) {
    return quick_launch_combo_up_down_get_app();
  }
  return INSTALL_ID_INVALID;
}

static bool prv_is_any_combo_active(void) {
  return (s_combo_back_hold_timer != nullptr) || prv_is_combo_pressed(COMBO_BACK_UP_BUTTONS) ||
         prv_is_combo_pressed(COMBO_UP_DOWN_BUTTONS);
}

static void prv_combo_back_timer_callback(void *data) {
  s_combo_back_hold_timer = nullptr;
  if (!prv_is_combo_pressed(s_active_combo_buttons)) {
    s_active_combo_buttons = BIT_CLEAR;
    return;
  }

  if (!prv_combo_is_enabled(s_active_combo_buttons)) {
    s_active_combo_buttons = BIT_CLEAR;
    return;
  }

  AppInstallId app_id = prv_combo_get_app(s_active_combo_buttons);
  const ButtonId source_button =
      (s_active_combo_buttons == COMBO_BACK_UP_BUTTONS) ? BUTTON_ID_BACK : BUTTON_ID_UP;
  s_active_combo_buttons = BIT_CLEAR;
  prv_launch_quick_launch_app(app_id, source_button, APP_LAUNCH_QUICK_LAUNCH,
                              APP_QUICK_LAUNCH_ACTION_COMBO);
}

static void prv_check_combo_back_hold(void) {
  uint8_t combo_buttons = BIT_CLEAR;

  if (prv_is_combo_pressed(COMBO_BACK_UP_BUTTONS) && !s_back_press_had_combo) {
    combo_buttons = COMBO_BACK_UP_BUTTONS;
  } else if (prv_is_combo_pressed(COMBO_UP_DOWN_BUTTONS)) {
    combo_buttons = COMBO_UP_DOWN_BUTTONS;
  }

  if (combo_buttons != BIT_CLEAR) {
    if (s_combo_back_hold_timer == nullptr) {
      s_active_combo_buttons = combo_buttons;
      // Cancel individual button timers to prevent them from firing.
      // This ensures only the combo executes, not individual hold handlers.
      if (combo_buttons == COMBO_BACK_UP_BUTTONS) {
        click_recognizer_reset(&s_click_manager.recognizers[BUTTON_ID_BACK]);
        click_recognizer_reset(&s_click_manager.recognizers[BUTTON_ID_UP]);
      } else {
        click_recognizer_reset(&s_click_manager.recognizers[BUTTON_ID_UP]);
        click_recognizer_reset(&s_click_manager.recognizers[BUTTON_ID_DOWN]);
      }
      s_combo_back_hold_timer =
          app_timer_register(QUICK_LAUNCH_HOLD_MS, prv_combo_back_timer_callback, nullptr);
    }
  } else {
    if (s_combo_back_hold_timer != nullptr) {
      app_timer_cancel(s_combo_back_hold_timer);
      s_combo_back_hold_timer = nullptr;
      s_active_combo_buttons = BIT_CLEAR;
    }
  }
}

static void prv_launch_timeline_app(AppInstallId app_id, ButtonId button, AppLaunchReason reason,
                                    AppQuickLaunchAction action) {
  static TimelineArgs s_timeline_args;
  s_timeline_args.launch_into_pin = true;
  s_timeline_args.stay_in_list_view = true;
  timeline_peek_get_item_id(&s_timeline_args.pin_id);

  const CompositorTransition *animation = nullptr;
  // A combo gesture carries no up/down intent, so its representative button
  // must not pick the timeline direction.
  const bool is_up = (action != APP_QUICK_LAUNCH_ACTION_COMBO) && (button == BUTTON_ID_UP);
  const bool is_future = (app_id == APP_ID_TIMELINE) || (app_id == APP_ID_TIMELINE_FULL && !is_up);

  if (app_id == APP_ID_TIMELINE) {
    s_timeline_args.direction = TimelineIterDirectionFuture;
  } else if (app_id == APP_ID_TIMELINE_PAST) {
    s_timeline_args.direction = TimelineIterDirectionPast;
  } else {
    s_timeline_args.direction = is_future ? TimelineIterDirectionFuture : TimelineIterDirectionPast;
  }

  const bool timeline_is_destination = true;
#if PBL_ROUND
  animation = compositor_dot_transition_timeline_get(is_future, timeline_is_destination);
#else
  const bool jump = (!uuid_is_invalid(&s_timeline_args.pin_id) && !timeline_peek_is_first_event());
  animation = jump ? compositor_peek_transition_timeline_get()
                   : compositor_slide_transition_timeline_get(is_future, timeline_is_destination,
                                                              timeline_peek_is_future_empty());
#endif
  app_manager_put_launch_app_event(&(AppLaunchEventConfig){
    .id = app_id,
    .common.reason = reason,
    .common.button = button,
    .common.args = &s_timeline_args,
    .common.transition = animation,
  });
}

//! The music actions send their command from here, so the watchface stays on screen.
static bool prv_send_music_command(AppInstallId app_id) {
  MusicCommand command;
  switch (app_id) {
    case APP_ID_QUICK_LAUNCH_MUSIC_PLAY_PAUSE:
      command = MusicCommandTogglePlayPause;
      break;
    case APP_ID_QUICK_LAUNCH_MUSIC_NEXT_TRACK:
      command = MusicCommandNextTrack;
      break;
    case APP_ID_QUICK_LAUNCH_MUSIC_PREVIOUS_TRACK:
      command = MusicCommandPreviousTrack;
      break;
    default:
      return false;
  }
  music_command_send(command);
  return true;
}

//! Returns true if an app was launched, which takes the rest of the press.
static bool prv_launch_quick_launch_app(AppInstallId app_id, ButtonId button,
                                        AppLaunchReason timeline_reason,
                                        AppQuickLaunchAction action) {
  // "Send to watchface" is handled by the tap and hold handlers; for a combo it is inert.
  if ((app_id == INSTALL_ID_INVALID) || (app_id == APP_ID_QUICK_LAUNCH_NOTHING) ||
      (app_id == APP_ID_QUICK_LAUNCH_WATCHFACE)) {
    return false;
  }

  if (prv_send_music_command(app_id)) {
    return false;
  }

  // The rest of this press goes to the launched app, so forget the buttons still held.
  s_buttons_pressed = BIT_CLEAR;

  const bool is_timeline = (app_id == APP_ID_TIMELINE) || (app_id == APP_ID_TIMELINE_PAST) ||
                           (app_id == APP_ID_TIMELINE_FULL);
  if (is_timeline) {
    prv_launch_timeline_app(app_id, button, timeline_reason, action);
  } else {
    app_manager_put_launch_app_event(&(AppLaunchEventConfig){
      .id = app_id,
      .common.reason = APP_LAUNCH_QUICK_LAUNCH,
      .common.button = button,
      .common.args = (void *)(uintptr_t)action,
    });
  }
  return true;
}

//! Replay a button event on the watchface's own event queue, where the watchface's click config
//! (window_single_click_subscribe() and friends) handles it as in any app. The real button
//! events stay masked from the app, which is what keeps the Quick Launch gestures working.
static bool prv_send_button_event_to_watchface(PebbleEventType type, ButtonId button,
                                               uint16_t held_ms) {
  PebbleEvent e = {
    .type = type,
    .button = {
      .button_id = button,
      .is_replayed = true,
      .held_ms = held_ms,
    },
  };
  return process_manager_send_event_to_process(PebbleTask_App, &e);
}

//! A tap the shell recognized reaches the watchface as a short press.
static void prv_send_tap_to_watchface(ButtonId button) {
  if (prv_send_button_event_to_watchface(PEBBLE_BUTTON_DOWN_EVENT, button, 0)) {
    prv_send_button_event_to_watchface(PEBBLE_BUTTON_UP_EVENT, button, 0);
  }
}

//! A hold the shell recognized reaches the watchface as a press already held for the hold
//! delay, so its long click fires that much sooner. The button up follows on release.
static void prv_send_hold_to_watchface(ButtonId button) {
  if (prv_send_button_event_to_watchface(PEBBLE_BUTTON_DOWN_EVENT, button, QUICK_LAUNCH_HOLD_MS)) {
    s_watchface_held_buttons |= (BIT_SET << button);
  }
}

static void prv_release_watchface_hold(ButtonId button) {
  if (s_watchface_held_buttons & (BIT_SET << button)) {
    s_watchface_held_buttons &= ~(BIT_SET << button);
    prv_send_button_event_to_watchface(PEBBLE_BUTTON_UP_EVENT, button, 0);
  }
}

static void prv_quick_launch_handler(ClickRecognizerRef recognizer, void *data) {
  ButtonId button = click_recognizer_get_button_id(recognizer);

  if (prv_is_any_combo_active()) {
    return;
  }
  if ((button == BUTTON_ID_BACK) && s_back_press_had_combo) {
    return;
  }

  AppInstallId app_id =
      quick_launch_is_enabled(button) ? quick_launch_get_app(button) : INSTALL_ID_INVALID;
  if (app_id == APP_ID_QUICK_LAUNCH_WATCHFACE) {
    prv_send_hold_to_watchface(button);
    return;
  }
  if (app_id == INSTALL_ID_INVALID) {
    app_id = app_install_get_id_for_uuid(&quick_launch_setup_get_app_info()->uuid);
  }

  prv_launch_quick_launch_app(app_id, button, APP_LAUNCH_QUICK_LAUNCH,
                              APP_QUICK_LAUNCH_ACTION_HOLD);
}

static void prv_launch_up_down(ClickRecognizerRef recognizer, void *data) {
  ButtonId button = click_recognizer_get_button_id(recognizer);

  if (prv_is_any_combo_active()) {
    return;
  }

  if (!quick_launch_single_click_is_enabled(button))
    return;
  const AppInstallId app_id = quick_launch_single_click_get_app(button);

  if (app_id == APP_ID_QUICK_LAUNCH_WATCHFACE) {
    prv_send_tap_to_watchface(button);
    return;
  }

  prv_launch_quick_launch_app(app_id, button, APP_LAUNCH_SYSTEM, APP_QUICK_LAUNCH_ACTION_TAP);
}

//! DrewPebbleOS extension: the buttons (bit 1 << ButtonId) whose single press on the
//! watchface goes to the watchface instead of Quick Launch. Exposed to apps through
//! DrewOSInfo.watchface_buttons.
DEFINE_SYSCALL(uint32_t, sys_watchface_get_tap_buttons, void) {
  // Compares the stored UUIDs rather than resolving install ids, which can mean an app
  // database lookup, too much for the syscall stack.
  uint32_t buttons = 0;
  if (quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_UP)) {
    buttons |= (1 << BUTTON_ID_UP);
  }
  if (quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_DOWN)) {
    buttons |= (1 << BUTTON_ID_DOWN);
  }
  if (quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_BACK)) {
    buttons |= (1 << BUTTON_ID_BACK);
  }
  return buttons;
}

//! DrewPebbleOS extension: the buttons (bit 1 << ButtonId) whose hold on the watchface goes to
//! the watchface instead of Quick Launch. Exposed to apps through
//! DrewOSInfo.watchface_hold_buttons.
DEFINE_SYSCALL(uint32_t, sys_watchface_get_hold_buttons, void) {
  uint32_t buttons = 0;
  for (ButtonId button = 0; button < NUM_BUTTONS; button++) {
    if (quick_launch_is_sent_to_watchface(button)) {
      buttons |= (1 << button);
    }
  }
  return buttons;
}

static void prv_configure_click_handler(ButtonId button_id, ClickHandler single_click_handler) {
  ClickConfig *cfg = &s_click_manager.recognizers[button_id].config;
  cfg->long_click.delay_ms = QUICK_LAUNCH_HOLD_MS;
  cfg->long_click.handler = prv_quick_launch_handler;
  cfg->click.handler = single_click_handler;
}

static void prv_launch_launcher_app(ClickRecognizerRef recognizer, void *data) {
  static const LauncherMenuArgs s_launcher_args = {.reset_scroll = true};
  prv_launch_app_via_button(
      &(AppLaunchEventConfig){
        .id = APP_ID_LAUNCHER_MENU,
        .common.args = &s_launcher_args,
      },
      recognizer);
}

static void prv_back_click_handler(ClickRecognizerRef recognizer, void *data) {
  if (prv_is_any_combo_active() || s_back_press_had_combo) {
    return;
  }
  // A timeline peek on screen still takes the tap, as it covers the watchface.
  TimelineItemId peek_item_id;
  timeline_peek_get_item_id(&peek_item_id);
  if (uuid_is_invalid(&peek_item_id) &&
      quick_launch_single_click_is_sent_to_watchface(BUTTON_ID_BACK)) {
    prv_send_tap_to_watchface(BUTTON_ID_BACK);
    return;
  }
  timeline_peek_dismiss();
}

static void prv_cancel_combo_back_hold(void) {
  if (s_combo_back_hold_timer != NULL) {
    app_timer_cancel(s_combo_back_hold_timer);
    s_combo_back_hold_timer = NULL;
    s_active_combo_buttons = BIT_CLEAR;
  }
}

static void prv_combo_tap_reset(void) {
  if (s_combo_tap_timer != NULL) {
    app_timer_cancel(s_combo_tap_timer);
    s_combo_tap_timer = NULL;
  }
  s_combo_tap_state = ComboTapStateIdle;
}

//! Returns true if an app was launched.
static bool prv_fire_combo_tap(ButtonId button, bool double_tap) {
  prv_combo_tap_reset();
  if (!quick_launch_combo_tap_is_enabled(button, double_tap)) {
    return false;
  }
  return prv_launch_quick_launch_app(quick_launch_combo_tap_get_app(button, double_tap),
                                     BUTTON_ID_BACK, APP_LAUNCH_QUICK_LAUNCH,
                                     APP_QUICK_LAUNCH_ACTION_COMBO);
}

//! No second tap came within the window, so the first was a single tap.
static void prv_combo_tap_timer_callback(void *data) {
  s_combo_tap_timer = NULL;
  const ButtonId button = s_combo_tap_button;
  s_combo_tap_state = ComboTapStateIdle;
  if (prv_should_ignore_button_click() || !app_manager_is_watchface_running()) {
    return;
  }
  prv_fire_combo_tap(button, false /* double_tap */);
}

static void prv_claim_combo_tap_buttons(ButtonId button) {
  s_back_press_had_combo = true;
  click_recognizer_reset(&s_click_manager.recognizers[BUTTON_ID_BACK]);
  click_recognizer_reset(&s_click_manager.recognizers[button]);
  prv_cancel_combo_back_hold();
}

//! Tap Back+Up/Down/Select and their double taps. A tap starts when the second of the two buttons
//! goes down, in either order. Without a double tap assigned, the tap fires right away; otherwise
//! it waits for a second tap until COMBO_DOUBLE_TAP_WINDOW_MS after the first one's release, and
//! the second tap fires as soon as it goes down. Returns true if @p pressed was taken by a
//! combo, which cancels the buttons' own tap and hold.
static bool prv_check_combo_tap(ButtonId pressed) {
  if (s_combo_tap_state == ComboTapStatePressed) {
    if ((pressed == BUTTON_ID_BACK) || (pressed == s_combo_tap_button)) {
      // Its release went elsewhere, e.g. to a modal; start over.
      prv_combo_tap_reset();
    } else {
      // Nothing else counts until the combo is released.
      click_recognizer_reset(&s_click_manager.recognizers[pressed]);
      return true;
    }
  }

  const bool back_held = (s_buttons_pressed & (BIT_SET << BUTTON_ID_BACK));
  const uint8_t others_held = s_buttons_pressed & ~(BIT_SET << BUTTON_ID_BACK);

  // Back plus exactly one of Up, Down and Select.
  bool is_combo = true;
  ButtonId other = pressed;
  if ((pressed != BUTTON_ID_BACK) && back_held) {
    other = pressed;
  } else if ((pressed == BUTTON_ID_BACK) && (others_held != 0) &&
             ((others_held & (others_held - 1)) == 0)) {
    other = (ButtonId)__builtin_ctz(others_held);
  } else {
    is_combo = false;
  }

  if (s_combo_tap_state == ComboTapStateWaiting) {
    if (is_combo && (other == s_combo_tap_button)) {
      prv_claim_combo_tap_buttons(other);
      prv_fire_combo_tap(other, true /* double_tap */);
      return true;
    }
    if (!is_combo && (pressed == BUTTON_ID_BACK || pressed == s_combo_tap_button)) {
      // Possibly the first half of the second tap; it does nothing on its own.
      click_recognizer_reset(&s_click_manager.recognizers[pressed]);
      if (pressed == BUTTON_ID_BACK) {
        s_back_press_had_combo = true;
      }
      return true;
    }
    // Any other press ends the wait, firing the single tap first.
    if (prv_fire_combo_tap(s_combo_tap_button, false /* double_tap */)) {
      click_recognizer_reset(&s_click_manager.recognizers[pressed]);
      return true;
    }
  }

  if (!is_combo) {
    return false;
  }
  const bool tap_enabled = quick_launch_combo_tap_is_enabled(other, false /* double_tap */);
  const bool double_tap_enabled = quick_launch_combo_tap_is_enabled(other, true /* double_tap */);
  if (!tap_enabled && !double_tap_enabled) {
    return false;
  }

  prv_claim_combo_tap_buttons(other);
  if (!double_tap_enabled) {
    prv_fire_combo_tap(other, false /* double_tap */);
    return true;
  }
  s_combo_tap_state = ComboTapStatePressed;
  s_combo_tap_button = other;
  return true;
}

//! Releasing either button of a pressed combo starts the double tap window.
static void prv_combo_tap_handle_button_up(ButtonId released) {
  if ((s_combo_tap_state != ComboTapStatePressed) ||
      ((released != BUTTON_ID_BACK) && (released != s_combo_tap_button))) {
    return;
  }
  s_combo_tap_state = ComboTapStateWaiting;
  s_combo_tap_timer =
      app_timer_register(COMBO_DOUBLE_TAP_WINDOW_MS, prv_combo_tap_timer_callback, NULL);
}

static void prv_watchface_configure_click_handlers(void) {
  prv_configure_click_handler(BUTTON_ID_UP, prv_launch_up_down);
  prv_configure_click_handler(BUTTON_ID_DOWN, prv_launch_up_down);
  prv_configure_click_handler(BUTTON_ID_SELECT, prv_launch_launcher_app);
  prv_configure_click_handler(BUTTON_ID_BACK, prv_back_click_handler);
}

void watchface_init(void) {
  click_manager_init(&s_click_manager);
  prv_watchface_configure_click_handlers();
}

void watchface_handle_button_event(PebbleEvent *e) {
  if (prv_should_ignore_button_click()) {
    return;
  }
  switch (e->type) {
    case PEBBLE_BUTTON_DOWN_EVENT:
      if (e->button.button_id == BUTTON_ID_BACK) {
        // A fresh Back press, even if the last one's release went elsewhere.
        s_back_press_had_combo = false;
      }
      s_buttons_pressed |= (BIT_SET << e->button.button_id);
      click_recognizer_handle_button_down(&s_click_manager.recognizers[e->button.button_id]);
      if (!prv_check_combo_tap(e->button.button_id)) {
        prv_check_combo_back_hold();
      }
      break;
    case PEBBLE_BUTTON_UP_EVENT:
      s_buttons_pressed &= ~(BIT_SET << e->button.button_id);
      prv_combo_tap_handle_button_up(e->button.button_id);
      if (s_combo_tap_state != ComboTapStatePressed) {
        prv_check_combo_back_hold();
      }
      click_recognizer_handle_button_up(&s_click_manager.recognizers[e->button.button_id]);
      prv_release_watchface_hold(e->button.button_id);
      if (e->button.button_id == BUTTON_ID_BACK) {
        s_back_press_had_combo = false;
      }
      break;
    default:
      PBL_CROAK("Invalid event type: %u", e->type);
      break;
  }
}

static void prv_watchface_launch_low_power(void) {
  PBL_LOG_DBG("Switching default watchface to low_power_mode watchface");
  app_manager_put_launch_app_event(&(AppLaunchEventConfig){
    .id = APP_ID_LOW_POWER_FACE,
  });
}

void watchface_launch_default(const CompositorTransition *animation) {
  app_manager_put_launch_app_event(&(AppLaunchEventConfig){
    .id = watchface_get_default_install_id(),
    .common.transition = animation,
  });
}

static void kernel_callback_watchface_launch(void *data) {
  watchface_launch_default(nullptr);
}

void command_watch(void) {
  launcher_task_add_callback(kernel_callback_watchface_launch, nullptr);
}

void watchface_start_low_power(void) {
  app_manager_set_minimum_run_level(ProcessAppRunLevelNormal);
  prv_watchface_launch_low_power();
}

void watchface_reset_click_manager(void) {
  click_manager_reset(&s_click_manager);
  prv_combo_tap_reset();
  // Whatever takes the buttons now gets their releases, so finish the watchface's holds.
  for (ButtonId button = 0; button < NUM_BUTTONS; button++) {
    prv_release_watchface_hold(button);
  }
}

/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pin_prompt.h"

#ifdef CONFIG_SERVICE_DREW_PIN_LOCK

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <pbl/services/drew_pin_lock.h>
#include <pbl/services/evented_timer.h>
#include <pbl/util/math.h>

#include <applib/fonts/fonts.h>
#include <applib/graphics/graphics.h>
#include <applib/graphics/graphics_circle.h>
#include <applib/graphics/text.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/vibes.h>
#include <applib/ui/window_stack.h>
#include <kernel/pbl_malloc.h>
#include <kernel/pebble_tasks.h>
#include <kernel/ui/modals/modal_manager.h>
#include <shell/prefs.h>
#ifdef CONFIG_TOUCH
#include <applib/touch_service.h>
#endif

#define NUM_KEYS       (12)
#define KEY_DELETE     (9)
#define KEY_ZERO       (10)
#define KEY_OK         (11)
#define KEY_NONE       (-1)
#define KEY_INSET      (3)
#define KEY_RADIUS     (6)
#define DOT_RADIUS     (5)
#define DOT_SPACING    (18)
#define TITLE_LENGTH   (32)
#define STATUS_LENGTH  (40)
#define REPEAT_MS      (150)
#define HEADER_PERCENT (30)

typedef struct PinPrompt {
  Window window;
  Layer layer;
  PinPromptMode mode;
  PinPromptCallback callback;
  void *context;
  char title[TITLE_LENGTH];
  char status[STATUS_LENGTH];
  char digits[DREW_PIN_LOCK_MAX_DIGITS + 1];
  //! The first entry of a new PIN, while it's being confirmed.
  char first[DREW_PIN_LOCK_MAX_DIGITS + 1];
  uint8_t num_digits;
  //! Digits of the stored PIN, which submits itself once complete (PinPromptMode_Verify).
  uint8_t length;
  int8_t pressed_key;
  int8_t focus_key;
  bool button_mode;
  bool done;
  EventedTimerID lockout_timer;
} PinPrompt;

static const char *const s_key_labels[NUM_KEYS] = {
  "1", "2", "3", "4", "5", "6", "7", "8", "9", "Del", "0", "OK",
};

static bool prv_on_kernel(void) {
  return pebble_task_get_current() == PebbleTask_KernelMain;
}

static void prv_wipe(void *buffer, size_t length) {
  volatile uint8_t *p = buffer;
  while (length--) {
    *p++ = 0;
  }
}

// Layout
//////////

static int16_t prv_header_height(const GRect *bounds) {
  return bounds->size.h * HEADER_PERCENT / 100;
}

static GRect prv_key_rect(const GRect *bounds, int key) {
  const int16_t top = prv_header_height(bounds);
  const int16_t row_h = (bounds->size.h - top) / 4;
  const int16_t col_w = bounds->size.w / 3;
  return GRect(bounds->origin.x + (key % 3) * col_w, bounds->origin.y + top + (key / 3) * row_h,
               col_w, row_h);
}

static int prv_key_at(PinPrompt *prompt, int16_t x, int16_t y) {
  const GRect bounds = prompt->layer.bounds;
  const GPoint point = GPoint(x, y);
  for (int key = 0; key < NUM_KEYS; key++) {
    const GRect rect = prv_key_rect(&bounds, key);
    if (grect_contains_point(&rect, &point)) {
      return key;
    }
  }
  return KEY_NONE;
}

// State
/////////

static uint32_t prv_lockout_remaining(PinPrompt *prompt) {
  return (prompt->mode == PinPromptMode_Verify) ? drew_pin_lock_get_lockout_remaining() : 0;
}

static void prv_set_idle_status(PinPrompt *prompt) {
  const char *text;
  if (prompt->mode == PinPromptMode_Verify) {
    text = "Enter PIN";
  } else if (prompt->first[0]) {
    text = "Confirm the PIN";
  } else {
    text = "Choose 4 to 6 digits";
  }
  strncpy(prompt->status, text, sizeof(prompt->status) - 1);
}

static void prv_update_lockout_status(PinPrompt *prompt, uint32_t remaining) {
  snprintf(prompt->status, sizeof(prompt->status), "Try again in %" PRIu32 " s", remaining);
}

static void prv_lockout_timer_cb(void *context) {
  PinPrompt *prompt = context;
  const uint32_t remaining = prv_lockout_remaining(prompt);
  if (remaining == 0) {
    evented_timer_cancel(prompt->lockout_timer);
    prompt->lockout_timer = EVENTED_TIMER_INVALID_ID;
    prv_set_idle_status(prompt);
  } else {
    prv_update_lockout_status(prompt, remaining);
  }
  layer_mark_dirty(&prompt->layer);
}

//! @return true if entry is locked out, after showing how long for.
static bool prv_check_lockout(PinPrompt *prompt) {
  const uint32_t remaining = prv_lockout_remaining(prompt);
  if (remaining == 0) {
    return false;
  }
  prv_update_lockout_status(prompt, remaining);
  if (prompt->lockout_timer == EVENTED_TIMER_INVALID_ID) {
    prompt->lockout_timer =
        evented_timer_register(1000, true /* repeating */, prv_lockout_timer_cb, prompt);
  }
  return true;
}

static void prv_clear_digits(PinPrompt *prompt) {
  prv_wipe(prompt->digits, sizeof(prompt->digits));
  prompt->num_digits = 0;
}

static void prv_finish(PinPrompt *prompt, bool success) {
  if (prompt->done) {
    return;
  }
  prompt->done = true;
  const PinPromptCallback callback = prompt->callback;
  void *context = prompt->context;
  char pin[DREW_PIN_LOCK_MAX_DIGITS + 1] = {};
  const bool give_pin = success && (prompt->mode == PinPromptMode_Create);
  if (give_pin) {
    memcpy(pin, prompt->digits, sizeof(pin));
  }
  prv_clear_digits(prompt);
  prv_wipe(prompt->first, sizeof(prompt->first));
  // The unload handler may free the prompt.
  window_stack_remove(&prompt->window, !success /* animated */);
  if (callback) {
    callback(success, give_pin ? pin : nullptr, context);
  }
  prv_wipe(pin, sizeof(pin));
}

//! @return true if the prompt finished, and may be freed already.
static bool prv_submit(PinPrompt *prompt) {
  if (prompt->mode == PinPromptMode_Verify) {
    const DrewPinResult result = drew_pin_lock_check(prompt->digits);
    prv_clear_digits(prompt);
    switch (result) {
      case DrewPinResult_Ok:
      case DrewPinResult_NoPin:
        prv_finish(prompt, true);
        return true;
      case DrewPinResult_Wrong:
      case DrewPinResult_LockedOut:
        vibes_short_pulse();
        if (!prv_check_lockout(prompt)) {
          const uint8_t left = drew_pin_lock_get_attempts_left();
          snprintf(prompt->status, sizeof(prompt->status), "Wrong PIN, %u %s left", left,
                   (left == 1) ? "try" : "tries");
        }
        return false;
    }
    return false;
  }

  if (!prompt->first[0]) {
    memcpy(prompt->first, prompt->digits, sizeof(prompt->first));
    prv_clear_digits(prompt);
    prv_set_idle_status(prompt);
  } else if (strcmp(prompt->first, prompt->digits) == 0) {
    prv_finish(prompt, true);
    return true;
  } else {
    vibes_short_pulse();
    prv_wipe(prompt->first, sizeof(prompt->first));
    prv_clear_digits(prompt);
    strncpy(prompt->status, "PINs didn't match", sizeof(prompt->status) - 1);
  }
  return false;
}

//! @return the number of digits that submits the entry by itself, 0 if only OK does.
static uint8_t prv_auto_submit_length(PinPrompt *prompt) {
  if (prompt->mode == PinPromptMode_Verify) {
    return prompt->length;
  }
  return prompt->first[0] ? (uint8_t)strlen(prompt->first) : DREW_PIN_LOCK_MAX_DIGITS;
}

static bool prv_key_enabled(PinPrompt *prompt, int key) {
  if (prompt->lockout_timer != EVENTED_TIMER_INVALID_ID) {
    return false;
  }
  if (key == KEY_OK) {
    return prompt->num_digits >= DREW_PIN_LOCK_MIN_DIGITS;
  }
  if (key == KEY_DELETE) {
    return prompt->num_digits > 0;
  }
  return prompt->num_digits < DREW_PIN_LOCK_MAX_DIGITS;
}

static void prv_press_key(PinPrompt *prompt, int key) {
  if (prompt->done || !prv_key_enabled(prompt, key) || prv_check_lockout(prompt)) {
    layer_mark_dirty(&prompt->layer);
    return;
  }
  if (key == KEY_DELETE) {
    prompt->digits[--prompt->num_digits] = '\0';
  } else if (key == KEY_OK) {
    if (prv_submit(prompt)) {
      return;
    }
  } else {
    const char digit = (key == KEY_ZERO) ? '0' : (char)('1' + key);
    prompt->digits[prompt->num_digits++] = digit;
    if ((prompt->num_digits == prv_auto_submit_length(prompt)) && prv_submit(prompt)) {
      return;
    }
  }
  layer_mark_dirty(&prompt->layer);
}

// Drawing
///////////

static void prv_draw_key(PinPrompt *prompt, GContext *ctx, const GRect *bounds, int key,
                         GColor highlight) {
  const GRect rect = grect_inset(prv_key_rect(bounds, key), GEdgeInsets(KEY_INSET));
  const bool lit =
      (key == prompt->pressed_key) || (prompt->button_mode && key == prompt->focus_key);
  const bool enabled = prv_key_enabled(prompt, key);
  const GColor fill = lit ? highlight : PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);
  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_round_rect(ctx, &rect, KEY_RADIUS, GCornersAll);
#if !PBL_COLOR
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_draw_round_rect(ctx, &rect, KEY_RADIUS);
#endif
  const bool small = (key == KEY_DELETE) || (key == KEY_OK);
  GFont font = fonts_get_system_font(small ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_24_BOLD);
  const int16_t font_h = fonts_get_font_height(font);
  GRect text_rect = rect;
  text_rect.origin.y += (rect.size.h - font_h) / 2 - fonts_get_font_cap_offset(font) / 2;
  text_rect.size.h = font_h + 4;
  GColor text_color = lit ? gcolor_legible_over(highlight) : GColorBlack;
  if (!enabled && !lit) {
    text_color = PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
  }
  graphics_context_set_text_color(ctx, text_color);
  graphics_draw_text(ctx, s_key_labels[key], font, text_rect, GTextOverflowModeFill,
                     GTextAlignmentCenter, nullptr);
}

static void prv_update_proc(Layer *layer, GContext *ctx) {
  PinPrompt *prompt = window_get_user_data(layer_get_window(layer));
  const GRect bounds = layer->bounds;
  const int16_t header_h = prv_header_height(&bounds);

  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_rect(ctx, &bounds);

  graphics_context_set_text_color(ctx, GColorBlack);
  GFont title_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
  graphics_draw_text(ctx, prompt->title, title_font, GRect(4, 0, bounds.size.w - 8, 22),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, nullptr);

  // One slot per digit of the stored PIN, else the entered digits and the next one.
  uint8_t slots = prv_auto_submit_length(prompt);
  if ((prompt->mode == PinPromptMode_Create) && !prompt->first[0]) {
    slots = MIN(prompt->num_digits + 1, DREW_PIN_LOCK_MAX_DIGITS);
    slots = MAX(slots, DREW_PIN_LOCK_MIN_DIGITS);
  }
  const int16_t dots_y = header_h * 45 / 100;
  const int16_t dots_x = bounds.size.w / 2 - ((slots - 1) * DOT_SPACING) / 2;
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  for (int i = 0; i < slots; i++) {
    const GPoint center = GPoint(dots_x + i * DOT_SPACING, dots_y);
    if (i < prompt->num_digits) {
      graphics_fill_circle(ctx, center, DOT_RADIUS);
    } else {
      graphics_draw_circle(ctx, center, DOT_RADIUS);
    }
  }

  GFont status_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  graphics_draw_text(ctx, prompt->status, status_font,
                     GRect(2, dots_y + DOT_RADIUS + 2, bounds.size.w - 4, 18),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, nullptr);

  const GColor highlight = PBL_IF_COLOR_ELSE(shell_prefs_get_theme_highlight_color(), GColorBlack);
  for (int key = 0; key < NUM_KEYS; key++) {
    prv_draw_key(prompt, ctx, &bounds, key, highlight);
  }
}

// Input
/////////

static bool prv_has_focus(PinPrompt *prompt) {
  if (prv_on_kernel()) {
    return modal_manager_is_window_focused(&prompt->window);
  }
  return app_window_stack_get_top_window() == &prompt->window;
}

#ifdef CONFIG_TOUCH
static void prv_touch_handler(const TouchEvent *event, void *context) {
  PinPrompt *prompt = context;
  if (prompt->done || !prv_has_focus(prompt)) {
    prompt->pressed_key = KEY_NONE;
    return;
  }
  const int8_t old_key = prompt->pressed_key;
  switch (event->type) {
    case TouchEvent_Touchdown:
      // Contact the system treats as unintended (say, in a pocket) mustn't type or lock out.
      prompt->pressed_key =
          event->non_navigational ? KEY_NONE : prv_key_at(prompt, event->x, event->y);
      if (prompt->pressed_key != KEY_NONE) {
        prompt->button_mode = false;
      }
      break;
    case TouchEvent_PositionUpdate:
      if ((prompt->pressed_key != KEY_NONE) &&
          (prv_key_at(prompt, event->x, event->y) != prompt->pressed_key)) {
        prompt->pressed_key = KEY_NONE;
      }
      break;
    case TouchEvent_Liftoff:
      prompt->pressed_key = KEY_NONE;
      if (old_key != KEY_NONE) {
        prv_press_key(prompt, old_key);
        return;
      }
      break;
  }
  if (old_key != prompt->pressed_key) {
    layer_mark_dirty(&prompt->layer);
  }
}
#endif

static void prv_move_focus(PinPrompt *prompt, int delta) {
  if (prompt->button_mode) {
    prompt->focus_key = (prompt->focus_key + delta + NUM_KEYS) % NUM_KEYS;
  }
  prompt->button_mode = true;
  layer_mark_dirty(&prompt->layer);
}

static void prv_up_click(ClickRecognizerRef recognizer, void *context) {
  prv_move_focus(context, -1);
}

static void prv_down_click(ClickRecognizerRef recognizer, void *context) {
  prv_move_focus(context, 1);
}

static void prv_select_click(ClickRecognizerRef recognizer, void *context) {
  PinPrompt *prompt = context;
  if (!prompt->button_mode) {
    prompt->button_mode = true;
    layer_mark_dirty(&prompt->layer);
    return;
  }
  prv_press_key(prompt, prompt->focus_key);
}

static void prv_back_click(ClickRecognizerRef recognizer, void *context) {
  PinPrompt *prompt = context;
  if (prompt->num_digits > 0) {
    prompt->digits[--prompt->num_digits] = '\0';
    layer_mark_dirty(&prompt->layer);
  } else {
    prv_finish(prompt, false);
  }
}

static void prv_click_config(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, REPEAT_MS, prv_up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, REPEAT_MS, prv_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click);
}

// Window
//////////

static void prv_window_load(Window *window) {
  PinPrompt *prompt = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  layer_init(&prompt->layer, &root->bounds);
  layer_set_update_proc(&prompt->layer, prv_update_proc);
  layer_add_child(root, &prompt->layer);
#ifdef CONFIG_TOUCH
  touch_service_subscribe(prv_touch_handler, prompt);
#endif
}

static void prv_window_appear(Window *window) {
  PinPrompt *prompt = window_get_user_data(window);
  prv_check_lockout(prompt);
}

static void prv_window_unload(Window *window) {
  PinPrompt *prompt = window_get_user_data(window);
#ifdef CONFIG_TOUCH
  touch_service_unsubscribe();
#endif
  if (prompt->lockout_timer != EVENTED_TIMER_INVALID_ID) {
    evented_timer_cancel(prompt->lockout_timer);
  }
  layer_deinit(&prompt->layer);
  // Closed from outside, e.g. by a higher priority modal popping everything.
  const bool notify = !prompt->done && prompt->callback;
  const PinPromptCallback callback = prompt->callback;
  void *context = prompt->context;
  prv_wipe(prompt, sizeof(*prompt));
  task_free(prompt);
  if (notify) {
    callback(false, nullptr, context);
  }
}

Window *pin_prompt_create(PinPromptMode mode, const char *title, PinPromptCallback callback,
                          void *context) {
  PinPrompt *prompt = task_zalloc_check(sizeof(*prompt));
  prompt->mode = mode;
  prompt->callback = callback;
  prompt->context = context;
  prompt->pressed_key = KEY_NONE;
  prompt->lockout_timer = EVENTED_TIMER_INVALID_ID;
  if (mode == PinPromptMode_Verify) {
    prompt->length = drew_pin_lock_get_length();
  }
  strncpy(prompt->title, title ?: "Enter PIN", sizeof(prompt->title) - 1);
  prv_set_idle_status(prompt);
#ifndef CONFIG_TOUCH
  prompt->button_mode = true;
#endif

  Window *window = &prompt->window;
  window_init(window, WINDOW_NAME("PIN Prompt"));
  window_set_user_data(window, prompt);
  window_set_background_color(window, GColorWhite);
  window_set_window_handlers(window, &(WindowHandlers){
                                       .load = prv_window_load,
                                       .appear = prv_window_appear,
                                       .unload = prv_window_unload,
                                     });
  window_set_click_config_provider_with_context(window, prv_click_config, prompt);
#ifdef CONFIG_TOUCH
  // Taps are keys here, never emulated button presses.
  window_set_touch_bridge_disabled(window, true);
  if (!touch_service_is_enabled()) {
    prompt->button_mode = true;
  }
#endif
  return window;
}

void pin_prompt_push(Window *window) {
  if (prv_on_kernel()) {
    modal_window_push(window, ModalPriorityAlert, true /* animated */);
  } else {
    app_window_stack_push(window, true /* animated */);
  }
}

void pin_prompt(const char *reason, PinPromptCallback callback, void *context) {
  pin_prompt_push(pin_prompt_create(PinPromptMode_Verify, reason, callback, context));
}

#endif // CONFIG_SERVICE_DREW_PIN_LOCK

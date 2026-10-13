/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include "clar.h"

#include "applib/ui/click.h"
#include "applib/ui/click_internal.h"

#include "fake_app_timer.h"

#include "stubs_logging.h"
#include "stubs_passert.h"

// Fixture
////////////////////////////////////////////////////////////////

static ClickManager s_click_manager;
static ClickRecognizer *s_recognizer;
static int s_single_clicks;
static int s_long_clicks;
static int s_long_releases;

static void prv_single_click(ClickRecognizerRef recognizer, void *context) {
  s_single_clicks++;
}

static void prv_long_click(ClickRecognizerRef recognizer, void *context) {
  s_long_clicks++;
}

static void prv_long_release(ClickRecognizerRef recognizer, void *context) {
  s_long_releases++;
}

void test_click__initialize(void) {
  fake_app_timer_init();
  click_manager_init(&s_click_manager);
  s_recognizer = &s_click_manager.recognizers[BUTTON_ID_UP];
  s_single_clicks = 0;
  s_long_clicks = 0;
  s_long_releases = 0;
}

void test_click__cleanup(void) {
  click_manager_clear(&s_click_manager);
  fake_app_timer_deinit();
}

// Tests
////////////////////////////////////////////////////////////////

void test_click__held_down_shortens_long_click_delay(void) {
  s_recognizer->config.long_click.handler = prv_long_click;
  s_recognizer->config.long_click.release_handler = prv_long_release;
  s_recognizer->config.long_click.delay_ms = 700;

  click_recognizer_handle_button_down_held(s_recognizer, 400);
  cl_assert_equal_i(fake_app_timer_get_timeout(s_recognizer->hold_timer), 300);

  cl_assert(app_timer_trigger(s_recognizer->hold_timer));
  cl_assert_equal_i(s_long_clicks, 1);
  click_recognizer_handle_button_up(s_recognizer);
  cl_assert_equal_i(s_long_releases, 1);
  cl_assert_equal_i(s_single_clicks, 0);
}

void test_click__held_past_long_click_delay_fires_at_once(void) {
  // The default long click delay is the same 400 ms the shell's hold takes.
  s_recognizer->config.long_click.handler = prv_long_click;

  click_recognizer_handle_button_down_held(s_recognizer, 400);
  cl_assert_equal_i(fake_app_timer_get_timeout(s_recognizer->hold_timer), 0);
}

void test_click__held_down_starts_repeating_at_once(void) {
  s_recognizer->config.click.handler = prv_single_click;
  s_recognizer->config.click.repeat_interval_ms = 100;

  click_recognizer_handle_button_down_held(s_recognizer, 400);
  // Hold-to-repeat fires its first click on the down, then waits out the repetition delay.
  cl_assert_equal_i(s_single_clicks, 1);
  cl_assert_equal_i(fake_app_timer_get_timeout(s_recognizer->hold_timer), 0);

  cl_assert(app_timer_trigger(s_recognizer->hold_timer));
  cl_assert_equal_i(s_single_clicks, 2);
  cl_assert(click_recognizer_is_repeating(s_recognizer));
  click_recognizer_handle_button_up(s_recognizer);
  cl_assert_equal_i(s_single_clicks, 2);
}

void test_click__held_down_without_long_click_is_a_single_click(void) {
  s_recognizer->config.click.handler = prv_single_click;

  click_recognizer_handle_button_down_held(s_recognizer, 400);
  cl_assert_equal_i(s_single_clicks, 1);
  click_recognizer_handle_button_up(s_recognizer);
  cl_assert_equal_i(s_single_clicks, 1);
}

void test_click__not_held_keeps_full_delay(void) {
  s_recognizer->config.long_click.handler = prv_long_click;
  s_recognizer->config.long_click.delay_ms = 700;

  click_recognizer_handle_button_down(s_recognizer);
  cl_assert_equal_i(fake_app_timer_get_timeout(s_recognizer->hold_timer), 700);

  // Released before the delay: a single click, no long click.
  s_recognizer->config.click.handler = prv_single_click;
  click_recognizer_handle_button_up(s_recognizer);
  cl_assert_equal_i(s_long_clicks, 0);
  cl_assert_equal_i(s_single_clicks, 1);
}

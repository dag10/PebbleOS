/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include "kernel/events.h"
#include "pbl/services/blob_db/pin_db.h"
#include "pbl/services/timeline/drew_calendar.h"
#include "pbl/services/timeline/event.h"
#include "pbl/services/timeline/timeline.h"
#include <pbl/logging/logging.h>

#include "clar.h"

#include <string.h>

// Stubs
////////////////////////////////////////////////////////////////
#include "stubs_analytics.h"
#include "stubs_ancs.h"
#include "stubs_ancs_notifications.h"
#include "stubs_app_cache.h"
#include "stubs_app_install_manager.h"
#include "stubs_app_manager.h"
#include "stubs_blob_db.h"
#include "stubs_blob_db_sync.h"
#include "stubs_blob_db_sync_util.h"
#include "stubs_event_loop.h"
#include "stubs_event_service_client.h"
#include "stubs_hexdump.h"
#include "stubs_i18n.h"
#include "stubs_layout_layer.h"
#include "stubs_logging.h"
#include "stubs_modal_manager.h"
#include "stubs_mutex.h"
#include "stubs_notification_storage.h"
#include "stubs_notifications.h"
#include "stubs_passert.h"
#include "stubs_phone_call_util.h"
#include "stubs_rand_ptr.h"
#include "stubs_regular_timer.h"
#include "stubs_reminder_db.h"
#include "stubs_session.h"
#include "stubs_sleep.h"
#include "stubs_syscall_internal.h"
#include "stubs_system_task.h"
#include "stubs_task_wdt.h"
#include "stubs_text_layer_flow.h"
#include "stubs_timeline.h"
#include "stubs_timeline_pin_window.h"
#include "stubs_window_stack.h"

// Fakes
////////////////////////////////////////////////////////////////
#include "fake_new_timer.h"
#include "fake_pbl_malloc.h"
#include "fake_pebble_tasks.h"
#include "fake_rtc.h"
#include "fake_spi_flash.h"
#include "fake_settings_file.h"
#include "fake_events.h"

bool calendar_layout_verify(bool existing_attributes[]) {
  return true;
}

bool weather_layout_verify(bool existing_attributes[]) {
  return true;
}

const TimelineEventImpl *calendar_get_event_service(void) {
  return NULL;
}

const TimelineEventImpl *timeline_peek_get_event_service(void) {
  return NULL;
}

// Fake pins
////////////////////////////////////////////////////////////////

static Attribute s_attrs[2];

// A pin with `id`, starting at minute `start_m` for `duration_m` minutes, titled `title`,
// at `location` (NULL for none).
static TimelineItem prv_pin(uint8_t id, int start_m, int duration_m, const char *title,
                            const char *location) {
  s_attrs[0] = (Attribute){.id = AttributeIdTitle, .cstring = (char *)title};
  s_attrs[1] = (Attribute){.id = AttributeIdLocationName, .cstring = (char *)location};
  return (TimelineItem){
    .header =
        {
          .id = {id},
          .timestamp = start_m * 60,
          .duration = duration_m,
          .type = TimelineItemTypePin,
          .all_day = false,
          .layout = LayoutIdCalendar,
        },
    .attr_list =
        {
          .num_attributes = location ? 2 : 1,
          .attributes = s_attrs,
        },
  };
}

static void prv_add(TimelineItem item) {
  cl_assert(timeline_add(&item));
  timeline_event_handle_blobdb_event();
}

static DrewCalendarEventsInfo s_out;

static bool prv_get(void) {
  memset(&s_out, 0xaa, sizeof(s_out));
  return sys_timeline_get_calendar_events(&s_out);
}

static void prv_assert_none(const DrewCalendarEvent *event) {
  const DrewCalendarEvent zero = {};
  cl_assert(memcmp(event, &zero, sizeof(zero)) == 0);
}

static void prv_assert_event(const DrewCalendarEvent *event, int start_m, int duration_m,
                             const char *title, const char *location) {
  cl_assert_equal_i(event->start_utc, start_m * 60);
  cl_assert_equal_i(event->duration_m, duration_m);
  cl_assert_equal_s(event->title, title);
  cl_assert_equal_s(event->location, location);
}

// Seconds until the event service's timer fires, or -1 if none is scheduled.
static int prv_timer_s(void) {
  const TimerID timer_id = stub_new_timer_get_next();
  if (timer_id == TIMER_INVALID_ID || !stub_new_timer_is_scheduled(timer_id)) {
    return -1;
  }
  return stub_new_timer_timeout(timer_id) / 1000;
}

// Setup
////////////////////////////////////////////////////////////////

void test_drew_calendar__initialize(void) {
  rtc_set_time(0);
  fake_event_init();
  pin_db_init();
}

void test_drew_calendar__cleanup(void) {
  timeline_event_deinit();
  stub_new_timer_cleanup();
  fake_settings_file_reset();
}

// Tests
////////////////////////////////////////////////////////////////

void test_drew_calendar__no_events(void) {
  timeline_event_init();
  cl_assert(!prv_get());
  prv_assert_none(&s_out.current);
  prv_assert_none(&s_out.next);
  cl_assert_equal_i(prv_timer_s(), -1);
}

void test_drew_calendar__future_event(void) {
  prv_add(prv_pin(1, 10, 30, "Standup", "Room 4"));
  timeline_event_init();
  cl_assert(prv_get());
  prv_assert_none(&s_out.current);
  prv_assert_event(&s_out.next, 10, 30, "Standup", "Room 4");
  cl_assert_equal_i(prv_timer_s(), 10 * 60);
}

void test_drew_calendar__ongoing_event(void) {
  prv_add(prv_pin(1, 10, 30, "Standup", NULL));
  rtc_set_time(15 * 60);
  timeline_event_init();
  cl_assert(prv_get());
  prv_assert_event(&s_out.current, 10, 30, "Standup", "");
  prv_assert_none(&s_out.next);
  cl_assert_equal_i(prv_timer_s(), 25 * 60); // until it ends
}

void test_drew_calendar__past_event(void) {
  prv_add(prv_pin(1, 10, 30, "Standup", NULL));
  rtc_set_time(40 * 60);
  timeline_event_init();
  cl_assert(!prv_get());
  cl_assert_equal_i(prv_timer_s(), -1);
}

void test_drew_calendar__latest_started_is_current(void) {
  prv_add(prv_pin(1, 10, 120, "Focus block", NULL));
  prv_add(prv_pin(2, 20, 30, "Meeting", "Room 1"));
  prv_add(prv_pin(3, 60, 15, "Lunch", NULL));
  prv_add(prv_pin(4, 45, 15, "Call", NULL));
  rtc_set_time(25 * 60);
  timeline_event_init();
  cl_assert(prv_get());
  prv_assert_event(&s_out.current, 20, 30, "Meeting", "Room 1");
  prv_assert_event(&s_out.next, 45, 15, "Call", "");
  cl_assert_equal_i(prv_timer_s(), 20 * 60); // the next one starts before Meeting ends
}

void test_drew_calendar__ignores_other_pins(void) {
  TimelineItem weather = prv_pin(1, 10, 30, "Sunny", NULL);
  weather.header.layout = LayoutIdWeather;
  prv_add(weather);
  TimelineItem all_day = prv_pin(2, 10, 30, "Holiday", NULL);
  all_day.header.all_day = true;
  prv_add(all_day);
  prv_add(prv_pin(3, 10, 24 * 60, "Conference", NULL)); // 24 h counts as all-day
  prv_add(prv_pin(4, 20, 10, "Dismissed", NULL));
  cl_assert_equal_i(pin_db_set_status_bits(&(TimelineItemId){4}, TimelineItemStatusDismissed),
                    S_SUCCESS);
  timeline_event_init();
  cl_assert(!prv_get());
  cl_assert_equal_i(prv_timer_s(), -1);
}

void test_drew_calendar__utf8_truncation(void) {
  // 62 ASCII bytes, then "é" (2 bytes) would end at byte 64: no room for the NUL.
  char title[80];
  memset(title, 'a', 62);
  strcpy(title + 62, "\xc3\xa9xyz");
  prv_add(prv_pin(1, 10, 30, title, "Caf\xc3\xa9 de la Paix, Place de l'Op\xc3\xa9ra"));
  timeline_event_init();
  cl_assert(prv_get());
  cl_assert_equal_i(strlen(s_out.next.title), 62);
  cl_assert(strncmp(s_out.next.title, title, 62) == 0);
  // 31 bytes fit; the cut falls before "é" at bytes 31-32.
  cl_assert_equal_s(s_out.next.location, "Caf\xc3\xa9 de la Paix, Place de l'Op");
}

void test_drew_calendar__updates_when_pins_change_and_time_passes(void) {
  timeline_event_init();
  cl_assert(!prv_get());

  prv_add(prv_pin(1, 10, 10, "First", NULL));
  prv_add(prv_pin(2, 30, 10, "Second", NULL));
  cl_assert(prv_get());
  prv_assert_none(&s_out.current);
  prv_assert_event(&s_out.next, 10, 10, "First", "");
  cl_assert_equal_i(prv_timer_s(), 10 * 60);

  // First starts: it becomes current, Second is next.
  rtc_set_time(10 * 60);
  cl_assert(stub_new_timer_fire(stub_new_timer_get_next()));
  cl_assert(prv_get());
  prv_assert_event(&s_out.current, 10, 10, "First", "");
  prv_assert_event(&s_out.next, 30, 10, "Second", "");
  cl_assert_equal_i(prv_timer_s(), 10 * 60);

  // First ends.
  rtc_set_time(20 * 60);
  cl_assert(stub_new_timer_fire(stub_new_timer_get_next()));
  cl_assert(prv_get());
  prv_assert_none(&s_out.current);
  prv_assert_event(&s_out.next, 30, 10, "Second", "");

  // Second is deleted (timeline_remove() goes through the stubbed BlobDB).
  const TimelineItemId second = {2};
  cl_assert_equal_i(pin_db_delete((const uint8_t *)&second, sizeof(second)), S_SUCCESS);
  timeline_event_handle_blobdb_event();
  cl_assert(!prv_get());
  cl_assert_equal_i(prv_timer_s(), -1);
}

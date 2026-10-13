/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/timeline/drew_calendar.h"

#include <pbl/drivers/rtc.h>
#include "kernel/pbl_malloc.h"
#include "pbl/kernel/mutex.h"
#include "pbl/services/blob_db/pin_db.h"
#include <pbl/logging/logging.h>
#include "pbl/util/units.h"
#include "syscall/syscall_internal.h"

#include <string.h>

PBL_LOG_MODULE_DECLARE(service_timeline, CONFIG_SERVICE_TIMELINE_LOG_LEVEL);

// The picks of one scan. The event service keeps one item per client and prefers earlier ones,
// so "the ongoing event that started last" is tracked here instead.
typedef struct DrewCalendarPicks {
  bool has_current;
  bool has_next;
  TimelineItemId current_id;
  TimelineItemId next_id;
  time_t current_start;
  time_t next_start;
} DrewCalendarPicks;

static PBL_MUTEX_DEFINE(s_mutex);
static DrewCalendarEventsInfo s_events;

// Copies `src` into `dst` (`size` bytes), cut on a UTF-8 character boundary.
static void prv_copy_utf8(char *dst, const char *src, size_t size) {
  size_t len = strlen(src);
  if (len >= size) {
    len = size - 1;
    while (len > 0 && ((uint8_t)src[len] & 0xc0) == 0x80) {
      len--;
    }
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static void prv_will_update(void **context) {
  *context = task_zalloc_check(sizeof(DrewCalendarPicks));
}

static void prv_did_update(void **context) {
  task_free(*context);
  *context = NULL;
}

static bool prv_filter(SerializedTimelineItemHeader *header, void **context) {
  DrewCalendarPicks *picks = *context;
  const CommonTimelineItemHeader *common = &header->common;
  if ((common->layout != LayoutIdCalendar) || (common->type != TimelineItemTypePin) ||
      common->dismissed || timeline_event_is_all_day(&header->common)) {
    return false;
  }
  const time_t now = rtc_get_time();
  const time_t start = common->timestamp;
  const time_t end = start + (common->duration * PBL_SEC_PER_MIN);
  if (now >= end) {
    return false;
  }
  if (start <= now) {
    if (!picks->has_current || (start > picks->current_start)) {
      picks->has_current = true;
      picks->current_id = common->id;
      picks->current_start = start;
    }
  } else if (!picks->has_next || (start < picks->next_start)) {
    picks->has_next = true;
    picks->next_id = common->id;
    picks->next_start = start;
  }
  // The picks live in the context, the event service's own item isn't needed.
  return false;
}

// Reads pin `id` into `out`; false if it is gone.
static bool prv_read_event(const TimelineItemId *id, DrewCalendarEvent *out) {
  TimelineItem item;
  if (pin_db_get(id, &item) != S_SUCCESS) {
    return false;
  }
  out->start_utc = item.header.timestamp;
  out->duration_m = item.header.duration;
  prv_copy_utf8(out->title, attribute_get_string(&item.attr_list, AttributeIdTitle, ""),
                sizeof(out->title));
  prv_copy_utf8(out->location,
                attribute_get_string(&item.attr_list, AttributeIdLocationName, ""),
                sizeof(out->location));
  timeline_item_free_allocated_buffer(&item);
  return true;
}

static uint32_t prv_update(TimelineItem *unused, void **context) {
  const DrewCalendarPicks *picks = *context;
  DrewCalendarEventsInfo events = {};
  if (picks->has_current) {
    prv_read_event(&picks->current_id, &events.current);
  }
  if (picks->has_next) {
    prv_read_event(&picks->next_id, &events.next);
  }

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  s_events = events;
  pbl_mutex_unlock(&s_mutex);
  PBL_LOG_DBG("Calendar events: current %" PRIu32 ", next %" PRIu32, events.current.start_utc,
              events.next.start_utc);

  // Pick again when the current event ends or the next one starts.
  const time_t now = rtc_get_time();
  uint32_t timeout_s = 0;
  if (picks->has_current) {
    const time_t end = picks->current_start + (events.current.duration_m * PBL_SEC_PER_MIN);
    if (end > now) {
      timeout_s = end - now;
    }
  }
  if (picks->has_next && (picks->next_start > now)) {
    const uint32_t next_s = picks->next_start - now;
    timeout_s = timeout_s ? MIN(timeout_s, next_s) : next_s;
  }
  return MIN(timeout_s, UINT32_MAX / PBL_MSEC_PER_SEC) * PBL_MSEC_PER_SEC;
}

const TimelineEventImpl *drew_calendar_get_event_service(void) {
  static const TimelineEventImpl s_event_impl = {
    .will_update = prv_will_update,
    .filter = prv_filter,
    .update = prv_update,
    .did_update = prv_did_update,
  };
  return &s_event_impl;
}

DEFINE_SYSCALL(bool, sys_timeline_get_calendar_events, DrewCalendarEventsInfo *out) {
  if (PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(out, sizeof(*out));
  }
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  *out = s_events;
  pbl_mutex_unlock(&s_mutex);
  return (out->current.start_utc != 0) || (out->next.start_utc != 0);
}

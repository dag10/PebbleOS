/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "event.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup services_timeline_drew_calendar DrewPebbleOS calendar events
 * @{
 *
 * The current and next calendar pins for apps (DrewPebbleOS api 8). A timeline event service
 * client: it picks them while the event service scans the pins and keeps their titles and
 * locations, so the syscall only copies.
 */

#define DREW_CALENDAR_TITLE_LENGTH 64
#define DREW_CALENDAR_LOCATION_LENGTH 32

/** @brief One calendar pin, as copied by sys_timeline_get_calendar_events(). */
typedef struct DrewCalendarEvent {
  /** Start, UTC seconds; 0 if there is no such event. */
  uint32_t start_utc;
  /** Duration in minutes. */
  uint16_t duration_m;
  uint8_t flags;
  uint8_t reserved;
  /** Title, NUL terminated (cut on a UTF-8 character boundary). */
  char title[DREW_CALENDAR_TITLE_LENGTH];
  /** Location name, NUL terminated, empty if none. */
  char location[DREW_CALENDAR_LOCATION_LENGTH];
} DrewCalendarEvent;

/** @brief The events sys_timeline_get_calendar_events() copies. */
typedef struct DrewCalendarEventsInfo {
  /** The ongoing event that started last. */
  DrewCalendarEvent current;
  /** The event that starts next. */
  DrewCalendarEvent next;
} DrewCalendarEventsInfo;

/**
 * Only timed calendar pins count (LayoutIdCalendar, not all-day or 24 h and longer, not
 * dismissed).
 */
const TimelineEventImpl *drew_calendar_get_event_service(void);

/**
 * @brief DrewPebbleOS extension: copy the current and next calendar events for an app.
 *
 * A syscall, so unprivileged apps (e.g. a watchface) may call it; @p out must be in app memory.
 *
 * @param[out] out The events; a missing one is zeroed.
 * @return True if either event is set.
 */
bool sys_timeline_get_calendar_events(DrewCalendarEventsInfo *out);

/** @} */

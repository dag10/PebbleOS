/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/kernel/compiler.h>
#include <pbl/services/notifications/drew_notification.h>

uint8_t PBL_WEAK drew_notification_get_flags(const TimelineItem *item) {
  return 0;
}

uint8_t PBL_WEAK drew_notification_get_stored_flags(const Uuid *id) {
  return 0;
}

const char *PBL_WEAK drew_notification_get_placeholder_title(const void *owner) {
  return "Notification";
}

bool PBL_WEAK drew_notification_hide_contents(TimelineItem *item) {
  return true;
}

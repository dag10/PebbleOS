/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <pbl/services/timeline/item.h>

/**
 * @defgroup services_drew_notification Drew notification flags
 * @ingroup services_notifications
 * @brief DrewPebbleOS: Critical, Discreet and Quiet Time breaking notifications.
 *
 * DrewMobileApp sets these in the AttributeIdDrewNotificationFlags attribute of a notification,
 * only for a watch that announces drew_notification_flags_support.
 *
 * - Critical: the notification popup repeats the alarm vibe until a button is pressed, for at
 *   most 10 minutes.
 * - Discreet: the popup, the Notifications app and its launcher glance show a placeholder
 *   until Select is pressed on the notification.
 * - Break Quiet Time: Quiet Time doesn't hide, silence or dim the notification.
 * @{
 */

typedef enum DrewNotificationFlag {
  DrewNotificationFlag_Critical = 1 << 0,
  DrewNotificationFlag_Discreet = 1 << 1,
  DrewNotificationFlag_BreakQuietTime = 1 << 2,
} DrewNotificationFlag;

/**
 * @brief Get the DrewNotificationFlag bits of an item.
 *
 * @param item Item to read, may be NULL.
 * @return The flags, 0 if the item has none.
 */
uint8_t drew_notification_get_flags(const TimelineItem *item);

/**
 * @brief Read the flags of a stored notification.
 *
 * @param id Notification id.
 * @return The flags, 0 if the notification can't be read or has none.
 */
uint8_t drew_notification_get_stored_flags(const Uuid *id);

/**
 * @brief Get the title a discreet notification shows instead of its contents.
 *
 * @param owner Owner of the returned i18n string, freed with i18n_free().
 */
const char *drew_notification_get_placeholder_title(const void *owner);

/**
 * @brief Replace an item's contents with the discreet placeholder.
 *
 * The title, subtitle, body, sender, app name, icons, colors and image of @p item are replaced by
 * a generic icon, a placeholder title and "Press Select to view". The header, the actions and the
 * other attributes are kept. The item's buffer is reallocated on the calling task's heap.
 *
 * @param item Item whose buffer was allocated by the timeline item functions.
 * @return false if out of memory; @p item is then unchanged.
 */
bool drew_notification_hide_contents(TimelineItem *item);

/** @} */

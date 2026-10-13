/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/util/uuid.h"

#include <stdint.h>

/**
 * @defgroup services_process_management_app_usage_storage App usage storage
 * @ingroup services_process_management
 * @brief Persisted time each app was last used, for the launcher's "Recently used" order.
 *
 * Stored in the settings file @c app_usage, keyed by app UUID, with the UTC time of the last use
 * as the value. An app is used when it launches or when an activity it owns starts in the
 * background (music starting to play, a workout starting).
 * @{
 */

/** @brief Time one app was last used. */
typedef struct AppUsageEntry {
  Uuid uuid;
  /** UTC seconds. */
  uint32_t last_used;
} AppUsageEntry;

/** @brief Every stored last-used time. */
typedef struct AppUsageList {
  uint16_t count;
  AppUsageEntry entries[];
} AppUsageList;

/**
 * @brief Record that an app was used now.
 *
 * Can be called from any task. The write happens later on KernelBG, and is skipped when the app
 * is already the most recently used one, since that cannot change the order.
 *
 * @param uuid UUID of the app.
 */
void app_usage_storage_record(const Uuid *uuid);

/**
 * @brief Read every stored last-used time.
 *
 * Must be called from the app task.
 *
 * @return List allocated on the app heap, to be freed by the caller with app_free(), or NULL if
 * nothing is stored or allocation failed.
 */
AppUsageList *app_usage_storage_read(void);

/**
 * @brief Look up an app's last-used time in a list.
 *
 * @param list List from app_usage_storage_read(), or NULL.
 * @param uuid UUID of the app.
 * @return UTC seconds of the last use, or 0 if the app was never used.
 */
uint32_t app_usage_list_get_last_used(const AppUsageList *list, const Uuid *uuid);

#if UNITTEST
/**
 * @brief Forget the most recently recorded app, as after a reboot.
 *
 * Only built for unit tests.
 */
void app_usage_storage_reset_for_tests(void);
#endif

/** @} */

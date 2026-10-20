/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "pbl/util/uuid.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * @defgroup services_process_management_drew_app_recommend DrewPebbleOS app recommendations
 * @ingroup services_process_management
 * @brief Apps the default watchface recommends, which the launcher lists first (DrewPebbleOS
 * api 11).
 *
 * Kept in RAM only. The list belongs to the watchface that set it: it outlives that watchface's
 * process (the launcher opens over the closed face) and is dropped when another watchface becomes
 * the default.
 * @{
 */

/** Most apps a watchface can recommend at once. */
#define DREW_APP_RECOMMEND_MAX 4

/**
 * @brief DrewPebbleOS extension: the default watchface sets the apps it recommends.
 *
 * A syscall, so the (unprivileged) watchface may call it; @p uuids must be in app memory. The
 * list replaces the previous one; more than DREW_APP_RECOMMEND_MAX are cut to the first
 * DREW_APP_RECOMMEND_MAX.
 *
 * @param uuids The apps, in the order the launcher lists them. May be NULL if @p count is 0.
 * @param count How many; 0 clears the list.
 * @return False, changing nothing, unless the caller is the current default watchface.
 */
bool sys_app_recommend_set(const Uuid *uuids, uint8_t count);

/**
 * @brief Copy the default watchface's recommended apps. Can be called from any task.
 *
 * @param[out] uuids_out Room for DREW_APP_RECOMMEND_MAX UUIDs.
 * @return How many were copied, 0 without any.
 */
uint8_t drew_app_recommend_get(Uuid uuids_out[DREW_APP_RECOMMEND_MAX]);

/**
 * @brief Drop the recommendations unless they belong to the watchface with @p uuid.
 *
 * Called whenever the default watchface pref is set, from the watch or the phone.
 *
 * @param uuid UUID of the new default watchface.
 */
void drew_app_recommend_handle_default_watchface(const Uuid *uuid);

#if UNITTEST
/** @brief Forget the recommendations, as after a reboot. Only built for unit tests. */
void drew_app_recommend_reset_for_tests(void);
#endif

/** @} */

/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/util/uuid.h>

/**
 * @defgroup services_drew_pin_lock Drew PIN lock
 * @ingroup services
 * @brief DrewPebbleOS: a watch PIN, and the apps that need it to launch.
 *
 * The PIN is 4 to 6 digits, stored only as a salted, stretched SHA-256 in the "drewpin" settings
 * file, with the UUIDs of the locked apps. After DREW_PIN_LOCK_FREE_ATTEMPTS wrong tries in a row,
 * each wrong try locks entry out, for 30 s doubling up to 16 min; the count survives a reboot.
 * Removing the PIN (or a factory reset) clears the file, and with it every app lock.
 * @{
 */

#define DREW_PIN_LOCK_MIN_DIGITS      (4)
#define DREW_PIN_LOCK_MAX_DIGITS      (6)
#define DREW_PIN_LOCK_FREE_ATTEMPTS   (5)
#define DREW_PIN_LOCK_LOCKOUT_BASE_S  (30)
#define DREW_PIN_LOCK_LOCKOUT_MAX_S   (16 * 60)
#define DREW_PIN_LOCK_HASH_ITERATIONS (1000)

typedef enum DrewPinResult {
  DrewPinResult_Ok,
  DrewPinResult_Wrong,
  DrewPinResult_LockedOut,
  DrewPinResult_NoPin,
} DrewPinResult;

//! @return true if a PIN is set.
bool drew_pin_lock_has_pin(void);

//! @return the number of digits of the PIN, 0 if none is set.
uint8_t drew_pin_lock_get_length(void);

//! @return true if @p pin is a NUL-terminated string of 4 to 6 ASCII digits.
bool drew_pin_lock_is_valid_pin(const char *pin);

//! Set or replace the PIN (keeping the locked apps), and clear any lockout.
//! @return false if @p pin isn't valid or it couldn't be stored.
bool drew_pin_lock_set_pin(const char *pin);

//! Remove the PIN and unlock every app.
void drew_pin_lock_remove(void);

//! Check @p pin, counting a wrong try towards the lockout.
DrewPinResult drew_pin_lock_check(const char *pin);

//! @return the seconds left of the current lockout, 0 if entry is allowed.
uint32_t drew_pin_lock_get_lockout_remaining(void);

//! @return how many more wrong tries are allowed before a lockout.
uint8_t drew_pin_lock_get_attempts_left(void);

//! @return true if @p uuid needs the PIN to launch (only while a PIN is set).
bool drew_pin_lock_is_app_locked(const Uuid *uuid);

void drew_pin_lock_set_app_locked(const Uuid *uuid, bool locked);

//! @return how many installed apps are locked.
uint16_t drew_pin_lock_get_num_locked_apps(void);

//! @return true if launching install id @p id must ask for the PIN first. Never true for a
//! watchface, Settings or the launcher.
bool drew_pin_lock_launch_needs_pin(int32_t id);

//! SHA-256 of @p length bytes at @p data, into @p digest.
void drew_pin_lock_sha256(const void *data, size_t length, uint8_t digest[32]);

//! Called on KernelMain for every PEBBLE_APP_LAUNCH_EVENT of an app that isn't running, with
//! the event's install id and data (a kernel_malloc'd PebbleLaunchAppEventExtended, or NULL).
//! @return true if it took the launch over to ask for the PIN first. It then owns @p data, which
//! the caller must clear from the event, and launches the app itself once the PIN is entered.
bool drew_pin_lock_handle_launch(int32_t id, void *data);

/** @} */

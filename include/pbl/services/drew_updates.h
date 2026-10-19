/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @defgroup services_drew_updates Drew updates
 * @ingroup services
 * @brief DrewPebbleOS: the updates DrewMobileApp knows about, for Settings > Updates.
 *
 * Talks to the phone over endpoint 0xde01 when its app announces
 * CommSessionDrewUpdatesSupport. Keeps the last list received since boot. Posts
 * PEBBLE_DREW_UPDATES_EVENT whenever the list, the check state or an install's status changes.
 * @{
 */

#define DREW_UPDATES_ENDPOINT_ID (0xde01)

#define DREW_UPDATES_MAX_ENTRIES      (8)
#define DREW_UPDATES_ID_LENGTH        (48)
#define DREW_UPDATES_NAME_LENGTH      (32)
#define DREW_UPDATES_VERSION_LENGTH   (32)
#define DREW_UPDATES_MESSAGE_LENGTH   (64)
#define DREW_UPDATES_CHANGELOG_LENGTH (512)

//! How long to wait for a fresh list after asking for a check, or after a Checking list.
#define DREW_UPDATES_CHECK_TIMEOUT_MS (60 * 1000)
//! How long an install may go without a status from the phone before it counts as failed.
#define DREW_UPDATES_INSTALL_TIMEOUT_MS (120 * 1000)

#define DREW_UPDATES_PERCENT_UNKNOWN (0xff)

typedef enum DrewUpdateKind {
  DrewUpdateKindFirmware = 0,
  DrewUpdateKindWatchface = 1,
  DrewUpdateKindWatchapp = 2,
} DrewUpdateKind;

typedef enum DrewUpdateFlag {
  DrewUpdateFlagInstalling = 1 << 0,
  DrewUpdateFlagReboots = 1 << 1,
  DrewUpdateFlagNotDrew = 1 << 2,
} DrewUpdateFlag;

typedef enum DrewUpdatesListStatus {
  DrewUpdatesListReady = 0,
  DrewUpdatesListChecking = 1,
  DrewUpdatesListCheckFailed = 2,
} DrewUpdatesListStatus;

typedef enum DrewUpdateInstallState {
  DrewUpdateInstallStarting = 0,
  DrewUpdateInstallDownloading = 1,
  DrewUpdateInstallTransferring = 2,
  DrewUpdateInstallDone = 3,
  DrewUpdateInstallFailed = 4,
  DrewUpdateInstallRebooting = 5,
} DrewUpdateInstallState;

typedef struct DrewUpdatesInfo {
  //! A list has arrived since boot.
  bool has_list;
  //! The status the phone gave the current list.
  DrewUpdatesListStatus status;
  //! Waiting for a fresh list from the phone.
  bool checking;
  //! The last wait for a fresh list ran out.
  bool timed_out;
  //! The phone's last successful check, UTC seconds; 0 if none.
  uint32_t last_checked_utc;
  uint8_t num_entries;
  char message[DREW_UPDATES_MESSAGE_LENGTH + 1];
} DrewUpdatesInfo;

typedef struct DrewUpdateEntry {
  DrewUpdateKind kind;
  uint8_t flags;
  //! 0 if unknown.
  uint32_t size_bytes;
  char id[DREW_UPDATES_ID_LENGTH + 1];
  char name[DREW_UPDATES_NAME_LENGTH + 1];
  char current_version[DREW_UPDATES_VERSION_LENGTH + 1];
  char new_version[DREW_UPDATES_VERSION_LENGTH + 1];
} DrewUpdateEntry;

typedef struct DrewUpdateInstallStatus {
  //! An install was requested since boot.
  bool valid;
  char id[DREW_UPDATES_ID_LENGTH + 1];
  DrewUpdateInstallState state;
  //! 0-100, or DREW_UPDATES_PERCENT_UNKNOWN.
  uint8_t percent;
  char message[DREW_UPDATES_MESSAGE_LENGTH + 1];
} DrewUpdateInstallStatus;

void drew_updates_init(void);

//! The connected phone app speaks the protocol.
bool drew_updates_phone_supported(void);

//! Asks the phone for its list, and with @p recheck also to check online. False if the phone
//! can't be asked.
bool drew_updates_request_list(bool recheck);

//! Asks the phone to install the entry @p id. False if the phone can't be asked.
bool drew_updates_request_install(const char *id);

void drew_updates_get_info(DrewUpdatesInfo *out);

//! False if there's no entry at @p index.
bool drew_updates_get_entry(uint8_t index, DrewUpdateEntry *out);

//! Copies the changelog of the entry at @p index, NUL terminated; returns its length.
size_t drew_updates_copy_changelog(uint8_t index, char *buffer, size_t buffer_size);

void drew_updates_get_install_status(DrewUpdateInstallStatus *out);

/** @} */

/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <pbl/util/uuid.h>

//! DrewPebbleOS (personal fork) extensions for third-party apps.
//!
//! There is no jump table entry for this: apps probe with
//! setlocale(DREW_OS_PROBE_CATEGORY, NULL). Upstream firmware returns NULL for
//! an unknown category; this fork returns a pointer to a DrewOSInfo, whose
//! leading magic is also a valid C string. The struct is ABI: append fields
//! only, bump DREW_OS_API_VERSION, never reorder.

#define DREW_OS_PROBE_CATEGORY 0x44524557 /* 'DREW' */
#define DREW_OS_MAGIC          "DrewPebbleOS"
#define DREW_OS_API_VERSION    3

//! Bits for DrewOSInfo.capabilities
typedef enum {
  //! app_next_wakeup is available (api_version >= 2)
  DREW_OS_CAP_APP_WAKEUP = (1 << 0),
  //! app_glance_subtitle is available (api_version >= 3)
  DREW_OS_CAP_APP_GLANCE = (1 << 1),
} DrewOSCapability;

typedef struct DrewOSInfo {
  //! DREW_OS_MAGIC, NUL padded
  char magic[16];
  //! DREW_OS_API_VERSION of the running firmware
  uint16_t api_version;
  //! sizeof(DrewOSInfo) of the running firmware
  uint16_t size;
  //! DrewOSCapability bits
  uint32_t capabilities;
  //! Firmware version tag, e.g. "v4.36.0-12-gabcdef0"
  const char *fw_version;

  // api_version >= 2

  //! Earliest future wakeup event scheduled by the app with @p app_uuid (any app,
  //! not only the caller), as a UTC timestamp. Returns false if there is none.
  //! Points at the sys_wakeup_get_next_for_app syscall, so unprivileged apps may
  //! call it; both pointers must be in app memory.
  bool (*app_next_wakeup)(const Uuid *app_uuid, time_t *out_timestamp);

  // api_version >= 3

  //! Copies the unrendered subtitle template string of the current slice of the
  //! glance of the app with @p app_uuid (any app) into @p buffer, NUL terminated
  //! and truncated to @p buffer_size. Returns false if it has none. Points at the
  //! sys_app_glance_get_current_subtitle syscall; both pointers must be in app
  //! memory.
  bool (*app_glance_subtitle)(const Uuid *app_uuid, char *buffer, size_t buffer_size);
} DrewOSInfo;

_Static_assert(sizeof(DrewOSInfo) == 36, "DrewOSInfo is ABI");

const DrewOSInfo *drew_os_get_info(void);

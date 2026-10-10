/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

//! DrewPebbleOS (personal fork) extensions for third-party apps.
//!
//! There is no jump table entry for this: apps probe with
//! setlocale(DREW_OS_PROBE_CATEGORY, NULL). Upstream firmware returns NULL for
//! an unknown category; this fork returns a pointer to a DrewOSInfo, whose
//! leading magic is also a valid C string. The struct is ABI: append fields
//! only, bump DREW_OS_API_VERSION, never reorder.

#define DREW_OS_PROBE_CATEGORY 0x44524557 /* 'DREW' */
#define DREW_OS_MAGIC          "DrewPebbleOS"
#define DREW_OS_API_VERSION    1

typedef struct DrewOSInfo {
  //! DREW_OS_MAGIC, NUL padded
  char magic[16];
  //! DREW_OS_API_VERSION of the running firmware
  uint16_t api_version;
  //! sizeof(DrewOSInfo) of the running firmware
  uint16_t size;
  //! DrewOSCapability bits; none are defined yet
  uint32_t capabilities;
  //! Firmware version tag, e.g. "v4.36.0-12-gabcdef0"
  const char *fw_version;
} DrewOSInfo;

_Static_assert(sizeof(DrewOSInfo) == 28, "DrewOSInfo is ABI");

const DrewOSInfo *drew_os_get_info(void);

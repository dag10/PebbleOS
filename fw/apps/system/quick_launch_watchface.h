/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "process_management/app_manager.h"

#define QUICK_LAUNCH_WATCHFACE_UUID \
  {0x5b, 0xa4, 0x62, 0xcf, 0x26, 0xa3, 0x41, 0xec, 0xbf, 0x03, 0x9c, 0xce, 0x5c, 0x72, 0x00, 0x12}

const PebbleProcessMd *quick_launch_watchface_get_app_info(void);

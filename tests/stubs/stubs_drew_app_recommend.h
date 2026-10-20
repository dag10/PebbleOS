/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/kernel/compiler.h>
#include <pbl/services/process_management/drew_app_recommend.h>

PBL_WEAK bool sys_app_recommend_set(const Uuid *uuids, uint8_t count) {
  return false;
}

PBL_WEAK uint8_t drew_app_recommend_get(Uuid uuids_out[DREW_APP_RECOMMEND_MAX]) {
  return 0;
}

PBL_WEAK void drew_app_recommend_handle_default_watchface(const Uuid *uuid) {
}

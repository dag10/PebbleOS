/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/process_management/drew_app_recommend.h"

#include "kernel/pebble_tasks.h"
#include "pbl/kernel/mutex.h"
#include "process_management/app_manager.h"
#include "shell/normal/watchface.h"
#include "syscall/syscall_internal.h"

#include <string.h>

static PBL_MUTEX_DEFINE(s_mutex);
//! Watchface that set the list
static Uuid s_owner = UUID_INVALID_INIT;
static uint8_t s_count;
static Uuid s_uuids[DREW_APP_RECOMMEND_MAX];

//! Whether the running app is the default watchface. Compares the stored UUIDs rather than
//! resolving install ids, which can mean an app database lookup, too much for the syscall stack.
static bool prv_caller_is_default_watchface(Uuid *uuid_out) {
  if (pebble_task_get_current() != PebbleTask_App) {
    return false;
  }
  const PebbleProcessMd *md = app_manager_get_current_app_md();
  if (!md || (md->process_type != ProcessTypeWatchface)) {
    return false;
  }
  Uuid default_uuid;
  watchface_get_default_uuid(&default_uuid);
  if (!uuid_equal(&md->uuid, &default_uuid)) {
    return false;
  }
  *uuid_out = md->uuid;
  return true;
}

DEFINE_SYSCALL(bool, sys_app_recommend_set, const Uuid *uuids, uint8_t count) {
  if (count > DREW_APP_RECOMMEND_MAX) {
    count = DREW_APP_RECOMMEND_MAX;
  }
  if (count && PRIVILEGE_WAS_ELEVATED) {
    syscall_assert_userspace_buffer(uuids, count * sizeof(Uuid));
  }
  if (count && !uuids) {
    return false;
  }

  Uuid owner;
  if (!prv_caller_is_default_watchface(&owner)) {
    return false;
  }

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  s_owner = owner;
  s_count = count;
  memcpy(s_uuids, uuids, count * sizeof(Uuid));
  pbl_mutex_unlock(&s_mutex);
  return true;
}

uint8_t drew_app_recommend_get(Uuid uuids_out[DREW_APP_RECOMMEND_MAX]) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const uint8_t count = s_count;
  memcpy(uuids_out, s_uuids, count * sizeof(Uuid));
  pbl_mutex_unlock(&s_mutex);
  return count;
}

void drew_app_recommend_handle_default_watchface(const Uuid *uuid) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  if (!uuid_equal(uuid, &s_owner)) {
    s_owner = UUID_INVALID;
    s_count = 0;
  }
  pbl_mutex_unlock(&s_mutex);
}

#if UNITTEST
void drew_app_recommend_reset_for_tests(void) {
  s_owner = UUID_INVALID;
  s_count = 0;
}
#endif

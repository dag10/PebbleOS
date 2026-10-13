/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/process_management/app_usage_storage.h"

#include "kernel/pbl_malloc.h"
#include "pbl/drivers/rtc.h"
#include "pbl/kernel/mutex.h"
#include "pbl/services/settings/settings_file.h"
#include "pbl/services/system_task.h"
#include "pbl/util/math.h"
#include "pbl/util/units.h"
#include <pbl/logging/logging.h>
#include "system/passert.h"

#include <inttypes.h>
#include <string.h>

PBL_LOG_MODULE_DECLARE(service_process_management, CONFIG_SERVICE_PROCESS_MANAGEMENT_LOG_LEVEL);

#define USAGE_FILE "app_usage"
// A record takes 28 bytes, so this fits about 140 apps before the oldest half is dropped.
#define USAGE_FILE_SIZE PBL_KIB(4)

typedef struct {
  Uuid uuid;
  uint32_t last_used;
} UsageRecord;

static PBL_MUTEX_DEFINE(s_mutex);
//! Most recently written app; recording it again cannot change the order.
static Uuid s_last_recorded = UUID_INVALID_INIT;

//////////////////
// Write

typedef struct {
  uint32_t min;
  uint32_t max;
} UsageRange;

static bool prv_find_range_cb(SettingsFile *file, SettingsRecordInfo *info, void *context) {
  if ((info->key_len != sizeof(Uuid)) || (info->val_len != sizeof(uint32_t))) {
    return true;
  }
  UsageRange *range = context;
  uint32_t last_used;
  info->get_val(file, &last_used, sizeof(last_used));
  range->min = MIN(range->min, last_used);
  range->max = MAX(range->max, last_used);
  return true;
}

static bool prv_keep_newer_half_cb(void *key, size_t key_len, void *value, size_t value_len,
                                   void *context) {
  if ((key_len != sizeof(Uuid)) || (value_len != sizeof(uint32_t))) {
    return false;
  }
  uint32_t last_used;
  memcpy(&last_used, value, sizeof(last_used));
  return (last_used > *(uint32_t *)context);
}

//! Drops the apps used in the older half of the stored time span.
static void prv_prune(SettingsFile *file) {
  UsageRange range = {.min = UINT32_MAX, .max = 0};
  settings_file_each(file, prv_find_range_cb, &range);
  if (range.min > range.max) {
    return;
  }
  uint32_t cutoff = range.min + (range.max - range.min) / 2;
  PBL_LOG_DBG("App usage file full, dropping uses before %" PRIu32, cutoff);
  settings_file_rewrite_filtered(file, prv_keep_newer_half_cb, &cutoff);
}

static void prv_write_cb(void *data) {
  UsageRecord *record = data;

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  if (!uuid_equal(&record->uuid, &s_last_recorded)) {
    SettingsFile file;
    if (settings_file_open(&file, USAGE_FILE, USAGE_FILE_SIZE) == S_SUCCESS) {
      status_t rv = settings_file_set(&file, &record->uuid, sizeof(Uuid), &record->last_used,
                                      sizeof(record->last_used));
      if (rv == E_OUT_OF_STORAGE) {
        prv_prune(&file);
        rv = settings_file_set(&file, &record->uuid, sizeof(Uuid), &record->last_used,
                               sizeof(record->last_used));
      }
      settings_file_close(&file);
      if (rv == S_SUCCESS) {
        s_last_recorded = record->uuid;
      } else {
        PBL_LOG_ERR("Failed to record app usage: %" PRId32, (int32_t)rv);
      }
    }
  }
  pbl_mutex_unlock(&s_mutex);

  kernel_free(record);
}

void app_usage_storage_record(const Uuid *uuid) {
  if (!uuid || uuid_is_invalid(uuid) || uuid_is_system(uuid)) {
    return;
  }
  UsageRecord *record = kernel_malloc(sizeof(UsageRecord));
  if (!record) {
    return;
  }
  *record = (UsageRecord){
    .uuid = *uuid,
    .last_used = rtc_get_time(),
  };
  if (!system_task_add_callback(prv_write_cb, record)) {
    kernel_free(record);
  }
}

//////////////////
// Read

typedef struct {
  AppUsageList *list;
  uint16_t capacity;
} ReadContext;

static bool prv_count_cb(SettingsFile *file, SettingsRecordInfo *info, void *context) {
  if ((info->key_len == sizeof(Uuid)) && (info->val_len == sizeof(uint32_t))) {
    (*(uint16_t *)context)++;
  }
  return true;
}

static bool prv_read_cb(SettingsFile *file, SettingsRecordInfo *info, void *context) {
  ReadContext *ctx = context;
  if ((info->key_len != sizeof(Uuid)) || (info->val_len != sizeof(uint32_t)) ||
      (ctx->list->count >= ctx->capacity)) {
    return true;
  }
  AppUsageEntry *entry = &ctx->list->entries[ctx->list->count++];
  info->get_key(file, &entry->uuid, sizeof(Uuid));
  info->get_val(file, &entry->last_used, sizeof(entry->last_used));
  return true;
}

AppUsageList *app_usage_storage_read(void) {
  PBL_ASSERT_TASK(PebbleTask_App);

  AppUsageList *list = NULL;
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  SettingsFile file;
  if (settings_file_open(&file, USAGE_FILE, USAGE_FILE_SIZE) == S_SUCCESS) {
    uint16_t count = 0;
    settings_file_each(&file, prv_count_cb, &count);
    if (count > 0) {
      list = app_malloc(sizeof(AppUsageList) + count * sizeof(AppUsageEntry));
      if (list) {
        list->count = 0;
        ReadContext ctx = {.list = list, .capacity = count};
        settings_file_each(&file, prv_read_cb, &ctx);
      }
    }
    settings_file_close(&file);
  }
  pbl_mutex_unlock(&s_mutex);
  return list;
}

uint32_t app_usage_list_get_last_used(const AppUsageList *list, const Uuid *uuid) {
  if (!list) {
    return 0;
  }
  for (uint16_t i = 0; i < list->count; i++) {
    if (uuid_equal(&list->entries[i].uuid, uuid)) {
      return list->entries[i].last_used;
    }
  }
  return 0;
}

#if UNITTEST
void app_usage_storage_reset_for_tests(void) {
  s_last_recorded = UUID_INVALID;
}
#endif

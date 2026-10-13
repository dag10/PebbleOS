/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/process_management/app_usage_storage.h"

#include "applib/app_malloc.h"
#include "kernel/pebble_tasks.h"
#include "pbl/kernel/malloc.h"
#include "pbl/kernel/mutex.h"
#include "pbl/kernel/time.h"
#include "pbl/services/system_task.h"
#include "pbl/settings/settings.h"
#include "pbl/util/math.h"
#include "pbl/util/units.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <pbl/logging/logging.h>

PBL_LOG_MODULE_DECLARE(service_process_management, CONFIG_SERVICE_PROCESS_MANAGEMENT_LOG_LEVEL);

#define USAGE_FILE "/app_usage"
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

static bool prv_find_range_cb(struct pbl_settings_file *file, struct pbl_settings_record *info,
                              void *context) {
  if ((info->key_len != sizeof(Uuid)) || (info->val_len != sizeof(uint32_t))) {
    return true;
  }
  UsageRange *range = context;
  uint32_t last_used;
  pbl_settings_read_val(file, &last_used, sizeof(last_used));
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
static void prv_prune(struct pbl_settings_file *file) {
  UsageRange range = {.min = UINT32_MAX, .max = 0};
  pbl_settings_each(file, prv_find_range_cb, &range);
  if (range.min > range.max) {
    return;
  }
  uint32_t cutoff = range.min + (range.max - range.min) / 2;
  PBL_LOG_DBG("App usage file full, dropping uses before %" PRIu32, cutoff);
  pbl_settings_rewrite_filtered(file, prv_keep_newer_half_cb, &cutoff);
}

static void prv_write_cb(void *data) {
  UsageRecord *record = data;

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  if (!uuid_equal(&record->uuid, &s_last_recorded)) {
    struct pbl_settings_file file;
    if (pbl_settings_open(&file, USAGE_FILE, USAGE_FILE_SIZE) == 0) {
      int rv = pbl_settings_set(&file, &record->uuid, sizeof(Uuid), &record->last_used,
                                sizeof(record->last_used));
      if (rv == -ENOSPC) {
        prv_prune(&file);
        rv = pbl_settings_set(&file, &record->uuid, sizeof(Uuid), &record->last_used,
                              sizeof(record->last_used));
      }
      pbl_settings_close(&file);
      if (rv == 0) {
        s_last_recorded = record->uuid;
      } else {
        PBL_LOG_ERR("Failed to record app usage: %d", rv);
      }
    }
  }
  pbl_mutex_unlock(&s_mutex);

  pbl_free(record);
}

void app_usage_storage_record(const Uuid *uuid) {
  if (!uuid || uuid_is_invalid(uuid) || uuid_is_system(uuid)) {
    return;
  }
  UsageRecord *record = pbl_malloc(sizeof(UsageRecord));
  if (!record) {
    return;
  }
  *record = (UsageRecord){
    .uuid = *uuid,
    .last_used = (uint32_t)pbl_realtime(),
  };
  if (!system_task_add_callback(prv_write_cb, record)) {
    pbl_free(record);
  }
}

//////////////////
// Read

typedef struct {
  AppUsageList *list;
  uint16_t capacity;
} ReadContext;

static bool prv_count_cb(struct pbl_settings_file *file, struct pbl_settings_record *info,
                         void *context) {
  if ((info->key_len == sizeof(Uuid)) && (info->val_len == sizeof(uint32_t))) {
    (*(uint16_t *)context)++;
  }
  return true;
}

static bool prv_read_cb(struct pbl_settings_file *file, struct pbl_settings_record *info,
                        void *context) {
  ReadContext *ctx = context;
  if ((info->key_len != sizeof(Uuid)) || (info->val_len != sizeof(uint32_t)) ||
      (ctx->list->count >= ctx->capacity)) {
    return true;
  }
  AppUsageEntry *entry = &ctx->list->entries[ctx->list->count++];
  pbl_settings_read_key(file, &entry->uuid, sizeof(Uuid));
  pbl_settings_read_val(file, &entry->last_used, sizeof(entry->last_used));
  return true;
}

AppUsageList *app_usage_storage_read(void) {
  PBL_ASSERT_TASK(PebbleTask_App);

  AppUsageList *list = NULL;
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  struct pbl_settings_file file;
  if (pbl_settings_open(&file, USAGE_FILE, USAGE_FILE_SIZE) == 0) {
    uint16_t count = 0;
    pbl_settings_each(&file, prv_count_cb, &count);
    if (count > 0) {
      list = app_malloc(sizeof(AppUsageList) + count * sizeof(AppUsageEntry));
      if (list) {
        list->count = 0;
        ReadContext ctx = {.list = list, .capacity = count};
        pbl_settings_each(&file, prv_read_cb, &ctx);
      }
    }
    pbl_settings_close(&file);
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

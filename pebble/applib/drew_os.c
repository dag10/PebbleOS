/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include "drew_os.h"

#include <syscall/syscall.h>
#include <system/version.h>

static const DrewOSInfo s_drew_os_info = {
  .magic = DREW_OS_MAGIC,
  .api_version = DREW_OS_API_VERSION,
  .size = sizeof(DrewOSInfo),
  .capabilities = DREW_OS_CAP_APP_WAKEUP | DREW_OS_CAP_APP_GLANCE,
  .fw_version = TINTIN_METADATA.version_tag,
  .app_next_wakeup = sys_wakeup_get_next_for_app,
  .app_glance_subtitle = sys_app_glance_get_current_subtitle,
};

const DrewOSInfo *drew_os_get_info(void) {
  return &s_drew_os_info;
}

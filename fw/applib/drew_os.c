/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include "drew_os.h"

#include <system/version.h>

static const DrewOSInfo s_drew_os_info = {
  .magic = DREW_OS_MAGIC,
  .api_version = DREW_OS_API_VERSION,
  .size = sizeof(DrewOSInfo),
  .capabilities = 0,
  .fw_version = TINTIN_METADATA.version_tag,
};

const DrewOSInfo *drew_os_get_info(void) {
  return &s_drew_os_info;
}

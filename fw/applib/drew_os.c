/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include "drew_os.h"

#include <syscall/syscall.h>
#include <system/version.h>

#ifdef CONFIG_SERVICE_HRM
#include <pbl/services/hrm/hrm_manager.h>

_Static_assert(sizeof(DrewOSHRSample) == sizeof(HRMLatestBPM), "DrewOSHRSample is HRMLatestBPM");
#define DREW_OS_HR_CAPS DREW_OS_CAP_HEART_RATE
#else
#define DREW_OS_HR_CAPS 0
#endif

#ifdef CONFIG_SERVICE_MUSIC
#include <pbl/services/music.h>

_Static_assert(sizeof(DrewOSNowPlaying) == sizeof(MusicNowPlayingInfo),
               "DrewOSNowPlaying is MusicNowPlayingInfo");
_Static_assert(MUSIC_BUFFER_LENGTH == 64, "DrewOSNowPlaying strings are MUSIC_BUFFER_LENGTH");
#define DREW_OS_MUSIC_CAPS DREW_OS_CAP_MUSIC
#else
#define DREW_OS_MUSIC_CAPS 0
#endif

static const DrewOSInfo s_drew_os_info = {
  .magic = DREW_OS_MAGIC,
  .api_version = DREW_OS_API_VERSION,
  .size = sizeof(DrewOSInfo),
  .capabilities = DREW_OS_CAP_APP_WAKEUP | DREW_OS_CAP_APP_GLANCE | DREW_OS_CAP_WATCHFACE_BUTTONS |
                  DREW_OS_HR_CAPS | DREW_OS_MUSIC_CAPS,
  .fw_version = TINTIN_METADATA.version_tag,
  .app_next_wakeup = sys_wakeup_get_next_for_app,
  .app_glance_subtitle = sys_app_glance_get_current_subtitle,
  .watchface_buttons = sys_watchface_get_tap_buttons,
#ifdef CONFIG_SERVICE_HRM
  .hr_request = sys_hrm_manager_app_request,
  .hr_latest = (bool (*)(DrewOSHRSample *))sys_hrm_manager_get_latest_bpm,
#endif
#ifdef CONFIG_SERVICE_MUSIC
  .music_now_playing = (bool (*)(DrewOSNowPlaying *))sys_music_get_now_playing_info,
#endif
};

const DrewOSInfo *drew_os_get_info(void) {
  return &s_drew_os_info;
}

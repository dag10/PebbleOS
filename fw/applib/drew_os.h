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
#define DREW_OS_API_VERSION    6

//! Bits for DrewOSInfo.capabilities
typedef enum {
  //! app_next_wakeup is available (api_version >= 2)
  DREW_OS_CAP_APP_WAKEUP = (1 << 0),
  //! app_glance_subtitle is available (api_version >= 3)
  DREW_OS_CAP_APP_GLANCE = (1 << 1),
  //! The firmware can send Up/Down taps to the watchface, and watchface_buttons is
  //! available (api_version >= 4)
  DREW_OS_CAP_WATCHFACE_BUTTONS = (1 << 2),
  //! The watch has a heart rate sensor, and hr_request / hr_latest are available
  //! (api_version >= 5)
  DREW_OS_CAP_HEART_RATE = (1 << 3),
  //! music_now_playing is available (api_version >= 6)
  DREW_OS_CAP_MUSIC = (1 << 4),
} DrewOSCapability;

//! Latest heart rate sample, as returned by DrewOSInfo.hr_latest. Same layout as the firmware's
//! HRMLatestBPM.
typedef struct DrewOSHRSample {
  //! When the sample arrived (UTC seconds), 0 if there was none since boot
  uint32_t time_utc;
  //! Beats per minute, 0 when off-wrist or implausible
  uint8_t bpm;
  //! HRMQuality: -1 off-wrist, 0 worst, 1 poor, 2 acceptable, 3 good, 4 excellent
  int8_t quality;
  uint16_t reserved;
} DrewOSHRSample;

//! Current track, as returned by DrewOSInfo.music_now_playing. Same layout as the firmware's
//! MusicNowPlayingInfo.
typedef struct DrewOSNowPlaying {
  //! Track title, NUL terminated, empty if unknown
  char title[64];
  //! Track artist, NUL terminated, empty if unknown
  char artist[64];
  //! Track album, NUL terminated, empty if unknown
  char album[64];
  //! MusicPlayState: 0 unknown (not reported), 1 playing, 2 paused, 3 forwarding, 4 rewinding
  uint8_t playback_state;
  uint8_t reserved[3];
} DrewOSNowPlaying;

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

  // api_version >= 4

  //! The buttons, as bits (1 << ButtonId), whose single press on the watchface is sent to
  //! the running watchface's click handlers instead of Quick Launch: Settings -> Quick
  //! Launch -> Tap Up / Tap Down -> "Send to watchface". Only BUTTON_ID_UP and
  //! BUTTON_ID_DOWN can be set. Holds, combos, Select and Back always stay with the system.
  //! Points at the sys_watchface_get_tap_buttons syscall.
  uint32_t (*watchface_buttons)(void);

  // api_version >= 5, NULL without DREW_OS_CAP_HEART_RATE

  //! Sets the foreground app's own heart rate sensor request: sample every @p interval_s
  //! seconds (1 or 2 gets the low-latency ~1 s cadence), with HRV events too if @p hrv, until
  //! @p expire_s seconds from now unless renewed. App exit never extends it. 0 clears it.
  //! Returns false if the heart rate pref is off. Points at the sys_hrm_manager_app_request
  //! syscall.
  bool (*hr_request)(uint16_t interval_s, uint16_t expire_s, bool hrv);
  //! Copies the latest heart rate sample, whoever asked for it. Returns false if there was
  //! none since boot. Points at the sys_hrm_manager_get_latest_bpm syscall; @p out must be in
  //! app memory.
  bool (*hr_latest)(DrewOSHRSample *out);

  // api_version >= 6, NULL without DREW_OS_CAP_MUSIC

  //! Copies the current track (as the Music app shows it) and the playback state. Returns false,
  //! with @p out zeroed, if no title or artist is known. Points at the
  //! sys_music_get_now_playing_info syscall; @p out must be in app memory.
  bool (*music_now_playing)(DrewOSNowPlaying *out);
} DrewOSInfo;

_Static_assert(sizeof(DrewOSInfo) == 52, "DrewOSInfo is ABI");
_Static_assert(sizeof(DrewOSNowPlaying) == 196, "DrewOSNowPlaying is ABI");
_Static_assert(sizeof(DrewOSHRSample) == 8, "DrewOSHRSample is ABI");

const DrewOSInfo *drew_os_get_info(void);

/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

//! The Play/Pause, Next Track and Previous Track Quick Launch actions. The shell sends the
//! command to the phone itself, so the watchface stays on screen; these only run if something
//! launches them some other way.

#include "quick_launch_music.h"

#include "pbl/services/i18n/i18n.h"
#include "pbl/services/music.h"

static void prv_play_pause_main(void) {
  music_command_send(MusicCommandTogglePlayPause);
}

static void prv_next_track_main(void) {
  music_command_send(MusicCommandNextTrack);
}

static void prv_previous_track_main(void) {
  music_command_send(MusicCommandPreviousTrack);
}

const PebbleProcessMd *quick_launch_music_play_pause_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common =
        {
          .main_func = &prv_play_pause_main,
          .uuid = QUICK_LAUNCH_MUSIC_PLAY_PAUSE_UUID,
          .visibility = ProcessVisibilityQuickLaunch,
        },
    /// Quick Launch action that plays or pauses the music on the phone.
    .name = i18n_noop("Play/Pause"),
  };
  return &s_app_info.common;
}

const PebbleProcessMd *quick_launch_music_next_track_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common =
        {
          .main_func = &prv_next_track_main,
          .uuid = QUICK_LAUNCH_MUSIC_NEXT_TRACK_UUID,
          .visibility = ProcessVisibilityQuickLaunch,
        },
    /// Quick Launch action that skips to the next track on the phone.
    .name = i18n_noop("Next Track"),
  };
  return &s_app_info.common;
}

const PebbleProcessMd *quick_launch_music_previous_track_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common =
        {
          .main_func = &prv_previous_track_main,
          .uuid = QUICK_LAUNCH_MUSIC_PREVIOUS_TRACK_UUID,
          .visibility = ProcessVisibilityQuickLaunch,
        },
    /// Quick Launch action that goes back to the previous track on the phone.
    .name = i18n_noop("Previous Track"),
  };
  return &s_app_info.common;
}

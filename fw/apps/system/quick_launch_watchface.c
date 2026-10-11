/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

//! The "Send to watchface" Quick Launch action, offered for Tap Up and Tap Down only.
//! Binding a tap to it makes the shell pass that single press on to the watchface's own
//! click handlers instead of launching anything.

#include "quick_launch_watchface.h"

#include "pbl/services/i18n/i18n.h"

static void prv_main(void) {
  // Never runs; the shell forwards the press to the watchface instead of launching this.
}

const PebbleProcessMd *quick_launch_watchface_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common =
        {
          .main_func = &prv_main,
          .uuid = QUICK_LAUNCH_WATCHFACE_UUID,
          .visibility = ProcessVisibilityQuickLaunch,
        },
    /// Quick Launch action for Tap Up / Tap Down that passes the press to the watchface.
    .name = i18n_noop("Send to watchface"),
  };
  return &s_app_info.common;
}

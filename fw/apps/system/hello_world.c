/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

//! Dummy launcher row used to verify the toolchain. Launching it exits
//! immediately, returning to the launcher.

#include "hello_world.h"

static void prv_main(void) {
}

const PebbleProcessMd *hello_world_get_app_info(void) {
  static const PebbleProcessMdSystem s_app_info = {
    .common =
        {
          .main_func = &prv_main,
          .uuid =
              {0x54, 0x96, 0xe2, 0xe4, 0xfa, 0x11, 0x48, 0x7b, 0x87, 0x8e, 0x30, 0x99, 0x6b, 0x06,
               0x5e, 0xee},
        },
    .name = "HELLO WORLD",
  };
  return &s_app_info.common;
}

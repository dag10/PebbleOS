/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/drivers/button_id.h>
#include <pbl/util/uuid.h>

#include <process_management/app_install_types.h>

bool quick_launch_is_enabled(ButtonId button);
AppInstallId quick_launch_get_app(ButtonId button);
void quick_launch_set_app(ButtonId button, AppInstallId app_id);
void quick_launch_set_enabled(ButtonId button, bool enabled);
void quick_launch_set_quick_launch_setup_opened(uint8_t version);
uint8_t quick_launch_get_quick_launch_setup_opened(void);

bool quick_launch_single_click_is_enabled(ButtonId button);
AppInstallId quick_launch_single_click_get_app(ButtonId button);
void quick_launch_single_click_set_app(ButtonId button, AppInstallId app_id);
void quick_launch_single_click_set_enabled(ButtonId button, bool enabled);
//! True if a tap of @p button (Up, Down or Back) is bound to the "Send to watchface" action.
//! Compares UUIDs only, so it is cheap and safe from a syscall.
bool quick_launch_single_click_is_sent_to_watchface(ButtonId button);
//! True if a hold of @p button is bound to the "Send to watchface" action. Compares UUIDs only.
bool quick_launch_is_sent_to_watchface(ButtonId button);

bool quick_launch_combo_back_up_is_enabled(void);
AppInstallId quick_launch_combo_back_up_get_app(void);
void quick_launch_combo_back_up_set_app(AppInstallId app_id);
void quick_launch_combo_back_up_set_enabled(bool enabled);

bool quick_launch_combo_up_down_is_enabled(void);
AppInstallId quick_launch_combo_up_down_get_app(void);
void quick_launch_combo_up_down_set_app(AppInstallId app_id);
void quick_launch_combo_up_down_set_enabled(bool enabled);

//! The most taps in a row a Back combo counts, Tap, Double Tap and Triple Tap.
#define QUICK_LAUNCH_COMBO_MAX_TAPS (3)

//! Back tapped together with @p button (BUTTON_ID_UP, BUTTON_ID_DOWN or BUTTON_ID_SELECT)
//! @p taps times in a row, 1 to QUICK_LAUNCH_COMBO_MAX_TAPS.
bool quick_launch_combo_tap_is_enabled(ButtonId button, uint8_t taps);
AppInstallId quick_launch_combo_tap_get_app(ButtonId button, uint8_t taps);
void quick_launch_combo_tap_set_app(ButtonId button, uint8_t taps, AppInstallId app_id);
void quick_launch_combo_tap_set_enabled(ButtonId button, uint8_t taps, bool enabled);

/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/services/compositor/compositor.h>

#include <kernel/events.h>
#include <process_management/app_install_manager.h>

void watchface_init(void);

void watchface_handle_button_event(PebbleEvent *e);

void watchface_set_default_install_id(AppInstallId id);

AppInstallId watchface_get_default_install_id(void);

//! The stored default watchface UUID, without resolving or validating it (no app database
//! lookup, so it is safe on a syscall stack).
void watchface_get_default_uuid(Uuid *uuid_out);

void watchface_launch_default(const CompositorTransition *animation);

void watchface_start_low_power(void);

void watchface_reset_click_manager(void);

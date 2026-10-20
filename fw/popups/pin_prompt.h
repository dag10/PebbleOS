/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

#include <applib/ui/window.h>

//! DrewPebbleOS: a keypad window that asks for the watch PIN (pbl/services/drew_pin_lock.h).
//!
//! Tap the keys on a touchscreen, or move the highlight with Up/Down and press it with Select.
//! Back deletes a digit, or cancels when there are none. Entered digits show as dots. Works from
//! KernelMain (pushed as a modal) and from a system app (pushed on its window stack).

typedef enum PinPromptMode {
  //! Ask for the current PIN. Wrong tries count towards the lockout, which the prompt shows.
  PinPromptMode_Verify,
  //! Ask for a new PIN of 4 to 6 digits, twice.
  PinPromptMode_Create,
} PinPromptMode;

//! Called once the prompt has closed. @p pin is the new PIN for PinPromptMode_Create on success,
//! valid only during the call, and NULL otherwise.
typedef void (*PinPromptCallback)(bool success, const char *pin, void *context);

//! Create a prompt without pushing it. @p title is copied; NULL shows "Enter PIN".
Window *pin_prompt_create(PinPromptMode mode, const char *title, PinPromptCallback callback,
                          void *context);

//! Push a prompt from pin_prompt_create(): as a modal on KernelMain, else on the app's stack.
void pin_prompt_push(Window *window);

//! Ask for the current PIN with @p reason as the title. A reusable gate for any feature.
void pin_prompt(const char *reason, PinPromptCallback callback, void *context);

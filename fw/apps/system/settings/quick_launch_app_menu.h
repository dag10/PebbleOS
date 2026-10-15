/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <applib/ui/window.h>
#include <shell/normal/quick_launch.h>

typedef enum QuickLaunchGesture {
  QuickLaunchGestureTap,
  QuickLaunchGestureHold,
  //! Back tapped together with the button (Up, Down or Select).
  QuickLaunchGestureComboTap,
  //! Back tapped together with the button (Up, Down or Select) twice in a row.
  QuickLaunchGestureComboDoubleTap,
  //! Back tapped together with the button (Up, Down or Select) three times in a row.
  QuickLaunchGestureComboTripleTap,
} QuickLaunchGesture;

typedef enum QuickLaunchMenuCategory {
  //! Entries that are only visible in Quick Launch, e.g. the system toggles.
  QuickLaunchMenuCategoryActions,
  //! Regular launcher apps.
  QuickLaunchMenuCategoryApps,
} QuickLaunchMenuCategory;

//! The action or app bound to @p gesture of @p button, INSTALL_ID_INVALID if none.
AppInstallId quick_launch_app_menu_get_app(QuickLaunchGesture gesture, ButtonId button);

//! @param gesture Which of the button's bindings is being edited.
//! @param button The button whose binding is being edited.
//! @param category Which kind of target the menu lists.
//! @param parent The category menu to unwind along with this one on selection, or NULL.
void quick_launch_app_menu_window_push(QuickLaunchGesture gesture, ButtonId button,
                                       QuickLaunchMenuCategory category, Window *parent);

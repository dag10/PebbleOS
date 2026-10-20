/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pin_lock.h"

#ifdef CONFIG_SERVICE_DREW_PIN_LOCK

#include "menu.h"
#include "window.h"

#include <stdio.h>

#include <pbl/services/drew_pin_lock.h>

#include <applib/graphics/graphics.h>
#include <applib/graphics/gtypes.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/dialogs/confirmation_dialog.h>
#include <applib/ui/menu_layer.h>
#include <applib/ui/option_menu_window.h>
#include <applib/ui/vibes.h>
#include <apps/system_app_ids.h>
#include <kernel/pbl_malloc.h>
#include <popups/pin_prompt.h>
#include <process_management/app_install_manager.h>
#include <process_management/app_menu_data_source.h>
#include <resource/resource_ids.auto.h>
#include <shell/prefs.h>

//! @file
//! Settings > PIN Lock. Opening it asks for the PIN when one is set, so nothing here changes
//! without it: Change PIN, Locked Apps (installed apps with checkboxes) and Remove PIN. With no
//! PIN it only offers Set PIN.

#define SUBTITLE_LENGTH  (24)
#define CHECKBOX_SPACING (7)

typedef enum {
  PinLockRowSet,
  PinLockRowApps,
  PinLockRowRemove,
  PinLockRowCount,
} PinLockRow;

typedef struct SettingsPinLockData {
  SettingsCallbacks callbacks;
  char subtitle[SUBTITLE_LENGTH];
} SettingsPinLockData;

typedef struct LockedAppsData {
  AppMenuDataSource data_source;
  OptionMenu *option_menu;
  GBitmap checked;
  GBitmap unchecked;
} LockedAppsData;

static void prv_reload(void) {
  settings_menu_reload_data(SettingsMenuItemPinLock);
}

// Locked Apps
///////////////

static bool prv_app_filter(struct AppMenuDataSource *source, AppInstallEntry *entry) {
  // Watchfaces are home: locking one would ask for the PIN every time an app closes.
  return !app_install_entry_is_watchface(entry) && !app_install_entry_is_hidden(entry) &&
         !app_install_entry_is_quick_launch_visible_only(entry) &&
         (entry->install_id != APP_ID_SETTINGS);
}

static uint16_t prv_apps_num_rows(OptionMenu *option_menu, void *context) {
  LockedAppsData *data = context;
  return app_menu_data_source_get_count(&data->data_source);
}

static void prv_apps_draw_row(OptionMenu *option_menu, GContext *ctx, const Layer *cell_layer,
                              const GRect *text_frame, uint32_t row, bool selected, void *context) {
  LockedAppsData *data = context;
  AppMenuNode *node = app_menu_data_source_get_node_at_index(&data->data_source, row);
  if (!node) {
    return;
  }
  const GBitmap *icon =
      drew_pin_lock_is_app_locked(&node->uuid) ? &data->checked : &data->unchecked;
  GRect icon_frame = {.size = gbitmap_get_bounds(icon).size};
  grect_align(&icon_frame, &cell_layer->bounds, GAlignRight, false /* clip */);
  icon_frame.origin.x -= CHECKBOX_SPACING;
  graphics_context_set_compositing_mode(ctx, GCompOpTint);
  graphics_draw_bitmap_in_rect(ctx, icon, &icon_frame);

  GRect frame = *text_frame;
  frame.size.w = MAX(0, icon_frame.origin.x - CHECKBOX_SPACING - frame.origin.x);
  option_menu_system_draw_row(option_menu, ctx, cell_layer, &frame, node->name, selected, context);
}

static void prv_apps_select(OptionMenu *option_menu, int selection, void *context) {
  LockedAppsData *data = context;
  AppMenuNode *node = app_menu_data_source_get_node_at_index(&data->data_source, selection);
  if (!node) {
    return;
  }
  drew_pin_lock_set_app_locked(&node->uuid, !drew_pin_lock_is_app_locked(&node->uuid));
  option_menu_reload_data(option_menu);
}

static void prv_apps_changed(void *context) {
  LockedAppsData *data = context;
  option_menu_reload_data(data->option_menu);
}

static void prv_apps_unload(OptionMenu *option_menu, void *context) {
  LockedAppsData *data = context;
  option_menu_destroy(option_menu);
  app_menu_data_source_deinit(&data->data_source);
  gbitmap_deinit(&data->checked);
  gbitmap_deinit(&data->unchecked);
  app_free(data);
  prv_reload();
}

static void prv_push_locked_apps(void) {
  LockedAppsData *data = app_zalloc_check(sizeof(*data));
  gbitmap_init_with_resource(&data->checked, RESOURCE_ID_CHECKBOX_ICON_CHECKED);
  gbitmap_init_with_resource(&data->unchecked, RESOURCE_ID_CHECKBOX_ICON_UNCHECKED);
  app_menu_data_source_init(&data->data_source,
                            &(AppMenuDataSourceCallbacks){
                              .changed = prv_apps_changed,
                              .filter = prv_app_filter,
                            },
                            data);

  OptionMenu *option_menu = option_menu_create();
  data->option_menu = option_menu;
  const GColor highlight = shell_prefs_get_theme_highlight_color();
  option_menu_configure(option_menu,
                        &(OptionMenuConfig){
                          .title = "Locked Apps",
                          .choice = OPTION_MENU_CHOICE_NONE,
                          .content_type = OptionMenuContentType_SingleLine,
                          .status_colors = {GColorWhite, GColorBlack},
                          .highlight_colors = {highlight, gcolor_legible_over(highlight)},
                          .icons_enabled = false,
                        });
  option_menu_set_callbacks(option_menu,
                            &(OptionMenuCallbacks){
                              .select = prv_apps_select,
                              .get_num_rows = prv_apps_num_rows,
                              .draw_row = prv_apps_draw_row,
                              .unload = prv_apps_unload,
                            },
                            data);
  app_window_stack_push(&option_menu->window, true /* animated */);
}

// PIN changes
///////////////

static void prv_new_pin_done(bool success, const char *pin, void *context) {
  if (success && drew_pin_lock_set_pin(pin)) {
    vibes_short_pulse();
  }
  prv_reload();
}

static void prv_push_new_pin(void) {
  pin_prompt_push(pin_prompt_create(PinPromptMode_Create, "New PIN", prv_new_pin_done, nullptr));
}

static void prv_remove_click(ClickRecognizerRef recognizer, void *context) {
  ConfirmationDialog *dialog = context;
  if (click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP) {
    drew_pin_lock_remove();
    prv_reload();
  }
  confirmation_dialog_pop(dialog);
}

static void prv_remove_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_remove_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_remove_click);
}

static void prv_push_remove_confirmation(void) {
  ConfirmationDialog *dialog = confirmation_dialog_create("Remove PIN");
  if (!dialog) {
    return;
  }
  dialog_set_text(confirmation_dialog_get_dialog(dialog), "Remove the PIN? Every app unlocks.");
  confirmation_dialog_set_click_config_provider(dialog, prv_remove_click_config);
  action_bar_layer_set_context(confirmation_dialog_get_action_bar(dialog), dialog);
  app_confirmation_dialog_push(dialog);
}

// Menu
////////

static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  return drew_pin_lock_has_pin() ? PinLockRowCount : 1;
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx, const Layer *cell_layer,
                            uint16_t row, bool selected) {
  SettingsPinLockData *data = (SettingsPinLockData *)context;
  const uint8_t length = drew_pin_lock_get_length();
  const char *title = "";
  const char *subtitle = nullptr;
  switch ((PinLockRow)row) {
    case PinLockRowSet:
      if (length) {
        title = "Change PIN";
        snprintf(data->subtitle, sizeof(data->subtitle), "%u digits", length);
        subtitle = data->subtitle;
      } else {
        title = "Set PIN";
        subtitle = "Lock apps with a PIN";
      }
      break;
    case PinLockRowApps: {
      title = "Locked Apps";
      const uint16_t count = drew_pin_lock_get_num_locked_apps();
      if (count == 0) {
        subtitle = "None";
      } else {
        snprintf(data->subtitle, sizeof(data->subtitle), "%u %s", count,
                 (count == 1) ? "app" : "apps");
        subtitle = data->subtitle;
      }
      break;
    }
    case PinLockRowRemove:
      title = "Remove PIN";
      subtitle = "Unlocks every app";
      break;
    case PinLockRowCount:
      break;
  }
  menu_cell_basic_draw(ctx, cell_layer, title, subtitle, nullptr);
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  switch ((PinLockRow)row) {
    case PinLockRowSet:
      prv_push_new_pin();
      break;
    case PinLockRowApps:
      prv_push_locked_apps();
      break;
    case PinLockRowRemove:
      prv_push_remove_confirmation();
      break;
    case PinLockRowCount:
      break;
  }
}

static void prv_deinit_cb(SettingsCallbacks *context) {
  app_free(context);
}

static Window *prv_create_menu(void) {
  SettingsPinLockData *data = app_zalloc_check(sizeof(*data));
  data->callbacks = (SettingsCallbacks){
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
  };
  return settings_window_create(SettingsMenuItemPinLock, &data->callbacks);
}

static void prv_unlocked(bool success, const char *pin, void *context) {
  if (success) {
    app_window_stack_push(prv_create_menu(), true /* animated */);
  }
}

static Window *prv_init(void) {
  if (drew_pin_lock_has_pin()) {
    return pin_prompt_create(PinPromptMode_Verify, "PIN Lock", prv_unlocked, nullptr);
  }
  return prv_create_menu();
}

const SettingsModuleMetadata *settings_pin_lock_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    .name = "PIN Lock",
    .init = prv_init,
  };
  return &s_module_info;
}

#endif // CONFIG_SERVICE_DREW_PIN_LOCK

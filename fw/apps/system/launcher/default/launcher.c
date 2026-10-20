/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "launcher.h"

#include "menu_layer.h"

#include <pbl/kernel/compiler.h>
#include <pbl/services/i18n/i18n.h>

#include <applib/app.h>
#include <applib/app_focus_service.h>
#include <applib/event_service_client.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/option_menu_window.h>
#include <applib/ui/window_stack.h>
#include <kernel/event_loop.h>
#include <kernel/events.h>
#include <kernel/pbl_malloc.h>
#include <process_state/app_state/app_state.h>
#include <shell/normal/app_idle_timeout.h>
#include <shell/prefs.h>

typedef struct LauncherAppWindowData {
  Window window;
  LauncherMenuLayer launcher_menu_layer;
  AppMenuDataSource app_menu_data_source;
  EventServiceInfo pref_change_event_info;
  //! The menu layer's click config, which ours wraps to add Back
  ClickConfigProvider menu_click_config_provider;
  void *menu_click_config_context;
} LauncherAppWindowData;

typedef struct LauncherAppPersistedData {
  bool valid;
  RtcTicks leave_time;
  LauncherMenuLayerSelectionState selection_state;
  //! App that was selected, to tell whether the sort order moved it
  AppInstallId selected_app;
  LauncherDrawState draw_state;
} LauncherAppPersistedData;

static LauncherAppPersistedData s_launcher_app_persisted_data;

/////////////////////////////
// AppFocusService handlers

static void prv_did_focus(bool in_focus) {
  LauncherAppWindowData *data = app_state_get_user_data();
  if (in_focus) {
    launcher_menu_layer_set_selection_animations_enabled(&data->launcher_menu_layer, true);
  }
}

static void prv_set_click_config(LauncherAppWindowData *data);

static void prv_will_focus(bool in_focus) {
  LauncherAppWindowData *data = app_state_get_user_data();
  if (!in_focus) {
    launcher_menu_layer_set_selection_animations_enabled(&data->launcher_menu_layer, false);
  } else {
    // Selecting an app turns clicks off until the app replaces the launcher. A launch the user
    // cancels from a modal (the DrewPebbleOS PIN prompt) leaves the launcher here, so turn them
    // back on.
    prv_set_click_config(data);
  }
}

static void prv_pref_change_handler([[maybe_unused]] PebbleEvent *event, void *context) {
  LauncherAppWindowData *data = context;
  launcher_menu_layer_update_content_size(&data->launcher_menu_layer);
  // Re-initializing the menu layer replaces our click config
  prv_set_click_config(data);
}

////////////////////////////////
// AppMenuDataSource callbacks

static bool prv_app_filter_callback([[maybe_unused]] AppMenuDataSource *source,
                                    AppInstallEntry *entry) {
  // Skip watchfaces and hidden apps
  return (!app_install_entry_is_watchface(entry) && !app_install_entry_is_hidden((entry)));
}

static void prv_data_changed(void *context) {
  LauncherAppWindowData *data = context;
  launcher_menu_layer_reload_data(&data->launcher_menu_layer);
}

//! We're not 100% sure of the order of the launcher list yet, so use this function to transform
//! the row index to achieve the desired list ordering
static uint16_t prv_transform_index(AppMenuDataSource *data_source, uint16_t original_index,
                                    void *context) {
#ifdef CONFIG_SHELL_SDK
  // We want the newest installed developer app to appear at the top
  // This works at the moment because there is only one system app, Watchfaces
  return app_menu_data_source_get_count(data_source) - 1 - original_index;
#else
  return original_index;
#endif
}

/////////////////////
// Sort mode

static AppMenuSortMode prv_get_app_menu_sort_mode(void) {
#ifdef CONFIG_SHELL_SDK
  return AppMenuSortMode_Default;
#else
  switch (shell_prefs_get_launcher_sort_mode()) {
    case LauncherSortMode_RecentlyUsed:
      return AppMenuSortMode_RecentlyUsed;
    case LauncherSortMode_Alphabetical:
      return AppMenuSortMode_Alphabetical;
    case LauncherSortMode_Default:
    case LauncherSortModeCount:
      break;
  }
  return AppMenuSortMode_Default;
#endif
}

#ifndef CONFIG_SHELL_SDK
static const char *s_sort_mode_labels[LauncherSortModeCount] = {
  [LauncherSortMode_Default] = i18n_ctx_noop("LauncherSort", "Default"),
  [LauncherSortMode_RecentlyUsed] = i18n_noop("Recently Used"),
  [LauncherSortMode_Alphabetical] = i18n_noop("Alphabetical"),
};

static uint16_t prv_sort_menu_get_num_rows([[maybe_unused]] OptionMenu *option_menu,
                                           [[maybe_unused]] void *context) {
  return LauncherSortModeCount;
}

static void prv_sort_menu_draw_row(OptionMenu *option_menu, GContext *ctx, const Layer *cell_layer,
                                   const GRect *text_frame, uint32_t row, bool selected,
                                   void *context) {
  option_menu_system_draw_row(option_menu, ctx, cell_layer, text_frame,
                              i18n_get(s_sort_mode_labels[row], option_menu), selected, context);
}

static void prv_sort_menu_select(OptionMenu *option_menu, int selection, void *context) {
  LauncherAppWindowData *data = context;
  shell_prefs_set_launcher_sort_mode((LauncherSortMode)selection);
  app_menu_data_source_set_sort_mode(&data->app_menu_data_source, prv_get_app_menu_sort_mode());

  // Start from the top of the newly ordered list
  LauncherMenuLayer *launcher_menu_layer = &data->launcher_menu_layer;
  launcher_menu_layer_set_selection_state(launcher_menu_layer,
                                          &(LauncherMenuLayerSelectionState){
                                            .content_size = launcher_menu_layer->content_size,
                                          });

  app_window_stack_remove(&option_menu->window, true /* animated */);
}

static void prv_sort_menu_unload(OptionMenu *option_menu, [[maybe_unused]] void *context) {
  i18n_free_all(option_menu);
  option_menu_destroy(option_menu);
}

static void prv_sort_menu_push(LauncherAppWindowData *data) {
  OptionMenu *option_menu = option_menu_create();
  if (!option_menu) {
    return;
  }

  const GColor highlight_bg = shell_prefs_get_theme_highlight_color();
  option_menu_configure(option_menu,
                        &(OptionMenuConfig){
                          /// Title of the menu for choosing the launcher's app order
                          .title = i18n_get("Sort Apps", option_menu),
                          .choice = shell_prefs_get_launcher_sort_mode(),
                          .content_type = OptionMenuContentType_SingleLine,
                          .status_colors = {GColorWhite, GColorBlack},
                          .highlight_colors = {highlight_bg, gcolor_legible_over(highlight_bg)},
                          .icons_enabled = true,
                        });
  option_menu_set_callbacks(option_menu,
                            &(OptionMenuCallbacks){
                              .select = prv_sort_menu_select,
                              .get_num_rows = prv_sort_menu_get_num_rows,
                              .draw_row = prv_sort_menu_draw_row,
                              .unload = prv_sort_menu_unload,
                            },
                            data);
  app_window_stack_push(&option_menu->window, true /* animated */);
}

static void prv_back_click_handler([[maybe_unused]] ClickRecognizerRef recognizer,
                                   [[maybe_unused]] void *context) {
  // What Back does in a window that doesn't override it
  window_stack_pop_with_transition(app_state_get_window_stack(), NULL /* transition */);
}

static void prv_back_long_click_handler([[maybe_unused]] ClickRecognizerRef recognizer,
                                        void *context) {
  // Holding Back force quits apps, so keep it from quitting the launcher under the menu
  launcher_cancel_force_quit();
  prv_sort_menu_push(context);
}

static void prv_click_config_provider(void *context) {
  LauncherAppWindowData *data = context;
  for (ButtonId button = 0; button < NUM_BUTTONS; button++) {
    window_set_click_context(button, data->menu_click_config_context);
  }
  data->menu_click_config_provider(data->menu_click_config_context);

  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click_handler);
  window_long_click_subscribe(BUTTON_ID_BACK, 0 /* default delay */, prv_back_long_click_handler,
                              NULL);
  window_set_click_context(BUTTON_ID_BACK, data);
}
#endif

static void prv_set_click_config(LauncherAppWindowData *data) {
  Window *window = &data->window;
#ifndef CONFIG_SHELL_SDK
  if (window_get_click_config_provider(window) == prv_click_config_provider) {
    return;
  }
#endif
  launcher_menu_layer_set_click_config_onto_window(&data->launcher_menu_layer, window);
#ifndef CONFIG_SHELL_SDK
  data->menu_click_config_provider = window_get_click_config_provider(window);
  data->menu_click_config_context = window_get_click_config_context(window);
  window_set_click_config_provider_with_context(window, prv_click_config_provider, data);
#endif
}

/////////////////////
// Window callbacks

static void prv_window_load(Window *window) {
  LauncherAppWindowData *data = window_get_user_data(window);

  Layer *window_root_layer = window_get_root_layer(window);

  AppMenuDataSource *data_source = &data->app_menu_data_source;
  app_menu_data_source_init(data_source,
                            &(AppMenuDataSourceCallbacks){
                              .changed = prv_data_changed,
                              .filter = prv_app_filter_callback,
                              .transform_index = prv_transform_index,
                            },
                            data);
  app_menu_data_source_set_sort_mode(data_source, prv_get_app_menu_sort_mode());

  LauncherMenuLayer *launcher_menu_layer = &data->launcher_menu_layer;
  launcher_menu_layer_init(launcher_menu_layer, data_source);
  prv_set_click_config(data);
  layer_add_child(window_root_layer, launcher_menu_layer_get_layer(launcher_menu_layer));

  // If we have a saved launcher selection state, restore it. Other than the default order, a sort
  // order can move apps (e.g. the app just used to the top); start from the top if it moved ours.
  const LauncherAppPersistedData *persisted = &s_launcher_app_persisted_data;
  const bool selected_app_moved =
      (data_source->sort_mode != AppMenuSortMode_Default) &&
      (app_menu_data_source_get_index_of_app_with_install_id(
           data_source, persisted->selected_app) != persisted->selection_state.row_index);
  if (persisted->valid && !selected_app_moved) {
    launcher_menu_layer_set_selection_state(launcher_menu_layer, &persisted->selection_state);
  }

  app_focus_service_subscribe_handlers((AppFocusHandlers){
    .did_focus = prv_did_focus,
    .will_focus = prv_will_focus,
  });

  data->pref_change_event_info = (EventServiceInfo){
    .type = PEBBLE_PREF_CHANGE_EVENT,
    .handler = prv_pref_change_handler,
    .context = data,
  };
  event_service_client_subscribe(&data->pref_change_event_info);
}

static void prv_window_unload(Window *window) {
  LauncherAppWindowData *data = window_get_user_data(window);

  // Capture the vertical range of the selection rectangle for compositor transition animations
  GRangeVertical launcher_selection_vertical_range;
  launcher_menu_layer_get_selection_vertical_range(&data->launcher_menu_layer,
                                                   &launcher_selection_vertical_range);

  // Save the current state of the launcher so we can know its draw state and restore it later
  s_launcher_app_persisted_data = (LauncherAppPersistedData){
    .valid = true,
    .leave_time = rtc_get_ticks(),
    .draw_state.selection_vertical_range = launcher_selection_vertical_range,
    .draw_state.selection_background_color = shell_prefs_get_theme_highlight_color(),
  };
  launcher_menu_layer_get_selection_state(&data->launcher_menu_layer,
                                          &s_launcher_app_persisted_data.selection_state);
  const AppMenuNode *selected_node = app_menu_data_source_get_node_at_index(
      &data->app_menu_data_source, s_launcher_app_persisted_data.selection_state.row_index);
  s_launcher_app_persisted_data.selected_app =
      selected_node ? selected_node->install_id : INSTALL_ID_INVALID;

  event_service_client_unsubscribe(&data->pref_change_event_info);
  app_focus_service_unsubscribe();
  launcher_menu_layer_deinit(&data->launcher_menu_layer);
  app_menu_data_source_deinit(&data->app_menu_data_source);
}

////////////////////
// App boilerplate

static void prv_launcher_menu_window_push(void) {
  LauncherAppWindowData *data = app_zalloc_check(sizeof(*data));
  app_state_set_user_data(data);

  Window *window = &data->window;
  window_init(window, WINDOW_NAME("Launcher Menu"));
  window_set_user_data(window, data);
  window_set_window_handlers(window, &(WindowHandlers){
                                       .load = prv_window_load,
                                       .unload = prv_window_unload,
                                     });

  const bool animated = false;
  app_window_stack_push(window, animated);
}

static void prv_main(void) {
  const LauncherMenuArgs *args = (const LauncherMenuArgs *)app_manager_get_task_context()->args;
  // Reset the selection state of the launcher if we're visiting it for the first time or if
  // it has been more than RETURN_TIMEOUT_TICKS since we were last in the launcher
  if (args && args->reset_scroll) {
    if ((s_launcher_app_persisted_data.leave_time + RETURN_TIMEOUT_TICKS) <= rtc_get_ticks()) {
      s_launcher_app_persisted_data.valid = false;
    }
  }

  prv_launcher_menu_window_push();

  app_idle_timeout_start(APP_IDLE_TIMEOUT_LAUNCHER_MS);

  app_event_loop();
}

const PebbleProcessMd *launcher_menu_app_get_app_info(void) {
  static const PebbleProcessMdSystem s_launcher_menu_app_info = {
    .common =
        {.main_func = prv_main,
         // UUID: dec0424c-0625-4878-b1f2-147e57e83688
         .uuid =
             {0xde, 0xc0, 0x42, 0x4c, 0x06, 0x25, 0x48, 0x78, 0xb1, 0xf2, 0x14, 0x7e, 0x57, 0xe8,
              0x36, 0x88},
         .visibility = ProcessVisibilityHidden},
    .name = "Launcher",
  };
  return (const PebbleProcessMd *)&s_launcher_menu_app_info;
}

const LauncherDrawState *launcher_app_get_draw_state(void) {
  return &s_launcher_app_persisted_data.draw_state;
}

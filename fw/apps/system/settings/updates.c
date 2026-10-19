/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include "updates.h"

#ifdef CONFIG_SERVICE_DREW_UPDATES

#include "menu.h"
#include "window.h"

#include <stdio.h>
#include <string.h>

#include <pbl/drivers/rtc.h>
#include <pbl/services/clock.h>
#include <pbl/services/comm_session/session.h>
#include <pbl/services/drew_updates.h>
#include <pbl/services/i18n/i18n.h>

#include <applib/event_service_client.h>
#include <applib/fonts/fonts.h>
#include <applib/graphics/graphics.h>
#include <applib/graphics/text.h>
#include <applib/ui/app_window_stack.h>
#include <applib/ui/dialogs/confirmation_dialog.h>
#include <applib/ui/option_menu_window.h>
#include <applib/ui/scroll_layer.h>
#include <applib/ui/status_bar_layer.h>
#include <applib/ui/ui.h>
#include <kernel/events.h>
#include <kernel/pbl_malloc.h>
#include <shell/normal/app_idle_timeout.h>

#define STATUS_ROW         (0)
#define SUBTITLE_LENGTH    (64)
#define DETAIL_LINE_LENGTH (96)
#define DETAIL_MARGIN      PBL_IF_RECT_ELSE(6, 24)
#define DETAIL_SPACING     (4)

typedef struct SettingsUpdatesData SettingsUpdatesData;

typedef struct UpdatesDetail {
  Window window;
  StatusBarLayer status_layer;
  ScrollLayer scroll_layer;
  SettingsUpdatesData *list;
  DrewUpdateEntry entry;
  char changelog[DREW_UPDATES_CHANGELOG_LENGTH + 1];
  char kind_line[DETAIL_LINE_LENGTH];
  char current_line[DETAIL_LINE_LENGTH];
  char new_line[DETAIL_LINE_LENGTH];
  char action_line[DETAIL_LINE_LENGTH];
  char confirm_text[DETAIL_LINE_LENGTH * 2];
  ConfirmationDialog *confirm_dialog;
} UpdatesDetail;

struct SettingsUpdatesData {
  SettingsCallbacks callbacks;
  DrewUpdatesInfo info;
  DrewUpdateInstallStatus install;
  //! A finished install's status as it was when the page opened; not shown again.
  DrewUpdateInstallStatus finished_at_open;
  bool connected;
  bool supported;
  bool requested;
  char title[SUBTITLE_LENGTH];
  char subtitle[SUBTITLE_LENGTH];
  char entry_subtitle[SUBTITLE_LENGTH];
  UpdatesDetail *detail;
  EventServiceInfo updates_event_info;
  EventServiceInfo session_event_info;
  EventServiceInfo capabilities_event_info;
};

// State
/////////

static void prv_refresh_state(SettingsUpdatesData *data) {
  drew_updates_get_info(&data->info);
  drew_updates_get_install_status(&data->install);
  data->connected = (comm_session_get_system_session() != nullptr);
  data->supported = drew_updates_phone_supported();
}

static void prv_request_list_if_needed(SettingsUpdatesData *data) {
  if (!data->requested && data->supported) {
    data->requested = drew_updates_request_list(true /* recheck */);
  }
}

static bool prv_install_finished(const DrewUpdateInstallStatus *install) {
  return install->state == DrewUpdateInstallDone || install->state == DrewUpdateInstallFailed;
}

static bool prv_install_running(SettingsUpdatesData *data) {
  return data->install.valid && !prv_install_finished(&data->install);
}

//! The install status for @p entry, as text; nullptr if there's none to show.
static const char *prv_install_text(SettingsUpdatesData *data, const DrewUpdateEntry *entry,
                                    char *buffer, size_t size) {
  const DrewUpdateInstallStatus *install = &data->install;
  const DrewUpdateInstallStatus *old = &data->finished_at_open;
  const bool stale = old->valid && install->state == old->state &&
                     strcmp(install->id, old->id) == 0 &&
                     strcmp(install->message, old->message) == 0;
  if (!install->valid || stale || strcmp(install->id, entry->id) != 0) {
    return (entry->flags & DrewUpdateFlagInstalling) ? i18n_get("Installing...", data) : nullptr;
  }
  switch (install->state) {
    case DrewUpdateInstallStarting:
      return i18n_get("Starting...", data);
    case DrewUpdateInstallDownloading:
      return i18n_get("Downloading...", data);
    case DrewUpdateInstallTransferring:
      if (install->percent <= 100) {
        snprintf(buffer, size, i18n_get("Installing... %u%%", data), install->percent);
        return buffer;
      }
      return i18n_get("Installing...", data);
    case DrewUpdateInstallDone:
      return i18n_get("Installed", data);
    case DrewUpdateInstallRebooting:
      return i18n_get("Restarting the watch...", data);
    case DrewUpdateInstallFailed:
      if (install->message[0]) {
        snprintf(buffer, size, i18n_get("Failed: %s", data), install->message);
        return buffer;
      }
      return i18n_get("Failed", data);
  }
  return nullptr;
}

static const char *prv_kind_name(SettingsUpdatesData *data, DrewUpdateKind kind) {
  switch (kind) {
    case DrewUpdateKindFirmware:
      return i18n_get("Firmware", data);
    case DrewUpdateKindWatchface:
      return i18n_get("Watchface", data);
    case DrewUpdateKindWatchapp:
      return i18n_get("App", data);
  }
  return "";
}

// Detail
//////////

static int16_t prv_draw_block(GContext *ctx, const char *text, GFont font, int16_t y,
                              int16_t width) {
  if (!text || !text[0]) {
    return y;
  }
  const GRect box = GRect(DETAIL_MARGIN, y, width, 2000);
  graphics_draw_text(ctx, text, font, box, GTextOverflowModeWordWrap, GTextAlignmentLeft, nullptr);
  const GSize size = app_graphics_text_layout_get_content_size(
      text, font, box, GTextOverflowModeWordWrap, GTextAlignmentLeft);
  return y + size.h + DETAIL_SPACING;
}

static void prv_detail_update_text(UpdatesDetail *detail) {
  SettingsUpdatesData *data = detail->list;
  const DrewUpdateEntry *entry = &detail->entry;

  const char *kind = prv_kind_name(data, entry->kind);
  if (entry->size_bytes >= 1000 * 1000) {
    snprintf(detail->kind_line, sizeof(detail->kind_line), "%s, %lu.%lu MB", kind,
             (unsigned long)(entry->size_bytes / (1000 * 1000)),
             (unsigned long)(entry->size_bytes % (1000 * 1000) / (100 * 1000)));
  } else if (entry->size_bytes > 0) {
    snprintf(detail->kind_line, sizeof(detail->kind_line), "%s, %lu KB", kind,
             (unsigned long)((entry->size_bytes + 999) / 1000));
  } else {
    strncpy(detail->kind_line, kind, sizeof(detail->kind_line) - 1);
  }

  detail->current_line[0] = '\0';
  if (entry->current_version[0]) {
    snprintf(detail->current_line, sizeof(detail->current_line), i18n_get("Installed: %s", data),
             entry->current_version);
  }
  snprintf(detail->new_line, sizeof(detail->new_line), i18n_get("New: %s", data),
           entry->new_version);

  char buffer[DETAIL_LINE_LENGTH];
  const char *install = prv_install_text(data, entry, buffer, sizeof(buffer));
  const char *action = install;
  if (!action) {
    if (data->supported) {
      action = i18n_get("Press Select to install", data);
    } else if (data->connected) {
      action = i18n_get("Needs DrewMobileApp to install", data);
    } else {
      action = i18n_get("Connect the phone to install", data);
    }
  }
  strncpy(detail->action_line, action, sizeof(detail->action_line) - 1);
}

static void prv_detail_content_update_proc(Layer *layer, GContext *ctx) {
  ScrollLayer *scroll_layer = (ScrollLayer *)layer->parent;
  UpdatesDetail *detail = scroll_layer->context;
  SettingsUpdatesData *data = detail->list;
  const DrewUpdateEntry *entry = &detail->entry;
  const int16_t width = layer->bounds.size.w - 2 * DETAIL_MARGIN;
  const GFont title_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
  const GFont body_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  const GFont bold_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);

  graphics_context_set_text_color(ctx, GColorBlack);
  int16_t y = 0;
  y = prv_draw_block(ctx, entry->name, title_font, y, width);
  y = prv_draw_block(ctx, detail->kind_line, body_font, y, width);
  y = prv_draw_block(ctx, detail->current_line, body_font, y, width);
  y = prv_draw_block(ctx, detail->new_line, body_font, y, width);
  if (entry->flags & DrewUpdateFlagNotDrew) {
    y = prv_draw_block(ctx, i18n_get("Official PebbleOS: replaces DrewPebbleOS.", data), bold_font,
                       y, width);
  }
  if (entry->flags & DrewUpdateFlagReboots) {
    y = prv_draw_block(ctx, i18n_get("The watch restarts to install it.", data), bold_font, y,
                       width);
  }
  graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack));
  y = prv_draw_block(ctx, detail->action_line, bold_font, y, width);
  graphics_context_set_text_color(ctx, GColorBlack);

  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_line(ctx, GPoint(DETAIL_MARGIN, y), GPoint(DETAIL_MARGIN + width, y));
  y += DETAIL_SPACING;
  const char *changelog =
      detail->changelog[0] ? detail->changelog : i18n_get("No release notes.", data);
  y = prv_draw_block(ctx, changelog, body_font, y, width);

  scroll_layer_set_content_size(scroll_layer,
                                GSize(layer->bounds.size.w, y + PBL_IF_ROUND_ELSE(60, 10)));
}

static void prv_detail_refresh(UpdatesDetail *detail) {
  for (uint8_t i = 0; i < detail->list->info.num_entries; i++) {
    DrewUpdateEntry entry;
    if (drew_updates_get_entry(i, &entry) && strcmp(entry.id, detail->entry.id) == 0) {
      detail->entry = entry;
      drew_updates_copy_changelog(i, detail->changelog, sizeof(detail->changelog));
      break;
    }
  }
  prv_detail_update_text(detail);
  layer_mark_dirty(scroll_layer_get_layer(&detail->scroll_layer));
}

static void prv_confirm_click(ClickRecognizerRef recognizer, void *context) {
  UpdatesDetail *detail = context;
  if (click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP) {
    drew_updates_request_install(detail->entry.id);
    // The progress shows near the top.
    scroll_layer_set_content_offset(&detail->scroll_layer, GPointZero, false /* animated */);
  }
  confirmation_dialog_pop(detail->confirm_dialog);
  detail->confirm_dialog = nullptr;
}

static void prv_confirm_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, prv_confirm_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, prv_confirm_click);
}

static void prv_detail_select_click(ClickRecognizerRef recognizer, void *context) {
  UpdatesDetail *detail = context;
  SettingsUpdatesData *data = detail->list;
  if (!data->supported || prv_install_running(data)) {
    return;
  }
  const DrewUpdateEntry *entry = &detail->entry;
  const char *warning = "";
  if (entry->flags & DrewUpdateFlagNotDrew) {
    warning = i18n_get(" It replaces DrewPebbleOS.", data);
  } else if (entry->flags & DrewUpdateFlagReboots) {
    warning = i18n_get(" The watch restarts.", data);
  }
  snprintf(detail->confirm_text, sizeof(detail->confirm_text), i18n_get("Install %s %s?%s", data),
           entry->name, entry->new_version, warning);

  ConfirmationDialog *dialog = confirmation_dialog_create("Install Update");
  if (!dialog) {
    return;
  }
  detail->confirm_dialog = dialog;
  dialog_set_text(confirmation_dialog_get_dialog(dialog), detail->confirm_text);
  confirmation_dialog_set_click_config_provider(dialog, prv_confirm_click_config);
  action_bar_layer_set_context(confirmation_dialog_get_action_bar(dialog), detail);
  app_confirmation_dialog_push(dialog);
}

static void prv_detail_click_config(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_detail_select_click);
}

static void prv_detail_unload(Window *window) {
  UpdatesDetail *detail = window_get_user_data(window);
  detail->list->detail = nullptr;
  scroll_layer_deinit(&detail->scroll_layer);
  status_bar_layer_deinit(&detail->status_layer);
  app_free(detail);
}

static void prv_push_detail(SettingsUpdatesData *data, uint8_t index) {
  UpdatesDetail *detail = app_zalloc_check(sizeof(UpdatesDetail));
  detail->list = data;
  if (!drew_updates_get_entry(index, &detail->entry)) {
    app_free(detail);
    return;
  }
  drew_updates_copy_changelog(index, detail->changelog, sizeof(detail->changelog));
  prv_detail_update_text(detail);

  Window *window = &detail->window;
  window_init(window, WINDOW_NAME("Update Detail"));
  window_set_user_data(window, detail);
  window_set_window_handlers(window, &(WindowHandlers){
                                       .unload = prv_detail_unload,
                                     });

  StatusBarLayer *status_layer = &detail->status_layer;
  status_bar_layer_init(status_layer);
  status_bar_layer_set_title(status_layer, i18n_get("Updates", data), false, false);
  status_bar_layer_set_colors(status_layer, GColorWhite, GColorBlack);
  status_bar_layer_set_separator_mode(status_layer, OPTION_MENU_STATUS_SEPARATOR_MODE);
  layer_add_child(&window->layer, status_bar_layer_get_layer(status_layer));

  const GRect bounds = grect_inset(window->layer.bounds, (GEdgeInsets){
                                                           .top = STATUS_BAR_LAYER_HEIGHT + 2,
                                                         });
  ScrollLayer *scroll_layer = &detail->scroll_layer;
  scroll_layer_init(scroll_layer, &bounds);
  scroll_layer_set_context(scroll_layer, detail);
  scroll_layer_set_shadow_hidden(scroll_layer, true);
  scroll_layer_set_callbacks(scroll_layer, (ScrollLayerCallbacks){
                                             .click_config_provider = prv_detail_click_config,
                                           });
  scroll_layer_set_click_config_onto_window(scroll_layer, window);
  layer_set_update_proc(&scroll_layer->content_sublayer, prv_detail_content_update_proc);
  layer_add_child(&window->layer, scroll_layer_get_layer(scroll_layer));

  data->detail = detail;
  app_window_stack_push(window, true /* animated */);
}

// List
////////

static void prv_status_row_text(SettingsUpdatesData *data, const char **title) {
  const DrewUpdatesInfo *info = &data->info;
  data->subtitle[0] = '\0';
  if (info->last_checked_utc) {
    if (rtc_get_time() - (time_t)info->last_checked_utc < 60) {
      strncpy(data->subtitle, i18n_get("Checked just now", data), sizeof(data->subtitle) - 1);
    } else {
      char since[32];
      clock_get_since_time(since, sizeof(since), info->last_checked_utc);
      snprintf(data->subtitle, sizeof(data->subtitle), i18n_get("Checked %s", data), since);
    }
  }

  if (!data->supported) {
    if (data->connected) {
      *title = i18n_get("Not available", data);
      strncpy(data->subtitle, i18n_get("Needs DrewMobileApp", data), sizeof(data->subtitle) - 1);
    } else {
      *title = i18n_get("Phone not connected", data);
    }
  } else if (info->checking) {
    *title = i18n_get("Checking...", data);
  } else if (info->timed_out) {
    *title = i18n_get("No answer from phone", data);
  } else if (!info->has_list) {
    *title = i18n_get("Check for updates", data);
  } else if (info->status == DrewUpdatesListCheckFailed) {
    *title = i18n_get("Check failed", data);
    if (info->message[0]) {
      strncpy(data->subtitle, info->message, sizeof(data->subtitle) - 1);
    }
  } else if (info->num_entries == 0) {
    *title = i18n_get("Up to date", data);
  } else if (info->num_entries == 1) {
    *title = i18n_get("1 update", data);
  } else {
    snprintf(data->title, sizeof(data->title), i18n_get("%u updates", data), info->num_entries);
    *title = data->title;
  }
}

static uint16_t prv_num_rows_cb(SettingsCallbacks *context) {
  SettingsUpdatesData *data = (SettingsUpdatesData *)context;
  return 1 + data->info.num_entries;
}

static void prv_draw_row_cb(SettingsCallbacks *context, GContext *ctx, const Layer *cell_layer,
                            uint16_t row, bool selected) {
  SettingsUpdatesData *data = (SettingsUpdatesData *)context;
  if (row == STATUS_ROW) {
    const char *title = "";
    prv_status_row_text(data, &title);
    menu_cell_basic_draw(ctx, cell_layer, title, data->subtitle[0] ? data->subtitle : nullptr,
                         nullptr);
    return;
  }

  DrewUpdateEntry entry;
  if (!drew_updates_get_entry(row - 1, &entry)) {
    return;
  }
  const char *subtitle =
      prv_install_text(data, &entry, data->entry_subtitle, sizeof(data->entry_subtitle));
  menu_cell_basic_draw(ctx, cell_layer, entry.name, subtitle ? subtitle : entry.new_version,
                       nullptr);
}

static void prv_select_click_cb(SettingsCallbacks *context, uint16_t row) {
  SettingsUpdatesData *data = (SettingsUpdatesData *)context;
  if (row == STATUS_ROW) {
    if (data->supported && !data->info.checking) {
      data->requested = drew_updates_request_list(true /* recheck */);
    }
    return;
  }
  prv_push_detail(data, row - 1);
}

static void prv_event_handler(PebbleEvent *event, void *context) {
  SettingsUpdatesData *data = context;
  prv_refresh_state(data);
  prv_request_list_if_needed(data);
  settings_menu_reload_data(SettingsMenuItemUpdates);
  if (data->detail) {
    prv_detail_refresh(data->detail);
  }
}

static void prv_expand_cb(SettingsCallbacks *context) {
  SettingsUpdatesData *data = (SettingsUpdatesData *)context;
  data->updates_event_info = (EventServiceInfo){
    .type = PEBBLE_DREW_UPDATES_EVENT,
    .handler = prv_event_handler,
    .context = data,
  };
  data->session_event_info = (EventServiceInfo){
    .type = PEBBLE_COMM_SESSION_EVENT,
    .handler = prv_event_handler,
    .context = data,
  };
  data->capabilities_event_info = (EventServiceInfo){
    .type = PEBBLE_CAPABILITIES_CHANGED_EVENT,
    .handler = prv_event_handler,
    .context = data,
  };
  event_service_client_subscribe(&data->updates_event_info);
  event_service_client_subscribe(&data->session_event_info);
  event_service_client_subscribe(&data->capabilities_event_info);

  // Re-check every time the page opens; the cached list shows meanwhile.
  prv_request_list_if_needed(data);
  prv_refresh_state(data);
  settings_menu_reload_data(SettingsMenuItemUpdates);
}

static void prv_hide_cb(SettingsCallbacks *context) {
  SettingsUpdatesData *data = (SettingsUpdatesData *)context;
  event_service_client_unsubscribe(&data->updates_event_info);
  event_service_client_unsubscribe(&data->session_event_info);
  event_service_client_unsubscribe(&data->capabilities_event_info);
}

static void prv_deinit_cb(SettingsCallbacks *context) {
  SettingsUpdatesData *data = (SettingsUpdatesData *)context;
  i18n_free_all(data);
  app_idle_timeout_set_duration(APP_IDLE_TIMEOUT_MENU_MS);
  app_free(data);
}

static Window *prv_init(void) {
  SettingsUpdatesData *data = app_zalloc_check(sizeof(SettingsUpdatesData));
  data->callbacks = (SettingsCallbacks){
    .deinit = prv_deinit_cb,
    .draw_row = prv_draw_row_cb,
    .select_click = prv_select_click_cb,
    .num_rows = prv_num_rows_cb,
    .expand = prv_expand_cb,
    .hide = prv_hide_cb,
  };
  prv_refresh_state(data);
  if (data->install.valid && prv_install_finished(&data->install)) {
    data->finished_at_open = data->install;
  }

  // Checks and installs take a while; don't close Settings under them.
  app_idle_timeout_set_duration(APP_IDLE_TIMEOUT_BLUETOOTH_MS);

  return settings_window_create(SettingsMenuItemUpdates, &data->callbacks);
}

const SettingsModuleMetadata *settings_updates_get_info(void) {
  static const SettingsModuleMetadata s_module_info = {
    .name = i18n_noop("Updates"),
    .init = prv_init,
  };

  return &s_module_info;
}

#endif // CONFIG_SERVICE_DREW_UPDATES

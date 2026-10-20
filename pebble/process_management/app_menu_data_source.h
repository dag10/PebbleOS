/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

//!@file app_menu_data_source.h
//!
//! This file provides a utility for populating a MenuLayer with the apps that are currently
//! installed. This should only be used by system apps such as the Launcher or the Watchface
//! Selector apps, as it integrates tightly with app_install_manager.

#include <pbl/kernel/compiler.h>
#include <pbl/services/process_management/app_order_storage.h>
#include <pbl/services/process_management/app_usage_storage.h>
#include <pbl/services/process_management/drew_app_recommend.h>
#include <pbl/util/list.h>

#include <applib/ui/kino/kino_reel.h>
#include <applib/ui/menu_layer.h>
#include <process_management/app_install_manager.h>
#include <process_management/pebble_process_info.h>
#include <process_management/pebble_process_md.h>

struct AppMenuDataSource;

//! This enum provides special cases of app storage order and helps calculate the fixed offset at
//! which the general storage order should begin
typedef enum AppMenuStorageOrder {
  AppMenuStorageOrder_NoOrder = 0,
  AppMenuStorageOrder_SettingsDefaultOrder,

  AppMenuStorageOrderGeneralOrderOffset
} AppMenuStorageOrder;

//! Order of the apps in the list
typedef enum AppMenuSortMode {
  //! Override apps, then the order stored by the phone, then system apps, then install order
  AppMenuSortMode_Default = 0,
  //! Most recently used first, then never used apps in the default order
  AppMenuSortMode_RecentlyUsed,
  //! By name, ignoring ASCII case
  AppMenuSortMode_Alphabetical,

  AppMenuSortModeCount
} AppMenuSortMode;

typedef struct PBL_PACKED AppMenuNode {
  ListNode node;
  AppInstallId install_id;
  ResAppNum app_num;
  uint32_t icon_resource_id;
  GBitmap *icon;
  Uuid uuid;
  GColor color;
  char *name;
  ProcessVisibility visibility;
  Version sdk_version;
  unsigned int storage_order; //!< See \ref AppMenuStorageOrder for special values of this field
  unsigned int record_order;  //!< 0 means not in the app registry
  uint32_t last_used;         //!< UTC seconds, 0 if never used or not loaded
} AppMenuNode;

// Clang makes the size of enums like AppMenuStorageOrder larger than the minimum size needed to
// represent all of the enum's values, so this assert doesn't make since when compiling using Clang
#if !__clang__
static_assert(
    sizeof(((AppMenuNode *)0)->storage_order) >=
        sizeof(((AppMenuOrderStorage *)0)->list_length) + sizeof(AppMenuStorageOrder),
    "The size of AppMenuNode.storage_order must be at least as large as the combined size of "
    "AppMenuOrderStorage.list_length and the AppMenuStorageOrder enum since the enum provides "
    "additional values for AppMenuNode.storage_order.");
#endif

typedef bool (*AppMenuFilterCallback)(struct AppMenuDataSource *source, AppInstallEntry *entry);

typedef void (*AppMenuDataSourceFunc)(void *context);

typedef uint16_t (*AppMenuDataSourceIndexTransform)(struct AppMenuDataSource *source,
                                                    uint16_t index, void *context);

typedef struct AppMenuDataSourceCallbacks {
  AppMenuFilterCallback filter;
  //! Which apps can be recommended (see app_menu_data_source_set_recommended()); `filter` if NULL
  AppMenuFilterCallback recommended_filter;
  AppMenuDataSourceFunc changed;
  AppMenuDataSourceIndexTransform transform_index;
} AppMenuDataSourceCallbacks;

typedef struct AppMenuDataSource {
  AppMenuNode *list;
  AppMenuOrderStorage *order_storage;
  AppUsageList *usage;
  AppMenuSortMode sort_mode;
  AppInstallCallbackNode app_install_callback_node;
  AppMenuDataSourceCallbacks callbacks;
  void *callback_context;
  GBitmap *default_icon;
  bool show_icons;
  bool is_list_loaded;
  //! Recommended apps as set, which may include apps that aren't installed
  uint8_t num_recommended_uuids;
  Uuid recommended_uuids[DREW_APP_RECOMMEND_MAX];
  //! Nodes of the installed recommended apps, in the recommended order. Not in the list.
  uint8_t num_recommended;
  AppMenuNode *recommended[DREW_APP_RECOMMEND_MAX];
} AppMenuDataSource;

//! Initialize the AppMenuDataSource
void app_menu_data_source_init(AppMenuDataSource *source,
                               const AppMenuDataSourceCallbacks *handlers, void *callback_context);

//! Deinitialize the AppMenuDataSource
void app_menu_data_source_deinit(AppMenuDataSource *source);

//! Sets the order of the apps, re-sorting a list that is already loaded.
void app_menu_data_source_set_sort_mode(AppMenuDataSource *source, AppMenuSortMode sort_mode);

//! Sets the recommended apps, which a client can list apart from (and in addition to) the regular
//! list. Apps that aren't installed, that the `recommended_filter` callback (or `filter` without
//! one) rejects, and repeats are skipped. Recommended apps can be ones the regular list hides.
//! @param uuids The apps, in order; at most DREW_APP_RECOMMEND_MAX are used
void app_menu_data_source_set_recommended(AppMenuDataSource *source, const Uuid *uuids,
                                          uint8_t count);

//! Returns how many recommended apps there are, after skipping.
uint16_t app_menu_data_source_get_recommended_count(AppMenuDataSource *source);

//! Returns the node of the recommended app at the given index, or NULL past the end. It is a node
//! of its own, apart from the regular list's.
AppMenuNode *app_menu_data_source_get_recommended_node_at_index(AppMenuDataSource *source,
                                                                uint16_t index);

//! Will load the icons for each `AppMenuNode`. Will automatically be unloaded when
//! `app_menu_data_source_deinit` is called.
//! @param source The AppMenuDataSource to enable icons for
//! @param fallback_icon_id The fallback resource id that should be used if the entry does not have
//! an icon.
void app_menu_data_source_enable_icons(AppMenuDataSource *source, uint32_t fallback_icon_id);

//! Returns the AppMenuNode at the given index.
AppMenuNode *app_menu_data_source_get_node_at_index(AppMenuDataSource *source, uint16_t row_index);

//! Returns the AppMenuNode with the given AppInstallId.
uint16_t app_menu_data_source_get_index_of_app_with_install_id(AppMenuDataSource *source,
                                                               AppInstallId install_id);

//! Returns the count of AppMenuNode's in the source.
uint16_t app_menu_data_source_get_count(AppMenuDataSource *source);

//! Calls `menu_cell_basic_draw` for the AppMenuNode at the given MenuIndex
//! @note Will only draw an icon if `show_icons` is set to True.
void app_menu_data_source_draw_row(AppMenuDataSource *source, GContext *ctx, Layer *cell_layer,
                                   MenuIndex *cell_index);

//! Returns the allocated `GBitmap *` for the given node. Will return NULL if no icon is loaded.
//! @note Will only return an icon if `show_icons` is set to True.
GBitmap *app_menu_data_source_get_node_icon(AppMenuDataSource *source, AppMenuNode *node);

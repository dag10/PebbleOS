/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "app_launch.h"

#include <pbl/logging/logging.h>
#include <pbl/services/comm_session/session.h>

#include <comm/ble/gatt_client_operations.h>
#include <system/passert.h>

//! See https://pebbletechnology.atlassian.net/wiki/display/DEV/Pebble+GATT+Services

// -------------------------------------------------------------------------------------------------
// Static variables

static pbl_bt_characteristic_t s_app_launch_characteristic = PBL_BT_CHARACTERISTIC_INVALID;

// -------------------------------------------------------------------------------------------------

void app_launch_handle_service_discovered(pbl_bt_characteristic_t *characteristics) {
  PBL_ASSERTN(characteristics);

  if (s_app_launch_characteristic != PBL_BT_CHARACTERISTIC_INVALID) {
    PBL_LOG_WRN("Multiple app launch services!? Will use most recent one.");
  }

  s_app_launch_characteristic = *characteristics;

  // If there was no system session, try launching the Pebble app:
  if (!comm_session_get_system_session()) {
    app_launch_trigger();
  }
}

void app_launch_invalidate_all_references(void) {
  s_app_launch_characteristic = PBL_BT_CHARACTERISTIC_INVALID;
}

void app_launch_handle_service_removed(pbl_bt_characteristic_t *characteristics,
                                       uint8_t num_characteristics) {
  app_launch_invalidate_all_references();
}

// -------------------------------------------------------------------------------------------------

bool app_launch_can_handle_characteristic(pbl_bt_characteristic_t characteristic) {
  return (characteristic == s_app_launch_characteristic);
}

// -------------------------------------------------------------------------------------------------

void app_launch_handle_disconnection(void) {
  s_app_launch_characteristic = PBL_BT_CHARACTERISTIC_INVALID;
}

// -------------------------------------------------------------------------------------------------

void app_launch_trigger(void) {
  if (s_app_launch_characteristic == PBL_BT_CHARACTERISTIC_INVALID) {
    return;
  }
  enum pbl_bt_errno err = gatt_client_op_read(s_app_launch_characteristic, GAPLEClientKernel);
  if (err != PBL_BT_ERRNO_OK) {
    PBL_LOG_ERR("App relaunch failed: %u", err);
  }
}

// -------------------------------------------------------------------------------------------------
//! The read is only there to make iOS wake or relaunch the phone app; its value is unused.
void app_launch_handle_read_response(pbl_bt_characteristic_t characteristic, const uint8_t *value,
                                     size_t value_length, enum pbl_bt_gatt_error error) {
  PBL_LOG_DBG("App launch read done: err=%u", error);
}

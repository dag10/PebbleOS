/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pbl/bluetooth/types.h>
#include <pbl/services/bluetooth/bluetooth_persistent_storage.h>

/**
 * @defgroup services_drew_protocol Drew protocol
 * @ingroup services
 * @brief DrewPebbleOS: the private endpoint DrewMobileApp and the watch talk over.
 *
 * Every message on endpoint 0xde01 starts with a 7-byte header: the magic "DREW", the frame
 * version, a service and a command (bit 7 set when the phone sent it). Both sides announce the
 * Drew features they implement in a hello; a feature is active when both announce it. None of
 * this uses upstream's capability bits, system messages or attribute ids.
 * @{
 */

#define DREW_PROTOCOL_ENDPOINT_ID (0xde01)

#define DREW_PROTOCOL_MAGIC         "DREW"
#define DREW_PROTOCOL_MAGIC_LENGTH  (4)
#define DREW_PROTOCOL_FRAME_VERSION (0x01)
#define DREW_PROTOCOL_HEADER_LENGTH (7)
//! The largest message the watch sends, header included.
#define DREW_PROTOCOL_MAX_SEND_LENGTH (1024)
//! Set in a command sent by the phone, clear in one sent by the watch.
#define DREW_PROTOCOL_COMMAND_FROM_PHONE (0x80)

typedef enum {
  DrewServiceCore = 0x00,
  DrewServiceUpdates = 0x01,
  DrewServiceFindPhone = 0x02,
} DrewService;

typedef enum {
  DrewCoreCommandWatchHello = 0x01,
  DrewCoreCommandHeartbeat = 0x02,
  DrewCoreCommandPhoneHello = 0x81,
  DrewCoreCommandHeartbeatAck = 0x82,
} DrewCoreCommand;

//! Bit numbers in the feature sets of both hellos. Never reuse a retired bit.
typedef enum {
  DrewFeatureUpdates = 0,
  DrewFeatureHeartbeat = 1,
  DrewFeatureFindPhone = 2,
  DrewFeatureNotificationFlags = 3,
  DrewFeaturePrefs = 4,
} DrewFeature;

#define DREW_FEATURE_BIT(feature) ((uint64_t)1 << (feature))

void drew_protocol_init(void);

//! The features this firmware announces in its hello.
uint64_t drew_protocol_local_features(void);

//! The features the phone announced, cached across sessions; 0 unless they came from the active
//! phone.
uint64_t drew_protocol_phone_features(void);

//! Both this firmware and the phone announce @p feature.
bool drew_protocol_has_feature(DrewFeature feature);

//! Frames and sends on the system session from KernelBG; false if there is none.
bool drew_protocol_send(DrewService service, uint8_t command, const uint8_t *payload,
                        size_t length);

//! Forgets the cached phone features when their phone's bonding is deleted.
void drew_protocol_handle_bonding_change(pbl_bt_bonding_id_t bonding, BtPersistBondingOp op);

/** @} */

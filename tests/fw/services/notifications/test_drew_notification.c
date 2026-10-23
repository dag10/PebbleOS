/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/notifications/drew_notification.h>
#include <pbl/services/timeline/attribute.h>

#include <clar.h>
#include <resource/timeline_resource_ids.auto.h>

// Stubs
////////////////////////////////////
#include <fake_rtc.h>
#include <stubs_fonts.h>
#include <stubs_i18n.h>
#include <stubs_layout_layer.h>
#include <stubs_logging.h>
#include <stubs_passert.h>
#include <stubs_pbl_malloc.h>
#include <stubs_pebble_tasks.h>
#include <stubs_rand_ptr.h>

static TimelineItem *s_stored;

bool notification_storage_get(const Uuid *id, TimelineItem *item_out) {
  if (!s_stored || !uuid_equal(id, &s_stored->header.id)) {
    return false;
  }
  TimelineItem *copy = timeline_item_copy(s_stored);
  *item_out = *copy;
  task_free(copy);
  return true;
}

static TimelineItem *prv_create_item(uint8_t flags) {
  AttributeList attr_list = {};
  attribute_list_add_cstring(&attr_list, AttributeIdTitle, "Secret title");
  attribute_list_add_cstring(&attr_list, AttributeIdSubtitle, "Secret subtitle");
  attribute_list_add_cstring(&attr_list, AttributeIdBody, "Secret body");
  attribute_list_add_cstring(&attr_list, AttributeIdSender, "Secret sender");
  attribute_list_add_cstring(&attr_list, AttributeIdAppName, "Secret app");
  attribute_list_add_resource_id(&attr_list, AttributeIdIconTiny, TIMELINE_RESOURCE_GENERIC_EMAIL);
  attribute_list_add_uint8(&attr_list, AttributeIdBgColor, 0xc3);
  attribute_list_add_uint8(&attr_list, AttributeIdImageAspectRatio, 16);
  attribute_list_add_uint32(&attr_list, AttributeIdLastUpdated, 1234);
  if (flags) {
    attribute_list_add_uint8(&attr_list, AttributeIdDrewNotificationFlags, flags);
  }

  AttributeList dismiss_attrs = {};
  attribute_list_add_cstring(&dismiss_attrs, AttributeIdTitle, "Dismiss");
  TimelineItemActionGroup action_group = {
    .num_actions = 1,
    .actions = (TimelineItemAction[]){
      {
        .id = 0,
        .type = TimelineItemActionTypeDismiss,
        .attr_list = dismiss_attrs,
      },
    },
  };

  TimelineItem *item = timeline_item_create_with_attributes(
      1000, 0, TimelineItemTypeNotification, LayoutIdNotification, &attr_list, &action_group);
  attribute_list_destroy_list(&attr_list);
  attribute_list_destroy_list(&dismiss_attrs);
  return item;
}

void test_drew_notification__initialize(void) {
  s_stored = nullptr;
}

void test_drew_notification__cleanup(void) {
  timeline_item_destroy(s_stored);
  s_stored = nullptr;
}

void test_drew_notification__get_flags(void) {
  cl_assert_equal_i(drew_notification_get_flags(nullptr), 0);

  TimelineItem *plain = prv_create_item(0);
  cl_assert_equal_i(drew_notification_get_flags(plain), 0);
  timeline_item_destroy(plain);

  TimelineItem *flagged =
      prv_create_item(DrewNotificationFlag_Critical | DrewNotificationFlag_BreakQuietTime);
  cl_assert_equal_i(drew_notification_get_flags(flagged),
                    DrewNotificationFlag_Critical | DrewNotificationFlag_BreakQuietTime);
  timeline_item_destroy(flagged);
}

void test_drew_notification__get_stored_flags(void) {
  s_stored = prv_create_item(DrewNotificationFlag_Discreet);
  cl_assert_equal_i(drew_notification_get_stored_flags(&s_stored->header.id),
                    DrewNotificationFlag_Discreet);

  const Uuid other = UUID_SYSTEM;
  cl_assert_equal_i(drew_notification_get_stored_flags(&other), 0);
}

void test_drew_notification__hide_contents(void) {
  const uint8_t flags = DrewNotificationFlag_Discreet | DrewNotificationFlag_Critical;
  TimelineItem *item = prv_create_item(flags);
  const CommonTimelineItemHeader header = item->header;

  cl_assert(drew_notification_hide_contents(item));

  cl_assert_equal_m(&item->header, &header, sizeof(header));
  const AttributeList *attrs = &item->attr_list;
  cl_assert_equal_s(attribute_get_string(attrs, AttributeIdTitle, ""), "Notification");
  cl_assert_equal_s(attribute_get_string(attrs, AttributeIdBody, ""), "Press Select to view");
  cl_assert_equal_i(attribute_get_uint32(attrs, AttributeIdIconTiny, 0),
                    TIMELINE_RESOURCE_NOTIFICATION_GENERIC);
  cl_assert(!attribute_find(attrs, AttributeIdSubtitle));
  cl_assert(!attribute_find(attrs, AttributeIdSender));
  cl_assert(!attribute_find(attrs, AttributeIdAppName));
  cl_assert(!attribute_find(attrs, AttributeIdBgColor));
  cl_assert(!attribute_find(attrs, AttributeIdImageAspectRatio));
  cl_assert_equal_i(attribute_get_uint32(attrs, AttributeIdLastUpdated, 0), 1234);
  cl_assert_equal_i(drew_notification_get_flags(item), flags);

  cl_assert_equal_i(item->action_group.num_actions, 1);
  cl_assert_equal_i(item->action_group.actions[0].type, TimelineItemActionTypeDismiss);
  cl_assert_equal_s(
      attribute_get_string(&item->action_group.actions[0].attr_list, AttributeIdTitle, ""),
      "Dismiss");

  timeline_item_destroy(item);
}

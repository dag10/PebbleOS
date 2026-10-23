/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/services/i18n/i18n.h>
#include <pbl/services/notifications/drew_notification.h>
#include <pbl/services/notifications/notification_storage.h>
#include <pbl/services/timeline/attribute.h>
#include <pbl/services/timeline/attributes_actions.h>

#include <kernel/pbl_malloc.h>
#include <resource/timeline_resource_ids.auto.h>

// Attributes that would show what a discreet notification is about.
static bool prv_is_content_attribute(AttributeId id) {
  switch (id) {
    case AttributeIdTitle:
    case AttributeIdSubtitle:
    case AttributeIdBody:
    case AttributeIdIconTiny:
    case AttributeIdIconSmall:
    case AttributeIdIconLarge:
    case AttributeIdShortTitle:
    case AttributeIdIconPin:
    case AttributeIdLocationName:
    case AttributeIdSender:
    case AttributeIdHeadings:
    case AttributeIdParagraphs:
    case AttributeIdPrimaryColor:
    case AttributeIdBgColor:
    case AttributeIdSecondaryColor:
    case AttributeIdAppName:
    case AttributeIdShortSubtitle:
    case AttributeIdAddress:
    case AttributeIdSubtitleTemplateString:
    case AttributeIdIcon:
    case AttributeIdImageAspectRatio:
      return true;
    default:
      return false;
  }
}

uint8_t drew_notification_get_flags(const TimelineItem *item) {
  if (!item) {
    return 0;
  }
  return attribute_get_uint8(&item->attr_list, AttributeIdDrewNotificationFlags, 0);
}

uint8_t drew_notification_get_stored_flags(const Uuid *id) {
  TimelineItem item;
  if (!notification_storage_get(id, &item)) {
    return 0;
  }
  const uint8_t flags = drew_notification_get_flags(&item);
  timeline_item_free_allocated_buffer(&item);
  return flags;
}

const char *drew_notification_get_placeholder_title(const void *owner) {
  return i18n_get("Notification", owner);
}

bool drew_notification_hide_contents(TimelineItem *item) {
  const AttributeList *src = &item->attr_list;
  const uint8_t num_placeholders = 3;
  Attribute *attributes = task_zalloc(sizeof(Attribute) * (src->num_attributes + num_placeholders));
  if (!attributes) {
    return false;
  }

  uint8_t num_attributes = 0;
  for (uint8_t i = 0; i < src->num_attributes; i++) {
    if (!prv_is_content_attribute(src->attributes[i].id)) {
      attributes[num_attributes++] = src->attributes[i];
    }
  }
  attributes[num_attributes++] = (Attribute){
    .id = AttributeIdTitle,
    .cstring = (char *)drew_notification_get_placeholder_title(attributes),
  };
  attributes[num_attributes++] = (Attribute){
    .id = AttributeIdBody,
    .cstring = (char *)i18n_get("Press Select to view", attributes),
  };
  attributes[num_attributes++] = (Attribute){
    .id = AttributeIdIconTiny,
    .uint32 = TIMELINE_RESOURCE_NOTIFICATION_GENERIC,
  };

  AttributeList placeholder_list = {
    .num_attributes = num_attributes,
    .attributes = attributes,
  };
  AttributeList attr_list;
  TimelineItemActionGroup action_group;
  const size_t size = attributes_actions_get_buffer_size(&placeholder_list, &item->action_group);
  uint8_t *buffer = task_malloc(size);
  const bool copied =
      buffer && attributes_actions_deep_copy(&placeholder_list, &attr_list, &item->action_group,
                                             &action_group, buffer, buffer + size);
  i18n_free_all(attributes);
  task_free(attributes);
  if (!copied) {
    task_free(buffer);
    return false;
  }

  timeline_item_free_allocated_buffer(item);
  item->allocated_buffer = buffer;
  item->attr_list = attr_list;
  item->action_group = action_group;
  return true;
}

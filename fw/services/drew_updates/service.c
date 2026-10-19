/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <pbl/kernel/mutex.h>
#include <pbl/logging/logging.h>
#include <pbl/services/comm_session/session.h>
#include <pbl/services/drew_updates.h>
#include <pbl/services/new_timer/new_timer.h>
#include <pbl/services/system_task.h>

#include <kernel/events.h>
#include <kernel/pbl_malloc.h>

PBL_LOG_MODULE_DEFINE(service_drew_updates, CONFIG_SERVICE_DREW_UPDATES_LOG_LEVEL);

//! @file
//! Settings > Updates protocol (docs/drew-os-extensions.md in pebble-workspace). Little-endian;
//! strings are a length (1 byte, 2 for the changelog) then UTF-8 without a NUL.

typedef enum {
  CmdListRequest = 0x01,
  CmdInstallRequest = 0x02,
  CmdListHeader = 0x81,
  CmdListEntry = 0x82,
  CmdInstallStatus = 0x83,
} DrewUpdatesCommand;

#define LIST_REQUEST_FLAG_RECHECK (1 << 0)

typedef struct StoredEntry {
  DrewUpdateEntry entry;
  char changelog[];
} StoredEntry;

typedef struct List {
  DrewUpdatesListStatus status;
  uint32_t last_checked_utc;
  char message[DREW_UPDATES_MESSAGE_LENGTH + 1];
  uint8_t num_entries;
  StoredEntry *entries[DREW_UPDATES_MAX_ENTRIES];
} List;

typedef struct Outbound {
  size_t length;
  uint8_t data[];
} Outbound;

static PBL_MUTEX_DEFINE(s_mutex);
static TimerID s_check_timer = TIMER_INVALID_ID;
static TimerID s_install_timer = TIMER_INVALID_ID;

static bool s_has_list;
static List s_list;
static bool s_checking;
static bool s_timed_out;

//! The list being received: its header arrived, not all of its entries yet.
static bool s_has_pending;
static uint16_t s_pending_id;
static uint8_t s_pending_count;
static uint8_t s_pending_received;
static List s_pending;

static DrewUpdateInstallStatus s_install;

// Reading
///////////

typedef struct Reader {
  const uint8_t *data;
  size_t length;
  bool ok;
} Reader;

static const uint8_t *prv_take(Reader *r, size_t n) {
  if (!r->ok || r->length < n) {
    r->ok = false;
    return nullptr;
  }
  const uint8_t *p = r->data;
  r->data += n;
  r->length -= n;
  return p;
}

static uint8_t prv_u8(Reader *r) {
  const uint8_t *p = prv_take(r, 1);
  return p ? p[0] : 0;
}

static uint16_t prv_u16(Reader *r) {
  const uint8_t *p = prv_take(r, 2);
  return p ? (uint16_t)(p[0] | (p[1] << 8)) : 0;
}

static uint32_t prv_u32(Reader *r) {
  const uint8_t *p = prv_take(r, 4);
  return p ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
              ((uint32_t)p[3] << 24))
           : 0;
}

//! Copies at most @p capacity bytes, cut between UTF-8 code points, and a NUL.
static void prv_copy_string(char *out, size_t capacity, const uint8_t *src, size_t length) {
  if (length > capacity) {
    length = capacity;
    while (length > 0 && (src[length] & 0xC0) == 0x80) {
      length--;
    }
  }
  memcpy(out, src, length);
  out[length] = '\0';
}

static void prv_string(Reader *r, size_t length_bytes, char *out, size_t capacity) {
  const size_t length = (length_bytes == 1) ? prv_u8(r) : prv_u16(r);
  const uint8_t *p = prv_take(r, length);
  if (p) {
    prv_copy_string(out, capacity, p, length);
  } else {
    out[0] = '\0';
  }
}

// State
/////////

static void prv_free_entries(List *list) {
  for (int i = 0; i < DREW_UPDATES_MAX_ENTRIES; i++) {
    kernel_free(list->entries[i]);
    list->entries[i] = nullptr;
  }
  list->num_entries = 0;
}

static void prv_post_event(void) {
  PebbleEvent e = {
    .type = PEBBLE_DREW_UPDATES_EVENT,
  };
  event_put(&e);
}

static void prv_check_timeout_cb(void *unused) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const bool was_checking = s_checking;
  s_checking = false;
  s_timed_out = was_checking;
  pbl_mutex_unlock(&s_mutex);
  if (was_checking) {
    PBL_LOG_DBG("No fresh list from the phone");
    prv_post_event();
  }
}

static void prv_install_timeout_cb(void *unused) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const bool waiting = s_install.valid && s_install.state != DrewUpdateInstallDone &&
                       s_install.state != DrewUpdateInstallFailed &&
                       s_install.state != DrewUpdateInstallRebooting;
  if (waiting) {
    s_install.state = DrewUpdateInstallFailed;
    s_install.percent = DREW_UPDATES_PERCENT_UNKNOWN;
    strncpy(s_install.message, "No answer from the phone", DREW_UPDATES_MESSAGE_LENGTH);
  }
  pbl_mutex_unlock(&s_mutex);
  if (waiting) {
    prv_post_event();
  }
}

//! Call with the mutex held.
static void prv_start_checking(void) {
  s_checking = true;
  s_timed_out = false;
  new_timer_start(s_check_timer, DREW_UPDATES_CHECK_TIMEOUT_MS, prv_check_timeout_cb, nullptr,
                  0 /* flags */);
}

//! Call with the mutex held.
static void prv_commit_pending(void) {
  prv_free_entries(&s_list);
  s_list = s_pending;
  s_list.num_entries = s_pending_count;
  s_pending = (List){};
  s_has_pending = false;
  s_has_list = true;
  if (s_list.status == DrewUpdatesListChecking) {
    prv_start_checking();
  } else {
    s_checking = false;
    s_timed_out = false;
    new_timer_stop(s_check_timer);
  }
}

// Inbound
///////////

static void prv_handle_header(Reader *r) {
  const uint16_t list_id = prv_u16(r);
  const uint8_t status = prv_u8(r);
  const uint8_t count = prv_u8(r);
  const uint32_t last_checked = prv_u32(r);
  char message[DREW_UPDATES_MESSAGE_LENGTH + 1];
  prv_string(r, 1, message, DREW_UPDATES_MESSAGE_LENGTH);
  if (!r->ok || status > DrewUpdatesListCheckFailed) {
    PBL_LOG_WRN("Bad list header");
    return;
  }
  PBL_LOG_DBG("List %u: status %u, %u entries", list_id, status, count);

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  prv_free_entries(&s_pending);
  s_pending = (List){
    .status = (DrewUpdatesListStatus)status,
    .last_checked_utc = last_checked,
  };
  strcpy(s_pending.message, message);
  s_has_pending = true;
  s_pending_id = list_id;
  s_pending_count = (count < DREW_UPDATES_MAX_ENTRIES) ? count : DREW_UPDATES_MAX_ENTRIES;
  s_pending_received = 0;
  const bool complete = (s_pending_count == 0);
  if (complete) {
    prv_commit_pending();
  }
  pbl_mutex_unlock(&s_mutex);

  if (complete) {
    prv_post_event();
  }
}

static void prv_handle_entry(Reader *r) {
  const uint16_t list_id = prv_u16(r);
  const uint8_t index = prv_u8(r);
  const uint8_t kind = prv_u8(r);
  const uint8_t flags = prv_u8(r);
  const uint32_t size_bytes = prv_u32(r);
  DrewUpdateEntry entry = {
    .kind = (DrewUpdateKind)kind,
    .flags = flags,
    .size_bytes = size_bytes,
  };
  prv_string(r, 1, entry.id, DREW_UPDATES_ID_LENGTH);
  prv_string(r, 1, entry.name, DREW_UPDATES_NAME_LENGTH);
  prv_string(r, 1, entry.current_version, DREW_UPDATES_VERSION_LENGTH);
  prv_string(r, 1, entry.new_version, DREW_UPDATES_VERSION_LENGTH);
  const size_t changelog_length = prv_u16(r);
  const uint8_t *changelog = prv_take(r, changelog_length);
  if (!r->ok) {
    PBL_LOG_WRN("Bad list entry");
    return;
  }

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  bool complete = false;
  if (s_has_pending && list_id == s_pending_id && index < s_pending_count &&
      !s_pending.entries[index]) {
    const size_t stored_length = (changelog_length < DREW_UPDATES_CHANGELOG_LENGTH)
                                     ? changelog_length
                                     : DREW_UPDATES_CHANGELOG_LENGTH;
    StoredEntry *stored = kernel_zalloc(sizeof(StoredEntry) + stored_length + 1);
    if (stored) {
      stored->entry = entry;
      prv_copy_string(stored->changelog, stored_length, changelog, changelog_length);
      s_pending.entries[index] = stored;
      s_pending_received++;
      complete = (s_pending_received == s_pending_count);
      if (complete) {
        prv_commit_pending();
      }
    }
  }
  pbl_mutex_unlock(&s_mutex);

  if (complete) {
    prv_post_event();
  }
}

static void prv_handle_install_status(Reader *r) {
  DrewUpdateInstallStatus status = {
    .valid = true,
  };
  prv_string(r, 1, status.id, DREW_UPDATES_ID_LENGTH);
  status.state = (DrewUpdateInstallState)prv_u8(r);
  status.percent = prv_u8(r);
  prv_string(r, 1, status.message, DREW_UPDATES_MESSAGE_LENGTH);
  if (!r->ok || status.state > DrewUpdateInstallRebooting) {
    PBL_LOG_WRN("Bad install status");
    return;
  }
  PBL_LOG_DBG("Install %s: state %u, %u%%", status.id, status.state, status.percent);

  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  s_install = status;
  const bool finished = (status.state == DrewUpdateInstallDone) ||
                        (status.state == DrewUpdateInstallFailed) ||
                        (status.state == DrewUpdateInstallRebooting);
  if (finished) {
    new_timer_stop(s_install_timer);
  } else {
    new_timer_start(s_install_timer, DREW_UPDATES_INSTALL_TIMEOUT_MS, prv_install_timeout_cb,
                    nullptr, 0 /* flags */);
  }
  pbl_mutex_unlock(&s_mutex);

  prv_post_event();
}

void drew_updates_protocol_msg_callback(CommSession *session, const uint8_t *data, size_t length) {
  Reader r = {
    .data = data,
    .length = length,
    .ok = true,
  };
  switch (prv_u8(&r)) {
    case CmdListHeader:
      prv_handle_header(&r);
      break;
    case CmdListEntry:
      prv_handle_entry(&r);
      break;
    case CmdInstallStatus:
      prv_handle_install_status(&r);
      break;
    default:
      PBL_LOG_WRN("Unknown message 0x%x", length ? data[0] : 0);
      break;
  }
}

// Outbound
////////////

static void prv_send_cb(void *data) {
  Outbound *message = data;
  CommSession *session = comm_session_get_system_session();
  if (session) {
    comm_session_send_data(session, DREW_UPDATES_ENDPOINT_ID, message->data, message->length,
                           COMM_SESSION_DEFAULT_TIMEOUT);
  }
  kernel_free(message);
}

static bool prv_send(const uint8_t *data, size_t length) {
  if (!drew_updates_phone_supported()) {
    return false;
  }
  Outbound *message = kernel_malloc(sizeof(Outbound) + length);
  if (!message) {
    return false;
  }
  message->length = length;
  memcpy(message->data, data, length);
  system_task_add_callback(prv_send_cb, message);
  return true;
}

bool drew_updates_phone_supported(void) {
  CommSession *session = comm_session_get_system_session();
  return session && comm_session_has_capability(session, CommSessionDrewUpdatesSupport);
}

bool drew_updates_request_list(bool recheck) {
  const uint8_t message[] = {CmdListRequest, recheck ? LIST_REQUEST_FLAG_RECHECK : 0};
  if (!prv_send(message, sizeof(message))) {
    return false;
  }
  if (recheck) {
    pbl_mutex_lock(&s_mutex, PBL_FOREVER);
    prv_start_checking();
    pbl_mutex_unlock(&s_mutex);
    prv_post_event();
  }
  return true;
}

bool drew_updates_request_install(const char *id) {
  uint8_t message[2 + DREW_UPDATES_ID_LENGTH];
  const size_t id_length = strnlen(id, DREW_UPDATES_ID_LENGTH);
  message[0] = CmdInstallRequest;
  message[1] = id_length;
  memcpy(&message[2], id, id_length);
  if (!prv_send(message, 2 + id_length)) {
    return false;
  }
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  s_install = (DrewUpdateInstallStatus){
    .valid = true,
    .state = DrewUpdateInstallStarting,
    .percent = DREW_UPDATES_PERCENT_UNKNOWN,
  };
  prv_copy_string(s_install.id, DREW_UPDATES_ID_LENGTH, (const uint8_t *)id, id_length);
  new_timer_start(s_install_timer, DREW_UPDATES_INSTALL_TIMEOUT_MS, prv_install_timeout_cb, nullptr,
                  0 /* flags */);
  pbl_mutex_unlock(&s_mutex);
  prv_post_event();
  return true;
}

// Queries
///////////

void drew_updates_get_info(DrewUpdatesInfo *out) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  *out = (DrewUpdatesInfo){
    .has_list = s_has_list,
    .status = s_list.status,
    .checking = s_checking,
    .timed_out = s_timed_out,
    .last_checked_utc = s_list.last_checked_utc,
    .num_entries = s_list.num_entries,
  };
  strcpy(out->message, s_list.message);
  pbl_mutex_unlock(&s_mutex);
}

bool drew_updates_get_entry(uint8_t index, DrewUpdateEntry *out) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  const bool found = (index < s_list.num_entries) && s_list.entries[index];
  if (found) {
    *out = s_list.entries[index]->entry;
  }
  pbl_mutex_unlock(&s_mutex);
  return found;
}

size_t drew_updates_copy_changelog(uint8_t index, char *buffer, size_t buffer_size) {
  if (buffer_size == 0) {
    return 0;
  }
  size_t length = 0;
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  if (index < s_list.num_entries && s_list.entries[index]) {
    const char *changelog = s_list.entries[index]->changelog;
    prv_copy_string(buffer, buffer_size - 1, (const uint8_t *)changelog, strlen(changelog));
    length = strlen(buffer);
  } else {
    buffer[0] = '\0';
  }
  pbl_mutex_unlock(&s_mutex);
  return length;
}

void drew_updates_get_install_status(DrewUpdateInstallStatus *out) {
  pbl_mutex_lock(&s_mutex, PBL_FOREVER);
  *out = s_install;
  pbl_mutex_unlock(&s_mutex);
}

void drew_updates_init(void) {
  s_check_timer = new_timer_create();
  s_install_timer = new_timer_create();
}

/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <pbl/services/comm_session/session.h>
#include <pbl/services/drew_updates.h>

#include <clar.h>
#include <fake_new_timer.h>
#include <fake_pbl_malloc.h>
#include <fake_system_task.h>
#include <kernel/events.h>
#include <stubs_logging.h>
#include <stubs_mutex.h>
#include <stubs_passert.h>

extern void drew_updates_protocol_msg_callback(CommSession *session, const uint8_t *data,
                                               size_t length);

static CommSession *s_session = (CommSession *)0x1;
static bool s_has_capability;
static int s_num_events;
static uint8_t s_sent[64];
static size_t s_sent_length;
static int s_num_sent;

CommSession *comm_session_get_system_session(void) {
  return s_session;
}

bool comm_session_has_capability(CommSession *session, CommSessionCapability capability) {
  return s_has_capability && capability == CommSessionDrewUpdatesSupport;
}

bool comm_session_send_data(CommSession *session, uint16_t endpoint_id, const uint8_t *data,
                            size_t length, uint32_t timeout_ms) {
  cl_assert_equal_i(endpoint_id, DREW_UPDATES_ENDPOINT_ID);
  cl_assert(length <= sizeof(s_sent));
  memcpy(s_sent, data, length);
  s_sent_length = length;
  s_num_sent++;
  return true;
}

void event_put(PebbleEvent *event) {
  cl_assert_equal_i(event->type, PEBBLE_DREW_UPDATES_EVENT);
  s_num_events++;
}

// Message building
////////////////////

typedef struct Buffer {
  uint8_t data[1024];
  size_t length;
} Buffer;

static void prv_u8(Buffer *b, uint8_t v) {
  b->data[b->length++] = v;
}

static void prv_u16(Buffer *b, uint16_t v) {
  prv_u8(b, v & 0xff);
  prv_u8(b, v >> 8);
}

static void prv_u32(Buffer *b, uint32_t v) {
  prv_u16(b, v & 0xffff);
  prv_u16(b, v >> 16);
}

static void prv_str8(Buffer *b, const char *s) {
  const size_t length = strlen(s);
  prv_u8(b, length);
  memcpy(&b->data[b->length], s, length);
  b->length += length;
}

static void prv_str16(Buffer *b, const char *s) {
  const size_t length = strlen(s);
  prv_u16(b, length);
  memcpy(&b->data[b->length], s, length);
  b->length += length;
}

static void prv_deliver(const Buffer *b) {
  drew_updates_protocol_msg_callback(s_session, b->data, b->length);
}

static void prv_header(uint16_t list_id, uint8_t status, uint8_t count, uint32_t last_checked,
                       const char *message) {
  Buffer b = {};
  prv_u8(&b, 0x81);
  prv_u16(&b, list_id);
  prv_u8(&b, status);
  prv_u8(&b, count);
  prv_u32(&b, last_checked);
  prv_str8(&b, message);
  prv_deliver(&b);
}

static void prv_entry(uint16_t list_id, uint8_t index, const char *id, const char *changelog) {
  Buffer b = {};
  prv_u8(&b, 0x82);
  prv_u16(&b, list_id);
  prv_u8(&b, index);
  prv_u8(&b, DrewUpdateKindWatchface);
  prv_u8(&b, DrewUpdateFlagInstalling);
  prv_u32(&b, 446858);
  prv_str8(&b, id);
  prv_str8(&b, "Quartz Plus");
  prv_str8(&b, "1.81");
  prv_str8(&b, "1.81.0 (e13610b)");
  prv_str16(&b, changelog);
  prv_deliver(&b);
}

static void prv_install_status(const char *id, uint8_t state, uint8_t percent,
                               const char *message) {
  Buffer b = {};
  prv_u8(&b, 0x83);
  prv_str8(&b, id);
  prv_u8(&b, state);
  prv_u8(&b, percent);
  prv_str8(&b, message);
  prv_deliver(&b);
}

static DrewUpdatesInfo prv_info(void) {
  DrewUpdatesInfo info;
  drew_updates_get_info(&info);
  return info;
}

// Tests
/////////

void test_drew_updates__initialize(void) {
  s_has_capability = true;
  s_num_events = 0;
  s_num_sent = 0;
  s_sent_length = 0;
  drew_updates_init();
}

void test_drew_updates__cleanup(void) {
  // Drop the list so each test starts empty.
  prv_header(0xffff, DrewUpdatesListReady, 0, 0, "");
  fake_system_task_callbacks_cleanup();
  stub_new_timer_cleanup();
}

void test_drew_updates__list_arrives_in_parts(void) {
  prv_header(7, DrewUpdatesListReady, 2, 1800000000, "");
  cl_assert_equal_i(s_num_events, 0);
  prv_entry(7, 1, "app:2", "second");
  prv_entry(7, 0, "fw:drew", "• first\nline");
  cl_assert_equal_i(s_num_events, 1);

  const DrewUpdatesInfo info = prv_info();
  cl_assert(info.has_list);
  cl_assert(!info.checking);
  cl_assert_equal_i(info.status, DrewUpdatesListReady);
  cl_assert_equal_i(info.num_entries, 2);
  cl_assert_equal_i(info.last_checked_utc, 1800000000);

  DrewUpdateEntry entry;
  cl_assert(drew_updates_get_entry(0, &entry));
  cl_assert_equal_s(entry.id, "fw:drew");
  cl_assert_equal_s(entry.name, "Quartz Plus");
  cl_assert_equal_s(entry.current_version, "1.81");
  cl_assert_equal_s(entry.new_version, "1.81.0 (e13610b)");
  cl_assert_equal_i(entry.kind, DrewUpdateKindWatchface);
  cl_assert_equal_i(entry.flags, DrewUpdateFlagInstalling);
  cl_assert_equal_i(entry.size_bytes, 446858);
  cl_assert(!drew_updates_get_entry(2, &entry));

  char changelog[8];
  cl_assert_equal_i(drew_updates_copy_changelog(0, changelog, sizeof(changelog)), 7);
  cl_assert_equal_s(changelog, "• fir");
  char small[4];
  // Cut between code points: "•" is three bytes.
  cl_assert_equal_i(drew_updates_copy_changelog(0, small, sizeof(small)), 3);
  cl_assert_equal_s(small, "•");
  cl_assert_equal_i(drew_updates_copy_changelog(1, small, sizeof(small)), 3);
  cl_assert_equal_s(small, "sec");
}

void test_drew_updates__older_list_entries_are_dropped(void) {
  prv_header(1, DrewUpdatesListReady, 1, 0, "");
  prv_header(2, DrewUpdatesListReady, 1, 0, "");
  prv_entry(1, 0, "stale", "");
  prv_entry(2, 5, "out of range", "");
  cl_assert(!prv_info().has_list || prv_info().num_entries == 0);
  prv_entry(2, 0, "fresh", "");
  DrewUpdateEntry entry;
  cl_assert(drew_updates_get_entry(0, &entry));
  cl_assert_equal_s(entry.id, "fresh");
}

void test_drew_updates__a_new_list_keeps_the_old_one_until_complete(void) {
  prv_header(1, DrewUpdatesListReady, 1, 0, "");
  prv_entry(1, 0, "old", "");
  prv_header(2, DrewUpdatesListReady, 1, 0, "");
  DrewUpdateEntry entry;
  cl_assert(drew_updates_get_entry(0, &entry));
  cl_assert_equal_s(entry.id, "old");
}

void test_drew_updates__empty_list_completes_at_once(void) {
  prv_header(3, DrewUpdatesListCheckFailed, 0, 5, "Couldn't check DrewPebbleOS");
  cl_assert_equal_i(s_num_events, 1);
  const DrewUpdatesInfo info = prv_info();
  cl_assert_equal_i(info.status, DrewUpdatesListCheckFailed);
  cl_assert_equal_i(info.num_entries, 0);
  cl_assert_equal_s(info.message, "Couldn't check DrewPebbleOS");
}

void test_drew_updates__too_many_entries_are_capped(void) {
  prv_header(4, DrewUpdatesListReady, DREW_UPDATES_MAX_ENTRIES + 2, 0, "");
  for (int i = 0; i < DREW_UPDATES_MAX_ENTRIES + 2; i++) {
    prv_entry(4, i, "x", "");
  }
  cl_assert_equal_i(prv_info().num_entries, DREW_UPDATES_MAX_ENTRIES);
}

void test_drew_updates__malformed_messages_are_ignored(void) {
  const uint8_t truncated[] = {0x81, 0x01};
  drew_updates_protocol_msg_callback(s_session, truncated, sizeof(truncated));
  const uint8_t unknown[] = {0x42};
  drew_updates_protocol_msg_callback(s_session, unknown, sizeof(unknown));
  cl_assert_equal_i(s_num_events, 0);
}

void test_drew_updates__request_list(void) {
  cl_assert(drew_updates_request_list(true /* recheck */));
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_num_sent, 1);
  cl_assert_equal_i(s_sent_length, 2);
  cl_assert_equal_i(s_sent[0], 0x01);
  cl_assert_equal_i(s_sent[1], 0x01);
  cl_assert(prv_info().checking);

  cl_assert(drew_updates_request_list(false /* recheck */));
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_sent[1], 0x00);
}

void test_drew_updates__nothing_is_sent_without_the_capability(void) {
  s_has_capability = false;
  cl_assert(!drew_updates_phone_supported());
  cl_assert(!drew_updates_request_list(true /* recheck */));
  cl_assert(!drew_updates_request_install("fw:drew"));
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_num_sent, 0);
  cl_assert(!prv_info().checking);
}

void test_drew_updates__checking_until_a_fresh_list_or_timeout(void) {
  prv_header(5, DrewUpdatesListChecking, 0, 0, "");
  cl_assert(prv_info().checking);
  prv_header(6, DrewUpdatesListReady, 0, 1800000000, "");
  cl_assert(!prv_info().checking);
  cl_assert(!prv_info().timed_out);

  cl_assert(drew_updates_request_list(true /* recheck */));
  cl_assert(prv_info().checking);
  const int events = s_num_events;
  stub_new_timer_fire(s_new_timer_start_param_timer_id);
  cl_assert(!prv_info().checking);
  cl_assert(prv_info().timed_out);
  cl_assert_equal_i(s_num_events, events + 1);
}

void test_drew_updates__install(void) {
  cl_assert(drew_updates_request_install("app:1"));
  fake_system_task_callbacks_invoke_pending();
  cl_assert_equal_i(s_sent_length, 7);
  cl_assert_equal_i(s_sent[0], 0x02);
  cl_assert_equal_i(s_sent[1], 5);
  cl_assert(memcmp(&s_sent[2], "app:1", 5) == 0);

  DrewUpdateInstallStatus status;
  drew_updates_get_install_status(&status);
  cl_assert(status.valid);
  cl_assert_equal_s(status.id, "app:1");
  cl_assert_equal_i(status.state, DrewUpdateInstallStarting);

  prv_install_status("app:1", DrewUpdateInstallTransferring, 40, "");
  drew_updates_get_install_status(&status);
  cl_assert_equal_i(status.state, DrewUpdateInstallTransferring);
  cl_assert_equal_i(status.percent, 40);

  prv_install_status("app:1", DrewUpdateInstallFailed, DREW_UPDATES_PERCENT_UNKNOWN,
                     "Couldn't install");
  drew_updates_get_install_status(&status);
  cl_assert_equal_i(status.state, DrewUpdateInstallFailed);
  cl_assert_equal_s(status.message, "Couldn't install");
}

void test_drew_updates__install_times_out_without_an_answer(void) {
  cl_assert(drew_updates_request_install("fw:drew"));
  stub_new_timer_fire(s_new_timer_start_param_timer_id);
  DrewUpdateInstallStatus status;
  drew_updates_get_install_status(&status);
  cl_assert_equal_i(status.state, DrewUpdateInstallFailed);
  cl_assert_equal_s(status.message, "No answer from the phone");
}

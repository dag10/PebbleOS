/* SPDX-FileCopyrightText: 2026 Drew Gottlieb */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <pbl/services/process_management/drew_app_recommend.h>

#include <clar.h>
#include <process_management/pebble_process_md.h>

// Stubs
////////////////////////////////////
#include <stubs_logging.h>
#include <stubs_mutex.h>
#include <stubs_passert.h>
#include <stubs_syscall_internal.h>

// Fakes
////////////////////////////////////
#include <fake_pebble_tasks.h>

static const Uuid s_face_a = {0xaa, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                              0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
static const Uuid s_face_b = {0xbb, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                              0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

static Uuid s_default_watchface;
static PebbleProcessMd s_current_app_md;

void watchface_get_default_uuid(Uuid *uuid_out) {
  *uuid_out = s_default_watchface;
}

const PebbleProcessMd *app_manager_get_current_app_md(void) {
  return &s_current_app_md;
}

static void prv_run_app(const Uuid *uuid, ProcessType type) {
  s_current_app_md = (PebbleProcessMd){
    .uuid = *uuid,
    .process_type = type,
  };
  stub_pebble_tasks_set_current(PebbleTask_App);
}

static void prv_set_default_watchface(const Uuid *uuid) {
  s_default_watchface = *uuid;
  drew_app_recommend_handle_default_watchface(uuid);
}

static Uuid prv_app_uuid(uint8_t n) {
  return (Uuid){0x10, n, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, n};
}

// Tests
////////////////////////////////////

void test_drew_app_recommend__initialize(void) {
  drew_app_recommend_reset_for_tests();
  prv_set_default_watchface(&s_face_a);
  prv_run_app(&s_face_a, ProcessTypeWatchface);
}

void test_drew_app_recommend__none_by_default(void) {
  Uuid out[DREW_APP_RECOMMEND_MAX];
  cl_assert_equal_i(drew_app_recommend_get(out), 0);
}

void test_drew_app_recommend__set_replaces_and_keeps_order(void) {
  const Uuid apps[] = {prv_app_uuid(1), prv_app_uuid(2), prv_app_uuid(3)};
  cl_assert(sys_app_recommend_set(apps, 3));

  Uuid out[DREW_APP_RECOMMEND_MAX];
  cl_assert_equal_i(drew_app_recommend_get(out), 3);
  for (int i = 0; i < 3; i++) {
    cl_assert(uuid_equal(&out[i], &apps[i]));
  }

  // A new list replaces the old one
  const Uuid other[] = {prv_app_uuid(9)};
  cl_assert(sys_app_recommend_set(other, 1));
  cl_assert_equal_i(drew_app_recommend_get(out), 1);
  cl_assert(uuid_equal(&out[0], &other[0]));
}

void test_drew_app_recommend__clamped_to_max(void) {
  Uuid apps[DREW_APP_RECOMMEND_MAX + 2];
  for (int i = 0; i < DREW_APP_RECOMMEND_MAX + 2; i++) {
    apps[i] = prv_app_uuid(i + 1);
  }
  cl_assert(sys_app_recommend_set(apps, DREW_APP_RECOMMEND_MAX + 2));

  Uuid out[DREW_APP_RECOMMEND_MAX];
  cl_assert_equal_i(drew_app_recommend_get(out), DREW_APP_RECOMMEND_MAX);
  for (int i = 0; i < DREW_APP_RECOMMEND_MAX; i++) {
    cl_assert(uuid_equal(&out[i], &apps[i]));
  }
}

void test_drew_app_recommend__count_zero_clears(void) {
  const Uuid apps[] = {prv_app_uuid(1), prv_app_uuid(2)};
  cl_assert(sys_app_recommend_set(apps, 2));
  cl_assert(sys_app_recommend_set(NULL, 0));

  Uuid out[DREW_APP_RECOMMEND_MAX];
  cl_assert_equal_i(drew_app_recommend_get(out), 0);
}

void test_drew_app_recommend__null_with_count_rejected(void) {
  const Uuid apps[] = {prv_app_uuid(1)};
  cl_assert(sys_app_recommend_set(apps, 1));
  cl_assert(!sys_app_recommend_set(NULL, 2));

  Uuid out[DREW_APP_RECOMMEND_MAX];
  cl_assert_equal_i(drew_app_recommend_get(out), 1);
}

void test_drew_app_recommend__only_the_default_watchface(void) {
  const Uuid apps[] = {prv_app_uuid(1)};
  const Uuid others[] = {prv_app_uuid(2), prv_app_uuid(3)};
  cl_assert(sys_app_recommend_set(apps, 1));

  // A watchface that isn't the default (e.g. previewed from the phone)
  prv_run_app(&s_face_b, ProcessTypeWatchface);
  cl_assert(!sys_app_recommend_set(others, 2));
  cl_assert(!sys_app_recommend_set(NULL, 0));

  // An app, even one with the default watchface's UUID
  prv_run_app(&s_face_a, ProcessTypeApp);
  cl_assert(!sys_app_recommend_set(others, 2));

  // The default watchface's worker
  prv_run_app(&s_face_a, ProcessTypeWatchface);
  stub_pebble_tasks_set_current(PebbleTask_Worker);
  cl_assert(!sys_app_recommend_set(others, 2));

  Uuid out[DREW_APP_RECOMMEND_MAX];
  cl_assert_equal_i(drew_app_recommend_get(out), 1);
  cl_assert(uuid_equal(&out[0], &apps[0]));
}

void test_drew_app_recommend__outlives_the_watchface_process(void) {
  const Uuid apps[] = {prv_app_uuid(1), prv_app_uuid(2)};
  cl_assert(sys_app_recommend_set(apps, 2));

  // The launcher replaces the watchface
  const Uuid launcher = prv_app_uuid(0x77);
  prv_run_app(&launcher, ProcessTypeApp);
  Uuid out[DREW_APP_RECOMMEND_MAX];
  cl_assert_equal_i(drew_app_recommend_get(out), 2);

  // Setting the same default watchface again keeps them
  prv_set_default_watchface(&s_face_a);
  cl_assert_equal_i(drew_app_recommend_get(out), 2);
}

void test_drew_app_recommend__watchface_change_clears(void) {
  const Uuid apps[] = {prv_app_uuid(1), prv_app_uuid(2)};
  cl_assert(sys_app_recommend_set(apps, 2));

  prv_set_default_watchface(&s_face_b);
  Uuid out[DREW_APP_RECOMMEND_MAX];
  cl_assert_equal_i(drew_app_recommend_get(out), 0);

  // Switching back doesn't bring the old list back
  prv_set_default_watchface(&s_face_a);
  cl_assert_equal_i(drew_app_recommend_get(out), 0);

  // The new default watchface can set its own
  prv_set_default_watchface(&s_face_b);
  prv_run_app(&s_face_b, ProcessTypeWatchface);
  cl_assert(sys_app_recommend_set(apps, 1));
  cl_assert_equal_i(drew_app_recommend_get(out), 1);
}

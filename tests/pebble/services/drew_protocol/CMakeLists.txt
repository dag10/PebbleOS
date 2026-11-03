# SPDX-FileCopyrightText: 2026 Drew Gottlieb
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_drew_protocol
  SOURCES
    pebble/services/drew_protocol/service.c
    tests/fakes/fake_settings.c
)

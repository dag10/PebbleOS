# SPDX-FileCopyrightText: 2026 Drew Gottlieb
# SPDX-License-Identifier: Apache-2.0

pbl_clar_test(test_drew_protocol
  SOURCES
    fw/services/drew_protocol/service.c
    tests/fakes/fake_settings_file.c
  DEFINES
    CONFIG_SERVICE_DREW_UPDATES=1
)

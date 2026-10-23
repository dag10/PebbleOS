/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <pbl/kernel/compiler.h>
#include <pbl/services/vibes/vibe_score.h>

PBL_WEAK VibeScore *vibe_score_create_with_resource_system(ResAppNum app_num,
                                                           uint32_t resource_id) {
  return nullptr;
}

PBL_WEAK void vibe_score_do_vibe(VibeScore *score) {
}

PBL_WEAK void vibe_score_destroy(VibeScore *score) {
}

PBL_WEAK unsigned int vibe_score_get_duration_ms(VibeScore *score) {
  return 0;
}

PBL_WEAK unsigned int vibe_score_get_repeat_delay_ms(VibeScore *score) {
  return 0;
}

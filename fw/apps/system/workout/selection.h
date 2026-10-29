/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <pbl/services/activity/activity.h>

typedef void (*SelectWorkoutCallback)(ActivitySessionType type);

typedef struct WorkoutSelectionWindow WorkoutSelectionWindow;

//! Push the workout type list, focused on the row for selected_type (the first row if it has none).
WorkoutSelectionWindow *workout_selection_push(ActivitySessionType selected_type,
                                               SelectWorkoutCallback select_workout_cb);

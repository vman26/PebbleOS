/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

// ------------------------------------------------------------------------------------------------
// Compute distance (in millimeters) covered by the taking the given number of steps in the given
// amount of time.
uint32_t activity_private_compute_distance_mm(uint32_t steps, uint32_t ms);

// ------------------------------------------------------------------------------------------------
// Estimate cycling speed (in millimeters per minute) from currently available motion signals.
uint32_t activity_private_compute_cycling_speed_mm_per_min(uint16_t vmc, uint16_t steps_per_min,
                                                           uint16_t bpm, uint32_t elapsed_s);

// ------------------------------------------------------------------------------------------------
// Estimate cycling distance (in millimeters) over elapsed time from currently available motion
// signals.
uint32_t activity_private_compute_cycling_distance_mm(uint32_t ms, uint16_t vmc,
                                                      uint16_t steps_per_min, uint16_t bpm,
                                                      uint32_t elapsed_s);


// ------------------------------------------------------------------------------------------------
// Compute active calories (in calories, not kcalories) covered by going the given distance in
// the given amount of time.
uint32_t activity_private_compute_active_calories(uint32_t distance_mm, uint32_t ms);

// ------------------------------------------------------------------------------------------------
// Compute resting calories (in calories, not kcalories) within the elapsed time given
uint32_t activity_private_compute_resting_calories(uint32_t elapsed_minutes);

/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

// ------------------------------------------------------------------------------------------------
// Compute distance (in millimeters) covered by the taking the given number of steps in the given
// amount of time.
uint32_t activity_private_compute_distance_mm(uint32_t steps, uint32_t ms);

// ------------------------------------------------------------------------------------------------
// Estimate cycling distance (in millimeters) over elapsed time using a VMC cube-root speed model.
//
// Speed (m/s) = k1 × VMC^(1/3) + k2 × (HR_current / HR_resting)
//
// Placeholder fixed-point constants are calibrated for typical wrist-based cycling signals on
// STM32 Cortex-M targets and avoid any floating-point instructions.
//
// @param ms      Elapsed time in milliseconds.
// @param vmc     Vector Magnitude Counts for this interval (Actigraph-equivalent units).
// @param hr_bpm  Current heart rate in BPM; pass 0 when unavailable (falls back to resting HR).
// @return Distance in millimetres.
uint32_t activity_private_compute_cycling_distance_mm(uint32_t ms, uint16_t vmc, uint8_t hr_bpm);

// ------------------------------------------------------------------------------------------------
// Compute active calories burned while cycling using the Keytel Heart Rate formula.
//
// Formula (fixed-point, all-integer):
//   mCal/min = (-55097 + 631 × HR + 199 × weight_kg + 202 × age_years) / 4.184
//
// A MET 6.0 fallback is used automatically when hr_bpm is 0 (no HR lock) or when the formula
// would produce a non-positive result.
//
// @param hr_bpm          Current heart rate in BPM; pass 0 to force the MET fallback.
// @param elapsed_minutes Duration of the interval in minutes.
// @return Total active calories burned (Pebble unit: 1000 calories = 1 kcalorie).
uint32_t activity_private_compute_cycling_active_calories_hr(uint8_t hr_bpm,
                                                             uint32_t elapsed_minutes);


// ------------------------------------------------------------------------------------------------
// Compute active calories (in calories, not kcalories) covered by going the given distance in
// the given amount of time.
uint32_t activity_private_compute_active_calories(uint32_t distance_mm, uint32_t ms);

// ------------------------------------------------------------------------------------------------
// Compute resting calories (in calories, not kcalories) within the elapsed time given
uint32_t activity_private_compute_resting_calories(uint32_t elapsed_minutes);

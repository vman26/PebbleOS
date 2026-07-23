/* SPDX-FileCopyrightText: 2024 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "pbl/services/activity/activity_calculators.h"

#include "pbl/services/activity/activity.h"
#include "pbl/services/activity/activity_private.h"
#include "util/units.h"

#include <pbl/util/math.h>

#include <stdint.h>
#include <stdbool.h>


// ------------------------------------------------------------------------------------------------
// Compute distance (in millimeters) covered by the taking the given number of steps in the given
// amount of time.
//
// This function first computes a stride length based on the user's height, gender, and
// rate of stepping. It then multiplies the stride length by the number of steps taken to get the
// distance covered.
//
// Generally, the faster you go, the longer your stride length, and stride length is roughly
// linearly proportional to cadence. The proportionality factor though depends on height, and
// shorter users will have a steeper slope than taller users.
// The general equation for stride length is:
//    stride_len = (a * steps/minute + b) * height
//    where a and b depend on height and gender
//
// @param[in] steps How many steps were taken
// @param[in] ms How many milliseconds elapsed while the steps were taken
// @param[out] distance covered (in millimeters)
uint32_t activity_private_compute_distance_mm(uint32_t steps, uint32_t ms) {
  if ((steps == 0) || (ms == 0)) {
    return 0;
  }

  // For a rough ballpack figure, according to
  //    http://livehealthy.chron.com/determine-stride-pedometer-height-weight-4518.html
  // The average stride length in mm is:
  //    men: 0.415 * height(mm)
  //    women: 0.413 * height(mm)
  // An average cadence would be about 100 steps/min, so plugging in that cadence into the
  // computations below should generate a stride length roughly around 0.414 * height.
  //
  const uint64_t steps_64 = steps;
  const uint64_t ms_64 = ms;
  const uint64_t height_mm_64 = activity_prefs_get_height_mm();

  // Generate the 'a' factor. Eventually, this will be based on height and/or gender. For now,
  // set it to .003129
  const uint64_t k_a_x10000 = 31;

  // Generate the 'b' factor. Eventually, this may be based on height and/or gender. For now,
  // set it to 0.14485
  const uint64_t k_b_x10000 = 1449;

  // The factor we use to avoid fractional arithmetic
  const uint64_t k_x10000 = 10000;

  // We want: stride_len = (a * steps/minute + b) * height
  // Since we have cadence in steps and milliseconds, this becomes:
  //  stride_len = (a * steps * 1000 * 60 / milliseconds + b) * height
  // Compute the "(a * steps * 1000 * 60 / milliseconds + b)" component:
  uint64_t stride_len_component = ROUND(k_a_x10000 * steps_64 * MS_PER_SECOND * SECONDS_PER_MINUTE,
                                        ms_64)  + k_b_x10000;

  // Multiply by height to get stride_len, then by steps to get distance, then factor out our
  // constant multiplier at the very end to minimize rounding errors.
  uint32_t distance_mm = ROUND(stride_len_component * height_mm_64 * steps, k_x10000);

  // Return distance in mm
  ACTIVITY_LOG_DEBUG("Got delta distance of %"PRIu32" mm", distance_mm);
  return distance_mm;
}

// ------------------------------------------------------------------------------------------------
uint32_t activity_private_compute_cycling_speed_mm_per_min(uint16_t vmc, uint16_t steps_per_min,
                                                           uint16_t bpm, uint32_t elapsed_s) {
  // Keep estimates in a plausible cycling range. We bias early session speed higher and let it
  // settle as the session progresses.
  const uint32_t k_min_speed_mm_per_min = 120 * MM_PER_METER;  // 7.2 km/h
  const uint32_t k_max_speed_mm_per_min = 520 * MM_PER_METER;  // 31.2 km/h
  const uint32_t k_base_speed_mm_per_min = 170 * MM_PER_METER; // 10.2 km/h

  uint32_t speed_mm_per_min = k_base_speed_mm_per_min;

  // Motion intensity bonus from VMC.
  const uint32_t k_vmc_cap = 800;
  const uint32_t vmc_capped = MIN(vmc, k_vmc_cap);
  speed_mm_per_min += vmc_capped * 180;

  // Cycling generally has fewer steps than running; penalize high step cadence.
  if (steps_per_min > 80) {
    const uint32_t excess_steps = steps_per_min - 80;
    speed_mm_per_min = MAX((int32_t)k_min_speed_mm_per_min,
                           (int32_t)speed_mm_per_min - (int32_t)(excess_steps * 1200));
  }

  // Heart rate can inform effort when available.
  if (bpm > 95) {
    speed_mm_per_min += MIN((uint32_t)(bpm - 95) * 900, (uint32_t)60 * MM_PER_METER);
  }

  // Start fast then decay over ~10 minutes.
  const uint32_t elapsed_min = elapsed_s / SECONDS_PER_MINUTE;
  if (elapsed_min < 10) {
    speed_mm_per_min += (10 - elapsed_min) * 6 * MM_PER_METER;
  }

  return CLIP(speed_mm_per_min, k_min_speed_mm_per_min, k_max_speed_mm_per_min);
}

// ------------------------------------------------------------------------------------------------
uint32_t activity_private_compute_cycling_distance_mm(uint32_t ms, uint16_t vmc,
                                                      uint16_t steps_per_min, uint16_t bpm,
                                                      uint32_t elapsed_s) {
  if (ms == 0) {
    return 0;
  }

  const uint64_t speed_mm_per_min =
      activity_private_compute_cycling_speed_mm_per_min(vmc, steps_per_min, bpm, elapsed_s);
  const uint64_t ms_per_min = (uint64_t)SECONDS_PER_MINUTE * MS_PER_SECOND;
  return (speed_mm_per_min * ms + (ms_per_min / 2)) / ms_per_min;
}


// ------------------------------------------------------------------------------------------------
// Compute active calories (in calories, not kcalories) covered by going the given distance in
// the given amount of time.
//
// This method uses a formula for active calories as presented in this paper:
// https://www.researchgate.net/profile/Glen_Duncan2/publication/
//   221568418_Validated_caloric_expenditure_estimation_using_a_single_body-worn_sensor/
//   links/0912f4fb562b675d63000000.pdf
//
// In the paper, the formulas for walking and running compute energy in ml:
// walking:
//   active_ml = 0.1 * speed_m_per_min * minutes * weight_kg
// running:
//   active_ml = 0.2 * speed_m_per_min * minutes * weight_kg
//
// Converting to calories (5.01 calories per ml) and plugging in distance for speed * time, we get
// the following. We will define walking as less then 4.5MPH (120 meters/minute)
// for walking:
//   active_cal = 0.1 * distance_m * weight_kg * 5.01
//              = 0.501 * distance_m * weight_kg
// for running:
//   active_cal = 0.2 * distance_m * weight_kg * 5.01
//              = 1.002 * distance_m * weight_kg
//
// For a rough ballpack figure, a 73kg person walking 80 meters in a minute burns about
// 2925 active calories (2.9 kcalories)
// That same 73kg person running 140 meters in a minute burns about 10,240 active calories
// (10.2 kcalories)
//
// @param[in] distance_mm distance covered in millimeters
// @param[in] ms How many milliseconds elapsed while the distance was covered
// @param[out] active calories
uint32_t activity_private_compute_active_calories(uint32_t distance_mm, uint32_t ms) {
  if ((distance_mm == 0) || (ms == 0)) {
    return 0;
  }

  uint64_t distance_mm_64 = distance_mm;
  uint64_t ms_64 = ms;

  // Figure out the rate and see if it's walking or running. We set the walking threshold at
  // 120 m/min. This is 2m/s or 2 mm/ms
  const unsigned int k_max_walking_rate_mm_per_min = 120 * MM_PER_METER;
  uint64_t rate_mm_per_min = distance_mm_64 * MS_PER_SECOND * SECONDS_PER_MINUTE / ms_64;
  bool walking = (rate_mm_per_min <= k_max_walking_rate_mm_per_min);
  uint64_t k_constant_x1000;
  if (walking) {
    k_constant_x1000 = 501;
  } else {
    k_constant_x1000 = 1002;
  }

  uint64_t weight_dag = activity_prefs_get_weight_dag();  // 10 grams = 1 dag

  uint32_t calories = ROUND(k_constant_x1000 * (uint64_t)distance_mm * weight_dag,
                            1000 * MM_PER_METER * ACTIVITY_DAG_PER_KG);

  // Return calories
  ACTIVITY_LOG_DEBUG("Got delta active calories of %"PRIu32" ", calories);
  return calories;
}


// --------------------------------------------------------------------------------------------
uint32_t activity_private_compute_resting_calories(uint32_t elapsed_minutes) {
  // This computes resting metabolic rate in calories based on the MD Mifflin and ST St jeor
  // formula. This formula gives the number of kcalories expended per day
  uint32_t calories_per_day;
  ActivityGender gender = activity_prefs_get_gender();
  uint64_t weight_dag = activity_prefs_get_weight_dag();
  uint64_t height_mm = activity_prefs_get_height_mm();
  uint64_t age_years = activity_prefs_get_age_years();

  // For men:   kcalories = 10 * weight(kg) + 6.25 * height(cm) - 5 * age(y) + 5
  // For women: kcalories = 10 * weight(kg) + 6.25 * height(cm) - 5 * age(y) - 161
  calories_per_day =   (100 * weight_dag)
                     + (625 * height_mm)
                     - (5000 * age_years);
  if (gender == ActivityGenderMale) {
    calories_per_day += 5000;
  } else if (gender == ActivityGenderFemale) {
    calories_per_day -= 161000;
  } else {
    // midpoint of 5000 and -161000
    calories_per_day -= 78000;
  }

  // Scale by the requested number of minutes
  uint32_t resting_calories = ROUND(calories_per_day * elapsed_minutes, MINUTES_PER_DAY);
  ACTIVITY_LOG_DEBUG("resting_calories: %"PRIu32"", resting_calories);
  return resting_calories;
}

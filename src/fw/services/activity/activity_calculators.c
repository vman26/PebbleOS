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
// Integer cube root via Newton-Raphson iteration.
// Computes floor(n^(1/3)) for any non-negative 32-bit n.
static uint32_t prv_icbrt(uint32_t n) {
  if (n == 0) {
    return 0;
  }
  // Seed the estimate at the highest power-of-two that does not overshoot.
  uint32_t x = 1;
  while (x * x * x <= n) {
    x <<= 1;
  }
  x >>= 1;  // x is now a conservative lower bound
  // Three Newton-Raphson steps from a good initial estimate are sufficient to reach
  // the correct floor for any 32-bit input (the error after seeding is at most a
  // factor of 2, and each step roughly cubes the convergence rate).
  for (int i = 0; i < 3; i++) {
    uint32_t x3 = x * x * x;
    if (x3 == n) {
      return x;
    }
    x = (2 * x + n / (x * x)) / 3;
  }
  // Round down to the exact floor
  while ((x + 1) * (x + 1) * (x + 1) <= n) {
    x++;
  }
  return x;
}

// ------------------------------------------------------------------------------------------------
// Estimate cycling distance (in millimetres) over elapsed time using a VMC cube-root speed model.
//
// Speed formula (fixed-point):
//   v_mm_per_s = k1_x1000 × cbrt(VMC) / 1000  [m/s contribution from motion intensity]
//              + k2_x1000 × HR / HR_resting / 1000  [m/s contribution from HR effort]
//
// The constants k1_x1000 = 320 and k2_x1000 = 850 are empirical placeholder values appropriate
// for wrist-mounted cycling detection.  They have been chosen so that:
//   - A resting-HR baseline at VMC=120 yields ~3.5 m/s  (12.6 km/h, casual cycling)
//   - A hard effort at VMC=400 yields ~7 m/s  (25 km/h)
//
// TODO: Re-calibrate k1_x1000 and k2_x1000 with on-device cycling data once available.
uint32_t activity_private_compute_cycling_distance_mm(uint32_t ms, uint16_t vmc, uint8_t hr_bpm) {
  if (ms == 0) {
    return 0;
  }

  // Fixed-point scaling constants (×1000 to avoid floating-point).
  // k1_x1000: VMC cube-root coefficient (m/s per cbrt(VMC)).
  // k2_x1000: HR ratio coefficient (m/s when HR equals resting HR).
  const uint32_t k1_x1000 = 320;
  const uint32_t k2_x1000 = 850;
  const uint32_t k_hr_resting_bpm = 60;  // Assumed resting HR; replace with user pref if exposed.

  // VMC component: k1 × cbrt(VMC)  (result in mm/s × 1000)
  uint32_t vmc_component = k1_x1000 * prv_icbrt(vmc);

  // HR component: k2 × (HR / HR_resting)  (result in mm/s × 1000)
  const uint32_t hr = (hr_bpm > 0) ? hr_bpm : k_hr_resting_bpm;
  uint32_t hr_component = k2_x1000 * hr / k_hr_resting_bpm;

  // Combined speed in mm/s (÷ 1000 to factor out the ×1000 scaling)
  uint32_t speed_mm_per_s_x1000 = vmc_component + hr_component;

  // Distance = speed × time: (speed_mm_s_x1000 × ms) / (1000 × 1000)
  //   ×1000 for speed scale, ×1000 to convert ms to s
  return (uint32_t)((uint64_t)speed_mm_per_s_x1000 * ms / 1000 / 1000);
}

// ------------------------------------------------------------------------------------------------
// Compute active calories burned during a cycling interval using the Keytel Heart Rate formula.
//
// Keytel formula (men, adapted):
//   Cal/min = (-55.097 + 0.631 × HR + 0.199 × weight_kg + 0.202 × age_years) / 4.184
//
// Implemented in fixed-point integer arithmetic (coefficients scaled ×1000) to avoid
// any floating-point overhead in background tasks on Cortex-M targets.
//
// MET 6.0 fallback (WHO cycling MET):
//   Cal/min = MET × weight_kg / 60  →  cal = 6 × weight_kg × elapsed_min / 60 × 1000
//
// Calorie units match the rest of the activity service: 1000 = 1 kcalorie.
uint32_t activity_private_compute_cycling_active_calories_hr(uint8_t hr_bpm,
                                                             uint32_t elapsed_minutes) {
  if (elapsed_minutes == 0) {
    return 0;
  }

  // ACTIVITY_DAG_PER_KG = 100 (10 grams per dag; 100 dag = 1 kg)
  const uint32_t k_dag_per_kg = ACTIVITY_DAG_PER_KG;
  const uint32_t weight_kg = (uint32_t)activity_prefs_get_weight_dag() / k_dag_per_kg;
  const uint32_t age_years = (uint32_t)activity_prefs_get_age_years();

  if (hr_bpm > 0 && weight_kg > 0 && age_years > 0) {
    // Keytel formula.  Constants are pre-multiplied ×1000 (e.g., 0.631 → 631) so all
    // arithmetic stays in integers; 4.184 is represented as the divisor 4184/1000.
    // mCal/min = (-55097 + 631 × HR + 199 × weight_kg + 202 × age_years) × 1000 / 4184
    int32_t keytel_x1 = -55097
                        + (int32_t)(631 * (uint32_t)hr_bpm)
                        + (int32_t)(199 * weight_kg)
                        + (int32_t)(202 * age_years);
    if (keytel_x1 > 0) {
      // Convert to Pebble calorie units (1000 cal = 1 kcal) × elapsed minutes.
      // keytel_x1 / 4.184 gives Cal/min; × elapsed_minutes = total Cal; × 1000 = Pebble units.
      uint32_t cal = (uint32_t)((uint64_t)keytel_x1 * 1000 / 4184 * elapsed_minutes);
      return cal;
    }
    // Fall through to MET fallback for non-positive Keytel result.
  }

  // MET 6.0 fallback: 6 × weight_kg × elapsed_min / 60 × 1000  (Pebble calorie units)
  if (weight_kg == 0) {
    return 0;
  }
  return 6 * weight_kg * elapsed_minutes * 1000 / 60;
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

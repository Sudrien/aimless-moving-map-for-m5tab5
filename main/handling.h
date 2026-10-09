/*
 * handling.h -- whether the device is being handled, from its
 * accelerometer: picked up, turned, set down, or just somewhere things
 * shake, as a vehicle or a pocket is and a desk is not.
 *
 * The accelerometer half of original/compass.cpp's compass_update(),
 * with no I2C in it, so it runs on the host; imu.c reads the samples.
 * The magnetometer, heading and calibration are not ported: the M135's
 * never gave a trustworthy heading where it is mounted.
 *
 * A low-passed gravity vector, and the last time a reading departed from
 * it. At rest the two agree to within the part's noise; tilting, lifting
 * or knocking the device separates them for as long as the movement
 * lasts, and then the filter catches up. A detector of change in
 * attitude, not of any attitude. Vector distance, not a change in
 * magnitude: picking a device up and turning it barely changes |a|.
 *
 * Raw counts at +-2 g, 16384 a g: only the thresholds are tuned, and
 * they are as easy to state in counts.
 *
 * Free of ESP-IDF.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* src: original/compass.cpp compass_update(): 10 Hz. */
#define HANDLING_PERIOD_MS      (100u)
/* src: original/compass.cpp MOTION_COUNTS, measured there on the M135's
 * BMI270 in a Tab5: a desk's 30 s peak 206-255 counts, picked up and
 * turned 18109, set down at a new angle and left 1078 while the filter
 * caught up -- the figure this must clear, or settling counts as
 * handling. About 0.09 g. The Tab5's own BMI270 is the same part at the
 * same range in the same body, but not the same spot: the peak is
 * logged so this can be checked against it (handling_peak_take()). */
#define HANDLING_MOTION_COUNTS  (1500.0f)
/* src: original/compass.cpp MOTION_RUN_NEEDED: two samples, 200 ms of
 * sustained departure; one is a knocked table. */
#define HANDLING_RUN            (2)
/* src: original/compass.cpp STIR_COUNTS: just clear of a desk's 188-255
 * count peaks. Not handling -- corroboration that the device is
 * somewhere things move. */
#define HANDLING_STIR_COUNTS    (500.0f)
/* src: original/compass.cpp GRAV_ALPHA: at 10 Hz, a new attitude settles
 * in a couple of seconds. */
#define HANDLING_ALPHA          (0.08f)

typedef struct {
    float    grav[3];
    bool     valid;
    uint8_t  run;
    uint32_t moved_ms;          /* last handled; 0 never */
    uint32_t stir_ms;           /* last stirred; 0 never */
    float    peak;              /* largest departure since last taken */
} handling_t;

/* One reading, raw counts, at `now_ms`. */
void  handling_sample(handling_t *h, int16_t ax, int16_t ay, int16_t az, uint32_t now_ms);

/* The largest departure since the last call, and clear it: for the log,
 * so the thresholds can be set against what this board produces. */
float handling_peak_take(handling_t *h);

#ifdef __cplusplus
}
#endif

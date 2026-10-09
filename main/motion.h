/*
 * motion.h -- what the device's speed decides: how often the receiver
 * solves, and whether a parked, untouched screen dims.
 *
 * original/tab5_map.cpp's gnssRatePolicy() and idleDimmed() /
 * applyIdleDim(), with no receiver and no clock in them, so they run on
 * the host. aimless.c sends the rate and sets the backlight. Times are
 * milliseconds that may wrap: everything compares differences, as the
 * original's millis() arithmetic did.
 *
 * Free of ESP-IDF.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "handling.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the receiver's rate ----
 *
 * The receiver is the largest draw after the panel and costs the same
 * parked as at 100 km/h. Slowing its solutions when there is nothing to
 * follow gives up nothing the low-power modes would: it keeps tracking,
 * and the next fix is a fix, not a reacquisition.
 *
 * src: original/tab5_map.cpp GNSS_RATE_FAST, _WALK, _IDLE, GNSS_MOVING_KMH,
 * GNSS_SLOW_KMH, GNSS_RATE_SETTLE_MS -- judgements there, unattributed
 * (original/PROVENANCE.md). FAST at vehicle speeds, where the view
 * shifts several times a minute; WALK where the marker moves about 2 m
 * a fix, inside the receiver's error; IDLE where nothing on screen
 * changes, and starting to move is still seen within seconds. Two
 * thresholds with a gap, so a speed near one does not flip every fix,
 * and a band must hold 10 s, with 10 s between changes, so pulling away
 * from a light does not change rate mid-manoeuvre.
 */
#define MOTION_RATE_FAST_MS     (1000)
#define MOTION_RATE_WALK_MS     (2000)
#define MOTION_RATE_IDLE_MS     (5000)
#define MOTION_MOVING_KMH       (12.0)
#define MOTION_SLOW_KMH         (4.0)
#define MOTION_SETTLE_MS        (10000u)

/*
 * The rate the fix asks for. FAST with no fix or one that is not 3D: a
 * receiver still searching spends its power on the search, and slowing
 * it lengthens it; a 2D fix's speed is not to be trusted. FAST while
 * `busy` -- the area cache is walking tiles around the position.
 */
uint16_t motion_rate_want(bool coarse, int mode, double kmh, bool busy);

typedef struct {
    uint16_t pending;           /* 0: none */
    uint32_t pending_since;
    uint32_t last_change;
    bool     changed;           /* last_change is set */
} motion_rate_t;

/*
 * Whether to change the rate: `want` from motion_rate_want(), `current`
 * what the receiver runs at. The rate to send, once `want` has held
 * MOTION_SETTLE_MS and as long has passed since the last change; 0 to
 * leave it.
 */
uint16_t motion_rate_step(motion_rate_t *s, uint16_t want, uint16_t current, uint32_t now_ms);

/* ---- the parked dim ----
 *
 * Left parked and untouched, the backlight steps down to a fraction of
 * the level in force; a touch, or setting off, brings it straight back.
 * Untouched alone would be wrong: driving is when the screen is read
 * constantly and touched not at all.
 *
 * src: original/tab5_map.cpp IDLE_DIM_MS, IDLE_DIM_PCT, IDLE_DIM_FLOOR
 * (30 of 255, as a percent), IDLE_UNDIM_MOVE_M, IDLE_UNDIM_ANCHOR_MS --
 * judgements there, the least examined numbers in the set
 * (original/PROVENANCE.md). Two minutes untouched; 40 % of the level, not
 * under 12 %, so a dimmed screen still looks lit and not crashed; 25 m
 * from where the device was is far outside a parked receiver's wander
 * and about three seconds at 30 km/h; the anchor rolls every 30 s so
 * slow drift cannot add up to 25 m.
 */
#define MOTION_IDLE_MS          (120000u)
#define MOTION_DIM_PCT          (40)
#define MOTION_DIM_FLOOR_PCT    (12)
#define MOTION_UNDIM_MOVE_M     (25.0)
#define MOTION_ANCHOR_MS        (30000u)
/* src: original/tab5_map.cpp IDLE_UNDIM_HOLD_MS, IDLE_UNDIM_STIR_MS,
 * judgements there. Handled in the last 15 s keeps the screen up: long
 * enough that setting it down mid-glance does not dim it in the hand.
 * A stir in the last 60 s corroborates a 25 m move: generous, since the
 * point is to reject a device provably undisturbed for minutes. */
#define MOTION_HANDLED_MS       (15000u)
#define MOTION_STIR_MS          (60000u)

typedef struct {
    double   lat, lon;
    uint32_t since;
    bool     anchored;
} motion_idle_t;

/*
 * Whether to dim now. Slow in, quick out: in only when untouched for
 * MOTION_IDLE_MS and the rate has settled to IDLE -- the band the rate
 * policy already debounced, not a second idea of stopped -- and out on a
 * touch or on MOTION_UNDIM_MOVE_M from a rolling anchor, which setting
 * off clears in seconds, long before the band follows. No 3D fix, no
 * dim: a receiver that cannot say whether the device moves has not said
 * it is still.
 *
 * With an accelerometer (`imu` not NULL, 0032), two more rules, the
 * original's: handled within MOTION_HANDLED_MS keeps the screen up --
 * the way out standing still, where GNSS cannot tell a device in the
 * hand from one on a seat -- and a 25 m move counts only with a stir
 * within MOTION_STIR_MS, since indoors the position wanders tens of
 * metres on a good HDOP and real travel is never silent. Without one, a
 * distance is taken on trust, as the original did with no compass.
 *
 * One departure: the anchor is kept up while the screen is being
 * touched, where the original left it, so the first test after two
 * minutes is not against an anchor from before them.
 */
bool motion_idle(motion_idle_t *s, uint32_t now_ms, uint32_t last_touch_ms,
                 bool fix3d, double lat, double lon, uint16_t rate_ms,
                 const handling_t *imu);

/* Whether `imu` says the device was handled within MOTION_HANDLED_MS:
 * for the log, which says which way out a dim took. */
bool motion_handled(const handling_t *imu, uint32_t now_ms);

/* The level a dimmed backlight runs at, from the one in force: never
 * brighter than it. */
int  motion_dim_pct(int pct);

#ifdef __cplusplus
}
#endif

/*
 * motion.c -- see motion.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "motion.h"

#include "waypoints.h"

uint16_t motion_rate_want(bool coarse, int mode, double kmh, bool busy)
{
    if (busy || !coarse || mode != 3) return MOTION_RATE_FAST_MS;
    if (kmh >= MOTION_MOVING_KMH) return MOTION_RATE_FAST_MS;
    if (kmh >= MOTION_SLOW_KMH) return MOTION_RATE_WALK_MS;
    return MOTION_RATE_IDLE_MS;
}

uint16_t motion_rate_step(motion_rate_t *s, uint16_t want, uint16_t current, uint32_t now_ms)
{
    if (want == current) {
        s->pending = 0;
        return 0;
    }
    if (want != s->pending) {
        s->pending = want;
        s->pending_since = now_ms;
        return 0;
    }
    if (now_ms - s->pending_since < MOTION_SETTLE_MS) return 0;
    if (s->changed && now_ms - s->last_change < MOTION_SETTLE_MS) return 0;
    s->changed = true;
    s->last_change = now_ms;
    s->pending = 0;
    return want;
}

bool motion_idle(motion_idle_t *s, uint32_t now_ms, uint32_t last_touch_ms,
                 bool fix3d, double lat, double lon, uint16_t rate_ms)
{
    /* The anchor is kept up whether or not a touch has been recent, as
     * the original's was not -- it returned on the touch first, and its
     * anchor could be minutes old at the first test. A stale anchor
     * reads as having moved the moment the dim would start. */
    if (!fix3d) {
        /* Nothing to anchor to. Dropped, so the first fix after a gap is
         * compared with itself, not with before the sky closed in. */
        s->anchored = false;
    } else if (!s->anchored) {
        s->lat = lat;
        s->lon = lon;
        s->since = now_ms;
        s->anchored = true;
    } else if (wp_distance_m(lat, lon, s->lat, s->lon) >= MOTION_UNDIM_MOVE_M) {
        /* Somewhere else: re-anchored either way, so a journey keeps
         * clearing the test, and a receiver that has wandered does not
         * stay 25 m from a stale anchor for ever. */
        s->lat = lat;
        s->lon = lon;
        s->since = now_ms;
        return false;
    } else if (now_ms - s->since >= MOTION_ANCHOR_MS) {
        s->lat = lat;
        s->lon = lon;
        s->since = now_ms;
    }
    if (now_ms - last_touch_ms < MOTION_IDLE_MS) return false;
    return fix3d && rate_ms == MOTION_RATE_IDLE_MS;
}

int motion_dim_pct(int pct)
{
    const int v = pct * MOTION_DIM_PCT / 100;
    if (v >= MOTION_DIM_FLOOR_PCT) return v;
    /* The floor, but never brighter than what was in force. */
    return pct < MOTION_DIM_FLOOR_PCT ? pct : MOTION_DIM_FLOOR_PCT;
}

/*
 * motiontest.c -- main/motion.c: the receiver's rate by speed, settled
 * and spaced, and the parked dim's way in and ways out.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "motion.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void want(void)
{
    printf("the rate a fix asks for\n");
    CHECK(motion_rate_want(false, 0, 0, false) == MOTION_RATE_FAST_MS, "no fix");
    CHECK(motion_rate_want(true, 2, 0, false) == MOTION_RATE_FAST_MS, "2D");
    CHECK(motion_rate_want(true, 3, 0, false) == MOTION_RATE_IDLE_MS, "still");
    CHECK(motion_rate_want(true, 3, 3.9, false) == MOTION_RATE_IDLE_MS, "3.9 km/h");
    CHECK(motion_rate_want(true, 3, 4.0, false) == MOTION_RATE_WALK_MS, "4 km/h");
    CHECK(motion_rate_want(true, 3, 11.9, false) == MOTION_RATE_WALK_MS, "11.9 km/h");
    CHECK(motion_rate_want(true, 3, 12.0, false) == MOTION_RATE_FAST_MS, "12 km/h");
    CHECK(motion_rate_want(true, 3, 0, true) == MOTION_RATE_FAST_MS, "busy");
}

/* Feed `w` every second from `t` for `secs`; the rate the receiver ends
 * at, starting from `*cur`, and how many changes were sent. */
static int feed(motion_rate_t *s, uint16_t *cur, uint16_t w, uint32_t *t, int secs)
{
    int sent = 0;
    for (int i = 0; i < secs; i++, *t += 1000) {
        const uint16_t r = motion_rate_step(s, w, *cur, *t);
        if (r) { *cur = r; sent++; }
    }
    return sent;
}

static void settle(void)
{
    printf("settled and spaced\n");
    motion_rate_t s;
    memset(&s, 0, sizeof(s));
    uint16_t cur = MOTION_RATE_FAST_MS;
    /* Start just short of the 32-bit wrap: differences must survive it. */
    uint32_t t = 0xFFFFFFFFu - 4000;

    CHECK(motion_rate_step(&s, MOTION_RATE_FAST_MS, cur, t) == 0, "same rate sent");
    /* IDLE for 10 s is not yet enough, 11 is. */
    int n = feed(&s, &cur, MOTION_RATE_IDLE_MS, &t, 10);
    CHECK(n == 0 && cur == MOTION_RATE_FAST_MS, "changed after %d s: %u", 10, cur);
    n = feed(&s, &cur, MOTION_RATE_IDLE_MS, &t, 1);
    CHECK(n == 1 && cur == MOTION_RATE_IDLE_MS, "not changed at 10 s: %u", cur);

    /* Moving off: wants FAST at once, gets it 10 s on. A blip back to
     * IDLE restarts the wait. */
    n = feed(&s, &cur, MOTION_RATE_FAST_MS, &t, 5);
    n += feed(&s, &cur, MOTION_RATE_IDLE_MS, &t, 1);
    n += feed(&s, &cur, MOTION_RATE_FAST_MS, &t, 10);
    CHECK(n == 0 && cur == MOTION_RATE_IDLE_MS, "a blip did not restart the wait: %u", cur);
    n = feed(&s, &cur, MOTION_RATE_FAST_MS, &t, 1);
    CHECK(n == 1 && cur == MOTION_RATE_FAST_MS, "FAST not reached: %u", cur);

    /* Two changes are 10 s apart even when the band settles sooner: a
     * fresh motion_rate_t sees only the spacing. */
    motion_rate_t q;
    memset(&q, 0, sizeof(q));
    uint16_t c2 = MOTION_RATE_FAST_MS;
    uint32_t t2 = 1000;
    feed(&q, &c2, MOTION_RATE_WALK_MS, &t2, 11);
    CHECK(c2 == MOTION_RATE_WALK_MS, "first change");
    const uint32_t first = q.last_change;
    /* Wanted IDLE at once: it settles at +10 s and is sent at +10 s. */
    int k = 0;
    while (c2 == MOTION_RATE_WALK_MS && k++ < 60) feed(&q, &c2, MOTION_RATE_IDLE_MS, &t2, 1);
    CHECK(c2 == MOTION_RATE_IDLE_MS && q.last_change - first >= MOTION_SETTLE_MS,
          "second change %u ms after the first", q.last_change - first);
}

/* Degrees per metre, near enough, at 42 N. */
#define M_LAT (1.0 / 111132.0)

static void idle(void)
{
    printf("the parked dim\n");
    motion_idle_t s;
    memset(&s, 0, sizeof(s));
    const double lat = 42.3, lon = -83.4;
    uint32_t t = 5000, touch = 5000;
    const uint16_t I = MOTION_RATE_IDLE_MS;

    /* Untouched under two minutes: no. */
    CHECK(!motion_idle(&s, t, touch, true, lat, lon, I), "dim at once");
    t = touch + MOTION_IDLE_MS - 1;
    CHECK(!motion_idle(&s, t, touch, true, lat, lon, I), "dim before two minutes");
    t = touch + MOTION_IDLE_MS;
    CHECK(motion_idle(&s, t, touch, true, lat, lon, I), "no dim at two minutes");
    /* Not while the rate says moving, nor without a 3D fix. */
    CHECK(!motion_idle(&s, t, touch, true, lat, lon, MOTION_RATE_WALK_MS), "dim walking");
    CHECK(!motion_idle(&s, t, touch, false, lat, lon, I), "dim with no fix");
    /* A touch ends it. */
    CHECK(!motion_idle(&s, t + 1, t, true, lat, lon, I), "dim after a touch");

    /* Parked receiver wander -- 10 m about the spot for minutes, the
     * anchor rolling -- does not undim. */
    memset(&s, 0, sizeof(s));
    touch = 0;
    t = MOTION_IDLE_MS;
    bool all = true;
    for (int i = 0; i < 300; i++, t += 1000) {
        const double d = (i % 3 - 1) * 10.0 * M_LAT;
        all &= motion_idle(&s, t, touch, true, lat + d, lon, I);
    }
    CHECK(all, "wander undimmed it");

    /* Setting off: 25 m from the anchor undims it at once, while the rate
     * is still IDLE. */
    double y = lat;
    int fixes = 0;
    while (motion_idle(&s, t, touch, true, y, lon, I) && fixes < 20) {
        y += 8.3 * M_LAT;               /* 30 km/h, a fix a second */
        t += 1000;
        fixes++;
    }
    CHECK(fixes >= 2 && fixes <= 5, "undimmed after %d fixes", fixes);

    /* Slow drift of 1 m a fix for a minute: the rolling anchor keeps it
     * under 25 m from any anchor, so it stays dim. */
    memset(&s, 0, sizeof(s));
    t = MOTION_IDLE_MS + 1;
    y = lat;
    all = true;
    for (int i = 0; i < 20; i++, t += 1000, y += 1.0 * M_LAT)
        all &= motion_idle(&s, t, 0, true, y, lon, I);
    CHECK(all, "20 m of drift over 20 s undimmed it");

    /* The first fix after a gap anchors afresh, not against before it. */
    CHECK(!motion_idle(&s, t, 0, false, 0, 0, I), "no fix, dimmed");
    CHECK(motion_idle(&s, t + 1000, 0, true, lat + 500 * M_LAT, lon, I),
          "a jump across a gap counted as moving");
}

static void level(void)
{
    printf("how dim\n");
    CHECK(motion_dim_pct(80) == 32, "80 -> %d", motion_dim_pct(80));
    CHECK(motion_dim_pct(55) == 22, "55 -> %d", motion_dim_pct(55));
    CHECK(motion_dim_pct(24) == 12, "24 -> %d", motion_dim_pct(24));
    CHECK(motion_dim_pct(10) == 10, "10 -> %d: brighter than it was", motion_dim_pct(10));
}

int main(void)
{
    want();
    settle();
    idle();
    level();
    printf("\nmotiontest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

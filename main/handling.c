/*
 * handling.c -- see handling.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "handling.h"

#include <math.h>

/* Zero means never, so a time that wraps to it is moved off it: once in
 * 49.7 days, at the cost of one missed millisecond. */
static uint32_t stamp(uint32_t now_ms) { return now_ms ? now_ms : 1; }

void handling_sample(handling_t *h, int16_t ax, int16_t ay, int16_t az, uint32_t now_ms)
{
    const float a[3] = { (float)ax, (float)ay, (float)az };
    if (!h->valid) {
        for (int i = 0; i < 3; i++) h->grav[i] = a[i];
        h->valid = true;
        return;
    }
    const float dx = a[0] - h->grav[0], dy = a[1] - h->grav[1], dz = a[2] - h->grav[2];
    const float dev = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dev > h->peak) h->peak = dev;

    /* One sample is enough for a stir: it only ever corroborates what
     * something else has to raise first. */
    if (dev > HANDLING_STIR_COUNTS) h->stir_ms = stamp(now_ms);

    if (dev > HANDLING_MOTION_COUNTS) {
        if (h->run < HANDLING_RUN) h->run++;
        if (h->run >= HANDLING_RUN) h->moved_ms = stamp(now_ms);
    } else {
        h->run = 0;
    }

    for (int i = 0; i < 3; i++) h->grav[i] += (a[i] - h->grav[i]) * HANDLING_ALPHA;
}

float handling_peak_take(handling_t *h)
{
    const float v = h->peak;
    h->peak = 0.0f;
    return v;
}

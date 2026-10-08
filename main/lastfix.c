/*
 * lastfix.c -- see lastfix.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "lastfix.h"

#include <math.h>
#include <string.h>

/* src: original/tab5_map.cpp LASTFIX_MAGIC, "LFX1". */
#define LASTFIX_MAGIC   (0x4C465831u)

static void put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) { p[i] = (uint8_t)v; v >>= 8; }
}

static uint64_t get_le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void put_float(uint8_t *p, float f)
{
    uint32_t b;
    memcpy(&b, &f, sizeof(b));
    put_le(p, b, 4);
}

static float get_float(const uint8_t *p)
{
    const uint32_t b = (uint32_t)get_le(p, 4);
    float f;
    memcpy(&f, &b, sizeof(f));
    return f;
}

void lastfix_encode(double lat, double lon, int64_t utc, uint8_t out[LASTFIX_BYTES])
{
    memset(out, 0, LASTFIX_BYTES);
    put_le(out, LASTFIX_MAGIC, 4);
    put_float(out + 4, (float)lat);
    put_float(out + 8, (float)lon);
    put_le(out + 16, (uint64_t)(utc > 0 ? utc : 0), 8);
}

bool lastfix_decode(const uint8_t *buf, size_t len, double *lat, double *lon)
{
    if (!buf || len < LASTFIX_BYTES || get_le(buf, 4) != LASTFIX_MAGIC) return false;
    const float a = get_float(buf + 4), o = get_float(buf + 8);
    /* 0,0 is the receiver's "nothing yet", not a place anyone saved. */
    if (!isfinite(a) || !isfinite(o) || fabsf(a) > 90.0f || fabsf(o) > 180.0f ||
        (a == 0.0f && o == 0.0f))
        return false;
    *lat = a;
    *lon = o;
    return true;
}

/*
 * waypoints.c -- see waypoints.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "waypoints.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* src: original/waypoints.cpp WP_MAGIC, "WPT1". */
#define WP_MAGIC    (0x57505431u)
#define WP_VERSION  (1)

/* src: original/waypoints.cpp EARTH_R_M, the WGS84 mean radius. */
#define EARTH_R_M   (6371008.8)
#define D2R         (0.017453292519943295)

void wp_init(wp_list_t *l)
{
    memset(l, 0, sizeof(*l));
    l->target = -1;
}

/* ---- the file's bytes, little-endian ---- */

static uint64_t get_le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) { p[i] = (uint8_t)v; v >>= 8; }
}

static double get_double(const uint8_t *p)
{
    const uint64_t b = get_le(p, 8);
    double d;
    memcpy(&d, &b, sizeof(d));
    return d;
}

static void put_double(uint8_t *p, double d)
{
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    put_le(p, b, 8);
}

int wp_load(wp_list_t *l, const uint8_t *buf, size_t len)
{
    wp_init(l);
    if (!buf || len < WP_HEAD_BYTES) return 0;
    if (get_le(buf, 4) != WP_MAGIC || get_le(buf + 4, 2) != WP_VERSION) return 0;
    const int count = (int)get_le(buf + 6, 2);
    if (count > WP_MAX) return 0;
    const int whole = (int)((len - WP_HEAD_BYTES) / WP_REC_BYTES);
    const int n = count < whole ? count : whole;
    for (int i = 0; i < n; i++) {
        const uint8_t *r = buf + WP_HEAD_BYTES + (size_t)i * WP_REC_BYTES;
        wp_point_t *p = &l->p[i];
        p->lat = get_double(r);
        p->lon = get_double(r + 8);
        memcpy(p->name, r + 16, WP_NAME_MAX);
        p->name[WP_NAME_MAX - 1] = '\0';
        p->saved_utc = (int64_t)get_le(r + 40, 8);
    }
    l->n = n;
    return n;
}

size_t wp_save(const wp_list_t *l, uint8_t *buf, size_t cap)
{
    const size_t need = WP_HEAD_BYTES + (size_t)l->n * WP_REC_BYTES;
    if (cap < need) return 0;
    put_le(buf, WP_MAGIC, 4);
    put_le(buf + 4, WP_VERSION, 2);
    put_le(buf + 6, (uint64_t)l->n, 2);
    for (int i = 0; i < l->n; i++) {
        uint8_t *r = buf + WP_HEAD_BYTES + (size_t)i * WP_REC_BYTES;
        const wp_point_t *p = &l->p[i];
        put_double(r, p->lat);
        put_double(r + 8, p->lon);
        /* The name and zeros after it, as the original's strncpy() left
         * them: the name is always terminated within its 24 bytes. */
        memset(r + 16, 0, WP_NAME_MAX);
        size_t k = 0;
        while (k < WP_NAME_MAX - 1 && p->name[k]) k++;
        memcpy(r + 16, p->name, k);
        put_le(r + 40, (uint64_t)p->saved_utc, 8);
    }
    return need;
}

/* ---- the list ---- */

int wp_add(wp_list_t *l, double lat, double lon, const char *name, int64_t utc)
{
    if (l->n >= WP_MAX) return -1;
    wp_point_t *p = &l->p[l->n];
    memset(p, 0, sizeof(*p));
    p->lat = lat;
    p->lon = lon;
    p->saved_utc = utc > 0 ? utc : 0;
    if (name && name[0]) {
        snprintf(p->name, sizeof(p->name), "%s", name);
    } else if (p->saved_utc) {
        const int64_t day = p->saved_utc % 86400;
        snprintf(p->name, sizeof(p->name), "%02d:%02d", (int)(day / 3600), (int)(day % 3600 / 60));
    } else {
        snprintf(p->name, sizeof(p->name), "pin %d", l->n + 1);
    }
    return l->n++;
}

bool wp_remove(wp_list_t *l, int i)
{
    if (i < 0 || i >= l->n) return false;
    memmove(&l->p[i], &l->p[i + 1], (size_t)(l->n - i - 1) * sizeof(l->p[0]));
    l->n--;
    /* The target is an index into a list that just shifted under it. */
    if (l->target == i) l->target = -1;
    else if (l->target > i) l->target--;
    return true;
}

void wp_set_target(wp_list_t *l, int i)
{
    l->target = (i >= 0 && i < l->n) ? i : -1;
}

int wp_target(const wp_list_t *l)
{
    return (l->target >= 0 && l->target < l->n) ? l->target : -1;
}

/* ---- geometry ---- */

double wp_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = lat1 * D2R, p2 = lat2 * D2R;
    const double dp = (lat2 - lat1) * D2R, dl = (lon2 - lon1) * D2R;
    const double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2.0 * EARTH_R_M * atan2(sqrt(a), sqrt(1.0 - a));
}

double wp_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = lat1 * D2R, p2 = lat2 * D2R, dl = (lon2 - lon1) * D2R;
    const double y = sin(dl) * cos(p2);
    const double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    const double b = atan2(y, x) / D2R;
    return b < 0 ? b + 360.0 : b;
}

static const char *compass16(double deg)
{
    static const char *const pts[16] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
    return pts[(int)((deg + 11.25) / 22.5) & 15];
}

void wp_target_text(const wp_list_t *l, double lat, double lon, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = '\0';
    const int t = wp_target(l);
    if (t < 0) return;
    const wp_point_t *p = &l->p[t];
    const double m = wp_distance_m(lat, lon, p->lat, p->lon);
    /* src: original wp_target_text(): under 30 m the bearing is noise. */
    if (m < 30.0)
        snprintf(out, cap, "%s: here (%d m)", p->name, (int)m);
    else if (m < 1000.0)
        snprintf(out, cap, "%s: %d m %s", p->name, (int)m,
                 compass16(wp_bearing_deg(lat, lon, p->lat, p->lon)));
    else
        snprintf(out, cap, "%s: %.1f km %s", p->name, m / 1000.0,
                 compass16(wp_bearing_deg(lat, lon, p->lat, p->lon)));
}

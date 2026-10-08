/*
 * waypoints.h -- saved points, and "get me back there".
 *
 * original/waypoints.cpp as C with no file or clock in it: the list, its
 * file format as bytes, one target at a time, and the straight-line
 * distance and bearing to it. aimless.c reads and writes the file on the
 * card and draws the result; this is tested on the host.
 *
 * NOT TURN-BY-TURN, for the original's reason: the archive holds drawing
 * geometry -- road lines cut at tile edges with no connection between
 * them -- so there is no network to route on. A bearing cannot say which
 * turn to take, but it is never wrong about which way the point is.
 *
 * THE FILE is the original's /waypoints.bin, byte for byte, so a card
 * that held points under the original keeps them: an 8-byte header
 * ("WPT1" as a little-endian u32, version 1, a u16 count) and a 48-byte
 * record per point -- lat and lon as doubles, a 24-byte name, the time
 * it was saved as an int64 (0 when the clock was not set). Little-endian
 * throughout, as the ESP32 laid the original's struct out. It is written
 * whole on every change: 32 points is under 2 KB, and a torn write then
 * costs the list rather than its meaning.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* src: original/waypoints.h WP_MAX and WP_NAME_MAX. */
#define WP_MAX          (32)
#define WP_NAME_MAX     (24)
/* src: original/waypoints.cpp: header and record sizes as written. */
#define WP_HEAD_BYTES   (8)
#define WP_REC_BYTES    (48)
#define WP_FILE_MAX     (WP_HEAD_BYTES + WP_MAX * WP_REC_BYTES)

typedef struct {
    double  lat, lon;
    char    name[WP_NAME_MAX];  /* always terminated */
    int64_t saved_utc;          /* seconds since 1970, 0 if unknown */
} wp_point_t;

typedef struct {
    wp_point_t p[WP_MAX];
    int        n;
    int        target;          /* -1 for none */
} wp_list_t;

void wp_init(wp_list_t *l);

/*
 * Read a file's bytes into the list. A header that is not the original's
 * leaves it empty; a short file keeps the whole records it has, since
 * losing the last pin is a nuisance and losing the other thirty-one is a
 * different thing (original wp_begin()). Returns how many were read.
 */
int  wp_load(wp_list_t *l, const uint8_t *buf, size_t len);

/* The list as file bytes into `buf` (WP_FILE_MAX is enough); the length. */
size_t wp_save(const wp_list_t *l, uint8_t *buf, size_t cap);

/*
 * Add a point. With no name, one is made: "HH:MM" (UTC) from `utc` when
 * the clock is set, else "pin N". The new index, or -1 when full.
 */
int  wp_add(wp_list_t *l, double lat, double lon, const char *name, int64_t utc);

/* Remove point i, keeping the target on the same point (or clearing it,
 * if it was this one). */
bool wp_remove(wp_list_t *l, int i);

/* Out of range clears it. */
void wp_set_target(wp_list_t *l, int i);
int  wp_target(const wp_list_t *l);

/* Great-circle distance in metres and initial bearing in degrees true,
 * on a sphere of the WGS84 mean radius (original's haversine). */
double wp_distance_m(double lat1, double lon1, double lat2, double lon2);
double wp_bearing_deg(double lat1, double lon1, double lat2, double lon2);

/*
 * "home: 1.4 km NE", "car: 230 m N", or "car: here (12 m)" under 30 m,
 * where a consumer receiver's bearing is noise (original
 * wp_target_text()). "" with no target.
 */
void wp_target_text(const wp_list_t *l, double lat, double lon, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

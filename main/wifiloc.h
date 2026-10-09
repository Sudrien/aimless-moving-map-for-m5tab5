/*
 * wifiloc.h -- a position without sky, from the access points the device
 * hears, matched against where it heard them before (0035).
 *
 * original/wifiloc.cpp's table, learning, locating, file rows and scan
 * timing, with no radio, file or clock in it, so it runs on the host.
 * aimless.c owns the task that scans (wifi_scan_list_quiet(), which
 * blocks for seconds) and the file on the card.
 *
 * Every access point heard with a good fix is folded into a running
 * centroid: where, on average, this device was when it heard it. Over
 * enough travel that is a rough map of the radio along the routes taken,
 * and with no sky a scan can be matched against it and the heard
 * centroids averaged, weighted by received power.
 *
 * Not a fix. The centroids are where this device stood, not where the
 * transmitters are -- biased onto the roads it travels -- and the
 * accuracy is the spread of the centroids used, how much they disagree,
 * not a confidence interval. The map shows the result as an estimate and
 * nothing that wants a real fix takes it.
 *
 * Centroids, not trilateration: a distance from RSSI needs a path-loss
 * exponent and a transmit power, neither known, and 6 dB -- a body in the
 * way -- doubles it. Received power as a weight claims nothing more.
 *
 * Free of ESP-IDF.
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

/* src: original/wifiloc.cpp WIFILOC_MAX: 16384 records, 768 KB, several
 * hundred km of urban driving. The cap is the table being flat and
 * rewritten whole, not memory. */
#define WIFILOC_MAX             (16384)
/* src: original/wifiloc.cpp WIFILOC_MOBILE_M, chosen: heard across more
 * than 400 m is not in a fixed place -- a hotspot, a bus. Over a fixed
 * AP's genuine spread from both ends of its range on an open road. */
#define WIFILOC_MOBILE_M        (400.0)
/* src: original/wifiloc.cpp WIFILOC_MIN_OBS: fewer is one or two passes
 * down one side, displaced by most of the range. Kept, not used. */
#define WIFILOC_MIN_OBS         (3)
/* src: original/wifiloc.cpp WIFILOC_MIN_APS: fewer is nearness to one AP,
 * not a position. */
#define WIFILOC_MIN_APS         (3)
/* src: original/wifiloc.cpp WIFILOC_MIN_RSSI: below about -88 dBm the
 * reading is mostly noise. */
#define WIFILOC_MIN_RSSI        (-88)
/* src: original/wifiloc.cpp locate()'s hit[32]. */
#define WIFILOC_HITS            (32)

/* src: original/wifiloc.cpp WIFILOC_LEARN_MS, WIFILOC_LEARN_M,
 * WIFILOC_LEARN_MAX_KMH (GNSS_MOVING_KMH, a judgement), WIFILOC_LOST_MS,
 * WIFILOC_RETRY_MS, WIFILOC_STALE_MS. Learn every 20 s, 40 m apart, under
 * 12 km/h -- a scan smears an observation along the track by speed times
 * its length, and the extent the mobility test reads never shrinks.
 * Locate after 15 s without a fix, so a bridge is not a garage, and every
 * 20 s after. An estimate over a minute old is not a position. */
#define WIFILOC_LEARN_MS        (20000u)
#define WIFILOC_LEARN_M         (40.0)
#define WIFILOC_LEARN_MAX_KMH   (12.0)
#define WIFILOC_LOST_MS         (15000u)
#define WIFILOC_RETRY_MS        (20000u)
#define WIFILOC_STALE_MS        (60000u)
/* src: original/wifiloc.cpp WIFILOC_WRITE_MS, WIFILOC_WRITE_DIRTY: the
 * table is rewritten whole, a second at 16k, so every three minutes, or
 * sooner after 400 changes -- a dense drive adds hundreds in minutes. */
#define WIFILOC_WRITE_MS        (180000u)
#define WIFILOC_WRITE_DIRTY     (400u)

/* One access point heard: the scan's, without the radio's types. */
typedef struct {
    uint8_t bssid[6];
    int8_t  rssi;
} wifiloc_ap_t;

/* src: original/wifiloc.cpp ApRec, 48 bytes. */
typedef struct {
    uint64_t id;                /* the BSSID, big-endian in the low 48 bits */
    uint32_t n;                 /* observations */
    double   sum_lat, sum_lon;  /* running sums; the centroid is sum / n */
    float    min_lat, max_lat;  /* extent, for the mobility test */
    float    min_lon, max_lon;
    int8_t   best_rssi;
    uint8_t  mobile;            /* sticky: once judged mobile, excluded */
    uint8_t  pad[2];
} wifiloc_rec_t;

typedef struct {
    wifiloc_rec_t *tab;
    uint32_t       cap, count;
    uint32_t       dirty;       /* changes since the last write */
} wifiloc_db_t;

void wifiloc_init(wifiloc_db_t *db, wifiloc_rec_t *tab, uint32_t cap);

/*
 * Fold a scan heard at (lat, lon) into the table: new records for new
 * access points while there is room, and a record heard over more than
 * WIFILOC_MOBILE_M after WIFILOC_MIN_OBS judged mobile, for good -- one
 * that flipped back would contribute the very observations that proved it
 * mobile. Counts in *added, *folded, *mobile.
 */
void wifiloc_learn(wifiloc_db_t *db, const wifiloc_ap_t *aps, int n,
                   double lat, double lon, int *added, int *folded, int *mobile);

typedef struct {
    double lat, lon;
    float  acc_m;               /* the weighted spread of the centroids used */
    int    used;                /* access points it rests on */
} wifiloc_est_t;

/*
 * An estimate from a scan: the received-power-weighted mean of the known,
 * fixed, well-observed access points heard, at most WIFILOC_HITS. False,
 * with est->used set, under WIFILOC_MIN_APS.
 */
bool wifiloc_locate(const wifiloc_db_t *db, const wifiloc_ap_t *aps, int n,
                    wifiloc_est_t *est);

/* ---- the file ----
 *
 * src: original/wifiloc.h THE FILE: CSV, a header line, then a row per
 * access point -- bssid,obs,lat,lon,min_lat,max_lat,min_lon,max_lon,
 * best_rssi,mobile -- with the centroid, not the sums. The BSSIDs as
 * heard: everything on the card is a record of where the device has
 * been, and hashing one file of it protected nothing. */
extern const char WIFILOC_HEADER[];

/* Is `line` the header? */
bool wifiloc_header_ok(const char *line);

/* Add the record in `line`; false for a row that is not one, which is
 * skipped rather than ending the load. */
bool wifiloc_parse(wifiloc_db_t *db, const char *line);

/* Record `i` as a row, no newline; its length, or -1 if `cap` is short. */
int  wifiloc_format(const wifiloc_db_t *db, uint32_t i, char *out, size_t cap);

/* Whether to write: something changed, and WIFILOC_WRITE_DIRTY changes or
 * WIFILOC_WRITE_MS since the last write (`written` false for none yet). */
bool wifiloc_write_due(const wifiloc_db_t *db, uint32_t now_ms, uint32_t last_ms, bool written);

/* ---- when to scan ---- */

typedef enum { WIFILOC_IDLE = 0, WIFILOC_LEARN, WIFILOC_LOCATE } wifiloc_why_t;

typedef struct {
    uint32_t last_learn, last_locate, lost_since;
    bool     learned, located, lost;
    double   learn_lat, learn_lon;
} wifiloc_plan_t;

/*
 * Whether to scan now, and why, as the original's poll did: LEARN with a
 * fine fix under WIFILOC_LEARN_MAX_KMH, WIFILOC_LEARN_MS since the last
 * and WIFILOC_LEARN_M away from it; LOCATE with no fix at all for
 * WIFILOC_LOST_MS, WIFILOC_RETRY_MS since the last, and a table to match
 * against. A coarse fix does neither. The plan is updated as if the scan
 * started: call only when one can be made.
 */
wifiloc_why_t wifiloc_next(wifiloc_plan_t *p, uint32_t now_ms, bool fine, bool coarse,
                           double kmh, double lat, double lon, bool have_records);

/*
 * Whether a learning scan begun at (lat0, lon0) at `t0_ms` may be folded
 * in at the fix now: still fine, and moved no further than the elapsed
 * time at WIFILOC_LEARN_MAX_KMH allows, plus 10 m. Otherwise it is thrown
 * away whole -- the sums cannot be taken back. src: original/wifiloc.cpp
 * wifiloc_poll_inner().
 */
bool wifiloc_keep(double lat0, double lon0, uint32_t t0_ms, uint32_t now_ms,
                  bool fine, double lat, double lon);

#ifdef __cplusplus
}
#endif

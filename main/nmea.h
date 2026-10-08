/*
 * nmea.h -- the GNSS module's NMEA sentences parsed into a fix.
 *
 * original/gnss.cpp's parser, "unchanged from the working sketch" there
 * and unchanged in substance here: splitFields(), nmeaCoord(),
 * conIndex() and parseSentence() as C, with GnssFix a plain struct and
 * millis() passed in. Header-only and free of ESP-IDF, so
 * test/nmeatest.c checks it on sentences with real checksums.
 *
 * What is read, and from where:
 *
 *   RMC  status (A valid, V not), UTC time and date, and -- only when
 *        valid -- latitude and longitude
 *   VTG  course over ground and speed in km/h
 *   GGA  satellites used, HDOP, altitude
 *   GSA  fix mode: 1 none, 2 2D, 3 3D
 *   GSV  per constellation: satellites in view, best and worst SNR of
 *        those actually tracked
 *
 * The checksum after '*' is not verified, as it was not in the
 * original: the module is on a short wire, and a sentence that fails is
 * replaced a second later. test/ checks that a sentence is still parsed
 * correctly with its checksum present.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    int visible;
    int best_snr;
    int worst_snr;
} gnss_constellation_t;

typedef struct {
    char     status;            /* RMC: 'A' valid, 'V' not */
    int      mode;              /* GSA: 1 none, 2 2D, 3 3D */
    int      sats;
    double   lat, lon, altitude, hdop;
    double   speed_kmh, course;
    char     utc[16], date[16];
    uint32_t last_sentence_ms;  /* when the last sentence was parsed */
    gnss_constellation_t cons[4];
} gnss_fix_t;

/* As GnssFix's member initialisers: no fix, HDOP 99.99. */
static inline void gnss_fix_init(gnss_fix_t *f)
{
    memset(f, 0, sizeof(*f));
    f->status = 'V';
    f->mode = 1;
    f->hdop = 99.99;
    f->cons[0].name = "GPS";
    f->cons[1].name = "GLO";
    f->cons[2].name = "GAL";
    f->cons[3].name = "BDS";
}

/* A position, of any quality. */
static inline bool gnss_coarse(const gnss_fix_t *f) { return f->status == 'A'; }

/* A 3D fix with HDOP under 2.5: good enough to draw the marker solid. */
static inline bool gnss_fine(const gnss_fix_t *f)
{
    return f->status == 'A' && f->mode == 3 && f->hdop > 0 && f->hdop < 2.5;
}

static inline int nmea_split(char *s, char **f, int maxf)
{
    int n = 0;
    f[n++] = s;
    for (char *p = s; *p && n < maxf; p++) {
        if (*p == ',') { *p = 0; f[n++] = p + 1; }
        else if (*p == '*') { *p = 0; break; }
    }
    return n;
}

/* ddmm.mmmm or dddmm.mmmm with a hemisphere: integer-dividing by 100
 * strips whichever degree field is there. */
static inline double nmea_coord(const char *v, const char *h)
{
    if (!v || !*v) return 0;
    const double raw = atof(v);
    const int deg = (int)(raw / 100);
    double d = deg + (raw - deg * 100) / 60.0;
    if (h && (*h == 'S' || *h == 'W')) d = -d;
    return d;
}

static inline int nmea_constellation(const char *talker)
{
    if (!strncmp(talker, "GP", 2)) return 0;
    if (!strncmp(talker, "GL", 2)) return 1;
    if (!strncmp(talker, "GA", 2)) return 2;
    if (!strncmp(talker, "GB", 2)) return 3;
    return -1;
}

/* Parse one sentence, '$' to end, without its line ending. Modifies `s`.
 * Returns true if it was a sentence at all. */
static inline bool nmea_parse(char *s, gnss_fix_t *fix, uint32_t now_ms)
{
    if (s[0] != '$') return false;
    fix->last_sentence_ms = now_ms;
    const char talker[3] = { s[1], s[2], 0 };
    const char type[4]   = { s[3], s[4], s[5], 0 };
    char *f[24];
    const int n = nmea_split(s, f, 24);

    if (!strcmp(type, "RMC") && n > 9) {
        fix->status = f[2][0] ? f[2][0] : 'V';
        strncpy(fix->utc, f[1], sizeof(fix->utc) - 1);
        strncpy(fix->date, f[9], sizeof(fix->date) - 1);
        if (fix->status == 'A') {
            fix->lat = nmea_coord(f[3], f[4]);
            fix->lon = nmea_coord(f[5], f[6]);
        }
    } else if (!strcmp(type, "VTG") && n > 7) {
        fix->course    = atof(f[1]);
        fix->speed_kmh = atof(f[7]);
    } else if (!strcmp(type, "GGA") && n > 9) {
        fix->sats = atoi(f[7]);
        fix->hdop = f[8][0] ? atof(f[8]) : 99.99;
        fix->altitude = atof(f[9]);
    } else if (!strcmp(type, "GSA") && n > 17) {
        const int m = atoi(f[2]);
        if (m > fix->mode || m == 1) fix->mode = m;
    } else if (!strcmp(type, "GSV") && n >= 4) {
        const int ci = nmea_constellation(talker);
        if (ci >= 0) {
            if (atoi(f[2]) == 1) {
                fix->cons[ci].best_snr  = 0;
                fix->cons[ci].worst_snr = 0;
            }
            fix->cons[ci].visible = atoi(f[3]);
            for (int i = 4; i + 3 < n; i += 4) {
                /* An empty SNR is a satellite in view and not tracked; it
                 * must not count as 0 dB. */
                if (!f[i + 3][0]) continue;
                const int snr = atoi(f[i + 3]);
                if (snr <= 0) continue;
                if (snr > fix->cons[ci].best_snr) fix->cons[ci].best_snr = snr;
                if (!fix->cons[ci].worst_snr || snr < fix->cons[ci].worst_snr)
                    fix->cons[ci].worst_snr = snr;
            }
        }
    }
    return true;
}

/* UBX's Fletcher checksum over class, id, length and payload. */
static inline void ubx_checksum(const uint8_t *b, size_t n, uint8_t *a, uint8_t *k)
{
    uint8_t ca = 0, ck = 0;
    for (size_t i = 0; i < n; i++) { ca += b[i]; ck += ca; }
    *a = ca; *k = ck;
}

#ifdef __cplusplus
}
#endif

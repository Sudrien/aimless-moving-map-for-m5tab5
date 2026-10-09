/*
 * trust.c -- see trust.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "trust.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Not in C11's <math.h>. */
#ifndef M_PI
#define M_PI (3.14159265358979323846)
#endif

void trust_reset(trust_t *t)
{
    memset(t, 0, sizeof(*t));
    t->level = TRUST_UNKNOWN;
}

static void flag(trust_t *t, uint32_t bit, uint32_t now_ms)
{
    for (int i = 0; i < TRUST_FLAGS; i++)
        if (bit == (1u << i)) t->flag_ms[i] = now_ms;
    t->flags |= bit;
}

static void expire(trust_t *t, uint32_t now_ms)
{
    for (int i = 0; i < TRUST_FLAGS; i++)
        if ((t->flags & (1u << i)) && now_ms - t->flag_ms[i] > TRUST_HOLD_MS)
            t->flags &= ~(1u << i);
}

/* src: original/gpstrust.cpp dist_m(): equirectangular, the distances
 * short and the thresholds wide. 111320 m a degree of latitude. */
static double dist_m(double lat1, double lon1, double lat2, double lon2)
{
    const double mlat = (lat1 + lat2) * 0.5 * M_PI / 180.0;
    const double dx = (lon2 - lon1) * 111320.0 * cos(mlat);
    const double dy = (lat2 - lat1) * 111320.0;
    return sqrt(dx * dx + dy * dy);
}

static int two(const char *s) { return (s[0] - '0') * 10 + (s[1] - '0'); }

static bool digits(const char *s, int n)
{
    for (int i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

int64_t trust_gnss_epoch(const char *utc, const char *date)
{
    if (!utc || !date || strlen(utc) < 6 || strlen(date) < 6) return 0;
    if (!digits(utc, 6) || !digits(date, 6)) return 0;
    const int h = two(utc), mi = two(utc + 2), s = two(utc + 4);
    const int d = two(date), mo = two(date + 2), yy = two(date + 4);
    if (h > 23 || mi > 59 || s > 60 || d < 1 || d > 31 || mo < 1 || mo > 12) return 0;
    /* src: original/gpstrust.cpp: no century in NMEA; 80 the pivot. */
    const int y = yy < 80 ? 2000 + yy : 1900 + yy;
    /* Days from the civil date (Howard Hinnant's days_from_civil). */
    const int yr = mo <= 2 ? y - 1 : y;
    const int era = yr / 400;
    const int yoe = yr - era * 400;
    const int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

/* Between this solution and the last: the jump, the speed, the frozen
 * altitude. */
static void between(trust_t *t, const gnss_fix_t *fix, uint32_t now_ms)
{
    const double m = dist_m(t->prev_lat, t->prev_lon, fix->lat, fix->lon);
    const double dt = (double)(now_ms - t->prev_ms) / 1000.0;
    /* src: original/gpstrust.cpp: under 250 ms apart is too close to say. */
    if (dt > 0.25) {
        const double kmh = m / dt * 3.6;
        if (kmh > TRUST_MAX_KMH) flag(t, TRUST_F_JUMP, now_ms);
        /* Doppler against geometry: different measurements, and a
         * transmitter has to fake both consistently. */
        if (fix->speed_kmh > TRUST_SPEED_MIN_KMH || kmh > TRUST_SPEED_MIN_KMH) {
            const double big = fix->speed_kmh > kmh ? fix->speed_kmh : kmh;
            const double small = fix->speed_kmh > kmh ? kmh : fix->speed_kmh;
            if (big > 0 && (big - small) / big > TRUST_SPEED_TOL) flag(t, TRUST_F_SPEED, now_ms);
        }
    }
    /* Some simulators hold the height constant: varying it convincingly
     * is work. */
    if (fabs(fix->altitude - t->prev_alt) < 0.01 && m > TRUST_ALT_FREEZE_MOVE_M) {
        if (++t->alt_same > TRUST_ALT_FREEZE_N) flag(t, TRUST_F_ALT, now_ms);
    } else {
        t->alt_same = 0;
    }
}

bool trust_update(trust_t *t, const gnss_fix_t *fix, uint32_t now_ms,
                  int64_t rtc_epoch, uint32_t pps_ms, const trust_wifi_t *wifi)
{
    const trust_level_t was = t->level;
    expire(t, now_ms);

    if (fix->status != 'A') {
        /* Not suspicious, but the history is stale: the next fix may be
         * anywhere, and the jump check must not reach across the gap. */
        t->have_prev = false;
        t->alt_same = 0;
        if (!t->flags) t->level = TRUST_UNKNOWN;
        return t->level != was;
    }

    const bool fresh = !t->have_prev || strcmp(fix->utc, t->prev_utc) != 0;
    if (fresh) {
        if (t->have_prev) between(t, fix, now_ms);

        if (fix->altitude < TRUST_ALT_MIN_M || fix->altitude > TRUST_ALT_MAX_M)
            flag(t, TRUST_F_ALT, now_ms);

        /* Against the RTC, which is set from NTP and never from GNSS, so
         * this is not a number compared with itself. */
        const int64_t g = trust_gnss_epoch(fix->utc, fix->date);
        if (g > 0 && rtc_epoch > TRUST_RTC_MIN_EPOCH) {
            const int64_t d = g > rtc_epoch ? g - rtc_epoch : rtc_epoch - g;
            if (d > TRUST_CLOCK_TOL_S) flag(t, TRUST_F_CLOCK, now_ms);
        }

        /* Across every tracked satellite, strongest against weakest:
         * the original's first version compared each constellation's
         * strongest, which agree on any healthy sky, and fired always. */
        int sats = 0, best = 0, worst = 0, seen = 0;
        for (int i = 0; i < 4; i++) {
            sats += fix->cons[i].visible;
            if (!fix->cons[i].visible || !fix->cons[i].best_snr) continue;
            seen++;
            if (fix->cons[i].best_snr > best) best = fix->cons[i].best_snr;
            if (fix->cons[i].worst_snr && (!worst || fix->cons[i].worst_snr < worst))
                worst = fix->cons[i].worst_snr;
        }
        if (seen >= 2 && worst && sats >= TRUST_SNR_MIN_SATS && best - worst < TRUST_SNR_MIN_SPREAD)
            flag(t, TRUST_F_SNR, now_ms);

        /* Below 3D the pulse is not disciplined to a solution, and says
         * nothing; 0 is no pulse seen, perhaps not wired. */
        if (fix->mode >= 3 && pps_ms && (pps_ms < TRUST_PPS_LO_MS || pps_ms > TRUST_PPS_HI_MS))
            flag(t, TRUST_F_PPS, now_ms);

        /* Wi-Fi: only a fresh estimate from enough access points. */
        if (wifi && wifi->used >= TRUST_WIFI_MIN_APS && wifi->age_ms < TRUST_WIFI_MAX_AGE_MS &&
            dist_m(fix->lat, fix->lon, wifi->lat, wifi->lon) > TRUST_WIFI_MAX_M + wifi->acc_m)
            flag(t, TRUST_F_WIFI, now_ms);

        snprintf(t->prev_utc, sizeof(t->prev_utc), "%s", fix->utc);
        t->prev_lat = fix->lat;
        t->prev_lon = fix->lon;
        t->prev_alt = fix->altitude;
        t->prev_ms = now_ms;
        t->have_prev = true;
    }

    int n = 0;
    for (int i = 0; i < TRUST_FLAGS; i++) n += (t->flags >> i) & 1u;
    t->level = n == 0 ? TRUST_OK : n == 1 ? TRUST_ODD : TRUST_BAD;
    return t->level != was;
}

void trust_text(const trust_t *t, char *out, size_t cap)
{
    /* src: original/gpstrust.cpp: named, not scored -- "wifi disagrees"
     * says what to go and check, "position disputed" nothing. */
    static const char *const NAMES[TRUST_FLAGS] = {
        "jump", "speed", "clock", "sat SNR", "PPS", "wifi disagrees", "altitude",
    };
    if (!cap) return;
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < TRUST_FLAGS; i++) {
        if (!(t->flags & (1u << i))) continue;
        const int k = snprintf(out + used, cap - used, "%s%s", used ? ", " : "", NAMES[i]);
        if (k < 0 || (size_t)k >= cap - used) break;
        used += (size_t)k;
    }
}

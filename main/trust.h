/*
 * trust.h -- consistency checks on the GNSS solution (0033).
 *
 * original/gpstrust.cpp, with no clock, RTC or receiver in it, so it runs
 * on the host. aimless.c feeds it each fix with the RTC's time and the
 * PPS interval, and colours the status line by what it says.
 *
 * A receiver believes whatever reaches its antenna: a transmitter nearby,
 * or a recording replayed, gives a solution with good checksums, a good
 * HDOP and plenty of satellites. What it cannot easily do is stay
 * consistent with everything else the device knows. Each check is one of
 * those contradictions.
 *
 * Not detection, and it refuses nothing. Every check has an innocent
 * cause far commoner than an attack -- a tunnel exit is a jump, a cold
 * reacquisition a clock step, an urban canyon velocity nonsense -- so one
 * flag is ordinary and several at once is the interesting case. The map
 * draws regardless: a device that stopped when suspicious would fail
 * under every bridge.
 *
 * Free of ESP-IDF.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nmea.h"

#ifdef __cplusplus
extern "C" {
#endif

/* src: original/gpstrust.h TrustLevel. */
typedef enum {
    TRUST_UNKNOWN = 0,          /* nothing to say: no fix yet */
    TRUST_OK,                   /* every check that could run passed */
    TRUST_ODD,                  /* one held: one fails all the time */
    TRUST_BAD,                  /* several at once */
} trust_level_t;

/* src: original/gpstrust.h, the same bits. Wi-Fi's is kept, unset: there
 * is no Wi-Fi positioning to check against yet. */
enum {
    TRUST_F_JUMP  = 1 << 0,     /* moved faster than physics allows */
    TRUST_F_SPEED = 1 << 1,     /* Doppler speed disagrees with movement */
    TRUST_F_CLOCK = 1 << 2,     /* GNSS time disagrees with the RTC */
    TRUST_F_SNR   = 1 << 3,     /* signal strengths implausibly even */
    TRUST_F_PPS   = 1 << 4,     /* the pulse is not at 1 Hz */
    TRUST_F_WIFI  = 1 << 5,     /* reserved */
    TRUST_F_ALT   = 1 << 6,     /* altitude impossible or frozen */
};
#define TRUST_FLAGS     (7)

/*
 * Thresholds, every one the original's judgement (original/gpstrust.cpp,
 * PROVENANCE.md), set where an innocent cause is already unlikely: an
 * amber word on every drive would teach the reader to ignore it.
 *
 * 400 km/h between solutions: faster than any road vehicle, slower than
 * a jump to a spoofer's chosen place. Doppler against movement only above
 * 15 km/h, where movement is more than noise, and only past a 50 %
 * disagreement. GNSS time 180 s from the RTC, which drifts seconds a day
 * and is set from NTP, never from GNSS: replaying shows as time behind.
 * PPS outside 900-1100 ms with a 3D fix. With 8 or more satellites in two
 * or more constellations, less than 4 dB from strongest to weakest: one
 * transmitter lights every channel from one direction at one power.
 * Altitude below -450 m or above 12 km, or the same to 1 cm for over 30
 * solutions while moving over 20 m each. A flag holds 8 s, or the line
 * flickers faster than it can be read.
 */
#define TRUST_MAX_KMH           (400.0)
#define TRUST_SPEED_MIN_KMH     (15.0)
#define TRUST_SPEED_TOL         (0.5)
#define TRUST_CLOCK_TOL_S       (180)
#define TRUST_PPS_LO_MS         (900u)
#define TRUST_PPS_HI_MS         (1100u)
#define TRUST_SNR_MIN_SATS      (8)
#define TRUST_SNR_MIN_SPREAD    (4)
#define TRUST_ALT_MIN_M         (-450.0)
#define TRUST_ALT_MAX_M         (12000.0)
#define TRUST_ALT_FREEZE_N      (30)
#define TRUST_ALT_FREEZE_MOVE_M (20.0)
#define TRUST_HOLD_MS           (8000u)
/* src: original/gpstrust.cpp: an RTC reading before 2026-01-01 has never
 * been set -- a flat battery, not an attack. */
#define TRUST_RTC_MIN_EPOCH     (1767225600LL)

typedef struct {
    /* The last solution, to compare the next with. */
    bool     have_prev;
    char     prev_utc[16];
    double   prev_lat, prev_lon, prev_alt;
    uint32_t prev_ms;
    int      alt_same;
    /* What is held, and since when. */
    uint32_t flags;
    uint32_t flag_ms[TRUST_FLAGS];
    trust_level_t level;
} trust_t;

void trust_reset(trust_t *t);

/*
 * The fix as it stands at `now_ms`; `rtc_epoch` the RTC's time (0 if it
 * cannot vouch for one), `pps_ms` the last pulse interval (0 for none).
 *
 * Called as often as the caller likes. The checks between solutions run
 * once per new one -- its UTC field changed -- and are timed by when each
 * arrived. The original ran on every pass of a loop going at about
 * 200 Hz with the same fix, so its jump and speed checks, which skip
 * anything under 250 ms, ran only after a stall, and its frozen-altitude
 * check, which wants 20 m between calls, never did. Returns true if the
 * level changed.
 */
bool trust_update(trust_t *t, const gnss_fix_t *fix, uint32_t now_ms,
                  int64_t rtc_epoch, uint32_t pps_ms);

/* The flags named, ", " between: "jump, clock". Empty with none. */
void trust_text(const trust_t *t, char *out, size_t cap);

/* GNSS time from RMC's hhmmss and ddmmyy, as a Unix epoch; 0 if either
 * is missing or malformed. UTC throughout: the original used mktime(),
 * local time, on both sides. */
int64_t trust_gnss_epoch(const char *utc, const char *date);

#ifdef __cplusplus
}
#endif

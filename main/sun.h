/*
 * sun.h -- whether the sun is up, from a position and a UTC time.
 *
 * The original switched between its day and night palettes by the sun's
 * actual position, not a clock (original/README.md "Day and night"), and
 * dimmed the backlight with it, with a step between for the half hour
 * either side of sunrise and sunset. It computed the crossings with
 * buelowp/sunset, an Arduino library. This is the same question answered
 * by the sunrise equation in C, with no library, so it can be tested on
 * the host:
 *
 *   the solar transit for the day, from the mean anomaly and the
 *   equation of the centre; the declination from the ecliptic longitude;
 *   the hour angle at which the sun's centre is 0.833 degrees below the
 *   horizon (refraction and the sun's radius). About a minute from
 *   NOAA's figures at the latitudes people live at; the test holds it to
 *   three of astral's.
 *
 * WHAT IS KEPT FROM THE ORIGINAL's sunIsUpAt() and minutesToTwilight():
 * minutes past midnight UTC, normalised to a day, so a sunset past UTC
 * midnight -- every evening in the Americas -- wraps rather than reading
 * as night for the hours after 00:00Z; and the circular distance to the
 * nearest crossing.
 *
 * WHAT DIFFERS: buelowp/sunset gave the same time for rise and set at a
 * latitude with no crossing that day and could not say which, so the
 * original called both polar day. The equation here can tell them
 * apart, and polar night is night.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SUN_CROSSES = 0,    /* rises and sets this day */
    SUN_ALWAYS_UP,      /* polar day */
    SUN_ALWAYS_DOWN,    /* polar night */
} sun_kind_t;

typedef struct {
    sun_kind_t kind;
    double rise_min, set_min;   /* minutes past 00:00 UTC, 0 <= m < 1440 */
} sun_day_t;

/* The day's sunrise and sunset at (lat, lon), east positive, for the UTC
 * date y-m-d. */
void sun_day(double lat, double lon, int y, int m, int d, sun_day_t *out);

/* Whether the sun is up at `now_min` minutes past 00:00 UTC. */
bool sun_up(const sun_day_t *s, double now_min);

/* Minutes to the nearer of sunrise and sunset, on a circular day; -1
 * when there is no crossing. */
double sun_to_crossing(const sun_day_t *s, double now_min);

/*
 * The date and time from RMC's fields: `date` ddmmyy, `utc` hhmmss with
 * any fraction. False if either is not that, or the date is not one.
 * Years are 20yy: RMC carries two digits and this receiver is from this
 * century.
 */
bool sun_from_nmea(const char *date, const char *utc, int *y, int *m, int *d,
                   double *now_min);

#ifdef __cplusplus
}
#endif

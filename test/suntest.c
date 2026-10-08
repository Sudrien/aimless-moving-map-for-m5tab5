/*
 * suntest.c -- main/sun.c: sunrise and sunset, and the day/night
 * question the palette asks of them.
 *
 * The reference times are astral 3.2's (Python), not this code's:
 *
 *   from astral import Observer; from astral.sun import sunrise, sunset
 *   sunrise(Observer(lat, lon, 0), date(y, m, d), tzinfo=timezone.utc)
 *
 * astral gives the events that fall on that UTC date; sun_day() gives the
 * ones around that date's local noon, which for a place far from
 * Greenwich can be the neighbouring day's. A day apart they differ by a
 * minute or two at most, so both are held to three.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "sun.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { const char *name; double lat, lon; int y, m, d; double rise, set; } ref_t;

static const ref_t REF[] = {
    { "London",      51.5074,   -0.1278, 2024,  6, 21,  223.6, 1221.3 },
    { "London",      51.5074,   -0.1278, 2024, 12, 21,  484.4,  953.3 },
    { "Los Angeles", 34.0522, -118.2437, 2024,  3, 15,  842.7,  120.2 },
    { "Sydney",     -33.8688,  151.2093, 2024,  7,  1, 1261.2,  417.0 },
    { "Quito",       -0.1807,  -78.4678, 2024,  9, 22,  663.3, 1389.4 },
    { "Auckland",   -36.8485,  174.7633, 2025,  1, 10, 1034.2,  463.1 },
    { "Honolulu",    21.3069, -157.8583, 2026, 10,  8,  985.0,  253.3 },
    { "Canton OH",   40.7989,  -81.3784, 2026, 10,  8,  689.6, 1375.7 },
    { "Reykjavik",   64.1466,  -21.9426, 2026,  5,  1,  300.6, 1311.4 },
};

/* Circular difference in minutes. */
static double diff(double a, double b)
{
    double x = fabs(a - b);
    return x > 720.0 ? 1440.0 - x : x;
}

int main(void)
{
    printf("crossings within three minutes of astral's\n");
    for (size_t i = 0; i < sizeof REF / sizeof REF[0]; i++) {
        const ref_t *r = &REF[i];
        sun_day_t s;
        sun_day(r->lat, r->lon, r->y, r->m, r->d, &s);
        CHECK(s.kind == SUN_CROSSES, "%s: kind %d", r->name, s.kind);
        if (getenv("SUN_SHOW")) printf("  %-12s %+5.1f %+5.1f\n", r->name, s.rise_min - r->rise, s.set_min - r->set);
        CHECK(diff(s.rise_min, r->rise) <= 3.0 && diff(s.set_min, r->set) <= 3.0,
              "%s %d-%02d-%02d: rise %.1f (want %.1f), set %.1f (want %.1f)", r->name,
              r->y, r->m, r->d, s.rise_min, r->rise, s.set_min, r->set);
        CHECK(s.rise_min >= 0 && s.rise_min < 1440 && s.set_min >= 0 && s.set_min < 1440,
              "%s: not normalised", r->name);
    }

    printf("up or down, including across UTC midnight\n");
    {
        sun_day_t s;
        sun_day(51.5074, -0.1278, 2024, 6, 21, &s);          /* London, midsummer */
        CHECK(sun_up(&s, 12 * 60), "London noon");
        CHECK(!sun_up(&s, 1 * 60), "London 01:00Z");
        CHECK(!sun_up(&s, 22 * 60), "London 22:00Z");
        sun_day(34.0522, -118.2437, 2024, 3, 15, &s);        /* LA: sets ~02:00Z */
        CHECK(s.set_min < s.rise_min, "LA should span UTC midnight");
        CHECK(sun_up(&s, 0 * 60 + 30), "LA 00:30Z is late afternoon");
        CHECK(sun_up(&s, 20 * 60), "LA 20:00Z is noon");
        CHECK(!sun_up(&s, 5 * 60), "LA 05:00Z is night");
        CHECK(!sun_up(&s, 13 * 60), "LA 13:00Z is before dawn");
        CHECK(sun_up(&s, 1440 + 20 * 60) == sun_up(&s, 20 * 60), "minutes past a day are wrapped");
    }

    printf("polar day and polar night are told apart\n");
    {
        sun_day_t s;
        sun_day(69.6492, 18.9553, 2024, 6, 21, &s);           /* Tromso, June */
        CHECK(s.kind == SUN_ALWAYS_UP && sun_up(&s, 0) && sun_up(&s, 720), "Tromso June: %d", s.kind);
        CHECK(sun_to_crossing(&s, 0) < 0, "no crossing to be near");
        sun_day(69.6492, 18.9553, 2024, 12, 21, &s);          /* Tromso, December */
        CHECK(s.kind == SUN_ALWAYS_DOWN && !sun_up(&s, 720), "Tromso December: %d", s.kind);
        sun_day(-77.85, 166.67, 2024, 6, 21, &s);             /* McMurdo, June */
        CHECK(s.kind == SUN_ALWAYS_DOWN, "McMurdo June: %d", s.kind);
    }

    printf("distance to the nearest crossing is circular\n");
    {
        const sun_day_t s = { SUN_CROSSES, 10.0, 600.0 };
        CHECK(fabs(sun_to_crossing(&s, 1430.0) - 20.0) < 1e-9, "23:50 to a 00:10 sunrise");
        CHECK(fabs(sun_to_crossing(&s, 590.0) - 10.0) < 1e-9, "ten to sunset");
        CHECK(fabs(sun_to_crossing(&s, 300.0) - 290.0) < 1e-9, "midday");
    }

    printf("RMC's date and time\n");
    {
        int y, m, d;
        double t;
        CHECK(sun_from_nmea("081026", "235959.00", &y, &m, &d, &t) &&
              y == 2026 && m == 10 && d == 8 && fabs(t - (23 * 60 + 59 + 59 / 60.0)) < 1e-9, "normal");
        CHECK(sun_from_nmea("010100", "000000", &y, &m, &d, &t) && y == 2000 && t == 0, "no fraction");
        CHECK(!sun_from_nmea("", "120000", &y, &m, &d, &t), "no date");
        CHECK(!sun_from_nmea("081026", "", &y, &m, &d, &t), "no time");
        CHECK(!sun_from_nmea("321026", "120000", &y, &m, &d, &t), "day 32");
        CHECK(!sun_from_nmea("081326", "120000", &y, &m, &d, &t), "month 13");
        CHECK(!sun_from_nmea("08102", "120000", &y, &m, &d, &t), "short date");
        CHECK(!sun_from_nmea("0810x6", "120000", &y, &m, &d, &t), "not digits");
        CHECK(!sun_from_nmea("081026", "246000", &y, &m, &d, &t), "hour 24");
    }

    printf("\nsuntest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

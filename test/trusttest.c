/*
 * trusttest.c -- main/trust.c: the consistency checks on the GNSS
 * solution, each one firing and each one's innocent case not; held 8 s;
 * one flag odd and two bad; and run once per solution however often
 * the fix is fed, which the original was not.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "trust.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Degrees per metre of latitude, as trust.c's dist_m() counts it. */
#define M_LAT (1.0 / 111320.0)
/* 2026-10-09 12:34:56 UTC. */
#define T0 (1791549296LL)

/* A healthy 3D fix at second `s` past 12:34:00 on 2026-10-09: a spread
 * sky over three constellations, 200 m up. */
static gnss_fix_t fix_at(int s, double lat, double kmh)
{
    gnss_fix_t f;
    gnss_fix_init(&f);
    f.status = 'A';
    f.mode = 3;
    f.sats = 12;
    f.hdop = 0.9;
    f.lat = lat;
    f.lon = -83.4;
    f.altitude = 200.0 + (s % 7) * 0.3;
    f.speed_kmh = kmh;
    snprintf(f.utc, sizeof(f.utc), "12%02u%02u.00", (unsigned)(34 + s / 60) % 60u, (unsigned)s % 60u);
    snprintf(f.date, sizeof(f.date), "091026");
    f.cons[0].visible = 7; f.cons[0].best_snr = 45; f.cons[0].worst_snr = 18;
    f.cons[1].visible = 4; f.cons[1].best_snr = 38; f.cons[1].worst_snr = 22;
    f.cons[2].visible = 3; f.cons[2].best_snr = 41; f.cons[2].worst_snr = 25;
    return f;
}

/* Drive at `kmh` for `n` solutions a second apart, each fed `feeds` times
 * 5 ms apart (the main loop), with the matching Doppler speed. */
static void drive(trust_t *t, int *s, double *lat, double kmh, int n, int feeds, uint32_t *ms)
{
    for (int i = 0; i < n; i++) {
        *lat += kmh / 3.6 * M_LAT;
        (*s)++;
        *ms += 1000;
        const gnss_fix_t f = fix_at(*s, *lat, kmh);
        for (int k = 0; k < feeds; k++) trust_update(t, &f, *ms + (uint32_t)k * 5, T0 + *s, 1000);
    }
}

static void epoch(void)
{
    printf("GNSS time\n");
    CHECK(trust_gnss_epoch("123456.00", "091026") == T0, "%lld", (long long)trust_gnss_epoch("123456.00", "091026"));
    CHECK(trust_gnss_epoch("235959", "311299") == 946684799LL, "1999");
    CHECK(trust_gnss_epoch("000000", "290200") == 951782400LL, "leap day 2000");
    CHECK(trust_gnss_epoch("", "091026") == 0 && trust_gnss_epoch("123456", "") == 0, "empty");
    CHECK(trust_gnss_epoch("12a456", "091026") == 0, "not digits");
    CHECK(trust_gnss_epoch("123456", "091326") == 0, "month 13");
    CHECK(trust_gnss_epoch("123456", "001026") == 0, "day 0");
    CHECK(trust_gnss_epoch("246000", "091026") == 0, "hour 24");
}

static void healthy(void)
{
    printf("a healthy drive says nothing\n");
    trust_t t;
    trust_reset(&t);
    CHECK(t.level == TRUST_UNKNOWN, "level before a fix");
    gnss_fix_t none;
    gnss_fix_init(&none);
    trust_update(&t, &none, 1000, 0, 0);
    CHECK(t.level == TRUST_UNKNOWN, "no fix is not OK");

    int s = 0;
    double lat = 42.3;
    uint32_t ms = 1000;
    const gnss_fix_t f = fix_at(s, lat, 0);
    CHECK(trust_update(&t, &f, ms, T0, 1000) && t.level == TRUST_OK, "first fix not OK");
    drive(&t, &s, &lat, 100.0, 60, 200, &ms);
    drive(&t, &s, &lat, 3.0, 30, 200, &ms);        /* walking: Doppler noisy */
    drive(&t, &s, &lat, 0.0, 30, 200, &ms);        /* parked */
    CHECK(t.flags == 0 && t.level == TRUST_OK, "flags 0x%x on a healthy drive", t.flags);
    char txt[64];
    trust_text(&t, txt, sizeof(txt));
    CHECK(txt[0] == '\0', "text %s", txt);
}

static void original_bug(void)
{
    printf("once per solution, however often fed\n");
    /* Fed at 200 Hz, one solution a second at 100 km/h. The original
     * compared calls: after a 300 ms stall it saw no movement in 300 ms
     * against a Doppler of 100 km/h, and flagged speed. */
    trust_t t;
    trust_reset(&t);
    int s = 0;
    double lat = 42.3;
    uint32_t ms = 1000;
    drive(&t, &s, &lat, 100.0, 5, 1, &ms);
    const gnss_fix_t f = fix_at(s, lat, 100.0);
    trust_update(&t, &f, ms + 300, T0 + s, 1000);   /* the same fix, after a stall */
    trust_update(&t, &f, ms + 700, T0 + s, 1000);
    CHECK(t.flags == 0, "the same solution compared with itself: 0x%x", t.flags);

    /* And the solution rate slowing to 5 s (0031) is not a jump. */
    for (int i = 0; i < 5; i++) {
        lat += 5 * 100.0 / 3.6 * M_LAT;
        s += 5;
        ms += 5000;
        const gnss_fix_t g = fix_at(s, lat, 100.0);
        trust_update(&t, &g, ms, T0 + s, 1000);
    }
    CHECK(t.flags == 0, "5 s solutions flagged 0x%x", t.flags);
}

static void each(void)
{
    printf("each check fires, and holds 8 s\n");
    trust_t t;
    int s;
    double lat;
    uint32_t ms;
    gnss_fix_t f;

    /* Jump: 2 km in a second. */
    trust_reset(&t); s = 0; lat = 42.3; ms = 1000;
    drive(&t, &s, &lat, 50.0, 3, 1, &ms);
    f = fix_at(++s, lat + 2000 * M_LAT, 50.0);
    ms += 1000;
    trust_update(&t, &f, ms, T0 + s, 1000);
    CHECK(t.flags & TRUST_F_JUMP, "2 km in 1 s not a jump");
    CHECK(t.flags & TRUST_F_SPEED, "2 km in 1 s at a Doppler 50 km/h, speed agrees");
    CHECK(t.level == TRUST_BAD, "two flags not bad: %d", t.level);
    char txt[64];
    trust_text(&t, txt, sizeof(txt));
    CHECK(strcmp(txt, "jump, speed") == 0, "text %s", txt);
    /* Held 8 s, then gone with a healthy drive. */
    lat += 2000 * M_LAT;
    drive(&t, &s, &lat, 50.0, 7, 1, &ms);
    CHECK(t.flags & TRUST_F_JUMP, "jump not held 7 s");
    drive(&t, &s, &lat, 50.0, 2, 1, &ms);
    CHECK(t.flags == 0 && t.level == TRUST_OK, "not cleared after 9 s: 0x%x", t.flags);

    /* Speed: Doppler says 80, the positions say 20. */
    trust_reset(&t); s = 0; lat = 42.3; ms = 1000;
    for (int i = 0; i < 3; i++) {
        lat += 20.0 / 3.6 * M_LAT;
        f = fix_at(++s, lat, 80.0);
        ms += 1000;
        trust_update(&t, &f, ms, T0 + s, 1000);
    }
    CHECK(t.flags == TRUST_F_SPEED && t.level == TRUST_ODD, "speed: 0x%x level %d", t.flags, t.level);
    /* But at a walk the two disagree honestly. */
    trust_reset(&t); s = 0; lat = 42.3; ms = 1000;
    for (int i = 0; i < 3; i++) {
        lat += 2.0 / 3.6 * M_LAT;
        f = fix_at(++s, lat, 9.0);
        ms += 1000;
        trust_update(&t, &f, ms, T0 + s, 1000);
    }
    CHECK(t.flags == 0, "walking speed flagged 0x%x", t.flags);

    /* A gap: no fix, then 5 km away. Not a jump. */
    trust_reset(&t); s = 0; lat = 42.3; ms = 1000;
    drive(&t, &s, &lat, 50.0, 3, 1, &ms);
    gnss_fix_t none;
    gnss_fix_init(&none);
    ms += 1000;
    trust_update(&t, &none, ms, 0, 0);
    f = fix_at(++s, lat + 5000 * M_LAT, 50.0);
    ms += 1000;
    trust_update(&t, &f, ms, T0 + s, 1000);
    CHECK(t.flags == 0, "a jump across a gap flagged 0x%x", t.flags);

    /* Clock: 200 s behind the RTC. Not with an RTC that cannot vouch. */
    trust_reset(&t);
    f = fix_at(0, 42.3, 0);
    trust_update(&t, &f, 1000, T0 + 200, 1000);
    CHECK(t.flags == TRUST_F_CLOCK, "200 s behind: 0x%x", t.flags);
    trust_reset(&t);
    trust_update(&t, &f, 1000, T0 + 100, 1000);
    CHECK(t.flags == 0, "100 s flagged");
    trust_reset(&t);
    trust_update(&t, &f, 1000, 0, 1000);
    trust_update(&t, &f, 1000, 946684800LL, 1000);
    CHECK(t.flags == 0, "an unset RTC flagged");

    /* SNR: every channel within 3 dB. */
    trust_reset(&t);
    f = fix_at(0, 42.3, 0);
    for (int i = 0; i < 3; i++) { f.cons[i].best_snr = 40; f.cons[i].worst_snr = 38; }
    trust_update(&t, &f, 1000, T0, 1000);
    CHECK(t.flags == TRUST_F_SNR, "bunched: 0x%x", t.flags);
    /* One constellation bunched like that, or too few satellites: no. */
    trust_reset(&t);
    f = fix_at(0, 42.3, 0);
    f.cons[1].visible = f.cons[2].visible = 0;
    f.cons[0].best_snr = 40; f.cons[0].worst_snr = 38; f.cons[0].visible = 9;
    trust_update(&t, &f, 1000, T0, 1000);
    CHECK(t.flags == 0, "one constellation flagged");
    trust_reset(&t);
    f = fix_at(0, 42.3, 0);
    f.cons[0].visible = 3; f.cons[1].visible = 2; f.cons[2].visible = 2;
    for (int i = 0; i < 3; i++) { f.cons[i].best_snr = 40; f.cons[i].worst_snr = 38; }
    trust_update(&t, &f, 1000, T0, 1000);
    CHECK(t.flags == 0, "7 satellites flagged");

    /* PPS: 1.5 s with 3D, not with 2D, not with none seen. */
    trust_reset(&t);
    f = fix_at(0, 42.3, 0);
    trust_update(&t, &f, 1000, T0, 1500);
    CHECK(t.flags == TRUST_F_PPS, "PPS 1500: 0x%x", t.flags);
    trust_reset(&t);
    f.mode = 2;
    trust_update(&t, &f, 1000, T0, 1500);
    CHECK(t.flags == 0, "PPS with 2D flagged");
    trust_reset(&t);
    f.mode = 3;
    trust_update(&t, &f, 1000, T0, 0);
    CHECK(t.flags == 0, "no PPS flagged");

    /* Altitude: impossible, and frozen while driving. */
    trust_reset(&t);
    f = fix_at(0, 42.3, 0);
    f.altitude = 15000.0;
    trust_update(&t, &f, 1000, T0, 1000);
    CHECK(t.flags == TRUST_F_ALT, "15 km: 0x%x", t.flags);
    trust_reset(&t); s = 0; lat = 42.3; ms = 1000;
    for (int i = 0; i < 40; i++) {
        lat += 25 * M_LAT;
        f = fix_at(++s, lat, 90.0);
        f.altitude = 200.0;
        ms += 1000;
        trust_update(&t, &f, ms, T0 + s, 1000);
        if (i == 29) CHECK(!(t.flags & TRUST_F_ALT), "frozen flagged after %d", i + 1);
    }
    CHECK(t.flags == TRUST_F_ALT, "frozen 40 s at 90 km/h: 0x%x", t.flags);
    /* Frozen parked is not frozen. */
    trust_reset(&t); s = 0; ms = 1000;
    for (int i = 0; i < 60; i++) {
        f = fix_at(++s, 42.3, 0);
        f.altitude = 200.0;
        ms += 1000;
        trust_update(&t, &f, ms, T0 + s, 1000);
    }
    CHECK(t.flags == 0, "parked at one height flagged 0x%x", t.flags);

    /* A cut text is still terminated. */
    trust_reset(&t);
    t.flags = TRUST_F_JUMP | TRUST_F_CLOCK | TRUST_F_ALT;
    trust_text(&t, txt, 8);
    CHECK(strlen(txt) < 8, "cut text %s", txt);
    trust_text(&t, txt, sizeof(txt));
    CHECK(strcmp(txt, "jump, clock, altitude") == 0, "text %s", txt);
}

int main(void)
{
    epoch();
    healthy();
    original_bug();
    each();
    printf("\ntrusttest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

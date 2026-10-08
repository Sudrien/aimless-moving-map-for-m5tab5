/*
 * nmeatest.c -- main/nmea.h on sentences shaped like the module's, with
 * real checksums, fed through a line splitter as the UART task feeds
 * them.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nmea.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* "$" + body + "*XX\r\n", with XX the XOR of the body. */
static void sentence(char *out, size_t cap, const char *body)
{
    unsigned x = 0;
    for (const char *p = body; *p; p++) x ^= (unsigned char)*p;
    snprintf(out, cap, "$%s*%02X\r\n", body, x);
}

/* The task's line assembly, over a whole buffer. */
static void feed(gnss_fix_t *fix, const char *bytes, uint32_t now)
{
    char line[128];
    int pos = 0;
    for (const char *c = bytes; *c; c++) {
        if (*c == '\n') { line[pos] = 0; nmea_parse(line, fix, now); pos = 0; }
        else if (*c != '\r' && pos < (int)sizeof(line) - 1) line[pos++] = *c;
    }
}

static void feed_one(gnss_fix_t *fix, const char *body, uint32_t now)
{
    char s[160];
    sentence(s, sizeof(s), body);
    feed(fix, s, now);
}

int main(void)
{
    gnss_fix_t f;
    gnss_fix_init(&f);
    CHECK(f.status == 'V' && f.mode == 1 && f.hdop == 99.99, "initial fix");
    CHECK(!gnss_coarse(&f) && !gnss_fine(&f), "no fix before any sentence");

    /* No fix yet: RMC with V keeps the position untouched. */
    feed_one(&f, "GNRMC,123519.00,V,,,,,,,081026,,,N", 10);
    CHECK(f.status == 'V' && f.lat == 0 && f.lon == 0, "void RMC moved the position");
    CHECK(!strcmp(f.utc, "123519.00") && !strcmp(f.date, "081026"), "utc/date: %s %s", f.utc, f.date);
    CHECK(f.last_sentence_ms == 10, "timestamp");

    /* A fix: 45 deg 30.5000' N, 073 deg 34.2500' W. */
    feed_one(&f, "GNRMC,123520.00,A,4530.50000,N,07334.25000,W,0.12,,081026,,,A", 20);
    CHECK(gnss_coarse(&f), "valid RMC not coarse");
    CHECK(fabs(f.lat - (45 + 30.5 / 60)) < 1e-9, "lat %.9f", f.lat);
    CHECK(fabs(f.lon + (73 + 34.25 / 60)) < 1e-9, "lon %.9f", f.lon);

    /* South and east. */
    gnss_fix_t s;
    gnss_fix_init(&s);
    feed_one(&s, "GPRMC,000000.00,A,3352.0000,S,15112.6000,E,,,010126,,,A", 1);
    CHECK(fabs(s.lat + (33 + 52.0 / 60)) < 1e-9 && fabs(s.lon - (151 + 12.6 / 60)) < 1e-9,
          "southern/eastern %.6f %.6f", s.lat, s.lon);

    feed_one(&f, "GNVTG,87.5,T,,M,1.20,N,2.22,K,A", 30);
    CHECK(fabs(f.course - 87.5) < 1e-9 && fabs(f.speed_kmh - 2.22) < 1e-9,
          "VTG %.2f %.2f", f.course, f.speed_kmh);

    feed_one(&f, "GNGGA,123520.00,4530.50000,N,07334.25000,W,1,09,1.10,52.3,M,-32.1,M,,", 40);
    CHECK(f.sats == 9 && fabs(f.hdop - 1.10) < 1e-9 && fabs(f.altitude - 52.3) < 1e-9,
          "GGA %d %.2f %.1f", f.sats, f.hdop, f.altitude);
    CHECK(!gnss_fine(&f), "fine before any 3D GSA");

    feed_one(&f, "GNGSA,A,3,05,07,13,15,18,23,,,,,,,1.90,1.10,1.55,1", 50);
    CHECK(f.mode == 3 && gnss_fine(&f), "GSA mode %d", f.mode);
    /* A later 2D GSA does not lower it (the original's rule: only up, or to 1). */
    feed_one(&f, "GNGSA,A,2,05,07,13,,,,,,,,,,2.10,1.90,0.90,2", 51);
    CHECK(f.mode == 3, "mode lowered to %d", f.mode);
    feed_one(&f, "GNGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99,1", 52);
    CHECK(f.mode == 1 && !gnss_fine(&f), "mode 1 not taken");

    /* GGA with HDOP empty is 99.99. */
    feed_one(&f, "GNGGA,123521.00,,,,,0,00,,,M,,M,,", 60);
    CHECK(f.hdop == 99.99, "empty HDOP %.2f", f.hdop);

    /* GSV: two sentences of GPS, one satellite untracked (empty SNR). */
    feed_one(&f, "GPGSV,2,1,06,05,40,083,46,07,20,140,31,13,55,300,,15,10,020,22", 70);
    feed_one(&f, "GPGSV,2,2,06,18,30,200,38,23,05,330,19", 71);
    CHECK(f.cons[0].visible == 6, "GPS in view %d", f.cons[0].visible);
    CHECK(f.cons[0].best_snr == 46 && f.cons[0].worst_snr == 19,
          "GPS SNR %d..%d", f.cons[0].worst_snr, f.cons[0].best_snr);
    /* A new sweep (sentence 1) starts the extremes again. */
    feed_one(&f, "GPGSV,1,1,02,05,40,083,30,07,20,140,28", 80);
    CHECK(f.cons[0].best_snr == 30 && f.cons[0].worst_snr == 28,
          "new sweep %d..%d", f.cons[0].worst_snr, f.cons[0].best_snr);
    feed_one(&f, "GLGSV,1,1,01,70,30,100,33", 81);
    feed_one(&f, "GAGSV,1,1,01,02,30,100,41", 82);
    feed_one(&f, "GBGSV,1,1,01,11,30,100,", 83);
    CHECK(f.cons[1].best_snr == 33 && f.cons[2].best_snr == 41 && f.cons[3].best_snr == 0 &&
          f.cons[3].visible == 1, "other constellations");

    /* Several sentences in one read, split across nothing in particular. */
    gnss_fix_t g;
    gnss_fix_init(&g);
    char a[160], b[160], buf[400];
    sentence(a, sizeof(a), "GNRMC,010203.00,A,0130.00000,N,10300.00000,E,,,020226,,,A");
    sentence(b, sizeof(b), "GNGGA,010203.00,0130.00000,N,10300.00000,E,1,12,0.70,10.0,M,0.0,M,,");
    snprintf(buf, sizeof(buf), "garbage\r\n%s%s", a, b);
    feed(&g, buf, 90);
    CHECK(gnss_coarse(&g) && fabs(g.lat - 1.5) < 1e-9 && fabs(g.lon - 103.0) < 1e-9 && g.sats == 12,
          "batched sentences");

    /* Not a sentence; a type it does not read; too few fields. */
    char junk[] = "hello";
    CHECK(!nmea_parse(junk, &g, 1), "non-sentence accepted");
    gnss_fix_t before = g;
    feed_one(&g, "GNZDA,010203.00,02,02,2026,00,00", 91);
    feed_one(&g, "GNRMC,1", 92);
    CHECK(g.lat == before.lat && g.status == before.status && g.sats == before.sats,
          "unknown or short sentence changed the fix");

    /* UBX checksum: CFG-RATE 1000 ms, known good B5 62 06 08 06 00 E8 03 01 00 01 00 01 39. */
    const uint8_t rate[] = { 0x06, 0x08, 0x06, 0x00, 0xE8, 0x03, 0x01, 0x00, 0x01, 0x00 };
    uint8_t ca, ck;
    ubx_checksum(rate, sizeof(rate), &ca, &ck);
    CHECK(ca == 0x01 && ck == 0x39, "UBX checksum %02X %02X", ca, ck);

    printf("nmeatest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

/*
 * waypointstest.c -- main/waypoints.c: the list, the file as the
 * original wrote it, and the distance and bearing to a target.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "waypoints.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The original's records as its compiler laid them out: this host is
 * little-endian with 8-byte alignment for double and int64, as the
 * ESP32-P4 is. original/waypoints.h struct Waypoint and WpFile. */
struct OrigWaypoint { double lat, lon; char name[24]; int64_t saved_utc; };
struct OrigFile { uint32_t magic; uint16_t version; uint16_t count; };

int main(void)
{
    wp_list_t l;

    printf("the original's layout is this file's\n");
    CHECK(sizeof(struct OrigWaypoint) == WP_REC_BYTES && sizeof(struct OrigFile) == WP_HEAD_BYTES,
          "record %zu, header %zu", sizeof(struct OrigWaypoint), sizeof(struct OrigFile));
    {
        /* A file the original would have written, read here. */
        uint8_t img[WP_FILE_MAX];
        const struct OrigFile h = { 0x57505431u, 1, 2 };
        struct OrigWaypoint w[2];
        memset(w, 0, sizeof w);
        w[0].lat = 40.79891; w[0].lon = -81.37842; strcpy(w[0].name, "home");  w[0].saved_utc = 1790000000;
        w[1].lat = -33.8688; w[1].lon = 151.2093;  strcpy(w[1].name, "sydney"); w[1].saved_utc = 0;
        memcpy(img, &h, sizeof h);
        memcpy(img + sizeof h, w, sizeof w);
        CHECK(wp_load(&l, img, sizeof h + sizeof w) == 2, "load");
        CHECK(l.n == 2 && l.p[0].lat == 40.79891 && l.p[0].lon == -81.37842 &&
              strcmp(l.p[0].name, "home") == 0 && l.p[0].saved_utc == 1790000000, "first");
        CHECK(strcmp(l.p[1].name, "sydney") == 0 && l.p[1].saved_utc == 0 && l.target == -1, "second");

        /* And written back, byte for byte what the original wrote. */
        uint8_t out[WP_FILE_MAX];
        const size_t n = wp_save(&l, out, sizeof out);
        CHECK(n == sizeof h + sizeof w && memcmp(out, img, n) == 0, "round trip differs");
        CHECK(wp_save(&l, out, n - 1) == 0, "saved into too little room");

        /* A short file keeps its whole records; a bad header nothing. */
        CHECK(wp_load(&l, img, sizeof h + WP_REC_BYTES + 10) == 1 && l.n == 1, "truncated");
        img[0] ^= 1;
        CHECK(wp_load(&l, img, sizeof h + sizeof w) == 0 && l.n == 0, "bad magic");
        img[0] ^= 1;
        img[4] = 2;
        CHECK(wp_load(&l, img, sizeof h + sizeof w) == 0, "version 2");
        img[4] = 1;
        img[6] = WP_MAX + 1;
        CHECK(wp_load(&l, img, sizeof img) == 0, "count past WP_MAX");
        CHECK(wp_load(&l, img, 3) == 0 && wp_load(&l, NULL, 0) == 0, "no header");
        /* A name with no terminator in the file is cut, not overrun. */
        img[6] = 1;
        memset(img + 8 + 16, 'x', 24);
        CHECK(wp_load(&l, img, sizeof h + WP_REC_BYTES) == 1 && strlen(l.p[0].name) == WP_NAME_MAX - 1,
              "unterminated name");
    }

    printf("names: given, from the clock, or numbered\n");
    wp_init(&l);
    CHECK(wp_add(&l, 1, 2, "car", 0) == 0 && strcmp(l.p[0].name, "car") == 0, "given");
    CHECK(wp_add(&l, 1, 2, NULL, 1790000000) == 1 && strcmp(l.p[1].name, "14:13") == 0,
          "clock: '%s'", l.p[1].name);
    CHECK(wp_add(&l, 1, 2, "", 0) == 2 && strcmp(l.p[2].name, "pin 3") == 0, "numbered: '%s'", l.p[2].name);
    CHECK(wp_add(&l, 1, 2, "a name far longer than twenty-three characters", 0) == 3 &&
          strlen(l.p[3].name) == WP_NAME_MAX - 1, "long name");
    while (l.n < WP_MAX) wp_add(&l, 0, 0, "x", 0);
    CHECK(wp_add(&l, 0, 0, "y", 0) == -1 && l.n == WP_MAX, "full");

    printf("the target follows its point through a removal\n");
    wp_init(&l);
    for (int i = 0; i < 5; i++) wp_add(&l, i, i, NULL, 0);
    wp_set_target(&l, 3);
    CHECK(wp_remove(&l, 1) && wp_target(&l) == 2 && strcmp(l.p[2].name, "pin 4") == 0, "shifted");
    CHECK(wp_remove(&l, 3) && wp_target(&l) == 2, "after it");
    CHECK(wp_remove(&l, 2) && wp_target(&l) == -1, "its own point");
    CHECK(!wp_remove(&l, 9) && !wp_remove(&l, -1), "out of range");
    wp_set_target(&l, 99);
    CHECK(wp_target(&l) == -1, "out-of-range target");

    printf("distance and bearing\n");
    {
        /* One degree of latitude on the mean sphere: 111.195 km. */
        const double d = wp_distance_m(0, 0, 1, 0);
        CHECK(fabs(d - 111195.08) < 1.0, "1 deg: %.2f", d);
        CHECK(fabs(wp_bearing_deg(0, 0, 1, 0) - 0) < 1e-9, "north");
        CHECK(fabs(wp_bearing_deg(0, 0, 0, 1) - 90) < 1e-9, "east");
        CHECK(fabs(wp_bearing_deg(0, 0, -1, 0) - 180) < 1e-9, "south");
        CHECK(fabs(wp_bearing_deg(0, 0, 0, -1) - 270) < 1e-9, "west");
        /* London to Paris: 343.6 km at 148 deg (great circle). */
        const double lp = wp_distance_m(51.5074, -0.1278, 48.8566, 2.3522);
        CHECK(fabs(lp - 343560) < 500, "London-Paris %.0f", lp);
        CHECK(fabs(wp_bearing_deg(51.5074, -0.1278, 48.8566, 2.3522) - 148.1) < 0.5, "bearing");
    }

    printf("the status line\n");
    {
        char t[64];
        wp_init(&l);
        wp_target_text(&l, 0, 0, t, sizeof t);
        CHECK(t[0] == 0, "no target");
        wp_add(&l, 51.5074, -0.1278, "london", 0);
        wp_set_target(&l, 0);
        wp_target_text(&l, 48.8566, 2.3522, t, sizeof t);
        CHECK(strcmp(t, "london: 343.6 km NNW") == 0, "'%s'", t);
        wp_target_text(&l, 51.5074 - 0.002, -0.1278, t, sizeof t);
        CHECK(strcmp(t, "london: 222 m N") == 0, "'%s'", t);
        wp_target_text(&l, 51.5074 - 0.0001, -0.1278, t, sizeof t);
        CHECK(strcmp(t, "london: here (11 m)") == 0, "'%s'", t);
    }

    printf("\nwaypointstest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

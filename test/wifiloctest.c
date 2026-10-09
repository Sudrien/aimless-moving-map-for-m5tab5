/*
 * wifiloctest.c -- main/wifiloc.c: access points learned and judged
 * mobile, a position found from a scan and refused from too few, the
 * file's rows round-tripped and bad ones skipped, and when to scan.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wifiloc.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Degrees per metre, as wifiloc.c counts them, at 42 N. */
#define M_LAT (1.0 / 111320.0)
#define M_LON (1.0 / (111320.0 * cos(42.0 * 3.14159265358979323846 / 180.0)))
#define LAT0 (42.0)
#define LON0 (-83.0)

static wifiloc_ap_t ap(int k, int rssi)
{
    wifiloc_ap_t a = { { 0x02, 0x11, 0x22, 0x33, (uint8_t)(k >> 8), (uint8_t)k }, (int8_t)rssi };
    return a;
}

/* Four access points at the corners of a 200 m square round (LAT0, LON0),
 * each heard from points within 60 m of it, five times. */
static void survey(wifiloc_db_t *db)
{
    static const int dx[4] = { -100, 100, -100, 100 }, dy[4] = { -100, -100, 100, 100 };
    for (int pass = 0; pass < 5; pass++) {
        for (int k = 0; k < 4; k++) {
            const double off = (pass - 2) * 30.0;
            wifiloc_ap_t a = ap(k, -60);
            wifiloc_learn(db, &a, 1, LAT0 + (dy[k] + off) * M_LAT, LON0 + (dx[k] - off) * M_LON,
                          NULL, NULL, NULL);
        }
    }
}

static void learning(void)
{
    printf("learned, folded, judged mobile\n");
    static wifiloc_rec_t tab[8];
    wifiloc_db_t db;
    wifiloc_init(&db, tab, 8);
    int added, folded, mobile;

    wifiloc_ap_t s[3] = { ap(1, -50), ap(2, -70), ap(3, -95) };
    wifiloc_learn(&db, s, 3, LAT0, LON0, &added, &folded, &mobile);
    CHECK(added == 2 && folded == 0 && db.count == 2, "first: %d added, %d records", added, db.count);
    CHECK(db.dirty == 2, "dirty %u", db.dirty);
    wifiloc_learn(&db, s, 3, LAT0 + 20 * M_LAT, LON0, &added, &folded, &mobile);
    CHECK(added == 0 && folded == 2, "second: %d added, %d folded", added, folded);
    CHECK(db.tab[0].n == 2 && fabs(db.tab[0].sum_lat / 2 - (LAT0 + 10 * M_LAT)) < 1e-9, "centroid");
    CHECK(db.tab[0].best_rssi == -50, "best %d", db.tab[0].best_rssi);

    /* Heard 1 km apart, three times: mobile, for good. */
    wifiloc_ap_t bus = ap(9, -60);
    wifiloc_learn(&db, &bus, 1, LAT0, LON0, NULL, NULL, &mobile);
    wifiloc_learn(&db, &bus, 1, LAT0 + 500 * M_LAT, LON0, NULL, NULL, &mobile);
    CHECK(mobile == 0, "mobile before %d observations", WIFILOC_MIN_OBS);
    wifiloc_learn(&db, &bus, 1, LAT0 + 1000 * M_LAT, LON0, NULL, NULL, &mobile);
    CHECK(mobile == 1 && db.tab[2].mobile, "1 km not mobile");
    wifiloc_learn(&db, &bus, 1, LAT0, LON0, NULL, NULL, &mobile);
    CHECK(mobile == 0 && db.tab[2].mobile, "mobile not sticky");

    /* A full table keeps what it knows and folds into it. */
    for (int k = 20; k < 40; k++) {
        wifiloc_ap_t a = ap(k, -60);
        wifiloc_learn(&db, &a, 1, LAT0, LON0, NULL, NULL, NULL);
    }
    CHECK(db.count == 8, "%u in a table of 8", db.count);
    wifiloc_learn(&db, s, 1, LAT0, LON0, &added, &folded, NULL);
    CHECK(added == 0 && folded == 1, "full table: %d added, %d folded", added, folded);
}

static void locating(void)
{
    printf("located, and not from too little\n");
    static wifiloc_rec_t tab[64];
    wifiloc_db_t db;
    wifiloc_init(&db, tab, 64);
    survey(&db);
    CHECK(db.count == 4, "%u surveyed", db.count);

    /* All four equally strong: the middle of the square. */
    wifiloc_ap_t s[4] = { ap(0, -60), ap(1, -60), ap(2, -60), ap(3, -60) };
    wifiloc_est_t e;
    CHECK(wifiloc_locate(&db, s, 4, &e) && e.used == 4, "no estimate from 4: used %d", e.used);
    CHECK(fabs(e.lat - LAT0) < 2 * M_LAT && fabs(e.lon - LON0) < 2 * M_LON,
          "middle %.6f %.6f", e.lat, e.lon);
    CHECK(e.acc_m > 130 && e.acc_m < 150, "spread %.0f m, want about 141", e.acc_m);

    /* One 10 dB stronger pulls the estimate toward it. */
    s[3].rssi = -50;
    CHECK(wifiloc_locate(&db, s, 4, &e), "estimate");
    CHECK(e.lat > LAT0 + 40 * M_LAT && e.lon > LON0 + 40 * M_LON, "not pulled to the strong one");

    /* Two known: no estimate. Unknown, too weak, or one observation: not
     * counted. */
    CHECK(!wifiloc_locate(&db, s, 2, &e) && e.used == 2, "an estimate from 2");
    wifiloc_ap_t t[4] = { ap(0, -60), ap(1, -60), ap(77, -40), ap(2, -95) };
    CHECK(!wifiloc_locate(&db, t, 4, &e) && e.used == 2, "used %d of unknown and weak", e.used);
    wifiloc_ap_t once = ap(50, -40);
    wifiloc_learn(&db, &once, 1, LAT0, LON0, NULL, NULL, NULL);
    wifiloc_ap_t u[3] = { ap(0, -60), ap(1, -60), ap(50, -40) };
    CHECK(!wifiloc_locate(&db, u, 3, &e) && e.used == 2, "a once-heard AP used");

    /* A mobile one is not used. */
    wifiloc_ap_t m = ap(60, -40);
    for (int i = 0; i < 3; i++) wifiloc_learn(&db, &m, 1, LAT0 + i * 600 * M_LAT, LON0, NULL, NULL, NULL);
    wifiloc_ap_t v[3] = { ap(0, -60), ap(1, -60), ap(60, -40) };
    CHECK(!wifiloc_locate(&db, v, 3, &e) && e.used == 2, "a mobile AP used");
}

static void file(void)
{
    printf("rows round-tripped, bad ones skipped\n");
    static wifiloc_rec_t a[64], b[64];
    wifiloc_db_t da, db;
    wifiloc_init(&da, a, 64);
    survey(&da);
    wifiloc_init(&db, b, 64);

    CHECK(wifiloc_header_ok(WIFILOC_HEADER), "header");
    CHECK(wifiloc_header_ok("bssid,obs,lat,lon,min_lat,max_lat,min_lon,max_lon,best_rssi,mobile\r"),
          "header with CR");
    CHECK(!wifiloc_header_ok("bssid,obs,lat") && !wifiloc_header_ok(""), "short header");

    char row[192];
    for (uint32_t i = 0; i < da.count; i++) {
        CHECK(wifiloc_format(&da, i, row, sizeof(row)) > 0, "format %u", i);
        CHECK(wifiloc_parse(&db, row), "parse %s", row);
    }
    CHECK(db.count == da.count, "%u of %u", db.count, da.count);
    for (uint32_t i = 0; i < db.count; i++) {
        CHECK(a[i].id == b[i].id && a[i].n == b[i].n && a[i].mobile == b[i].mobile &&
              a[i].best_rssi == b[i].best_rssi, "record %u fields", i);
        CHECK(fabs(a[i].sum_lat / a[i].n - b[i].sum_lat / b[i].n) < 1e-7 &&
              fabs(a[i].sum_lon / a[i].n - b[i].sum_lon / b[i].n) < 1e-7, "record %u centroid", i);
        CHECK(fabsf(a[i].min_lat - b[i].min_lat) < 1e-5f && fabsf(a[i].max_lon - b[i].max_lon) < 1e-5f,
              "record %u extent", i);
    }
    CHECK(wifiloc_format(&da, 0, row, 20) < 0, "short buffer formatted");
    CHECK(wifiloc_format(&da, 999, row, sizeof(row)) < 0, "past the end formatted");
    /* The format is the original's: a known row. */
    wifiloc_format(&da, 0, row, sizeof(row));
    CHECK(strncmp(row, "02:11:22:33:00:00,5,", 20) == 0, "row %s", row);

    const uint32_t before = db.count;
    CHECK(!wifiloc_parse(&db, "02:11:22:33:00:00,5,42.0,-83.0"), "short row");
    CHECK(!wifiloc_parse(&db, "zz:11:22:33:00:00,5,42,-83,42,42,-83,-83,-60,0"), "bad bssid");
    CHECK(!wifiloc_parse(&db, "02:11:22:33:00:00,0,42,-83,42,42,-83,-83,-60,0"), "no observations");
    CHECK(!wifiloc_parse(&db, "02:11:22:33:00:00,5,95,-83,42,42,-83,-83,-60,0"), "latitude 95");
    CHECK(!wifiloc_parse(&db, "02:11:22:33:00:00,5,nan,-83,42,42,-83,-83,-60,0"), "NaN");
    CHECK(db.count == before, "bad rows kept: %u", db.count);
}

static void timing(void)
{
    printf("when to scan\n");
    wifiloc_plan_t p;
    memset(&p, 0, sizeof(p));
    double lat = LAT0;
    uint32_t t = 1000;

    CHECK(wifiloc_next(&p, t, true, true, 5, lat, LON0, false) == WIFILOC_LEARN, "first learn");
    CHECK(wifiloc_next(&p, t + 10000, true, true, 5, lat + 100 * M_LAT, LON0, false) == WIFILOC_IDLE,
          "learned again in 10 s");
    CHECK(wifiloc_next(&p, t + 20000, true, true, 5, lat + 10 * M_LAT, LON0, false) == WIFILOC_IDLE,
          "learned again 10 m on");
    CHECK(wifiloc_next(&p, t + 20000, true, true, 20, lat + 100 * M_LAT, LON0, false) == WIFILOC_IDLE,
          "learned at 20 km/h");
    CHECK(wifiloc_next(&p, t + 20000, true, true, 5, lat + 100 * M_LAT, LON0, false) == WIFILOC_LEARN,
          "not learned 100 m on after 20 s");

    /* A coarse fix: neither. No fix: 15 s, then locate, every 20 s. */
    t += 30000;
    CHECK(wifiloc_next(&p, t, false, true, 0, lat, LON0, true) == WIFILOC_IDLE, "coarse");
    CHECK(wifiloc_next(&p, t, false, false, 0, 0, 0, true) == WIFILOC_IDLE, "located at once");
    CHECK(wifiloc_next(&p, t + 14999, false, false, 0, 0, 0, true) == WIFILOC_IDLE, "located under 15 s");
    CHECK(wifiloc_next(&p, t + 15000, false, false, 0, 0, 0, true) == WIFILOC_LOCATE, "not located at 15 s");
    CHECK(wifiloc_next(&p, t + 30000, false, false, 0, 0, 0, true) == WIFILOC_IDLE, "again in 15 s");
    CHECK(wifiloc_next(&p, t + 35000, false, false, 0, 0, 0, true) == WIFILOC_LOCATE, "not again at 20 s");
    /* Nothing learned: nothing to locate against. */
    wifiloc_plan_t q;
    memset(&q, 0, sizeof(q));
    wifiloc_next(&q, 0, false, false, 0, 0, 0, false);
    CHECK(wifiloc_next(&q, 20000, false, false, 0, 0, 0, false) == WIFILOC_IDLE, "located with no table");
    /* A fix back resets the wait. */
    wifiloc_next(&p, t + 40000, false, true, 0, lat, LON0, true);
    wifiloc_next(&p, t + 41000, false, false, 0, 0, 0, true);
    CHECK(wifiloc_next(&p, t + 50000, false, false, 0, 0, 0, true) == WIFILOC_IDLE, "the wait not restarted");

    printf("whether a learning scan is kept\n");
    CHECK(wifiloc_keep(LAT0, LON0, 0, 4000, true, LAT0 + 20 * M_LAT, LON0), "20 m in 4 s refused");
    CHECK(!wifiloc_keep(LAT0, LON0, 0, 4000, true, LAT0 + 30 * M_LAT, LON0), "30 m in 4 s kept");
    CHECK(!wifiloc_keep(LAT0, LON0, 0, 4000, false, LAT0, LON0), "kept with the fix lost");

    printf("when to write\n");
    wifiloc_db_t d = { 0 };
    CHECK(!wifiloc_write_due(&d, 0, 0, false), "written clean");
    d.dirty = 1;
    CHECK(wifiloc_write_due(&d, 0, 0, false), "first not written");
    CHECK(!wifiloc_write_due(&d, 100000, 0, true), "written at 100 s");
    CHECK(wifiloc_write_due(&d, WIFILOC_WRITE_MS, 0, true), "not written at 3 min");
    d.dirty = WIFILOC_WRITE_DIRTY;
    CHECK(wifiloc_write_due(&d, 1000, 0, true), "400 changes not written");
}

int main(void)
{
    learning();
    locating();
    file();
    timing();
    printf("\nwifiloctest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

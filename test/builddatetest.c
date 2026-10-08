/*
 * builddatetest.c -- main/builddate.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "builddate.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    CHECK(bd_days(1970, 1, 1) == 0, "epoch is %d", bd_days(1970, 1, 1));
    CHECK(bd_days(2000, 3, 1) == 11017, "2000-03-01 is %d", bd_days(2000, 3, 1));
    CHECK(bd_days(2026, 10, 8) == 20734, "2026-10-08 is %d", bd_days(2026, 10, 8));

    /* Every day 2024..2040 round-trips, leap days included. */
    int bad = 0;
    for (int32_t d = bd_days(2024, 1, 1); d < bd_days(2040, 1, 1); d++) {
        int y, m, dd;
        bd_ymd(d, &y, &m, &dd);
        if (bd_days(y, m, dd) != d) bad++;
    }
    CHECK(bad == 0, "%d days did not round-trip", bad);

    int y, m, d;
    bd_ymd(bd_days(2028, 3, 1) - 1, &y, &m, &d);
    CHECK(y == 2028 && m == 2 && d == 29, "day before 2028-03-01 is %d-%d-%d", y, m, d);

    CHECK(bd_from_ddmmyy("081026") == bd_days(2026, 10, 8), "RMC date");
    CHECK(bd_from_ddmmyy("081006") == 0, "2006 refused (week rollover)");
    CHECK(bd_from_ddmmyy("320126") == 0, "day 32 refused");
    CHECK(bd_from_ddmmyy("0810") == 0, "short refused");
    CHECK(bd_from_ddmmyy("") == 0 && bd_from_ddmmyy(NULL) == 0, "empty refused");

    char name[9];
    CHECK(bd_name(bd_days(2026, 1, 5), name) && !strcmp(name, "20260105"), "name %s", name);
    CHECK(bd_parse_name("20260105") == bd_days(2026, 1, 5), "parse name");
    CHECK(bd_parse_name("v4.3.0") == 0, "pinned version is not a date");
    CHECK(bd_parse_name("202601051") == 0, "nine digits refused");

    /* 0019: which build to open. */
    {
        const int32_t today = bd_days(2026, 10, 8);
        CHECK(bd_choose(true, "20260820", true, today, 0, 30) == BD_PINNED, "pinned wins");
        CHECK(bd_choose(false, "", false, today, 0, 30) == BD_DISCOVER, "nothing recorded");
        CHECK(bd_choose(false, "", false, 0, 0, 30) == BD_WAIT_DATE, "nothing recorded, no date");
        CHECK(bd_choose(false, "20261005", false, today, bd_days(2026, 10, 5), 30) == BD_RECORDED,
              "recorded and young");
        CHECK(bd_choose(false, "20261005", false, today, bd_days(2026, 9, 1), 30) == BD_DISCOVER,
              "adopted 37 days ago");
        /* The board's case: build.txt from the original, adoption unknown,
         * the build itself 49 days old. */
        CHECK(bd_choose(false, "20260820", false, today, 0, 30) == BD_DISCOVER,
              "an unknown adoption is aged by the build's name");
        CHECK(bd_choose(false, "20261001", false, today, 0, 30) == BD_RECORDED,
              "unknown adoption, a recent build");
        /* Gone from the server: probe, whatever its age; wait for a date. */
        CHECK(bd_choose(false, "20261005", true, today, bd_days(2026, 10, 5), 30) == BD_DISCOVER,
              "a 404 means look again");
        CHECK(bd_choose(false, "20261005", true, 0, 0, 30) == BD_WAIT_DATE, "gone, no date");
        /* No date: keep using what is recorded. */
        CHECK(bd_choose(false, "20260820", false, 0, 0, 30) == BD_RECORDED, "no date, keep it");
        /* A recorded name that is not a date, adoption unknown: keep it. */
        CHECK(bd_choose(false, "v4.3.0", false, today, 0, 30) == BD_RECORDED, "not a date");
    }

    printf("builddatetest: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

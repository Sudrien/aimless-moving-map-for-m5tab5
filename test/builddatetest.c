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

    printf("builddatetest: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

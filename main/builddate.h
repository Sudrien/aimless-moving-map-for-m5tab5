/*
 * builddate.h -- the date arithmetic that finds a Protomaps daily build.
 *
 * original/netsource.cpp's date helpers, as a header so the host test
 * reaches them. Protomaps keeps about a week of daily builds, named
 * YYYYMMDD.pmtiles; the device probes backwards from today's UTC date,
 * which it has from GNSS (RMC's DDMMYY) or from SNTP, whichever comes
 * first.
 *
 * Days are counted from 1970-01-01. Only differences matter.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* src: Howard Hinnant's days_from_civil, as original/netsource.cpp
 * epoch_days(), shifted to count from 1970. */
static inline int32_t bd_days(int y, int m, int d)
{
    if (m <= 2) { y -= 1; m += 12; }
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const int32_t yoe = y - era * 400;
    const int32_t doy = (153 * (m - 3) + 2) / 5 + d - 1;
    const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* The inverse; original/netsource.cpp days_to_ymd(). */
static inline void bd_ymd(int32_t days, int *y, int *m, int *d)
{
    const int32_t z = days + 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int32_t doe = z - era * 146097;
    const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int32_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/*
 * Today from RMC's date field, DDMMYY. 0 if it is not a date, or is
 * outside the years a build can exist in -- the original's backstop
 * against a receiver that has not yet decoded the almanac's week number,
 * which reports a date decades out.
 */
static inline int32_t bd_from_ddmmyy(const char *s)
{
    for (int i = 0; i < 6; i++)
        if (!s || s[i] < '0' || s[i] > '9') return 0;
    const int d = (s[0] - '0') * 10 + (s[1] - '0');
    const int m = (s[2] - '0') * 10 + (s[3] - '0');
    const int y = 2000 + (s[4] - '0') * 10 + (s[5] - '0');
    if (d < 1 || d > 31 || m < 1 || m > 12) return 0;
    /* src: original/netsource.cpp netsource_set_date_from_clock(), whose
     * plausible years are 2026..2036. */
    if (y < 2026 || y > 2036) return 0;
    return bd_days(y, m, d);
}

/* The same bound for a year from the system clock. */
static inline bool bd_year_plausible(int y) { return y >= 2026 && y <= 2036; }

/* A build name, YYYYMMDD, for `days`; false if it would not be eight
 * digits. */
static inline bool bd_name(int32_t days, char out[9])
{
    int y, m, d;
    bd_ymd(days, &y, &m, &d);
    if (y < 1970 || y > 9999) return false;
    snprintf(out, 9, "%04d%02d%02d", y, m, d);
    return true;
}

/* A build name's date; 0 if it is not YYYYMMDD (a pinned version, say). */
static inline int32_t bd_parse_name(const char *s)
{
    for (int i = 0; i < 8; i++)
        if (!s || s[i] < '0' || s[i] > '9') return 0;
    if (s[8]) return 0;
    const int y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    const int m = (s[4]-'0')*10 + (s[5]-'0');
    const int d = (s[6]-'0')*10 + (s[7]-'0');
    if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
    return bd_days(y, m, d);
}

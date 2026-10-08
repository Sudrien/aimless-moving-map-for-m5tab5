/*
 * sun.c -- see sun.h.
 *
 * The constants are the sunrise equation's, as the Astronomical
 * Almanac's low-precision formulae give them for J2000:
 *   357.5291 + 0.98560028 n      mean anomaly, degrees
 *   1.9148, 0.0200, 0.0003       equation of the centre
 *   102.9372                     argument of perihelion
 *   0.0053, -0.0069              equation of time, in days
 *   23.4397                      obliquity of the ecliptic
 *   -0.833                       altitude of sunrise: refraction 34',
 *                                semidiameter 16'
 *   0.0008                       leap seconds since J2000 (TT - UTC),
 *                                as days, near enough
 *
 * SPDX-License-Identifier: MIT
 */
#include "sun.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEG     (3.14159265358979323846 / 180.0)
#define J2000   (2451545.0)

/* Julian day at 12:00 UTC of a Gregorian date. src: the Fliegel-Van
 * Flandern integer algorithm, Communications of the ACM 11 (1968). */
static long jdn(int y, int m, int d)
{
    const long a = (14 - m) / 12;
    const long yy = y + 4800 - a, mm = m + 12 * a - 3;
    return d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}

static double wrap_day(double m)
{
    m = fmod(m, 1440.0);
    return m < 0 ? m + 1440.0 : m;
}

void sun_day(double lat, double lon, int y, int m, int d, sun_day_t *out)
{
    const double jd_noon = (double)jdn(y, m, d);
    const double n = jd_noon - J2000 + 0.0008;
    const double jstar = n - lon / 360.0;
    const double M = fmod(357.5291 + 0.98560028 * jstar, 360.0);
    const double C = 1.9148 * sin(M * DEG) + 0.0200 * sin(2 * M * DEG) + 0.0003 * sin(3 * M * DEG);
    const double L = fmod(M + C + 180.0 + 102.9372, 360.0);
    const double transit = J2000 + jstar + 0.0053 * sin(M * DEG) - 0.0069 * sin(2 * L * DEG);
    const double sin_dec = sin(L * DEG) * sin(23.4397 * DEG);
    const double cos_dec = cos(asin(sin_dec));
    const double cos_w = (sin(-0.833 * DEG) - sin(lat * DEG) * sin_dec) / (cos(lat * DEG) * cos_dec);

    memset(out, 0, sizeof(*out));
    if (cos_w > 1.0)  { out->kind = SUN_ALWAYS_DOWN; return; }
    if (cos_w < -1.0) { out->kind = SUN_ALWAYS_UP;   return; }
    const double w = acos(cos_w) / DEG;
    /* Julian days count from noon: this date's midnight is jd_noon - 0.5. */
    const double midnight = jd_noon - 0.5;
    out->kind = SUN_CROSSES;
    out->rise_min = wrap_day((transit - w / 360.0 - midnight) * 1440.0);
    out->set_min  = wrap_day((transit + w / 360.0 - midnight) * 1440.0);
}

bool sun_up(const sun_day_t *s, double now_min)
{
    if (s->kind == SUN_ALWAYS_UP) return true;
    if (s->kind == SUN_ALWAYS_DOWN) return false;
    const double now = wrap_day(now_min);
    /* src: original/tab5_map.cpp sunIsUpAt(): an ordinary day, or one that
     * spans UTC midnight. */
    if (s->rise_min < s->set_min) return now >= s->rise_min && now < s->set_min;
    return now >= s->rise_min || now < s->set_min;
}

double sun_to_crossing(const sun_day_t *s, double now_min)
{
    if (s->kind != SUN_CROSSES) return -1.0;
    const double now = wrap_day(now_min);
    /* src: original/tab5_map.cpp minutesToTwilight(): circular distance,
     * so 23:50 is twenty minutes from a 00:10 sunrise. */
    double dr = fabs(now - s->rise_min); if (dr > 720.0) dr = 1440.0 - dr;
    double ds = fabs(now - s->set_min);  if (ds > 720.0) ds = 1440.0 - ds;
    return dr < ds ? dr : ds;
}

static int two(const char *p)
{
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

bool sun_from_nmea(const char *date, const char *utc, int *y, int *m, int *d,
                   double *now_min)
{
    if (!date || !utc || strlen(date) != 6 || strlen(utc) < 6) return false;
    const int dd = two(date), mo = two(date + 2), yy = two(date + 4);
    const int hh = two(utc), mi = two(utc + 2), ss = two(utc + 4);
    if (dd < 1 || dd > 31 || mo < 1 || mo > 12 || yy < 0) return false;
    if (hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0 || ss > 60) return false;
    *y = 2000 + yy;
    *m = mo;
    *d = dd;
    *now_min = hh * 60.0 + mi + ss / 60.0;
    return true;
}

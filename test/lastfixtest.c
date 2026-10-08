/*
 * lastfixtest.c -- main/lastfix.c against the original's own struct.
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lastfix.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* original/tab5_map.cpp struct LastFix, laid out by this host as by the
 * ESP32: little-endian, int64 at an 8-byte boundary. */
struct LastFix { uint32_t magic; float lat, lon; int64_t written_utc; };

int main(void)
{
    double lat, lon;
    CHECK(sizeof(struct LastFix) == LASTFIX_BYTES, "struct is %zu", sizeof(struct LastFix));

    /* A file the original wrote. */
    struct LastFix o = { 0x4C465831u, 42.338f, -83.470f, 1791498815 };
    uint8_t buf[LASTFIX_BYTES];
    memset(buf, 0, sizeof buf);
    memcpy(buf, &o, sizeof o);
    CHECK(lastfix_decode(buf, sizeof buf, &lat, &lon) &&
          fabs(lat - 42.338) < 1e-5 && fabs(lon + 83.470) < 1e-5, "original's %f %f", lat, lon);

    /* What this writes is what the original would have. */
    uint8_t mine[LASTFIX_BYTES];
    lastfix_encode(42.338, -83.470, 1791498815, mine);
    CHECK(memcmp(mine, buf, sizeof buf) == 0, "encoding differs from the original's");
    lastfix_encode(1, 2, -5, mine);
    CHECK(mine[16] == 0 && mine[23] == 0, "a negative time is written as 0");

    /* What is refused. */
    CHECK(!lastfix_decode(buf, LASTFIX_BYTES - 1, &lat, &lon), "short");
    CHECK(!lastfix_decode(NULL, 0, &lat, &lon), "nothing");
    buf[0] ^= 1;
    CHECK(!lastfix_decode(buf, sizeof buf, &lat, &lon), "bad magic");
    buf[0] ^= 1;
    lastfix_encode(0, 0, 0, mine);
    CHECK(!lastfix_decode(mine, sizeof mine, &lat, &lon), "0,0");
    lastfix_encode(91, 0, 0, mine);
    CHECK(!lastfix_decode(mine, sizeof mine, &lat, &lon), "latitude 91");
    lastfix_encode(10, NAN, 0, mine);
    CHECK(!lastfix_decode(mine, sizeof mine, &lat, &lon), "NaN");
    lastfix_encode(-33.8688, 151.2093, 0, mine);
    CHECK(lastfix_decode(mine, sizeof mine, &lat, &lon) && fabs(lon - 151.2093) < 1e-4, "south and east");

    printf("\nlastfixtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

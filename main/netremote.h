/*
 * netremote.h -- the remote PMTiles archive, and the cache that goes with it.
 *
 * original/netsource.cpp's network half: a Protomaps build read with HTTP
 * range requests through the same PMTiles reader the card uses, the build
 * found by date, and the tile cache opened for it.
 *
 * WHICH BUILD. Protomaps keeps about a week of daily builds, named
 * YYYYMMDD.pmtiles. With no pinned build (Kconfig AIMLESS_PINNED_BUILD)
 * the device probes backwards from today's UTC date -- from GNSS or from
 * SNTP, whichever arrives -- and records the build it adopted in
 * `<dir>/build.txt`. After 30 days it probes again; a new build gets a
 * new cache and the old one's two files are removed.
 *
 * WHERE FROM. Kconfig AIMLESS_TILE_BASE, the original's TILE_BASE:
 * Protomaps' own build bucket by default, which they ask people not to
 * hotlink -- fine for trying it, and the reason the base is a setting.
 * Any host that answers range requests serves.
 *
 * ESP-IDF; one task (the render worker) calls everything here.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "maptile.h"
#include "tilesrc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* `dir` must exist: the cache files and build.txt live there. */
void netremote_init(const char *dir, const maptile_alloc_t *mem);

/* Today, in days since 1970 (builddate.h). 0 is ignored. */
void netremote_set_today(int32_t days);

/*
 * Before each tile: point `s` at the remote archive when there is one
 * and the network is up, NULL otherwise, and at the build's cache. Opens,
 * discovers and refreshes as needed; that can take a few seconds of
 * HTTP the first time, and is not retried more than every 30 s after a
 * failure.
 */
void netremote_update(tilesrc_t *s);

typedef struct {
    char     build[16];
    bool     open;
    uint32_t requests, fresh, failed;
    uint64_t bytes;
    uint32_t last_ms;
} netremote_stats_t;

void netremote_stats(netremote_stats_t *out);

#ifdef __cplusplus
}
#endif

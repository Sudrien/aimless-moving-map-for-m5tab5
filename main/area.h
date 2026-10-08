/*
 * area.h -- the area cache: a square of tiles around where the map is,
 * fetched into the tile cache ahead of time (0026).
 *
 * original/mapengine.cpp's map_prefetch_start() and the footer button
 * that ran it. The grid only fetches the tiles the device is standing on,
 * so a cache warmed by use covers a few tiles each way -- ten minutes of
 * walking. This fills a usable area while there is still a network to
 * fill it from: PREFETCH_RADIUS 7, so 15 x 15 tiles at z14, about 27 km
 * across at 42 N.
 *
 * WHAT DIFFERS FROM THE ORIGINAL:
 *   - Nearest first, ring by ring, rather than row by row, so a fetch
 *     cut short has the middle of the square rather than its top.
 *   - The overview's z12 tiles over the same square as well (0014 drew
 *     them; the original's overview predates its prefetch and was never
 *     added to it), 5 x 5 of them.
 *   - It runs on the render task, between the tiles the screen wants,
 *     rather than on a task of its own: the tile source is not
 *     thread-safe (tilesrc.h), and "a tile the user is looking at always
 *     beats one being stored for later" is then the order the loop
 *     already takes.
 *
 * This file is the walk. Free of ESP-IDF; tested on the host.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mapset.h"
#include "tile_grid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* src: original/tab5_map.cpp PREFETCH_RADIUS. */
#define AREA_RADIUS     (7)
/* The overview's zoom offset (mapview.h COARSE_STEP), and the radius at
 * it that covers the same square: 7 z14 tiles is under 2 z12 tiles. */
#define AREA_CZ_STEP    (2)
#define AREA_CZ_RADIUS  ((AREA_RADIUS + (1 << AREA_CZ_STEP) - 1) >> AREA_CZ_STEP)

typedef struct {
    bool     active;
    uint8_t  z;                 /* the grid's zoom */
    int32_t  cx, cy;            /* the centre tile at z */
    /* where the walk is */
    int      level;             /* 0: z, 1: z - AREA_CZ_STEP */
    int      ring, k;
    /* counts */
    int      total, done;
    int      fetched, cached, offline, empty, failed;
} area_t;

/* Tiles in the square at both levels: 15^2 + 5^2. */
int  area_total(void);

/* Start a walk around tile (cx, cy) at zoom z. */
void area_start(area_t *a, uint8_t z, int32_t cx, int32_t cy);

/* The next tile, nearest first at z and then at the overview's zoom;
 * false when the walk is over (and a->active is then false). Tiles past
 * the poles are skipped; x wraps round the world. */
bool area_next(area_t *a, tile_id_t *id);

/* 0..100. */
int  area_progress(const area_t *a);

/* Of the square around (cx, cy) at z, how many tiles no archive's header
 * covers -- 0 means the area is already offline, and there is nothing to
 * fetch (original map_prefetch_pending()). Header fields only, no reads. */
int  area_pending(const mapset_t *set, uint8_t z, int32_t cx, int32_t cy);

#ifdef __cplusplus
}
#endif

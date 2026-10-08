/*
 * mapset.h -- every .pmtiles on the card, and which one holds a tile.
 *
 * original/netsource.cpp's "local archives": a set rather than a single
 * file, because FAT32 caps a file at 4 GiB and a useful map is larger,
 * so plan-extracts.py splits it into bands by zoom and longitude. Each
 * archive's header says which zooms and which box it holds, so a tile is
 * asked of the archives that cover it, in the order they were added,
 * until one has it.
 *
 * Free of ESP-IDF; the archives are opened by the caller.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "maptile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* src: original/netsource.cpp LOCAL_ARCHIVE_MAX. */
#define MAPSET_MAX  (16)

typedef struct {
    maparchive_t *arc[MAPSET_MAX];
    int           n;
} mapset_t;

/* Add an opened archive. False when the set is full. */
bool mapset_add(mapset_t *s, maparchive_t *a);

/* Whether archive `a`'s header covers z/x/y: z within its zooms, and the
 * tile's box overlapping its bounds. */
bool mapset_covers(const maparchive_t *a, uint8_t z, uint32_t x, uint32_t y);

/*
 * Draw tile `id` from the first covering archive that has it. TILE_NODATA
 * if none does; TILE_ERROR only if every archive that was asked failed
 * and none simply lacked it.
 */
tile_state_t mapset_render(mapset_t *s, maprender_t *r, tile_id_t id,
                           uint16_t *px, int split);

/* Where to look before there is a fix: the first archive's centre,
 * from its header. False with no archives. */
bool mapset_centre(const mapset_t *s, double *lat, double *lon);

#ifdef __cplusplus
}
#endif

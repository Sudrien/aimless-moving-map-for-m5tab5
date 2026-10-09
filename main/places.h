/*
 * places.h -- where you are, in words: "Neighbourhood, Locality, Region,
 * Country" for the status line.
 *
 * original/mapengine.cpp's place lookup (PlaceIndex, place_part(),
 * load_place_block(), ensure_place_blocks(), search_index(),
 * update_place_names(), map_place_text()), with no file and no task in
 * it, so it runs on the host. aimless.c reads the tiles and calls this.
 *
 * Not from the grid. A place's point is one centroid, and the grid is
 * four z14 tiles: a township's centroid is usually not in them at all.
 * So places come from their own tiles, 3 x 3 around you, at zooms where
 * one tile is large enough to hold the centroid:
 *
 *   locality, neighbourhood   z12   about 7 km a tile
 *   region, country           z6    about 460 km a tile
 *
 * Nearest centroid of each rank, not point in polygon: the basemap has
 * points, not boundaries, so near an edge this can name the neighbour.
 *
 * Free of ESP-IDF.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "maplabel.h"
#include "mvt.h"
#include "tile_grid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* src: original/mapengine.cpp PLACE_ZOOM. */
#define PLACES_FINE_Z       (12)
/* src: original/mapengine.cpp REGION_ZOOM, which is WORLD_FLOOR_ZOOM
 * (original/mapconfig.h): the zoom the original's floor archive holds,
 * and the one its notes say z5 failed for, having no archive at all. */
#define PLACES_COARSE_Z     (6)
/* src: original/mapengine.cpp PLACE_INDEX_MAX: generous enough not to
 * lose the nearest in nine z12 tiles of a dense metro. */
#define PLACES_MAX          (160)
/* src: original/mapengine.cpp PLACE_HOOD_RADIUS, PLACE_LOCALITY_RADIUS,
 * in tiles at the view's zoom: about 1 km and 7 km at z14 and 42 N. A
 * neighbourhood two tiles away is a different one; a township's
 * centroid can be several km from its edge. Regions and countries have
 * none: their centroids are of something enormous. */
#define PLACES_HOOD_R       (0.4)
#define PLACES_LOCALITY_R   (4.0)

/* src: original/mapengine.cpp PL_HOOD .. PL_COUNTRY, finest first. */
enum { PLACES_HOOD = 0, PLACES_LOCALITY, PLACES_REGION, PLACES_COUNTRY, PLACES_RANKS };

/* One block's points. fx, fy are world tiles at `z`, not a fraction of
 * one tile as a maplabel_set_t's are: nine tiles' points are compared. */
typedef struct {
    uint8_t    z;
    uint16_t   n;
    bool       full;            /* a point was dropped for room */
    maplabel_t v[PLACES_MAX];
} places_index_t;

/* The block a position is in: the tile at `z` under (wx, wy), world
 * tiles at `view_z`. */
tile_id_t places_centre(uint8_t view_z, double wx, double wy, uint8_t z);

/*
 * The block's nine tiles, centre first, in `out`: y past a pole left
 * out, x wrapped round the world. Returns how many.
 */
int  places_block(tile_id_t centre, tile_id_t out[9]);

/* Start an index for a block at `z`. */
void places_begin(places_index_t *idx, uint8_t z);

/*
 * A decoded part of tile `id`'s places layer: kept if it is a named
 * point of a rank the index is for -- localities and neighbourhoods at
 * PLACES_FINE_Z, regions and countries otherwise. The original's
 * place_part() and its `want` mask: a z6 block holds thousands of
 * localities, and kept, they would fill the index before the region
 * points it exists for. Returns non-zero, to stop the decode, when full.
 */
typedef struct { places_index_t *idx; tile_id_t id; } places_sink_t;
int  places_part(void *ctx, const mvt_part_t *part);

/* What the status line says, rank by rank, and what it said before. */
typedef struct {
    char name[PLACES_RANKS][MAPLABEL_TEXT_MAX];
} places_t;

/*
 * Pick the nearest point of each rank to (wx, wy), world tiles at
 * `view_z`, from both indexes (either may be NULL). Both are searched
 * for every rank: a z12 block often has the region's point too, and a
 * block that moves every 7 km is the better source for it. As the
 * original, a rank with nothing near keeps the name it had, so open
 * country between towns does not flicker -- except the neighbourhood,
 * which is cleared, since a stale one names a district you have left.
 */
void places_pick(places_t *p, const places_index_t *fine,
                 const places_index_t *coarse, uint8_t view_z, double wx, double wy);

/*
 * The names, finest first, joined with ", ", a rank that repeats an
 * earlier one left out ("Singapore, Singapore"). True if anything was
 * written. src: original/mapengine.cpp map_place_text().
 */
bool places_text(const places_t *p, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

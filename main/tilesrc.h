/*
 * tilesrc.h -- where a tile comes from: the cache, the card, the network.
 *
 * original/netsource.cpp's netsource_get(), in the original's order:
 *
 *   1. the tile cache       a tile fetched before (tilecache.h)
 *   2. the local archives   every .pmtiles on the card or drive (mapset.h)
 *   3. the remote archive   the same PMTiles reader over HTTP range reads
 *
 * A negative marker in the cache stops the search before the network: the
 * remote archive was asked once and has no such tile. What the network
 * returns is cached, data or marker, so it is asked once per tile.
 *
 * The remote archive is an ordinary maparchive_t whose read callback
 * happens to be HTTP; this file does not know. NULL means offline.
 *
 * Free of ESP-IDF. Not thread-safe: one render worker calls it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mapset.h"
#include "maptile.h"
#include "tilecache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TILESRC_NONE = 0,
    TILESRC_CACHE,
    TILESRC_LOCAL,
    TILESRC_NET,
} tilesrc_from_t;

typedef struct {
    uint32_t cache_hits, local_hits, net_hits;
    uint32_t misses, errors;
} tilesrc_stats_t;

typedef struct {
    mapset_t       *local;      /* may be empty */
    tilecache_t    *cache;      /* may be closed */
    maparchive_t   *remote;     /* NULL: offline */
    tilesrc_stats_t st;
} tilesrc_t;

/*
 * Draw tile `id` (data `split` zooms coarser) into `px`, from the first
 * source that has it. *from, if not NULL, says which.
 *
 *   TILE_READY    drawn
 *   TILE_NODATA   no source has it (and the network, if asked, said so)
 *   TILE_ERROR    a source that should have had it failed -- the
 *                 network dropped, or a payload would not inflate.
 *                 Nothing is cached, so it is asked again next time.
 */
tile_state_t tilesrc_draw(tilesrc_t *s, maprender_t *r, tile_id_t id,
                          uint16_t *px, int split, tilesrc_from_t *from);

/*
 * Make sure tile `id` is held offline, without drawing it: the area
 * cache's step (0026), as the original prefetch_task(). A tile a local
 * archive's header covers is offline already and is not read at all; one
 * in the cache, payload or "no data" marker, is not asked for again;
 * otherwise the network is asked and what it says is cached.
 *
 *   TILE_READY    held: *from says where (LOCAL, CACHE or NET)
 *   TILE_NODATA   there is no such tile (a marker is held for it)
 *   TILE_ERROR    the network failed, or there is no network, or no
 *                 cache to keep it in; nothing was stored
 *
 * The payload is stored as the network sent it, after a check that it is
 * gzip, as the original's was: drawing 250 tiles to validate them would
 * cost what the walk is meant to save, and one that will not draw is
 * fetched again by tilesrc_draw() (tilesrc.h's rule for a cached payload
 * that fails).
 */
tile_state_t tilesrc_store(tilesrc_t *s, maprender_t *r, tile_id_t id,
                           int split, tilesrc_from_t *from);

/*
 * Tile `id`'s payload into r->tile, *len bytes, from the first source
 * that has it, in tilesrc_draw()'s order, without drawing it: for the
 * place lookup (0030), which decodes one layer of it. What the network
 * sends is cached, after a check that it is gzip, as tilesrc_store()
 * does. Counted as a draw is.
 *
 *   TILE_READY    *len bytes in r->tile; *from says where
 *   TILE_NODATA   no source has it
 *   TILE_ERROR    a source that should have had it failed
 *
 * The original read place tiles from the card only: nine failing range
 * requests in a row stalled its worker. Here a tile the network has not
 * got is cached as a marker and not asked for again, and a failure
 * waits for the caller's retry, so the network is asked too -- without
 * it, a card of z14 alone, or no card, would have no place names.
 */
tile_state_t tilesrc_fetch(tilesrc_t *s, maprender_t *r, tile_id_t id,
                           uint32_t *len, tilesrc_from_t *from);

#ifdef __cplusplus
}
#endif

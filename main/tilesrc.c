/*
 * tilesrc.c -- see tilesrc.h. original/netsource.cpp netsource_get_locked().
 *
 * SPDX-License-Identifier: MIT
 */
#include "tilesrc.h"

tile_state_t tilesrc_draw(tilesrc_t *s, maprender_t *r, tile_id_t id,
                          uint16_t *px, int split, tilesrc_from_t *from)
{
    if (from) *from = TILESRC_NONE;
    const uint8_t  dz = (uint8_t)(id.z - split);
    const uint32_t dx = (uint32_t)id.x >> split;
    const uint32_t dy = (uint32_t)id.y >> split;

    /* 1. The cache. Its keys are the payload's own z/x/y, which with a
     *    split is coarser than the subtile's. */
    bool empty_marked = false;
    if (s->cache && tilecache_is_open(s->cache)) {
        uint32_t n = r->tile_cap;
        if (tilecache_get(s->cache, dz, dx, dy, r->tile, &n)) {
            if (n == 0) {
                empty_marked = true;
            } else {
                const tile_state_t t = maprender_payload(r, n, id, px, split);
                if (t == TILE_READY) {
                    s->st.cache_hits++;
                    if (from) *from = TILESRC_CACHE;
                    return t;
                }
                /* A cached payload that will not draw is not trusted for
                 * the network's sake; the card may still have it. */
            }
        }
    }

    /* 2. The card. */
    bool local_failed = false;
    if (s->local && s->local->n) {
        uint32_t n = 0;
        const tile_state_t f = mapset_fetch(s->local, r, id, split, &n);
        if (f == TILE_READY) {
            const tile_state_t t = maprender_payload(r, n, id, px, split);
            if (t == TILE_READY) {
                s->st.local_hits++;
                if (from) *from = TILESRC_LOCAL;
                return t;
            }
            local_failed = true;
        } else if (f == TILE_ERROR) {
            local_failed = true;
        }
    }

    if (empty_marked) { s->st.misses++; return TILE_NODATA; }

    /* 3. The network. */
    if (s->remote) {
        uint32_t n = 0;
        const tile_state_t f = maprender_fetch(r, s->remote, id, split, &n);
        if (f == TILE_NODATA) {
            if (s->cache) tilecache_put(s->cache, dz, dx, dy, NULL, 0);
            s->st.misses++;
            return TILE_NODATA;
        }
        if (f == TILE_READY) {
            /* Cached once it has drawn, so a payload that will not is
             * never kept; maprender_payload() leaves r->tile as it was. */
            const tile_state_t t = maprender_payload(r, n, id, px, split);
            if (t == TILE_READY) {
                if (s->cache) tilecache_put(s->cache, dz, dx, dy, r->tile, n);
                s->st.net_hits++;
                if (from) *from = TILESRC_NET;
                return t;
            }
        }
        s->st.errors++;
        return TILE_ERROR;
    }

    if (local_failed) { s->st.errors++; return TILE_ERROR; }
    s->st.misses++;
    return TILE_NODATA;
}

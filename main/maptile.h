/*
 * maptile.h -- one PMTiles archive, and one tile of it drawn into RGB565.
 *
 * The middle of the original's mapengine.cpp, without the rest of it:
 * open an archive through a read callback (netsource.cpp's fit_buffers()
 * and gz_inflate()), and render a tile from it (mapengine.cpp's
 * render_tile()) -- fetch, check it is gzip, inflate, then decode the
 * MVT once per layer in draw order and rasterise each pass. Labels,
 * the network and the tile cache are not here; this is the offline path
 * from a file on the card to pixels.
 *
 * Free of ESP-IDF. Memory comes from the caller's allocator, so the
 * firmware can put the big buffers in PSRAM and the hot ones in internal
 * RAM (mapengine's alloc_fast()), and test/ can use malloc.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pmtiles.h"
#include "raster.h"
#include "tile_grid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where memory comes from. `fast` may return NULL and is then retried
 * with `big`; `big` NULL is a failure. One `release` frees both. */
typedef struct {
    void *(*big)(size_t n);
    void *(*fast)(size_t n);
    void  (*release)(void *p);
} maptile_alloc_t;

/* ---- the archive ---- */

typedef struct {
    pmt_t     pmt;
    maptile_alloc_t mem;
    uint8_t  *raw, *dir, *root;
    uint32_t  cap;              /* raw's size; dir and root are 4x */
} maparchive_t;

/*
 * Open an archive read through `read(ctx, off, len, dst)` (0 on success).
 * Directory buffers start at 256 KB and grow, up to 4 MB, if a directory
 * turns out larger. Tiles must be gzip-compressed MVT, which is what
 * the Protomaps basemap builds are.
 */
pmt_err_t maparchive_open(maparchive_t *a, pmt_read_fn read, void *ctx,
                          const maptile_alloc_t *mem);
void      maparchive_close(maparchive_t *a);

/* ---- rendering ---- */

typedef struct {
    maptile_alloc_t mem;
    int        size;            /* output tile edge in pixels */
    uint8_t   *tile;  uint32_t tile_cap;    /* compressed payload */
    uint8_t   *mvt;   uint32_t mvt_cap;     /* inflated payload */
    rs_edge_t *edges;
    uint16_t  *active;
    int32_t   *xs;
    int8_t    *dirs;
    uint16_t  *cov;
    int32_t   *pts;
    uint8_t   *val;
    const char **val_name;
    uint16_t  *val_name_len;
    /* For the log, from the last call. */
    uint32_t   last_bytes, last_inflated;
} maprender_t;

/* Scratch for tiles `size` pixels on an edge. 0 on success. */
int  maprender_init(maprender_t *r, int size, const maptile_alloc_t *mem);
void maprender_free(maprender_t *r);

/*
 * Draw tile `id` -- whose data is `split` zooms coarser, as the original's
 * SUBTILE_SPLIT -- into `px`, size x size RGB565, in the current style.
 *
 *   TILE_READY    drawn
 *   TILE_NODATA   the archive has no such tile (outside it, or ocean);
 *                 px is untouched
 *   TILE_ERROR    the archive could not be read, or the tile was not
 *                 gzip MVT, or would not inflate; px is untouched
 */
tile_state_t maprender_tile(maprender_t *r, maparchive_t *a, tile_id_t id,
                            uint16_t *px, int split);

#ifdef __cplusplus
}
#endif

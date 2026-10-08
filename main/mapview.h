/*
 * mapview.h -- the map around a position: which tiles, drawn when, and
 * the window of them that is on screen.
 *
 * original/mapengine.cpp's grid handling (map_begin(), recentre(),
 * view_follow(), the render worker's commit) cut down to milestone 1:
 * one zoom, one task. tile_grid.c holds GRID_N x GRID_N subtiles of
 * SUBTILE_PX around the position; when the position drifts more than
 * half a tile from the grid's centre the grid shifts, keeping the tiles
 * it still covers and queueing the rest, nearest first. mapview_step()
 * renders one queued tile. mapview_compose() copies the window centred
 * on the position into a framebuffer.
 *
 * Free of ESP-IDF, and without a lock: a caller with a render worker on
 * another task holds its own around everything but the draw (see
 * mapview_take()).
 *
 * THE OVERVIEW (0014). One tile COARSE_STEP zooms below the grid's,
 * drawn at COARSE_PX, covering 4 x 4 grid tiles around its middle --
 * original/mapengine.cpp's coarse overview. Wherever a slot has nothing
 * of its own to show -- not drawn yet, no data, failed -- the window is
 * filled from it, scaled up nearest-neighbour, so the map is soft rather
 * than blank while tiles arrive and where they never will. The original
 * copied the overview into the slot's buffer; here the render worker
 * draws straight into a PENDING slot's buffer, so the overview is
 * sampled at compose time instead and the slot is left alone. Two
 * buffers, so the one on screen is never the one being drawn.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mapconfig.h"
#include "mapset.h"
#include "tile_grid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draw tile `id` (data `split` zooms coarser) into `px`. mapset_render()
 * and tilesrc_draw() are two. */
typedef tile_state_t (*mapview_draw_fn)(void *ctx, tile_id_t id, uint16_t *px, int split);

typedef struct {
    mapview_draw_fn draw;
    void         *draw_ctx;
    tile_grid_t   grid;
    uint16_t     *bufs[GRID_COUNT];
    render_job_t  jobs[GRID_COUNT];
    int           njobs;
    uint8_t       z;
    double        fx, fy;           /* the position, in tiles at z */
    bool          placed;
    uint16_t      background;
    /* The overview: none until mapview_set_coarse(). */
    uint16_t     *coarse_px[2];     /* [cz_front] composed, the other drawn */
    int           cz_front;
    tile_id_t     cz_have;          /* what coarse_px[cz_front] holds... */
    bool          cz_ok;            /* ...if this is set */
    tile_id_t     cz_want;          /* what the grid now needs */
    bool          cz_busy;          /* taken and not yet committed */
    bool          cz_void;          /* restyled while busy: discard it */
    tile_id_t     cz_tried;         /* last that did not draw, and why */
    tile_state_t  cz_tried_state;   /* TILE_EMPTY: nothing tried */
    /* Overview pixel for each grid-tile pixel along a row or a column,
     * within one grid tile's share of the overview. */
    uint16_t      cz_map[SUBTILE_PX];
} mapview_t;

/* src: original/mapengine.cpp COARSE_STEP_CFG. The overview is this
 * many zooms below the grid: z12 under z14, 4 x 4 grid tiles across. */
#define COARSE_STEP     (2)

/* `bufs` are GRID_COUNT buffers of SUBTILE_PX x SUBTILE_PX RGB565. */
void mapview_init(mapview_t *v, mapview_draw_fn draw, void *draw_ctx,
                  uint16_t *const *bufs, uint8_t z, uint16_t background);

/* Follow a position. Queues whatever tiles it brings into the grid. */
void mapview_centre(mapview_t *v, double lat, double lon);

/* The same, at a point in tiles at the view's zoom -- where a pan has put
 * the view (0016), which is not a position anyone measured. */
void mapview_centre_tiles(mapview_t *v, double fx, double fy);

/* Render the nearest queued tile. True if one was rendered (whatever its
 * result); false with nothing queued. */
bool mapview_step(mapview_t *v);

/*
 * mapview_step() in three, for a render worker on another task: take the
 * nearest job under the caller's lock, draw into *px without it, commit
 * under it again. A job the grid has moved on from in between is refused
 * by the commit (tile_grid.c's generation check), and its pixels were a
 * PENDING slot's all along, so nothing half-drawn is ever composed.
 */
bool mapview_take(mapview_t *v, render_job_t *job, uint16_t **px);
void mapview_commit(mapview_t *v, const render_job_t *job, tile_state_t t);

/*
 * Queue again every tile that failed, and with `nodata_too` every tile
 * that had no data -- for when a source has appeared since (the network
 * came up) or a failure may have passed (it dropped mid-tile). Returns
 * how many.
 */
int  mapview_redo(mapview_t *v, bool nodata_too);

/* Tiles still to render. */
int  mapview_pending(const mapview_t *v);

/*
 * The palette changed (0015): every drawn tile and the overview are in
 * the old colours, so all of them are drawn again, nearest first, and
 * the background is `background` from now on. A tile with no data or a
 * failure has no pixels to recolour and is left as it is.
 */
void mapview_restyle(mapview_t *v, uint16_t background);

/* Turn the overview on: two COARSE_PX x COARSE_PX RGB565 buffers. */
void mapview_set_coarse(mapview_t *v, uint16_t *a, uint16_t *b);

/*
 * The overview's render, in the same three parts as a slot's: take
 * returns true, with the tile and where to draw it COARSE_PX square,
 * when the grid has moved off the one held and it has not been tried
 * yet; commit swaps it in if it drew and the grid still wants it. A
 * failure is not taken again until mapview_redo() -- TILE_ERROR on any
 * redo, TILE_NODATA with nodata_too -- or the grid wants another tile.
 */
bool mapview_coarse_take(mapview_t *v, tile_id_t *id, uint16_t **px);
void mapview_coarse_commit(mapview_t *v, tile_id_t id, tile_state_t t);

/* Whether the overview held now covers the grid. */
bool mapview_coarse_ok(const mapview_t *v);

/*
 * Copy the w x h window centred on the position into `fb` (row stride
 * `stride` pixels). Tiles not drawn yet, or with no data, come from the
 * overview where it covers them, and are the background colour where
 * it does not.
 */
void mapview_compose(const mapview_t *v, uint16_t *fb, int w, int h, int stride);

#ifdef __cplusplus
}
#endif

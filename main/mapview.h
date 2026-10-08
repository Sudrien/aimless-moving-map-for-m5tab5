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
 * Free of ESP-IDF. Rendering and composing are called from the same
 * task, so there is no lock: the original's render worker on its own
 * core comes back with the second milestone, when a tile render must
 * stop holding up the screen.
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

typedef struct {
    mapset_t     *set;
    maprender_t  *render;
    tile_grid_t   grid;
    uint16_t     *bufs[GRID_COUNT];
    render_job_t  jobs[GRID_COUNT];
    int           njobs;
    uint8_t       z;
    double        fx, fy;           /* the position, in tiles at z */
    bool          placed;
    uint16_t      background;
} mapview_t;

/* `bufs` are GRID_COUNT buffers of SUBTILE_PX x SUBTILE_PX RGB565. */
void mapview_init(mapview_t *v, mapset_t *set, maprender_t *render,
                  uint16_t *const *bufs, uint8_t z, uint16_t background);

/* Follow a position. Queues whatever tiles it brings into the grid. */
void mapview_centre(mapview_t *v, double lat, double lon);

/* Render the nearest queued tile. True if one was rendered (whatever its
 * result); false with nothing queued. */
bool mapview_step(mapview_t *v);

/* Tiles still to render. */
int  mapview_pending(const mapview_t *v);

/*
 * Copy the w x h window centred on the position into `fb` (row stride
 * `stride` pixels). Tiles not drawn yet, or with no data, are the
 * background colour.
 */
void mapview_compose(const mapview_t *v, uint16_t *fb, int w, int h, int stride);

#ifdef __cplusplus
}
#endif

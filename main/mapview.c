/*
 * mapview.c -- see mapview.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mapview.h"

#include <math.h>
#include <string.h>

#include "mercator.h"

void mapview_init(mapview_t *v, mapset_t *set, maprender_t *render,
                  uint16_t *const *bufs, uint8_t z, uint16_t background)
{
    memset(v, 0, sizeof(*v));
    v->set = set;
    v->render = render;
    v->z = z;
    v->background = background;
    for (int i = 0; i < GRID_COUNT; i++) v->bufs[i] = bufs[i];
}

/* Distance of a slot from the grid's middle, doubled; tile_grid.c's
 * slot_rank(), so the queue is nearest first as the original's is. */
static int rank(int i)
{
    const int r = i / GRID_N, c = i % GRID_N;
    const int dr = 2 * r - (GRID_N - 1), dc = 2 * c - (GRID_N - 1);
    return dr * dr + dc * dc;
}

/* Every slot still PENDING, at the grid's current generation. Rebuilt
 * after every move rather than appended to: a job queued before a shift
 * carries the old generation and grid_commit() would refuse it, leaving
 * a slot that survived the shift PENDING for ever. */
static void requeue(mapview_t *v)
{
    v->njobs = 0;
    for (int pass = 0; pass <= 2 * (GRID_N - 1) * (GRID_N - 1); pass++) {
        for (int i = 0; i < GRID_COUNT; i++) {
            if (rank(i) != pass || v->grid.slots[i].state != TILE_PENDING) continue;
            v->jobs[v->njobs].id = v->grid.slots[i].id;
            v->jobs[v->njobs].slot = (uint8_t)i;
            v->jobs[v->njobs].generation = v->grid.generation;
            v->njobs++;
        }
    }
}

void mapview_centre(mapview_t *v, double lat, double lon)
{
    const merc_pt_t p = merc_from_ll(lat, lon, v->z);
    v->fx = p.x;
    v->fy = p.y;
    v->placed = true;

    if (!v->grid.initialised) {
        const tile_id_t origin = { v->z, grid_origin_for(p.x), grid_origin_for(p.y) };
        grid_init(&v->grid, v->bufs, origin);
        requeue(v);
        return;
    }
    /* A jump of more than one tile is a new grid, not a shift. */
    int dx, dy;
    grid_drift(&v->grid, p.x, p.y, &dx, &dy);
    const double mid = (double)GRID_N / 2.0;
    if (fabs(p.x - (v->grid.origin.x + mid)) > GRID_N || fabs(p.y - (v->grid.origin.y + mid)) > GRID_N) {
        const tile_id_t origin = { v->z, grid_origin_for(p.x), grid_origin_for(p.y) };
        render_job_t unused[GRID_COUNT];
        grid_set_zoom(&v->grid, origin, unused, GRID_COUNT);
        requeue(v);
        return;
    }
    if (dx || dy) {
        render_job_t unused[GRID_COUNT];
        grid_shift(&v->grid, dx, dy, unused, GRID_COUNT);
        requeue(v);
    }
}

int mapview_pending(const mapview_t *v) { return v->njobs; }

bool mapview_step(mapview_t *v)
{
    if (v->njobs == 0) return false;
    const render_job_t job = v->jobs[0];
    memmove(&v->jobs[0], &v->jobs[1], (size_t)(v->njobs - 1) * sizeof(v->jobs[0]));
    v->njobs--;
    subtile_t *s = &v->grid.slots[job.slot];
    const tile_state_t t = mapset_render(v->set, v->render, job.id, s->pixels, SUBTILE_SPLIT);
    grid_commit(&v->grid, &job, t);
    return true;
}

void mapview_compose(const mapview_t *v, uint16_t *fb, int w, int h, int stride)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) fb[(size_t)y * stride + x] = v->background;
    if (!v->placed || !v->grid.initialised) return;

    /* The position in grid pixels, and the window's top-left. */
    const double gx = (v->fx - v->grid.origin.x) * SUBTILE_PX;
    const double gy = (v->fy - v->grid.origin.y) * SUBTILE_PX;
    const int left = (int)floor(gx) - w / 2;
    const int top  = (int)floor(gy) - h / 2;

    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            const subtile_t *s = &v->grid.slots[r * GRID_N + c];
            if (!tile_drawable(s->state)) continue;
            /* This slot's rectangle in window coordinates, clipped. */
            const int sx0 = c * SUBTILE_PX - left, sy0 = r * SUBTILE_PX - top;
            const int x0 = sx0 < 0 ? 0 : sx0, y0 = sy0 < 0 ? 0 : sy0;
            int x1 = sx0 + SUBTILE_PX, y1 = sy0 + SUBTILE_PX;
            if (x1 > w) x1 = w;
            if (y1 > h) y1 = h;
            if (x0 >= x1 || y0 >= y1) continue;
            for (int y = y0; y < y1; y++)
                memcpy(&fb[(size_t)y * stride + x0],
                       &s->pixels[(size_t)(y - sy0) * SUBTILE_PX + (x0 - sx0)],
                       (size_t)(x1 - x0) * sizeof(uint16_t));
        }
    }
}

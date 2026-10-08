/*
 * area.c -- see area.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "area.h"

#include <string.h>

static int radius_of(int level) { return level == 0 ? AREA_RADIUS : AREA_CZ_RADIUS; }

int area_total(void)
{
    const int a = 2 * AREA_RADIUS + 1, b = 2 * AREA_CZ_RADIUS + 1;
    return a * a + b * b;
}

void area_start(area_t *a, uint8_t z, int32_t cx, int32_t cy)
{
    memset(a, 0, sizeof(*a));
    a->active = true;
    a->z = z;
    a->cx = cx;
    a->cy = cy;
    a->total = area_total();
}

/* Cell k of ring r around 0,0: the top edge left to right, the right
 * edge down, the bottom right to left, the left edge up; 8r cells, or
 * the one in the middle for r 0. */
static void ring_cell(int r, int k, int *dx, int *dy)
{
    if (r == 0) { *dx = *dy = 0; return; }
    const int side = 2 * r;
    if (k < side + 1)          { *dx = -r + k;                    *dy = -r; return; }
    k -= side + 1;
    if (k < side)              { *dx = r;                         *dy = -r + 1 + k; return; }
    k -= side;
    if (k < side)              { *dx = r - 1 - k;                 *dy = r; return; }
    k -= side;
    *dx = -r;
    *dy = r - 1 - k;
}

static int ring_len(int r) { return r == 0 ? 1 : 8 * r; }

bool area_next(area_t *a, tile_id_t *id)
{
    while (a->active) {
        if (a->level > 1) { a->active = false; break; }
        const int rad = radius_of(a->level);
        if (a->ring > rad) {
            a->level++;
            a->ring = a->k = 0;
            continue;
        }
        int dx, dy;
        ring_cell(a->ring, a->k, &dx, &dy);
        if (++a->k >= ring_len(a->ring)) { a->ring++; a->k = 0; }
        a->done++;

        const int step = a->level == 0 ? 0 : AREA_CZ_STEP;
        const uint8_t z = (uint8_t)(a->z >= step ? a->z - step : 0);
        const int64_t n = (int64_t)1 << z;
        /* The centre at this level: the z tile's ancestor. */
        const int64_t bx = (int64_t)a->cx >> (a->z - z);
        const int64_t by = (int64_t)a->cy >> (a->z - z);
        const int64_t y = by + dy;
        if (y < 0 || y >= n) continue;
        int64_t x = (bx + dx) % n;
        if (x < 0) x += n;
        id->z = z;
        id->x = (int32_t)x;
        id->y = (int32_t)y;
        return true;
    }
    return false;
}

int area_progress(const area_t *a)
{
    if (a->total <= 0) return 0;
    const int p = a->done * 100 / a->total;
    return p > 100 ? 100 : p;
}

int area_pending(const mapset_t *set, uint8_t z, int32_t cx, int32_t cy)
{
    area_t a;
    area_start(&a, z, cx, cy);
    int need = 0;
    tile_id_t id;
    while (area_next(&a, &id))
        if (!mapset_covers_any(set, id.z, (uint32_t)id.x, (uint32_t)id.y)) need++;
    return need;
}

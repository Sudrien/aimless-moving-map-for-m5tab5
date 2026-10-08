/*
 * worldtile.c -- see worldtile.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "worldtile.h"

#include <string.h>

tile_state_t worldtile_draw(maprender_t *r, const uint8_t *gz, size_t len, uint16_t *px)
{
    if (!gz || len == 0) return TILE_NODATA;
    if (len > r->tile_cap) return TILE_ERROR;
    memcpy(r->tile, gz, len);
    const tile_id_t world = { 0, 0, 0 };
    return maprender_payload(r, (uint32_t)len, world, px, 0);
}

void worldtile_compose(const uint16_t *px, int size, uint16_t *fb, int w, int h, int stride)
{
    if (!px || size <= 0 || w <= 0 || h <= 0) return;
    const int side = w > h ? w : h;
    const int ox = (side - w) / 2, oy = (side - h) / 2;
    int prev = -1;
    for (int y = 0; y < h; y++) {
        uint16_t *dst = &fb[(size_t)y * stride];
        const int sy = (int)((int64_t)(y + oy) * size / side);
        if (sy == prev) {
            memcpy(dst, dst - stride, (size_t)w * sizeof(uint16_t));
            continue;
        }
        const uint16_t *row = &px[(size_t)sy * size];
        if (side == size) {
            memcpy(dst, row + ox, (size_t)w * sizeof(uint16_t));
        } else {
            for (int x = 0; x < w; x++)
                dst[x] = row[(int64_t)(x + ox) * size / side];
        }
        prev = sy;
    }
}

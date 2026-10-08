/*
 * mapset.c -- see mapset.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "mapset.h"

#include "mercator.h"

bool mapset_add(mapset_t *s, maparchive_t *a)
{
    if (s->n >= MAPSET_MAX) return false;
    s->arc[s->n++] = a;
    return true;
}

bool mapset_covers(const maparchive_t *a, uint8_t z, uint32_t x, uint32_t y)
{
    const pmt_header_t *h = &a->pmt.hdr;
    if (z < h->min_zoom || z > h->max_zoom) return false;

    /* The tile's corners in degrees: west/north at (x, y), east/south at
     * (x+1, y+1). */
    double n_lat, w_lon, s_lat, e_lon;
    merc_to_ll((merc_pt_t){ (double)x, (double)y, z }, &n_lat, &w_lon);
    merc_to_ll((merc_pt_t){ (double)x + 1, (double)y + 1, z }, &s_lat, &e_lon);

    const double min_lon = h->min_lon_e7 / 1e7, max_lon = h->max_lon_e7 / 1e7;
    const double min_lat = h->min_lat_e7 / 1e7, max_lat = h->max_lat_e7 / 1e7;
    /* An archive with no bounds written (all zero) claims everything. */
    if (min_lon == 0 && max_lon == 0 && min_lat == 0 && max_lat == 0) return true;
    return e_lon > min_lon && w_lon < max_lon && n_lat > min_lat && s_lat < max_lat;
}

tile_state_t mapset_render(mapset_t *s, maprender_t *r, tile_id_t id,
                           uint16_t *px, int split)
{
    const uint8_t  dz = (uint8_t)(id.z - split);
    const uint32_t dx = (uint32_t)id.x >> split;
    const uint32_t dy = (uint32_t)id.y >> split;
    int asked = 0, failed = 0;
    for (int i = 0; i < s->n; i++) {
        if (!mapset_covers(s->arc[i], dz, dx, dy)) continue;
        asked++;
        const tile_state_t t = maprender_tile(r, s->arc[i], id, px, split);
        if (t == TILE_READY) return t;
        if (t == TILE_ERROR) failed++;
    }
    return (asked && failed == asked) ? TILE_ERROR : TILE_NODATA;
}

bool mapset_centre(const mapset_t *s, double *lat, double *lon)
{
    if (s->n == 0) return false;
    const pmt_header_t *h = &s->arc[0]->pmt.hdr;
    *lat = h->center_lat_e7 / 1e7;
    *lon = h->center_lon_e7 / 1e7;
    if (*lat == 0 && *lon == 0) {
        *lat = (h->min_lat_e7 + (double)h->max_lat_e7) / 2e7;
        *lon = (h->min_lon_e7 + (double)h->max_lon_e7) / 2e7;
    }
    return true;
}

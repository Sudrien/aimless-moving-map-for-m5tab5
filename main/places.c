/*
 * places.c -- see places.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "places.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "style.h"

tile_id_t places_centre(uint8_t view_z, double wx, double wy, uint8_t z)
{
    const double scale = ldexp(1.0, (int)z - (int)view_z);
    const int64_t n = (int64_t)1 << z;
    int64_t x = (int64_t)floor(wx * scale), y = (int64_t)floor(wy * scale);
    x %= n;
    if (x < 0) x += n;
    if (y < 0) y = 0;
    if (y >= n) y = n - 1;
    const tile_id_t c = { z, (int32_t)x, (int32_t)y };
    return c;
}

int places_block(tile_id_t centre, tile_id_t out[9])
{
    /* Centre first: if the index fills, what is lost is furthest away.
     * The original went row by row from the north-west. */
    static const int8_t ORDER[9][2] = {
        { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 },
        { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 },
    };
    const int64_t n = (int64_t)1 << centre.z;
    int k = 0;
    for (int i = 0; i < 9; i++) {
        const int64_t y = (int64_t)centre.y + ORDER[i][1];
        if (y < 0 || y >= n) continue;
        int64_t x = ((int64_t)centre.x + ORDER[i][0]) % n;
        if (x < 0) x += n;
        /* At z0 and z1 the wrap brings a column round twice. */
        bool dup = false;
        for (int j = 0; j < k && !dup; j++) dup = out[j].x == (int32_t)x && out[j].y == (int32_t)y;
        if (dup) continue;
        out[k].z = centre.z;
        out[k].x = (int32_t)x;
        out[k].y = (int32_t)y;
        k++;
    }
    return k;
}

void places_begin(places_index_t *idx, uint8_t z)
{
    idx->z = z;
    idx->n = 0;
    idx->full = false;
}

int places_part(void *ctx, const mvt_part_t *part)
{
    const places_sink_t *ps = ctx;
    places_index_t *idx = ps->idx;
    if (!part->name || part->n_pts < 1 || part->geom != MVT_POINT) return 0;
    const bool fine = part->style == S_PLACE_HOOD || part->style == S_PLACE_LOCALITY;
    const bool coarse = part->style == S_PLACE_REGION || part->style == S_PLACE_COUNTRY;
    if (!(idx->z == PLACES_FINE_Z ? fine : coarse)) return 0;
    if (idx->n >= PLACES_MAX) {
        idx->full = true;
        return 1;
    }
    maplabel_t *m = &idx->v[idx->n];
    if (maplabel_copy(m->text, part->name, part->name_len) == 0) return 0;
    /* src: original/mapengine.cpp place_part(): 4096 to the tile, MVT's
     * extent in every Protomaps build. */
    m->fx = (float)((double)ps->id.x + (double)part->pts[0] / 4096.0);
    m->fy = (float)((double)ps->id.y + (double)part->pts[1] / 4096.0);
    m->style = part->style;
    idx->n++;
    return 0;
}

static void search(const places_index_t *idx, uint8_t view_z, double wx, double wy,
                   double best[PLACES_RANKS], const maplabel_t *pick[PLACES_RANKS])
{
    if (!idx || idx->n == 0) return;
    const double scale = ldexp(1.0, (int)idx->z - (int)view_z);
    const double px = wx * scale, py = wy * scale;
    /* The radii are in tiles at the view's zoom; brought to this one's. */
    const double hood = PLACES_HOOD_R * scale, loc = PLACES_LOCALITY_R * scale;
    for (int k = 0; k < idx->n; k++) {
        const maplabel_t *m = &idx->v[k];
        int r;
        double lim;
        switch (m->style) {
        case S_PLACE_HOOD:     r = PLACES_HOOD;     lim = hood; break;
        case S_PLACE_LOCALITY: r = PLACES_LOCALITY; lim = loc;  break;
        case S_PLACE_REGION:   r = PLACES_REGION;   lim = 0;    break;
        case S_PLACE_COUNTRY:  r = PLACES_COUNTRY;  lim = 0;    break;
        default: continue;
        }
        const double dx = (double)m->fx - px, dy = (double)m->fy - py;
        const double d2 = dx * dx + dy * dy;
        if (lim > 0 && d2 > lim * lim) continue;
        if (d2 < best[r]) {
            best[r] = d2;
            pick[r] = m;
        }
    }
}

void places_pick(places_t *p, const places_index_t *fine,
                 const places_index_t *coarse, uint8_t view_z, double wx, double wy)
{
    double best[PLACES_RANKS];
    const maplabel_t *pick[PLACES_RANKS] = { NULL };
    for (int r = 0; r < PLACES_RANKS; r++) best[r] = INFINITY;
    search(fine, view_z, wx, wy, best, pick);
    search(coarse, view_z, wx, wy, best, pick);
    for (int r = 0; r < PLACES_RANKS; r++)
        if (pick[r]) snprintf(p->name[r], sizeof(p->name[r]), "%s", pick[r]->text);
    if (!pick[PLACES_HOOD]) p->name[PLACES_HOOD][0] = '\0';
}

bool places_text(const places_t *p, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';
    size_t used = 0;
    for (int r = 0; r < PLACES_RANKS; r++) {
        if (!p->name[r][0]) continue;
        bool dup = false;
        for (int q = 0; q < r && !dup; q++) dup = strcmp(p->name[q], p->name[r]) == 0;
        if (dup) continue;
        const int n = snprintf(out + used, cap - used, "%s%s", used ? ", " : "", p->name[r]);
        if (n < 0 || (size_t)n >= cap - used) {
            out[cap - 1] = '\0';
            break;
        }
        used += (size_t)n;
    }
    return out[0] != '\0';
}

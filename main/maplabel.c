/*
 * maplabel.c -- see maplabel.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "maplabel.h"

#include <string.h>

#include "style.h"

void maplabel_reset(maplabel_set_t *s)
{
    if (s) s->n = 0;
}

/* The longest prefix of `len` bytes or fewer that does not end inside a
 * UTF-8 sequence: back off any continuation bytes, then the lead byte
 * they belong to if its sequence would not fit. */
static uint32_t utf8_cut(const char *t, uint32_t len, uint32_t max)
{
    if (len <= max) return len;
    uint32_t n = max;
    while (n > 0 && ((uint8_t)t[n] & 0xC0) == 0x80) n--;
    return n;
}

uint32_t maplabel_copy(char dst[MAPLABEL_TEXT_MAX], const char *text, uint32_t len)
{
    if (!text) return 0;
    len = utf8_cut(text, len, MAPLABEL_TEXT_MAX - 1);
    if (len == 0) return 0;
    memcpy(dst, text, len);
    dst[len] = '\0';
    return len;
}

bool maplabel_add(maplabel_set_t *s, float fx, float fy, uint8_t style,
                  const char *text, uint32_t len)
{
    if (!s || !text || len == 0 || s->n >= MAPLABEL_PER_TILE) return false;
    if (!(fx >= 0.0f && fx < 1.0f && fy >= 0.0f && fy < 1.0f)) return false;
    maplabel_t *m = &s->v[s->n];
    if (maplabel_copy(m->text, text, len) == 0) return false;
    s->n++;
    m->fx = fx;
    m->fy = fy;
    m->style = style;
    return true;
}

int maplabel_rank(uint8_t style)
{
    switch (style) {
    case S_PLACE_COUNTRY:  return 0;
    case S_PLACE_REGION:   return 1;
    case S_PLACE_LOCALITY: return 2;
    case S_PLACE_HOOD:     return 3;
    default:               return 4;
    }
}

int maplabel_scale(uint8_t style)
{
    switch (style) {
    case S_PLACE_COUNTRY:
    case S_PLACE_REGION:
    case S_PLACE_LOCALITY: return 3;
    default:               return 2;
    }
}

static bool overlaps(const maplabel_placed_t *a, const maplabel_placed_t *b)
{
    return a->bx < b->bx + b->bw && b->bx < a->bx + a->bw &&
           a->by < b->by + b->bh && b->by < a->by + a->bh;
}

int maplabel_layout(const maplabel_src_t *src, int n, int tile_px,
                    int w, int top, int bot, int glyph_h,
                    maplabel_width_fn width,
                    maplabel_placed_t *out, int max)
{
    int placed = 0;
    for (int rank = 0; rank < MAPLABEL_RANKS && placed < max; rank++) {
        for (int i = 0; i < n && placed < max; i++) {
            const maplabel_set_t *ls = src[i].set;
            if (!ls) continue;
            for (int k = 0; k < ls->n && placed < max; k++) {
                const maplabel_t *m = &ls->v[k];
                if (maplabel_rank(m->style) != rank) continue;

                maplabel_placed_t *p = &out[placed];
                p->style = m->style;
                p->scale = (uint8_t)maplabel_scale(m->style);
                p->ax = src[i].x + (int)(m->fx * (float)tile_px);
                p->ay = src[i].y + (int)(m->fy * (float)tile_px);
                p->tw = width(m->text, p->scale);
                const int th = glyph_h * p->scale;

                /* src: original/mapengine.cpp draw_labels(): a POI's text
                 * centred 3 px above its dot, a place's on its point;
                 * the box 5 px wider each side, 2 or 3 px taller, and
                 * down over the dot for a POI. */
                const bool poi = m->style == S_POI;
                const int cy = poi ? p->ay - MAPLABEL_DOT_R - th / 2 - 3 : p->ay;
                p->tx = p->ax - p->tw / 2;
                p->ty = cy - th / 2;
                const int b0 = poi ? p->ay - MAPLABEL_DOT_R - th - 5 : p->ay - th / 2 - 3;
                const int b1 = poi ? p->ay + MAPLABEL_DOT_R + 2 : p->ay + th / 2 + 3;
                p->bx = p->ax - p->tw / 2 - 5;
                p->bw = p->tw + 10;
                p->by = b0;
                p->bh = b1 - b0;

                if (p->bx + p->bw <= 0 || p->bx >= w) continue;
                if (p->by < top || p->by + p->bh > bot) continue;

                bool hit = false;
                for (int j = 0; j < placed && !hit; j++) hit = overlaps(p, &out[j]);
                if (hit) continue;

                memcpy(p->text, m->text, sizeof(p->text));
                placed++;
            }
        }
    }
    return placed;
}

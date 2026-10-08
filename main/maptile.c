/*
 * maptile.c -- see maptile.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "maptile.h"

#include <string.h>

#include "inflate.h"
#include "mvt.h"
#include "style.h"

/* src: original/netsource.cpp DIR_CAP_MIN, DIR_CAP_MAX, DIR_EXPAND. */
#define DIR_CAP_MIN     (256u * 1024u)
#define DIR_CAP_MAX     (4u * 1024u * 1024u)
#define DIR_EXPAND      (4u)

/* src: original/mapengine.cpp TILE_CAP, MVT_CAP, MVT_CAP_MAX, PT_CAP,
 *      VAL_CAP, EDGE_CAP, XS_CAP. */
#define TILE_CAP        (192u * 1024u)
#define MVT_CAP         (384u * 1024u)
#define MVT_CAP_MAX     (3u * 1024u * 1024u)
#define PT_CAP          (2048u)
#define VAL_CAP         (1024u)
#define EDGE_CAP        (16384u)
#define XS_CAP          (4096u)

static void *get_big(const maptile_alloc_t *m, size_t n) { return m->big(n); }

static void *get_fast(const maptile_alloc_t *m, size_t n)
{
    void *p = m->fast ? m->fast(n) : NULL;
    return p ? p : m->big(n);
}

static void put(const maptile_alloc_t *m, void *p)
{
    if (p) m->release(p);
}

/* ---- the archive ---------------------------------------------------- */

/* Directories are gzip in every archive this reads; anything else fails,
 * as original/netsource.cpp gz_inflate() does. */
static int dir_inflate(void *ctx, uint8_t codec, const uint8_t *src, uint32_t src_len,
                       uint8_t *dst, uint32_t *dst_len)
{
    (void)ctx;
    if (codec == PMT_COMPRESS_NONE) {
        if (src_len > *dst_len) return -1;
        memcpy(dst, src, src_len);
        *dst_len = src_len;
        return 0;
    }
    if (codec != PMT_COMPRESS_GZIP) return -1;
    return inflate_auto(src, src_len, dst, dst_len) == INF_OK ? 0 : -1;
}

/* Buffers for directories of `want` compressed bytes. As fit_buffers(). */
static int fit(maparchive_t *a, uint32_t want)
{
    if (want < DIR_CAP_MIN) want = DIR_CAP_MIN;
    if (want > DIR_CAP_MAX) want = DIR_CAP_MAX;
    if (want <= a->cap) return 0;
    const uint32_t dcap = want * DIR_EXPAND;
    put(&a->mem, a->raw); put(&a->mem, a->dir); put(&a->mem, a->root);
    a->raw  = get_big(&a->mem, want);
    a->dir  = get_big(&a->mem, dcap);
    a->root = get_big(&a->mem, dcap);
    if (!a->raw || !a->dir || !a->root) { a->cap = 0; return -1; }
    a->cap = want;
    a->pmt.raw_buf = a->raw;  a->pmt.raw_cap = want;
    a->pmt.dir_buf = a->dir;  a->pmt.dir_cap = dcap;
    a->pmt.root_cache = a->root;  a->pmt.root_cache_cap = dcap;
    a->pmt.root_cache_len = 0;
    return 0;
}

pmt_err_t maparchive_open(maparchive_t *a, pmt_read_fn read, void *ctx,
                          const maptile_alloc_t *mem)
{
    memset(a, 0, sizeof(*a));
    a->mem = *mem;
    a->pmt.read = read;
    a->pmt.inflate = dir_inflate;
    a->pmt.io_ctx = ctx;
    if (fit(a, DIR_CAP_MIN) != 0) return PMT_ENOMEM;

    pmt_err_t e = pmt_open(&a->pmt);
    if (e != PMT_OK) return e;
    if (a->pmt.hdr.tile_type != PMT_TYPE_MVT) return PMT_EFORMAT;

    /* The root first, growing once if it is bigger than the default. */
    e = pmt_prime_root(&a->pmt);
    if (e == PMT_ENOMEM) {
        uint32_t need = a->pmt.need_raw;
        if (a->pmt.need_dir / DIR_EXPAND > need) need = a->pmt.need_dir / DIR_EXPAND;
        if (fit(a, need) != 0) return PMT_ENOMEM;
        e = pmt_prime_root(&a->pmt);
    }
    return e;
}

void maparchive_close(maparchive_t *a)
{
    put(&a->mem, a->raw); put(&a->mem, a->dir); put(&a->mem, a->root);
    memset(a, 0, sizeof(*a));
}

/* ---- rendering ------------------------------------------------------ */

int maprender_init(maprender_t *r, int size, const maptile_alloc_t *mem)
{
    memset(r, 0, sizeof(*r));
    r->mem = *mem;
    r->size = size;
    r->tile_cap = TILE_CAP;
    r->mvt_cap  = MVT_CAP;
    r->tile  = get_big(mem, TILE_CAP);
    r->mvt   = get_big(mem, MVT_CAP);
    r->edges = get_big(mem, EDGE_CAP * sizeof(rs_edge_t));
    /* Hottest first, as alloc_all() asks for them. */
    r->cov    = get_fast(mem, (size_t)size * sizeof(uint16_t));
    r->val    = get_fast(mem, VAL_CAP);
    r->xs     = get_fast(mem, XS_CAP * sizeof(int32_t));
    r->dirs   = get_fast(mem, XS_CAP);
    r->active = get_fast(mem, XS_CAP * sizeof(uint16_t));
    r->pts    = get_fast(mem, PT_CAP * 2 * sizeof(int32_t));
    r->val_name     = get_fast(mem, VAL_CAP * sizeof(const char *));
    r->val_name_len = get_fast(mem, VAL_CAP * sizeof(uint16_t));
    if (!r->tile || !r->mvt || !r->edges || !r->cov || !r->val || !r->xs ||
        !r->dirs || !r->active || !r->pts || !r->val_name || !r->val_name_len) {
        maprender_free(r);
        return -1;
    }
    /* rs_clear() leaves cov zeroed and the fillers expect it so. */
    memset(r->cov, 0, (size_t)size * sizeof(uint16_t));
    return 0;
}

void maprender_free(maprender_t *r)
{
    const maptile_alloc_t m = r->mem;
    if (!m.release) return;
    put(&m, r->tile); put(&m, r->mvt); put(&m, r->edges); put(&m, r->cov);
    put(&m, r->val); put(&m, r->xs); put(&m, r->dirs); put(&m, r->active);
    put(&m, r->pts); put(&m, (void *)r->val_name); put(&m, r->val_name_len);
    memset(r, 0, sizeof(*r));
}

/* The layer the current decode pass draws; rl_layer() in the original. */
typedef struct {
    rs_t       *rs;
    const char *want;
} pass_t;

static int pass_layer(void *ctx, const mvt_layer_t *l)
{
    const pass_t *p = ctx;
    return p->want && l->name_len == strlen(p->want) &&
           memcmp(l->name, p->want, l->name_len) == 0;
}

static int pass_part(void *ctx, const mvt_part_t *part)
{
    const pass_t *p = ctx;
    return rs_part(p->rs, part);
}

static uint8_t pass_style(void *ctx, const mvt_layer_t *l, const char *s, uint32_t n)
{
    (void)ctx;
    return style_lookup(NULL, l, s, n);
}

tile_state_t maprender_fetch(maprender_t *r, maparchive_t *a, tile_id_t id,
                             int split, uint32_t *len)
{
    *len = 0;
    const uint8_t  dz = (uint8_t)(id.z - split);
    const uint32_t dx = (uint32_t)id.x >> split;
    const uint32_t dy = (uint32_t)id.y >> split;

    uint32_t got = r->tile_cap;
    pmt_err_t e = pmt_get(&a->pmt, dz, dx, dy, r->tile, &got);
    if (e == PMT_ENOMEM && a->pmt.need_raw) {
        /* A leaf directory bigger than the buffers: grow and retry once. */
        uint32_t need = a->pmt.need_raw;
        if (a->pmt.need_dir / DIR_EXPAND > need) need = a->pmt.need_dir / DIR_EXPAND;
        if (fit(a, need) == 0) {
            got = r->tile_cap;
            e = pmt_get(&a->pmt, dz, dx, dy, r->tile, &got);
        }
    }
    if (e == PMT_NOTFOUND || e == PMT_ERANGE) return TILE_NODATA;
    if (e != PMT_OK) return TILE_ERROR;
    if (got == 0) return TILE_NODATA;
    *len = got;
    return TILE_READY;
}

tile_state_t maprender_payload(maprender_t *r, uint32_t got, tile_id_t id,
                               uint16_t *px, int split)
{
    r->last_bytes = r->last_inflated = 0;
    if (got == 0) return TILE_NODATA;
    if (got > r->tile_cap) return TILE_ERROR;
    r->last_bytes = got;
    const uint32_t qx = (uint32_t)id.x & ((1u << split) - 1u);
    const uint32_t qy = (uint32_t)id.y & ((1u << split) - 1u);

    /* Not gzip means not this tile: a transport problem, in the original's
     * words, wearing an inflate problem's clothes. */
    if (got < 18 || r->tile[0] != 0x1F || r->tile[1] != 0x8B) return TILE_ERROR;

    const uint32_t need = gzip_isize(r->tile, got);
    if (need > r->mvt_cap) {
        if (need > MVT_CAP_MAX) return TILE_ERROR;
        const uint32_t want = need + need / 4;      /* headroom for the next one */
        uint8_t *bigger = get_big(&r->mem, want);
        if (!bigger) return TILE_ERROR;
        put(&r->mem, r->mvt);
        r->mvt = bigger;
        r->mvt_cap = want;
    }
    uint32_t mlen = r->mvt_cap;
    if (inflate_auto_fast(r->tile, got, r->mvt, &mlen) != INF_OK) return TILE_ERROR;
    r->last_inflated = mlen;

    rs_t rs;
    memset(&rs, 0, sizeof(rs));
    rs.px = px; rs.w = rs.h = r->size; rs.extent = 4096;
    rs.src_span = 4096 >> split;
    rs.src_x0 = (int32_t)(qx * (uint32_t)rs.src_span);
    rs.src_y0 = (int32_t)(qy * (uint32_t)rs.src_span);
    rs.edges = r->edges;   rs.edge_cap = EDGE_CAP;
    rs.active = r->active; rs.active_cap = XS_CAP;
    rs.xs = r->xs; rs.dirs = r->dirs; rs.xs_cap = XS_CAP;
    rs.cov = r->cov;
    rs.styles = STYLES; rs.n_styles = S_COUNT;
    rs.cur_feature = -1;

    pass_t pass = { .rs = &rs, .want = NULL };
    mvt_decoder_t d;
    memset(&d, 0, sizeof(d));
    d.layer_cb = pass_layer;
    d.style_cb = pass_style;
    d.part_cb  = pass_part;
    d.ctx      = &pass;
    d.pt_buf   = r->pts;  d.pt_cap = PT_CAP;
    d.val_style = r->val; d.val_cap = VAL_CAP;
    d.val_name  = r->val_name;
    d.val_name_len = r->val_name_len;

    rs_clear(&rs, style_background());
    for (int i = 0; i < N_DRAW_ORDER; i++) {
        pass.want = DRAW_ORDER[i];
        mvt_decode(&d, r->mvt, mlen);
        rs_flush(&rs);
    }
    return TILE_READY;
}

tile_state_t maprender_tile(maprender_t *r, maparchive_t *a, tile_id_t id,
                            uint16_t *px, int split)
{
    r->last_bytes = r->last_inflated = 0;
    uint32_t got = 0;
    const tile_state_t t = maprender_fetch(r, a, id, split, &got);
    return t == TILE_READY ? maprender_payload(r, got, id, px, split) : t;
}

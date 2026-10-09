/*
 * maplabeltest.c -- main/maplabel.c: names kept, cut and ranked; laid
 * out over the window; and collected from the fixture's tiles through
 * maptile.c and mapview.c, staying with their pixels across a shift.
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maplabel.h"
#include "mapview.h"
#include "mercator.h"
#include "style.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define Z   14
#define CX  4823
#define CY  6160
#define W   1280
#define H   720
/* As aimless.c: the status bar's height, and a band ending above the
 * button row. Any band will do; these are the device's. */
#define TOP 36
#define BOT 620
#define GLYPH_H 12

/* ark12's narrow advance, 6 + 1 px a character at scale 1, for ASCII. */
static int width(const char *s, int scale)
{
    return (int)strlen(s) * 7 * scale;
}

static void set_one(maplabel_set_t *s, float fx, float fy, uint8_t st, const char *t)
{
    maplabel_add(s, fx, fy, st, t, (uint32_t)strlen(t));
}

static void keeping(void)
{
    printf("names kept, and cut at a character\n");
    static maplabel_set_t s;
    maplabel_reset(&s);
    CHECK(!maplabel_add(&s, 0.5f, 0.5f, S_POI, "x", 0), "an empty name kept");
    CHECK(!maplabel_add(&s, 0.5f, 0.5f, S_POI, NULL, 3), "a NULL name kept");
    CHECK(!maplabel_add(&s, 1.0f, 0.5f, S_POI, "east", 4), "x = 1 is the next tile's");
    CHECK(!maplabel_add(&s, 0.5f, -0.01f, S_POI, "north", 5), "y < 0 is the tile above's");
    CHECK(!maplabel_add(&s, NAN, 0.5f, S_POI, "nan", 3), "NaN kept");
    CHECK(s.n == 0, "%d kept", s.n);

    CHECK(maplabel_add(&s, 0.0f, 0.999f, S_POI, "Cafe", 4), "corner dropped");
    CHECK(s.n == 1 && strcmp(s.v[0].text, "Cafe") == 0 && s.v[0].style == S_POI, "kept wrong");

    /* Not NUL-terminated in the tile: only `len` bytes are read. */
    CHECK(maplabel_add(&s, 0.1f, 0.1f, S_POI, "Barbaz", 3), "prefix");
    CHECK(strcmp(s.v[1].text, "Bar") == 0, "prefix %s", s.v[1].text);

    char t[64];
    /* 39 bytes fit exactly. */
    memset(t, 'a', 39);
    CHECK(maplabel_add(&s, 0.1f, 0.1f, S_POI, t, 39) && strlen(s.v[2].text) == 39, "39 bytes");
    /* 38 + a two-byte character: 40 bytes, cut before the character. */
    memset(t, 'a', 38);
    memcpy(t + 38, "\xC3\xA9", 2);
    CHECK(maplabel_add(&s, 0.1f, 0.1f, S_POI, t, 40), "40 bytes");
    CHECK(strlen(s.v[3].text) == 38, "cut at %zu, inside the e-acute", strlen(s.v[3].text));
    /* 37 + a three-byte character ending at byte 40: cut before it. */
    memset(t, 'a', 37);
    memcpy(t + 37, "\xE5\x8C\x97", 3);
    CHECK(maplabel_add(&s, 0.1f, 0.1f, S_POI, t, 40), "40 bytes");
    CHECK(strlen(s.v[4].text) == 37, "cut at %zu", strlen(s.v[4].text));
    /* 36 + the same, ending at 39: kept whole. */
    memset(t, 'a', 36);
    memcpy(t + 36, "\xE5\x8C\x97", 3);
    CHECK(maplabel_add(&s, 0.1f, 0.1f, S_POI, t, 39), "39 bytes");
    CHECK(strlen(s.v[5].text) == 39, "cut at %zu", strlen(s.v[5].text));

    maplabel_reset(&s);
    int kept = 0;
    for (int i = 0; i < MAPLABEL_PER_TILE + 5; i++) kept += maplabel_add(&s, 0.5f, 0.5f, S_POI, "p", 1);
    CHECK(kept == MAPLABEL_PER_TILE && s.n == MAPLABEL_PER_TILE, "%d kept of a full set", kept);

    CHECK(maplabel_rank(S_PLACE_COUNTRY) == 0 && maplabel_rank(S_PLACE_REGION) == 1 &&
          maplabel_rank(S_PLACE_LOCALITY) == 2 && maplabel_rank(S_PLACE_HOOD) == 3 &&
          maplabel_rank(S_POI) == 4, "ranks");
    CHECK(maplabel_scale(S_PLACE_LOCALITY) == 3 && maplabel_scale(S_POI) == 2 &&
          maplabel_scale(S_PLACE_HOOD) == 2, "scales");
}

static void layout(void)
{
    printf("laid out: rank first, no overlaps, inside the band\n");
    static maplabel_set_t a, b;
    static maplabel_placed_t out[MAPLABEL_ON_SCREEN];
    maplabel_reset(&a);
    maplabel_reset(&b);

    /* A POI listed before a town on the same spot, in another tile: the
     * town is placed and the POI dropped. Tile a at (0,0), b at (-640,0),
     * so b's fx 0.75 and a's 0.25 are the same window x of 320. */
    set_one(&a, 0.25f, 0.30f, S_POI, "Petrol");
    set_one(&b, 0.75f, 0.30f, S_PLACE_LOCALITY, "Town");
    maplabel_src_t src[2] = { { &a, 0, 0 }, { &b, -640, 0 } };
    int n = maplabel_layout(src, 2, 1280, W, TOP, BOT, GLYPH_H, width, out, MAPLABEL_ON_SCREEN);
    CHECK(n == 1 && strcmp(out[0].text, "Town") == 0, "%d placed, first %s", n, n ? out[0].text : "-");
    CHECK(out[0].ax == 320 && out[0].ay == 384, "town at %d,%d", out[0].ax, out[0].ay);
    CHECK(out[0].scale == 3 && out[0].tw == 4 * 7 * 3, "scale %d width %d", out[0].scale, out[0].tw);
    /* A place is centred on its point. */
    CHECK(out[0].tx == 320 - out[0].tw / 2 && out[0].ty == 384 - 18, "text at %d,%d", out[0].tx, out[0].ty);

    /* Moved apart, both are placed; the POI's text above its dot, its
     * box down over the dot. */
    maplabel_reset(&a);
    set_one(&a, 0.25f, 0.45f, S_POI, "Petrol");
    n = maplabel_layout(src, 2, 1280, W, TOP, BOT, GLYPH_H, width, out, MAPLABEL_ON_SCREEN);
    CHECK(n == 2 && strcmp(out[0].text, "Town") == 0 && strcmp(out[1].text, "Petrol") == 0, "%d placed", n);
    const maplabel_placed_t *p = &out[1];
    CHECK(p->ty + 24 <= p->ay - MAPLABEL_DOT_R, "POI text bottom %d into its dot at %d", p->ty + 24, p->ay);
    CHECK(p->by + p->bh >= p->ay + MAPLABEL_DOT_R, "POI box misses its dot");
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            CHECK(!(out[i].bx < out[j].bx + out[j].bw && out[j].bx < out[i].bx + out[i].bw &&
                    out[i].by < out[j].by + out[j].bh && out[j].by < out[i].by + out[i].bh),
                  "%d and %d overlap", i, j);

    /* The band: under the status bar, over the row, and off the side
     * are dropped; half off the side is kept. */
    maplabel_reset(&a);
    maplabel_reset(&b);
    set_one(&a, 0.30f, 20.0f / 1280, S_PLACE_HOOD, "Under the bar");
    set_one(&a, 0.30f, 610.0f / 1280, S_PLACE_HOOD, "Over the row");
    set_one(&a, 0.99f, 0.30f, S_PLACE_HOOD, "Off the side");
    set_one(&a, 0.0f, 0.30f, S_PLACE_HOOD, "Half off");
    maplabel_src_t one[1] = { { &a, 0, 0 } };
    n = maplabel_layout(one, 1, 1280, 1000, TOP, BOT, GLYPH_H, width, out, MAPLABEL_ON_SCREEN);
    CHECK(n == 1 && strcmp(out[0].text, "Half off") == 0, "%d placed, first %s", n, n ? out[0].text : "-");
    CHECK(out[0].tx < 0, "half off at %d", out[0].tx);

    /* At most `max`, and no more than were asked for. */
    maplabel_reset(&a);
    for (int i = 0; i < MAPLABEL_PER_TILE; i++)
        set_one(&a, (float)(i % 8) / 8.0f, 0.05f + (float)(i / 8) * 0.07f, S_PLACE_HOOD, "x");
    n = maplabel_layout(one, 1, 1280, W, 0, 1280, GLYPH_H, width, out, 5);
    CHECK(n == 5, "%d placed of a max of 5", n);
    n = maplabel_layout(one, 1, 1280, W, 0, 1280, GLYPH_H, width, out, MAPLABEL_ON_SCREEN);
    CHECK(n == MAPLABEL_ON_SCREEN, "%d placed of %d", n, MAPLABEL_ON_SCREEN);
    CHECK(maplabel_layout(one, 0, 1280, W, 0, 1280, GLYPH_H, width, out, MAPLABEL_ON_SCREEN) == 0,
          "no tiles, and something placed");
}

/* ---- the fixture ---- */

static int file_read(void *ctx, uint64_t off, uint32_t len, uint8_t *dst)
{
    FILE *f = ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, len, f) == len ? 0 : -1;
}

static void *big(size_t n) { return malloc(n); }
static const maptile_alloc_t MEM = { big, NULL, free };

typedef struct { mapset_t *set; maprender_t *r; mapview_t *v; } draw_t;

/* As the render task: the buffer's set is where its names go. */
static tile_state_t draw(void *ctx, tile_id_t id, uint16_t *px, int split)
{
    draw_t *d = ctx;
    d->r->labels = mapview_labels_for(d->v, px);
    const tile_state_t t = mapset_render(d->set, d->r, id, px, split);
    d->r->labels = NULL;
    return t;
}

/* src: test/make_fixture.py tile_layers(): one cafe a tile at (1800, 1300)
 * of 4096, named for its tile. */
static void cafe(tile_id_t id, char *out, size_t n)
{
    snprintf(out, n, "Cafe %d", (int)((id.x * 3 + id.y) % 97));
}

static void fixture(void)
{
    printf("collected from the fixture, and kept with their pixels\n");
    FILE *f = fopen("fixture.pmtiles", "rb");
    if (!f) { CHECK(0, "run from test/: fixture.pmtiles not found"); return; }
    maparchive_t a;
    CHECK(maparchive_open(&a, file_read, f, &MEM) == PMT_OK, "open");
    mapset_t set = { 0 };
    CHECK(mapset_add(&set, &a), "add");

    maprender_t r;
    CHECK(maprender_init(&r, SUBTILE_PX, &MEM) == 0, "render scratch");
    CHECK(r.labels == NULL, "labels on by default");
    style_init(SUBTILE_PX, 0);

    /* Without a set, nothing is collected and the pixels are the same
     * as with one: collecting must not change the drawing. */
    uint16_t *px0 = malloc((size_t)SUBTILE_PX * SUBTILE_PX * sizeof(uint16_t));
    uint16_t *px1 = malloc((size_t)SUBTILE_PX * SUBTILE_PX * sizeof(uint16_t));
    static maplabel_set_t one;
    const tile_id_t mid = { Z, CX, CY };
    CHECK(maprender_tile(&r, &a, mid, px0, SUBTILE_SPLIT) == TILE_READY, "drawn without");
    r.labels = &one;
    one.n = 7;
    CHECK(maprender_tile(&r, &a, mid, px1, SUBTILE_SPLIT) == TILE_READY, "drawn with");
    r.labels = NULL;
    CHECK(memcmp(px0, px1, (size_t)SUBTILE_PX * SUBTILE_PX * sizeof(uint16_t)) == 0,
          "labels changed the pixels");
    char want[32];
    cafe(mid, want, sizeof(want));
    CHECK(one.n == 1, "%d labels in the middle tile (stale ones not cleared?)", one.n);
    CHECK(one.n >= 1 && strcmp(one.v[0].text, want) == 0 && one.v[0].style == S_POI,
          "label %s, want %s", one.n ? one.v[0].text : "-", want);
    CHECK(one.n >= 1 && fabsf(one.v[0].fx - 1800.0f / 4096) < 1e-6f &&
          fabsf(one.v[0].fy - 1300.0f / 4096) < 1e-6f, "at %f,%f", one.v[0].fx, one.v[0].fy);

    /* A tile not in the archive empties its set: no stale names. */
    r.labels = &one;
    const tile_id_t far = { Z, CX + 5, CY };
    CHECK(maprender_tile(&r, &a, far, px1, SUBTILE_SPLIT) == TILE_NODATA, "far tile");
    r.labels = NULL;
    CHECK(one.n == 1, "a fetch that fails leaves the set alone: %d", one.n);
    free(px0);
    free(px1);

    /* Through the view. */
    uint16_t *bufs[GRID_COUNT];
    static maplabel_set_t sets[GRID_COUNT];
    maplabel_set_t *setp[GRID_COUNT];
    for (int i = 0; i < GRID_COUNT; i++) {
        bufs[i] = malloc((size_t)SUBTILE_PX * SUBTILE_PX * sizeof(uint16_t));
        setp[i] = &sets[i];
        sets[i].n = 3;
    }
    static mapview_t v;
    draw_t d = { &set, &r, &v };
    mapview_init(&v, draw, &d, bufs, Z, style_background());
    mapview_set_labels(&v, setp);
    for (int i = 0; i < GRID_COUNT; i++) CHECK(sets[i].n == 0, "set %d not emptied", i);
    CHECK(mapview_labels_for(&v, bufs[2]) == &sets[2], "buffer 2's set");
    CHECK(mapview_labels_for(&v, (uint16_t *)&one) == NULL, "a stranger's set");

    maplabel_src_t src[GRID_COUNT];
    CHECK(mapview_label_srcs(&v, W, H, src) == 0, "sources before a position");

    /* Near the middle tile's cafe, with the 2 x 2 grid on the fixture
     * before and after a shift east. */
    const merc_pt_t c = { CX + 0.45, CY + 0.35, Z };
    mapview_centre_tiles(&v, c.x, c.y);
    CHECK(mapview_label_srcs(&v, W, H, src) == 0, "sources before a tile is drawn");
    while (mapview_step(&v)) {}
    const int n = mapview_label_srcs(&v, W, H, src);
    CHECK(n == GRID_COUNT, "%d sources", n);

    /* Each source is its slot's set at its slot's place in the window:
     * the window's centre is the position. */
    for (int i = 0; i < n; i++) {
        int slot = -1;
        for (int k = 0; k < GRID_COUNT; k++)
            if (mapview_labels_for(&v, v.grid.slots[k].pixels) == src[i].set) slot = k;
        CHECK(slot >= 0, "source %d is no slot's", i);
        if (slot < 0) continue;
        const tile_id_t id = v.grid.slots[slot].id;
        cafe(id, want, sizeof(want));
        CHECK(src[i].set->n == 1 && strcmp(src[i].set->v[0].text, want) == 0,
              "slot %d (%d/%d) has %s, want %s", slot, (int)id.x, (int)id.y,
              src[i].set->n ? src[i].set->v[0].text : "-", want);
        const int ex = W / 2 + (int)floor((id.x - c.x) * SUBTILE_PX);
        const int ey = H / 2 + (int)floor((id.y - c.y) * SUBTILE_PX);
        CHECK(abs(src[i].x - ex) <= 1 && abs(src[i].y - ey) <= 1,
              "slot %d at %d,%d, want %d,%d", slot, src[i].x, src[i].y, ex, ey);
    }

    /* Laid out over the window: the cafe whose point is on screen. */
    static maplabel_placed_t out[MAPLABEL_ON_SCREEN];
    const int placed = maplabel_layout(src, n, SUBTILE_PX, W, TOP, BOT, GLYPH_H, width,
                                       out, MAPLABEL_ON_SCREEN);
    cafe(mid, want, sizeof(want));
    CHECK(placed == 1 && strcmp(out[0].text, want) == 0, "%d placed, first %s",
          placed, placed ? out[0].text : "-");
    if (placed >= 1) {
        const int ex = W / 2 + (int)floor((CX + 1800.0 / 4096 - c.x) * SUBTILE_PX);
        const int ey = H / 2 + (int)floor((CY + 1300.0 / 4096 - c.y) * SUBTILE_PX);
        CHECK(abs(out[0].ax - ex) <= 1 && abs(out[0].ay - ey) <= 1,
              "cafe at %d,%d, want %d,%d", out[0].ax, out[0].ay, ex, ey);
    }

    /* A shift east hands buffers between slots. The kept tiles keep
     * their names, the new ones are not READY and give none -- and once
     * drawn, every slot names its own tile. */
    mapview_centre_tiles(&v, c.x + 1.0, c.y);
    const int kept = mapview_label_srcs(&v, W, H, src);
    CHECK(kept == GRID_COUNT / 2, "%d sources after a shift", kept);
    for (int i = 0; i < GRID_COUNT; i++) {
        const subtile_t *s = &v.grid.slots[i];
        if (s->state != TILE_READY) continue;
        const maplabel_set_t *ls = mapview_labels_for(&v, s->pixels);
        cafe(s->id, want, sizeof(want));
        CHECK(ls && ls->n == 1 && strcmp(ls->v[0].text, want) == 0,
              "kept slot %d names %s, want %s", i, ls && ls->n ? ls->v[0].text : "-", want);
    }
    while (mapview_step(&v)) {}
    for (int i = 0; i < GRID_COUNT; i++) {
        const subtile_t *s = &v.grid.slots[i];
        const maplabel_set_t *ls = mapview_labels_for(&v, s->pixels);
        if (s->state == TILE_READY) {
            cafe(s->id, want, sizeof(want));
            CHECK(ls && ls->n == 1 && strcmp(ls->v[0].text, want) == 0,
                  "slot %d names %s, want %s", i, ls && ls->n ? ls->v[0].text : "-", want);
        }
    }
    CHECK(mapview_label_srcs(&v, W, H, src) == GRID_COUNT, "sources after the shift drew");

    /* Off the fixture to the east: no data, so not READY, so no names,
     * though the sets still hold what was drawn there before. */
    mapview_centre_tiles(&v, c.x + 3.0, c.y);
    while (mapview_step(&v)) {}
    CHECK(mapview_label_srcs(&v, W, H, src) == 0, "%d sources off the archive",
          mapview_label_srcs(&v, W, H, src));

    for (int i = 0; i < GRID_COUNT; i++) free(bufs[i]);
    maprender_free(&r);
    maparchive_close(&a);
    fclose(f);
}

int main(void)
{
    keeping();
    layout();
    fixture();
    printf("\nmaplabeltest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

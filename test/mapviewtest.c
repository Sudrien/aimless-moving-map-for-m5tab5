/*
 * mapviewtest.c -- main/mapset.c and main/mapview.c on the fixture: the
 * grid placed on a position, filled tile by tile, moved, and composed
 * into a 1280 x 720 window as the screen gets it.
 *
 *   ./mapviewtest [out.ppm]   writes the composed window
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

static int file_read(void *ctx, uint64_t off, uint32_t len, uint8_t *dst)
{
    FILE *f = ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, len, f) == len ? 0 : -1;
}

static void *big(size_t n) { return malloc(n); }
static const maptile_alloc_t MEM = { big, NULL, free };

typedef struct { mapset_t *set; maprender_t *r; } setdraw_t;

static tile_state_t set_draw(void *ctx, tile_id_t id, uint16_t *px, int split)
{
    setdraw_t *d = ctx;
    return mapset_render(d->set, d->r, id, px, split);
}

static bool same(tile_id_t a, tile_id_t b) { return a.z == b.z && a.x == b.x && a.y == b.y; }

static int count(const uint16_t *fb, uint16_t c)
{
    int n = 0;
    for (int i = 0; i < W * H; i++) n += fb[i] == c;
    return n;
}

int main(int argc, char **argv)
{
    FILE *f = fopen("fixture.pmtiles", "rb");
    if (!f) { printf("run from test/: fixture.pmtiles not found\n"); return 1; }
    maparchive_t a;
    CHECK(maparchive_open(&a, file_read, f, &MEM) == PMT_OK, "open");

    mapset_t set = { 0 };
    CHECK(mapset_add(&set, &a), "add");
    CHECK(mapset_covers(&a, Z, CX, CY), "centre tile not covered");
    CHECK(!mapset_covers(&a, Z, CX + 5, CY), "far tile covered");
    CHECK(!mapset_covers(&a, Z - 1, CX / 2, CY / 2), "other zoom covered");

    double lat, lon;
    CHECK(mapset_centre(&set, &lat, &lon), "centre");
    const merc_pt_t c = merc_from_ll(lat, lon, Z);
    CHECK(fabs(c.x - (CX + 0.5)) < 0.01 && fabs(c.y - (CY + 0.5)) < 0.01,
          "header centre at %.3f,%.3f", c.x, c.y);

    maprender_t r;
    CHECK(maprender_init(&r, SUBTILE_PX, &MEM) == 0, "render scratch");
    style_init(SUBTILE_PX, 0);
    const uint16_t bg = style_background();

    uint16_t *bufs[GRID_COUNT];
    for (int i = 0; i < GRID_COUNT; i++)
        bufs[i] = malloc((size_t)SUBTILE_PX * SUBTILE_PX * sizeof(uint16_t));

    mapview_t v;
    setdraw_t sd = { &set, &r };
    mapview_init(&v, set_draw, &sd, bufs, Z, bg);

    static uint16_t fb[W * H];
    mapview_compose(&v, fb, W, H, W);
    CHECK(count(fb, bg) == W * H, "before a position, all background");

    /* A position just south-east of the centre tile's middle. */
    mapview_centre(&v, lat - 0.002, lon + 0.003);
    CHECK(mapview_pending(&v) == GRID_COUNT, "%d tiles queued", mapview_pending(&v));
    mapview_compose(&v, fb, W, H, W);
    CHECK(count(fb, bg) == W * H, "nothing drawn before a step");

    int steps = 0;
    while (mapview_step(&v)) steps++;
    CHECK(steps == GRID_COUNT, "%d steps", steps);
    for (int i = 0; i < GRID_COUNT; i++)
        CHECK(v.grid.slots[i].state == TILE_READY, "slot %d state %d", i, v.grid.slots[i].state);
    mapview_compose(&v, fb, W, H, W);
    /* The rasteriser's edge coverage leaves a few tile-edge pixels at
     * the background colour; maptiletest allows 1 %, this 0.5 %. */
    CHECK(count(fb, bg) < W * H / 200, "background showing: %d px", count(fb, bg));
    CHECK(count(fb, STYLES[S_EARTH].fill) > W * H / 10, "earth %d", count(fb, STYLES[S_EARTH].fill));

    if (argc > 1) {
        FILE *o = fopen(argv[1], "wb");
        fprintf(o, "P6 %d %d 255\n", W, H);
        for (int i = 0; i < W * H; i++) {
            const uint16_t px = fb[i];
            const unsigned char rgb[3] = { (unsigned char)((px >> 11) << 3),
                                           (unsigned char)(((px >> 5) & 63) << 2),
                                           (unsigned char)((px & 31) << 3) };
            fwrite(rgb, 1, 3, o);
        }
        fclose(o);
    }

    /* The composed window is the grid at the position: the pixel under
     * the screen centre is the slot pixel under the position. */
    {
        const merc_pt_t p = merc_from_ll(lat - 0.002, lon + 0.003, Z);
        const int col = (int)floor(p.x) - v.grid.origin.x, row = (int)floor(p.y) - v.grid.origin.y;
        const int sx = (int)floor((p.x - floor(p.x)) * SUBTILE_PX);
        const int sy = (int)floor((p.y - floor(p.y)) * SUBTILE_PX);
        const uint16_t want = v.grid.slots[row * GRID_N + col].pixels[sy * SUBTILE_PX + sx];
        CHECK(fb[(H / 2) * W + W / 2] == want, "centre pixel is not the position's");
    }

    /* The same place given in tiles (0016's pan) is the same view. */
    {
        const merc_pt_t p = merc_from_ll(lat - 0.002, lon + 0.003, Z);
        const tile_id_t was = v.grid.origin;
        mapview_centre_tiles(&v, p.x, p.y);
        CHECK(mapview_pending(&v) == 0 && v.grid.origin.x == was.x && v.grid.origin.y == was.y &&
              v.fx == p.x && v.fy == p.y, "centre in tiles moved the grid");
    }

    /* A small move inside the grid's half-tile: nothing new to render. */
    mapview_centre(&v, lat - 0.0021, lon + 0.0031);
    CHECK(mapview_pending(&v) == 0, "small move queued %d", mapview_pending(&v));

    /* A tile east: more than half a tile from the grid's centre, so it
     * shifts by one column. The column it keeps stays READY with the same
     * buffers; the new column is queued. */
    uint16_t *kept = v.grid.slots[1].pixels;
    const double tile_lon = 360.0 / (1 << Z);
    mapview_centre(&v, lat - 0.002, lon + 0.003 + 1.0 * tile_lon);
    CHECK(mapview_pending(&v) == GRID_N, "east shift queued %d", mapview_pending(&v));
    CHECK(v.grid.slots[0].pixels == kept && v.grid.slots[0].state == TILE_READY,
          "kept column not reused");
    while (mapview_step(&v)) {}
    /* The new column is CX+2, outside the fixture: no data, background. */
    CHECK(v.grid.slots[1].state == TILE_NODATA, "outside tile state %d", v.grid.slots[1].state);
    mapview_compose(&v, fb, W, H, W);
    const int bg_now = count(fb, bg);
    CHECK(bg_now > 0 && bg_now < W * H, "after the shift, background %d px", bg_now);

    /* The buffers are a permutation of the originals: none lost, none doubled. */
    int seen = 0;
    for (int i = 0; i < GRID_COUNT; i++)
        for (int j = 0; j < GRID_COUNT; j++) seen += v.grid.slots[i].pixels == bufs[j];
    CHECK(seen == GRID_COUNT, "buffers not a permutation (%d)", seen);

    /* A jump across the world is a new grid, all queued. */
    mapview_centre(&v, 51.5, -0.12);
    CHECK(mapview_pending(&v) == GRID_COUNT, "jump queued %d", mapview_pending(&v));
    while (mapview_step(&v)) {}
    for (int i = 0; i < GRID_COUNT; i++)
        CHECK(v.grid.slots[i].state == TILE_NODATA, "far slot %d state %d", i, v.grid.slots[i].state);

    /* Redo: errors always, no-data on request; READY tiles are left. */
    CHECK(mapview_redo(&v, false) == 0 && mapview_pending(&v) == 0, "redo with no errors");
    v.grid.slots[2].state = TILE_ERROR;
    CHECK(mapview_redo(&v, false) == 1 && mapview_pending(&v) == 1, "redo the error");
    while (mapview_step(&v)) {}
    CHECK(mapview_redo(&v, true) == GRID_COUNT && mapview_pending(&v) == GRID_COUNT,
          "redo the no-data");
    while (mapview_step(&v)) {}

    /* take/commit: a shift between them refuses the commit and requeues. */
    {
        mapview_centre(&v, lat, lon);
        render_job_t job;
        uint16_t *px;
        CHECK(mapview_take(&v, &job, &px), "take");
        const int left = mapview_pending(&v);
        mapview_centre(&v, lat, lon + 1.0 * tile_lon);
        mapview_commit(&v, &job, TILE_READY);
        int ready = 0;
        for (int i = 0; i < GRID_COUNT; i++) ready += v.grid.slots[i].state == TILE_READY;
        CHECK(ready == 0, "a stale commit landed (%d ready, %d left)", ready, left);
        while (mapview_step(&v)) {}
    }

    /* ---- the overview (0014) ---- */
    {
        uint16_t *cz[2] = { malloc((size_t)COARSE_PX * COARSE_PX * 2),
                            malloc((size_t)COARSE_PX * COARSE_PX * 2) };
        mapview_t o;
        mapview_init(&o, set_draw, &sd, bufs, Z, bg);
        mapview_set_coarse(&o, cz[0], cz[1]);
        tile_id_t id;
        uint16_t *px;
        CHECK(!mapview_coarse_take(&o, &id, &px), "an overview before a position");

        mapview_centre(&o, lat - 0.002, lon + 0.003);
        CHECK(mapview_coarse_take(&o, &id, &px), "take");
        const int32_t midx = o.grid.origin.x + GRID_N / 2, midy = o.grid.origin.y + GRID_N / 2;
        CHECK(id.z == Z - COARSE_STEP && id.x == midx >> COARSE_STEP && id.y == midy >> COARSE_STEP,
              "overview %u/%d/%d for the grid's middle %d,%d", id.z, id.x, id.y, midx, midy);
        tile_id_t id2;
        uint16_t *px2;
        CHECK(!mapview_coarse_take(&o, &id2, &px2), "taken twice while busy");

        /* Composed before the commit: nothing to show yet. */
        mapview_compose(&o, fb, W, H, W);
        CHECK(count(fb, bg) == W * H, "an overview shown before it was drawn");

        /* A pattern that names each overview pixel, drawn and committed. */
        for (int y = 0; y < COARSE_PX; y++)
            for (int x = 0; x < COARSE_PX; x++)
                px[y * COARSE_PX + x] = (uint16_t)((y * 7919 + x * 31) | 1);
        mapview_coarse_commit(&o, id, TILE_READY);
        CHECK(mapview_coarse_ok(&o), "not ok after its commit");
        CHECK(!mapview_coarse_take(&o, &id2, &px2), "taken again when held");

        /* Every window pixel of a slot the overview covers is the overview
         * pixel under it; the others are background. Worked out here from
         * the geometry, not from mapview's tables. */
        mapview_compose(&o, fb, W, H, W);
        const double gx = (o.fx - o.grid.origin.x) * SUBTILE_PX;
        const double gy = (o.fy - o.grid.origin.y) * SUBTILE_PX;
        const int left = (int)floor(gx) - W / 2, top = (int)floor(gy) - H / 2;
        const int span = 1 << COARSE_STEP;
        int bad = 0, covered = 0, uncovered = 0;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const int ax = left + x, ay = top + y;
                const int c = ax / SUBTILE_PX, rr = ay / SUBTILE_PX;
                const int64_t tx = (int64_t)o.grid.origin.x + c, ty = (int64_t)o.grid.origin.y + rr;
                const int64_t cx = tx - (int64_t)id.x * span, cy = ty - (int64_t)id.y * span;
                uint16_t want = bg;
                if (cx >= 0 && cy >= 0 && cx < span && cy < span) {
                    const int64_t sx = cx * (COARSE_PX / span) + (int64_t)(ax % SUBTILE_PX) * (COARSE_PX / span) / SUBTILE_PX;
                    const int64_t sy = cy * (COARSE_PX / span) + (int64_t)(ay % SUBTILE_PX) * (COARSE_PX / span) / SUBTILE_PX;
                    want = px[sy * COARSE_PX + sx];
                    covered++;
                } else {
                    uncovered++;
                }
                bad += fb[y * W + x] != want;
            }
        }
        CHECK(bad == 0 && covered > 0, "%d pixels wrong (%d covered, %d not)", bad, covered, uncovered);

        /* Drawn tiles win over it; where a tile has no data it stays. */
        while (mapview_step(&o)) {}
        mapview_compose(&o, fb, W, H, W);
        int from_tile = 0;
        for (int i = 0; i < GRID_COUNT; i++) from_tile += o.grid.slots[i].state == TILE_READY;
        CHECK(from_tile > 0, "no tile drew");
        {
            const merc_pt_t p = merc_from_ll(lat - 0.002, lon + 0.003, Z);
            const int col = (int)floor(p.x) - o.grid.origin.x, row = (int)floor(p.y) - o.grid.origin.y;
            const int sx = (int)floor((p.x - floor(p.x)) * SUBTILE_PX);
            const int sy = (int)floor((p.y - floor(p.y)) * SUBTILE_PX);
            CHECK(o.grid.slots[row * GRID_N + col].state == TILE_READY &&
                  fb[(H / 2) * W + W / 2] == o.grid.slots[row * GRID_N + col].pixels[sy * SUBTILE_PX + sx],
                  "a drawn tile did not win over the overview");
        }

        /* A failure is not taken again until a redo says so. */
        mapview_centre(&o, 51.5, -0.12);
        CHECK(!mapview_coarse_ok(&o), "the old overview still counts as covering");
        mapview_compose(&o, fb, W, H, W);
        CHECK(count(fb, bg) == W * H, "the old overview drawn somewhere it does not cover");
        CHECK(mapview_coarse_take(&o, &id, &px), "take for the new grid");
        mapview_coarse_commit(&o, id, TILE_NODATA);
        CHECK(!mapview_coarse_take(&o, &id2, &px2), "no-data taken again at once");
        mapview_redo(&o, false);
        CHECK(!mapview_coarse_take(&o, &id2, &px2), "no-data taken again on an error redo");
        mapview_redo(&o, true);
        CHECK(mapview_coarse_take(&o, &id2, &px2) && same(id, id2), "no-data not taken on a no-data redo");
        mapview_coarse_commit(&o, id2, TILE_ERROR);
        CHECK(!mapview_coarse_take(&o, &id2, &px2), "error taken again at once");
        mapview_redo(&o, false);
        CHECK(mapview_coarse_take(&o, &id2, &px2), "error not taken on a redo");
        /* The buffer drawn into is never the one on screen. */
        CHECK(px2 != o.coarse_px[o.cz_front], "drawing into the front buffer");
        mapview_coarse_commit(&o, id2, TILE_ERROR);

        /* Back where the first overview was: still held, so nothing to
         * draw, and it covers again. */
        mapview_centre(&o, lat - 0.002, lon + 0.003);
        CHECK(!mapview_coarse_take(&o, &id2, &px2), "the held overview drawn again");
        CHECK(mapview_coarse_ok(&o), "the held overview not used again");
        while (mapview_step(&o)) {}

        /* A restyle (0015): drawn tiles queued again, no-data left, the
         * overview dropped, the background changed. */
        {
            int drawn = 0, nodata = 0;
            for (int i = 0; i < GRID_COUNT; i++) {
                drawn += o.grid.slots[i].state == TILE_READY;
                nodata += o.grid.slots[i].state == TILE_NODATA;
            }
            CHECK(drawn > 0 && mapview_coarse_ok(&o), "nothing to restyle");
            mapview_restyle(&o, 0x1234);
            CHECK(mapview_pending(&o) == drawn, "%d queued for %d drawn", mapview_pending(&o), drawn);
            int still = 0;
            for (int i = 0; i < GRID_COUNT; i++) still += o.grid.slots[i].state == TILE_NODATA;
            CHECK(still == nodata, "no-data tiles changed");
            CHECK(!mapview_coarse_ok(&o), "the overview kept its old colours");
            mapview_compose(&o, fb, W, H, W);
            CHECK(count(fb, 0x1234) == W * H, "the new background not used");
            /* An overview being drawn across a restyle is thrown away. */
            CHECK(mapview_coarse_take(&o, &id2, &px2), "take after a restyle");
            mapview_restyle(&o, 0x1234);
            mapview_coarse_commit(&o, id2, TILE_READY);
            CHECK(!mapview_coarse_ok(&o), "an overview drawn in the old colours was kept");
            CHECK(mapview_coarse_take(&o, &id2, &px2), "not asked for again");
            mapview_coarse_commit(&o, id2, TILE_READY);
            CHECK(mapview_coarse_ok(&o), "the redraw not kept");
            while (mapview_step(&o)) {}
        }
        free(cz[0]);
        free(cz[1]);
    }

    for (int i = 0; i < GRID_COUNT; i++) free(bufs[i]);
    maprender_free(&r);
    maparchive_close(&a);
    fclose(f);
    printf("mapviewtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

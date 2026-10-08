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
    mapview_init(&v, &set, &r, bufs, Z, bg);

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

    for (int i = 0; i < GRID_COUNT; i++) free(bufs[i]);
    maprender_free(&r);
    maparchive_close(&a);
    fclose(f);
    printf("mapviewtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

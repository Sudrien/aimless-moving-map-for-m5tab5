/*
 * maptiletest.c -- main/maptile.c and components/mapcore on the host,
 * against test/fixture.pmtiles (see make_fixture.py).
 *
 * The whole offline path the firmware takes: open the archive through a
 * read callback, look tiles up, inflate them, decode the MVT layer by
 * layer and rasterise. Then look at the pixels: the colours the style
 * says each layer is must be where the fixture put that layer.
 *
 *   ./maptiletest [out.ppm]   writes the 3 x 3 block, for looking at
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maptile.h"
#include "style.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define Z   14
#define CX  4823
#define CY  6160
#define PX  512

static int file_read(void *ctx, uint64_t off, uint32_t len, uint8_t *dst)
{
    FILE *f = ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, len, f) == len ? 0 : -1;
}

static void *big(size_t n) { return malloc(n); }
static const maptile_alloc_t MEM = { big, NULL, free };

/* How many pixels of a tile are exactly colour c. */
static int count(const uint16_t *px, uint16_t c)
{
    int n = 0;
    for (int i = 0; i < PX * PX; i++) n += px[i] == c;
    return n;
}

int main(int argc, char **argv)
{
    FILE *f = fopen("fixture.pmtiles", "rb");
    if (!f) { printf("run from test/: fixture.pmtiles not found\n"); return 1; }

    maparchive_t a;
    pmt_err_t e = maparchive_open(&a, file_read, f, &MEM);
    CHECK(e == PMT_OK, "open: %s", pmt_strerror(e));
    if (e != PMT_OK) return 1;
    CHECK(a.pmt.hdr.min_zoom == Z && a.pmt.hdr.max_zoom == Z, "zooms %u..%u",
          a.pmt.hdr.min_zoom, a.pmt.hdr.max_zoom);
    CHECK(a.pmt.hdr.tile_compression == PMT_COMPRESS_GZIP, "tile compression");

    maprender_t r;
    CHECK(maprender_init(&r, PX, &MEM) == 0, "render scratch");

    style_init(PX, 0);
    const uint16_t bg    = style_background();
    const uint16_t earth = STYLES[S_EARTH].fill;
    const uint16_t water = STYLES[S_WATER].fill;
    const uint16_t major = STYLES[S_MAJOR].stroke;
    const uint16_t bldg  = STYLES[S_BUILDING].fill;
    const uint16_t park  = STYLES[S_PARK].fill;
    const uint16_t grass = STYLES[S_GRASS].fill;

    static uint16_t mosaic[3 * PX * 3 * PX];
    static uint16_t px[PX * PX];
    int ready = 0;
    for (int ty = 0; ty < 3; ty++) {
        for (int tx = 0; tx < 3; tx++) {
            const tile_id_t id = { Z, CX - 1 + tx, CY - 1 + ty };
            const tile_state_t s = maprender_tile(&r, &a, id, px, 0);
            CHECK(s == TILE_READY, "tile %d/%ld/%ld: state %d", Z, (long)id.x, (long)id.y, s);
            if (s != TILE_READY) continue;
            ready++;
            CHECK(r.last_bytes > 0 && r.last_inflated > r.last_bytes,
                  "tile %ld/%ld: %u bytes inflated to %u", (long)id.x, (long)id.y,
                  r.last_bytes, r.last_inflated);
            const int n_earth = count(px, earth), n_bg = count(px, bg);
            CHECK(n_earth > PX * PX / 10, "tile %ld/%ld: earth %d px", (long)id.x, (long)id.y, n_earth);
            CHECK(n_bg < PX * PX / 100, "tile %ld/%ld: background showing %d px (earth not drawn?)",
                  (long)id.x, (long)id.y, n_bg);
            CHECK(count(px, major) > PX * 4, "tile %ld/%ld: major roads %d px",
                  (long)id.x, (long)id.y, count(px, major));
            CHECK(count(px, bldg) > PX * 8, "tile %ld/%ld: buildings %d px",
                  (long)id.x, (long)id.y, count(px, bldg));
            /* The river is in the middle column only. */
            const int n_water = count(px, water);
            if (tx == 1) CHECK(n_water > PX * 20, "middle column tile %d: water %d px", ty, n_water);
            else         CHECK(n_water == 0, "side tile %d,%d: water %d px", tx, ty, n_water);
            /* Park on even x+y, grass on odd. */
            if (((CX - 1 + tx) + (CY - 1 + ty)) % 2 == 0)
                CHECK(count(px, park) > PX * 8, "tile %d,%d: no park", tx, ty);
            else
                CHECK(count(px, grass) > PX * 8, "tile %d,%d: no grass", tx, ty);
            /* A major road at x = 0 of the tile, so column 0..2 is mostly road. */
            int road_col = 0;
            for (int y = 0; y < PX; y++) road_col += px[y * PX + 1] == major;
            CHECK(road_col > PX / 2, "tile %d,%d: west-edge major road %d/%d px", tx, ty, road_col, PX);

            for (int y = 0; y < PX; y++)
                memcpy(&mosaic[(size_t)(ty * PX + y) * 3 * PX + tx * PX], &px[y * PX],
                       PX * sizeof(uint16_t));
        }
    }
    CHECK(ready == 9, "%d of 9 tiles drawn", ready);

    /* Outside the block, and outside the archive's zooms. */
    memset(px, 0xAB, sizeof(px));
    tile_state_t s = maprender_tile(&r, &a, (tile_id_t){ Z, CX + 5, CY }, px, 0);
    CHECK(s == TILE_NODATA, "tile outside the block: state %d", s);
    CHECK(px[0] == 0xABAB, "a missing tile must leave px alone");
    s = maprender_tile(&r, &a, (tile_id_t){ 10, CX >> 4, CY >> 4 }, px, 0);
    CHECK(s == TILE_NODATA, "zoom outside the archive: state %d", s);

    /* split 1: a z15 subtile is a quarter of its z14 tile, drawn at full
     * size -- so the same road is twice as wide. */
    s = maprender_tile(&r, &a, (tile_id_t){ Z + 1, CX * 2, CY * 2 }, px, 1);
    CHECK(s == TILE_READY, "split subtile: state %d", s);
    int road_w = 0;
    for (int x = 0; x < 40; x++) road_w += px[(PX / 3) * PX + x] == major;
    CHECK(road_w >= 2, "split subtile: west-edge road %d px wide", road_w);

    /* Dark style draws different colours from the same tile. */
    style_init(PX, 1);
    s = maprender_tile(&r, &a, (tile_id_t){ Z, CX, CY }, px, 0);
    CHECK(s == TILE_READY && count(px, STYLES[S_EARTH].fill) > PX * PX / 10 &&
          count(px, earth) == 0, "dark style not applied");

    if (argc > 1) {
        FILE *o = fopen(argv[1], "wb");
        fprintf(o, "P6 %d %d 255\n", 3 * PX, 3 * PX);
        for (size_t i = 0; i < sizeof(mosaic) / sizeof(mosaic[0]); i++) {
            const uint16_t c = mosaic[i];
            const unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3),
                                           (unsigned char)(((c >> 5) & 63) << 2),
                                           (unsigned char)((c & 31) << 3) };
            fwrite(rgb, 1, 3, o);
        }
        fclose(o);
    }

    /* A scratch made for a larger size, resized down, draws exactly what
     * one made for this size does (0014: the overview uses the grid's). */
    {
        maprender_t big2;
        CHECK(maprender_init(&big2, 2 * PX, &MEM) == 0, "big scratch");
        CHECK(maprender_resize(&big2, 2 * PX + 1) == -1, "resized past its scratch");
        CHECK(maprender_resize(&big2, 0) == -1, "resized to nothing");
        CHECK(maprender_resize(&big2, PX) == 0, "resize");
        static uint16_t a1[PX * PX], a2[PX * PX];
        const tile_id_t id = { Z, CX, CY };
        CHECK(maprender_tile(&r, &a, id, a1, 0) == TILE_READY, "draw at PX");
        CHECK(maprender_tile(&big2, &a, id, a2, 0) == TILE_READY, "draw resized");
        CHECK(memcmp(a1, a2, sizeof(a1)) == 0, "a resized scratch drew differently");
        /* And back up again. */
        static uint16_t b2[4 * PX * PX];
        CHECK(maprender_resize(&big2, 2 * PX) == 0, "resize back");
        const tile_state_t sb = maprender_tile(&big2, &a, id, b2, 0);
        int nb = 0;
        for (int i = 0; i < 4 * PX * PX; i++) nb += b2[i] == STYLES[S_EARTH].fill;   /* the dark style, set above */
        CHECK(sb == TILE_READY && nb > 4 * PX * PX / 10,
              "draw at the full size after a resize: state %d, earth %d px", sb, nb);
        maprender_free(&big2);
    }

    maprender_free(&r);
    maparchive_close(&a);
    fclose(f);
    printf("maptiletest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

/*
 * worldtiletest.c -- tools/fetch_worldtile.py and main/worldtile.c.
 *
 *   ./worldtiletest TILE   TILE from fetch_worldtile.py --file
 *                          fixture.pmtiles --zxy 14/4823/6160
 *
 * The fixture has no z0, so its z14 centre tile stands in: what is
 * checked is that the script's walk of the archive got exactly the bytes
 * mapcore's does, and that worldtile.c draws and places them.
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "maptile.h"
#include "style.h"
#include "worldtile.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PX 512

static int file_read(void *ctx, uint64_t off, uint32_t len, uint8_t *dst)
{
    FILE *f = ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, len, f) == len ? 0 : -1;
}
static void *big(size_t n) { return malloc(n); }
static const maptile_alloc_t MEM = { big, NULL, free };

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: worldtiletest TILE\n"); return 1; }
    FILE *t = fopen(argv[1], "rb");
    static uint8_t got[1 << 20];
    const size_t n = t ? fread(got, 1, sizeof got, t) : 0;
    if (t) fclose(t);

    FILE *f = fopen("fixture.pmtiles", "rb");
    maparchive_t a;
    CHECK(f && maparchive_open(&a, file_read, f, &MEM) == PMT_OK, "open fixture");
    maprender_t r;
    CHECK(maprender_init(&r, PX, &MEM) == 0, "scratch");
    style_init(PX, 0);

    printf("the script's walk gets mapcore's bytes\n");
    uint32_t len = 0;
    CHECK(maprender_fetch(&r, &a, (tile_id_t){ 14, 4823, 6160 }, 0, &len) == TILE_READY, "fetch");
    CHECK(n == len && memcmp(got, r.tile, len) == 0, "script %zu bytes, mapcore %u", n, (unsigned)len);

    printf("drawn as the world, and placed to cover\n");
    static uint16_t px[PX * PX], ref[PX * PX];
    CHECK(maprender_payload(&r, len, (tile_id_t){ 0, 0, 0 }, ref, 0) == TILE_READY, "reference");
    CHECK(worldtile_draw(&r, got, n, px) == TILE_READY, "draw");
    CHECK(memcmp(px, ref, sizeof px) == 0, "drawn differently from the renderer");
    CHECK(worldtile_draw(&r, got, 0, px) == TILE_NODATA, "nothing embedded");
    CHECK(worldtile_draw(&r, got, (size_t)r.tile_cap + 1, px) == TILE_ERROR, "too big");
    uint8_t junk[64] = { 0 };
    CHECK(worldtile_draw(&r, junk, sizeof junk, px) == TILE_ERROR, "not gzip");

    /* At the tile's own width: the middle rows, unscaled. */
    {
        enum { W = PX, H = PX * 9 / 16 };
        static uint16_t fb[W * H];
        worldtile_compose(ref, PX, fb, W, H, W);
        const int oy = (PX - H) / 2;
        int bad = 0;
        for (int y = 0; y < H; y++)
            bad += memcmp(&fb[y * W], &ref[(y + oy) * PX], W * 2) != 0;
        CHECK(bad == 0, "%d rows not the middle of the tile", bad);
    }
    /* Twice the tile's width: every pixel from the square that covers. */
    {
        enum { W = 2 * PX, H = PX };
        static uint16_t fb[W * H];
        worldtile_compose(ref, PX, fb, W, H, W);
        int bad = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const int sx = x * PX / W, sy = (y + (W - H) / 2) * PX / W;
                bad += fb[y * W + x] != ref[sy * PX + sx];
            }
        CHECK(bad == 0, "%d pixels wrong scaled", bad);
    }

    maprender_free(&r);
    maparchive_close(&a);
    fclose(f);
    printf("\nworldtiletest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

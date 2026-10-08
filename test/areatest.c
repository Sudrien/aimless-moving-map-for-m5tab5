/*
 * areatest.c -- main/area.c: the area cache's walk, and what of it the
 * card already holds.
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "area.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int file_read(void *ctx, uint64_t off, uint32_t len, uint8_t *dst)
{
    FILE *f = ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, len, f) == len ? 0 : -1;
}
static void *big(size_t n) { return malloc(n); }
static const maptile_alloc_t MEM = { big, NULL, free };

#define Z   14
#define CX  4823
#define CY  6160

int main(void)
{
    const int S = 2 * AREA_RADIUS + 1, C = 2 * AREA_CZ_RADIUS + 1;
    CHECK(area_total() == S * S + C * C && AREA_CZ_RADIUS == 2, "total %d", area_total());

    printf("every tile of the square once, nearest first\n");
    {
        area_t a;
        area_start(&a, Z, CX, CY);
        static int seen14[15][15], seen12[5][5];
        int n = 0, last_ring = 0, back = 0, prog_back = 0, last_prog = 0;
        tile_id_t id;
        while (area_next(&a, &id)) {
            n++;
            const int p = area_progress(&a);
            prog_back += p < last_prog;
            last_prog = p;
            if (id.z == Z) {
                const int dx = id.x - CX, dy = id.y - CY;
                CHECK(dx >= -7 && dx <= 7 && dy >= -7 && dy <= 7, "z14 off the square %d,%d", dx, dy);
                if (dx < -7 || dx > 7 || dy < -7 || dy > 7) continue;
                seen14[dy + 7][dx + 7]++;
                const int ring = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                back += ring < last_ring;
                last_ring = ring;
            } else {
                CHECK(id.z == Z - 2, "zoom %u", id.z);
                const int dx = id.x - (CX >> 2), dy = id.y - (CY >> 2);
                if (dx < -2 || dx > 2 || dy < -2 || dy > 2) { CHECK(0, "z12 off %d,%d", dx, dy); continue; }
                seen12[dy + 2][dx + 2]++;
            }
        }
        int bad = 0;
        for (int y = 0; y < 15; y++) for (int x = 0; x < 15; x++) bad += seen14[y][x] != 1;
        for (int y = 0; y < 5; y++) for (int x = 0; x < 5; x++) bad += seen12[y][x] != 1;
        CHECK(n == area_total() && bad == 0, "%d tiles, %d cells not seen once", n, bad);
        CHECK(back == 0, "a nearer ring came after a farther one %d times", back);
        CHECK(!a.active && area_progress(&a) == 100 && prog_back == 0, "ends at 100");
        CHECK(!area_next(&a, &id), "next after the end");
        /* The first tile is the centre. */
        area_start(&a, Z, CX, CY);
        CHECK(area_next(&a, &id) && id.x == CX && id.y == CY && id.z == Z, "centre first");
    }

    printf("x wraps round the world, y stops at the poles\n");
    {
        area_t a;
        area_start(&a, Z, 0, 3);
        int wrapped = 0, n = 0;
        tile_id_t id;
        const int32_t w = 1 << Z;
        while (area_next(&a, &id)) {
            n++;
            if (id.z == Z && id.x > w - 10) wrapped++;
            CHECK(id.x >= 0 && id.x < (1 << id.z) && id.y >= 0 && id.y < (1 << id.z),
                  "tile off the world %u/%d/%d", id.z, id.x, id.y);
        }
        CHECK(wrapped == 7 * 11, "%d wrapped to the far side", wrapped);
        /* Past the pole: rows -4..-1 at z14, 4 rows of 15; and at z12,
         * centred on row 0, rows -2 and -1, 2 rows of 5. */
        CHECK(n == area_total() - 4 * 15 - 2 * 5, "%d tiles near the pole", n);
    }

    printf("what the card holds is not pending\n");
    {
        FILE *f = fopen("fixture.pmtiles", "rb");
        maparchive_t arc;
        CHECK(f && maparchive_open(&arc, file_read, f, &MEM) == PMT_OK, "fixture");
        mapset_t none = { 0 }, set = { 0 };
        mapset_add(&set, &arc);
        CHECK(area_pending(&none, Z, CX, CY) == area_total(), "nothing held");
        /* The fixture is the 3 x 3 around CX,CY at z14 only. Coverage is
         * by its header's bounding box, as the original's, which touches
         * the neighbours along two of its edges: so the 9, and no more
         * than the 5 x 5 round them. */
        const int p = area_pending(&set, Z, CX, CY);
        CHECK(p <= area_total() - 9 && p >= area_total() - 25, "pending %d", p);
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                CHECK(mapset_covers_any(&set, Z, CX + dx, CY + dy), "%d,%d not covered", dx, dy);
        maparchive_close(&arc);
        fclose(f);
    }

    printf("\nareatest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

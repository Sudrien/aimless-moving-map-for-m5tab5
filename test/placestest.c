/*
 * placestest.c -- main/places.c, the status line's "Locality, Region":
 * blocks, which points an index keeps, the nearest of each rank with
 * the original's radii and holds, and the text. Then the two pieces
 * that feed it on the device, against the fixture: tilesrc_fetch() from
 * the network, the cache and the card, and maprender_points() decoding
 * one layer's names. The fixture has no places layer, so its POIs stand
 * in: the decode does not care which layer it is asked for.
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
#include <unistd.h>

#include "places.h"
#include "style.h"
#include "tilesrc.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define VZ  14
#define CX  4823
#define CY  6160

static bool has(const tile_id_t *t, int n, int32_t x, int32_t y)
{
    for (int i = 0; i < n; i++) if (t[i].x == x && t[i].y == y) return true;
    return false;
}

static void blocks(void)
{
    printf("blocks\n");
    /* Fractional z14 position (CX + 0.5, CY + 0.5): z12 is >> 2, z6 >> 8. */
    tile_id_t c = places_centre(VZ, CX + 0.5, CY + 0.5, PLACES_FINE_Z);
    CHECK(c.z == 12 && c.x == CX >> 2 && c.y == CY >> 2, "z12 centre %d/%d", (int)c.x, (int)c.y);
    c = places_centre(VZ, CX + 0.5, CY + 0.5, PLACES_COARSE_Z);
    CHECK(c.z == 6 && c.x == CX >> 8 && c.y == CY >> 8, "z6 centre %d/%d", (int)c.x, (int)c.y);
    /* Off the east edge of the world wraps; past the pole holds. */
    c = places_centre(VZ, 16384.0 + 3.0, -5.0, PLACES_FINE_Z);
    CHECK(c.x == 0 && c.y == 0, "wrapped %d/%d", (int)c.x, (int)c.y);

    tile_id_t b[9];
    const tile_id_t mid = { 12, 100, 200 };
    int n = places_block(mid, b);
    CHECK(n == 9 && b[0].x == 100 && b[0].y == 200, "%d tiles, first %d/%d", n, (int)b[0].x, (int)b[0].y);
    bool all = true;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) all &= has(b, n, 100 + dx, 200 + dy);
    CHECK(all, "not the 3 x 3");

    const tile_id_t top_left = { 6, 0, 0 };
    n = places_block(top_left, b);
    CHECK(n == 6 && has(b, n, 63, 0) && has(b, n, 63, 1) && !has(b, n, 0, -1),
          "corner: %d tiles", n);
    const tile_id_t z1 = { 1, 0, 0 };
    n = places_block(z1, b);
    CHECK(n == 4, "z1 has 4 tiles, not %d", n);
    const tile_id_t z0 = { 0, 0, 0 };
    n = places_block(z0, b);
    CHECK(n == 1, "z0 has 1 tile, not %d", n);
}

static mvt_part_t point(uint8_t style, const char *name, int32_t *pts)
{
    mvt_part_t p;
    memset(&p, 0, sizeof(p));
    p.geom = MVT_POINT;
    p.style = style;
    p.name = name;
    p.name_len = name ? (uint32_t)strlen(name) : 0;
    p.pts = pts;
    p.n_pts = 1;
    return p;
}

static void add(places_index_t *idx, tile_id_t id, uint8_t style, const char *name, int x, int y)
{
    int32_t pts[2] = { x, y };
    places_sink_t s = { idx, id };
    const mvt_part_t p = point(style, name, pts);
    places_part(&s, &p);
}

static places_index_t fine, coarse;

static void indexes(void)
{
    printf("what an index keeps\n");
    const tile_id_t f = { PLACES_FINE_Z, 1000, 2000 };
    places_begin(&fine, PLACES_FINE_Z);
    add(&fine, f, S_PLACE_LOCALITY, "Canton", 2048, 1024);
    add(&fine, f, S_PLACE_HOOD, "Cherry Hill", 0, 4095);
    add(&fine, f, S_PLACE_REGION, "Michigan", 10, 10);       /* not this index's */
    add(&fine, f, S_PLACE_COUNTRY, "United States", 10, 10);
    add(&fine, f, S_POI, "Cafe", 10, 10);
    add(&fine, f, S_PLACE_LOCALITY, NULL, 10, 10);
    add(&fine, f, S_PLACE_LOCALITY, "", 10, 10);
    {
        int32_t pts[4] = { 1, 1, 2, 2 };
        places_sink_t s = { &fine, f };
        mvt_part_t line = point(S_PLACE_LOCALITY, "A line", pts);
        line.geom = MVT_LINESTRING;
        line.n_pts = 2;
        places_part(&s, &line);
    }
    CHECK(fine.n == 2, "%d kept", fine.n);
    CHECK(fine.n >= 1 && strcmp(fine.v[0].text, "Canton") == 0 &&
          fabsf(fine.v[0].fx - 1000.5f) < 1e-3f && fabsf(fine.v[0].fy - 2000.25f) < 1e-3f,
          "Canton at %f,%f", fine.v[0].fx, fine.v[0].fy);

    const tile_id_t c = { PLACES_COARSE_Z, 15, 31 };   /* where z12 1000/2000 is */
    places_begin(&coarse, PLACES_COARSE_Z);
    add(&coarse, c, S_PLACE_LOCALITY, "Detroit", 100, 100);    /* not this index's */
    add(&coarse, c, S_PLACE_REGION, "Michigan", 2000, 1000);
    add(&coarse, c, S_PLACE_REGION, "Ohio", 2000, 3900);
    add(&coarse, c, S_PLACE_COUNTRY, "United States", 0, 3000);
    CHECK(coarse.n == 3 && !coarse.full, "%d kept", coarse.n);

    /* Full stops the decode, and says so. */
    static places_index_t big;
    places_begin(&big, PLACES_FINE_Z);
    int stop = 0;
    for (int i = 0; i < PLACES_MAX + 3 && !stop; i++) {
        int32_t pts[2] = { i, i };
        places_sink_t s = { &big, f };
        const mvt_part_t p = point(S_PLACE_HOOD, "h", pts);
        stop = places_part(&s, &p);
    }
    CHECK(stop && big.n == PLACES_MAX && big.full, "full: stop %d, %d kept", stop, big.n);
}

static void picking(void)
{
    printf("the nearest of each rank, held\n");
    places_t p;
    memset(&p, 0, sizeof(p));
    char t[160];
    CHECK(!places_text(&p, t, sizeof(t)) && t[0] == '\0', "nothing yet, and text");

    /* At Canton's point, in z14 tiles: z12 x 1000.5 is z14 4002. */
    const double wx = 1000.5 * 4, wy = 2000.25 * 4;
    places_pick(&p, &fine, &coarse, VZ, wx, wy);
    CHECK(strcmp(p.name[PLACES_LOCALITY], "Canton") == 0, "locality %s", p.name[PLACES_LOCALITY]);
    CHECK(p.name[PLACES_HOOD][0] == '\0', "Cherry Hill is 3 z14 tiles off and named: %s",
          p.name[PLACES_HOOD]);
    /* Michigan is nearer than Ohio from z12 2000.25 = z6 31.25 in y. */
    CHECK(strcmp(p.name[PLACES_REGION], "Michigan") == 0, "region %s", p.name[PLACES_REGION]);
    CHECK(strcmp(p.name[PLACES_COUNTRY], "United States") == 0, "country %s", p.name[PLACES_COUNTRY]);
    CHECK(places_text(&p, t, sizeof(t)) && strcmp(t, "Canton, Michigan, United States") == 0, "text %s", t);

    /* Near Cherry Hill (z12 1000, 2001): within 0.4 z14 tiles. */
    places_pick(&p, &fine, &coarse, VZ, 1000.0 * 4 + 0.2, 2001.0 * 4 - 0.2);
    CHECK(strcmp(p.name[PLACES_HOOD], "Cherry Hill") == 0, "hood %s", p.name[PLACES_HOOD]);
    CHECK(strcmp(p.name[PLACES_LOCALITY], "Canton") == 0, "Canton is 3.5 z14 tiles off: %s",
          p.name[PLACES_LOCALITY]);
    CHECK(places_text(&p, t, sizeof(t)) && strcmp(t, "Cherry Hill, Canton, Michigan, United States") == 0,
          "text %s", t);

    /* Out in the country, 20 z14 tiles from either: the hood is cleared,
     * the locality held. */
    places_pick(&p, &fine, &coarse, VZ, wx + 20, wy);
    CHECK(p.name[PLACES_HOOD][0] == '\0', "stale hood %s", p.name[PLACES_HOOD]);
    CHECK(strcmp(p.name[PLACES_LOCALITY], "Canton") == 0, "locality not held: %s", p.name[PLACES_LOCALITY]);

    /* Either index may be missing. */
    places_t q;
    memset(&q, 0, sizeof(q));
    places_pick(&q, NULL, &coarse, VZ, wx, wy);
    CHECK(q.name[PLACES_LOCALITY][0] == '\0' && strcmp(q.name[PLACES_REGION], "Michigan") == 0,
          "coarse only: %s / %s", q.name[PLACES_LOCALITY], q.name[PLACES_REGION]);
    places_pick(&q, NULL, NULL, VZ, wx, wy);
    CHECK(strcmp(q.name[PLACES_REGION], "Michigan") == 0, "held with no indexes");

    /* A finer index's region point wins over the coarse one's when it is
     * nearer: both are searched for every rank. */
    static places_index_t f2;
    const tile_id_t f = { PLACES_FINE_Z, 1000, 2000 };
    places_begin(&f2, PLACES_FINE_Z);
    add(&f2, f, S_PLACE_LOCALITY, "Canton", 2048, 1024);
    f2.v[f2.n] = f2.v[0];                    /* a region point, by hand: */
    f2.v[f2.n].style = S_PLACE_REGION;       /* places_part() keeps none */
    snprintf(f2.v[f2.n].text, sizeof(f2.v[f2.n].text), "Wayne County");
    f2.n++;
    places_pick(&q, &f2, &coarse, VZ, wx, wy);
    CHECK(strcmp(q.name[PLACES_REGION], "Wayne County") == 0, "region %s", q.name[PLACES_REGION]);

    /* Repeats are left out. */
    places_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.name[PLACES_LOCALITY], MAPLABEL_TEXT_MAX, "Singapore");
    snprintf(s.name[PLACES_COUNTRY], MAPLABEL_TEXT_MAX, "Singapore");
    CHECK(places_text(&s, t, sizeof(t)) && strcmp(t, "Singapore") == 0, "text %s", t);
    /* And a short buffer is cut, terminated. */
    CHECK(places_text(&p, t, 8) && strlen(t) == 7, "cut to %zu", strlen(t));
}

/* ---- the fixture ---- */

typedef struct { FILE *f; int reads; int fail; } net_t;

static int net_read(void *ctx, uint64_t off, uint32_t len, uint8_t *dst)
{
    net_t *n = ctx;
    n->reads++;
    if (n->fail) return -1;
    if (fseeko(n->f, (off_t)off, SEEK_SET) != 0) return -1;
    return fread(dst, 1, len, n->f) == len ? 0 : -1;
}

static void *big(size_t n) { return malloc(n); }
static const maptile_alloc_t MEM = { big, NULL, free };

typedef struct { int n; char first[MAPLABEL_TEXT_MAX]; } names_t;

static int collect(void *ctx, const mvt_part_t *part)
{
    names_t *c = ctx;
    if (part->name && c->n++ == 0) maplabel_copy(c->first, part->name, part->name_len);
    return 0;
}

static void fixture(void)
{
    printf("fetched and decoded, from the network, the cache and the card\n");
    char dir[] = "/tmp/placestest.XXXXXX";
    if (!mkdtemp(dir)) { CHECK(0, "mkdtemp"); return; }

    net_t net = { fopen("fixture.pmtiles", "rb"), 0, 0 };
    if (!net.f) { CHECK(0, "run from test/: fixture.pmtiles not found"); return; }
    maparchive_t remote;
    CHECK(maparchive_open(&remote, net_read, &net, &MEM) == PMT_OK, "remote open");
    maprender_t r;
    CHECK(maprender_init(&r, 256, &MEM) == 0, "scratch");
    tilecache_t c;
    memset(&c, 0, sizeof(c));
    CHECK(tilecache_open(&c, dir, "20261009", 64, big, free), "cache open");
    mapset_t none = { 0 };
    tilesrc_t s = { .local = &none, .cache = &c, .remote = &remote };

    const tile_id_t id = { 14, CX, CY };
    char want[32];
    snprintf(want, sizeof(want), "Cafe %d", (CX * 3 + CY) % 97);

    /* The network, then the cache with no network asked. */
    uint32_t len = 0;
    tilesrc_from_t from;
    CHECK(tilesrc_fetch(&s, &r, id, &len, &from) == TILE_READY && from == TILESRC_NET && len > 0,
          "network: from %d, %u bytes", from, len);
    names_t got = { 0 };
    CHECK(maprender_points(&r, len, "pois", collect, &got) == TILE_READY, "decoded");
    CHECK(got.n == 1 && strcmp(got.first, want) == 0, "%d names, %s", got.n, got.first);
    names_t other = { 0 };
    CHECK(maprender_points(&r, len, "places", collect, &other) == TILE_READY && other.n == 0,
          "%d from a layer the tile has not got", other.n);

    const int reads = net.reads;
    CHECK(tilesrc_fetch(&s, &r, id, &len, &from) == TILE_READY && from == TILESRC_CACHE,
          "cache: from %d", from);
    CHECK(net.reads == reads, "the network asked again");
    memset(&got, 0, sizeof(got));
    CHECK(maprender_points(&r, len, "pois", collect, &got) == TILE_READY && strcmp(got.first, want) == 0,
          "cached payload: %s", got.first);

    /* A tile the network has not got: a marker, and not asked twice. */
    const tile_id_t far = { 14, CX + 9, CY };
    CHECK(tilesrc_fetch(&s, &r, far, &len, &from) == TILE_NODATA && len == 0, "far tile");
    const int reads2 = net.reads;
    CHECK(tilesrc_fetch(&s, &r, far, &len, &from) == TILE_NODATA, "far tile again");
    CHECK(net.reads == reads2, "the network asked twice for a tile it has not got");

    /* A network that fails: an error, nothing cached. */
    const tile_id_t next = { 14, CX + 1, CY };
    net.fail = 1;
    CHECK(tilesrc_fetch(&s, &r, next, &len, &from) == TILE_ERROR, "failing network");
    net.fail = 0;
    CHECK(tilesrc_fetch(&s, &r, next, &len, &from) == TILE_READY && from == TILESRC_NET,
          "asked again after a failure: from %d", from);

    /* The card, with no cache and no network. */
    maparchive_t card;
    FILE *cf = fopen("fixture.pmtiles", "rb");
    net_t cn = { cf, 0, 0 };
    CHECK(maparchive_open(&card, net_read, &cn, &MEM) == PMT_OK, "card open");
    mapset_t set = { 0 };
    CHECK(mapset_add(&set, &card), "add");
    tilesrc_t local = { .local = &set };
    CHECK(tilesrc_fetch(&local, &r, id, &len, &from) == TILE_READY && from == TILESRC_LOCAL,
          "card: from %d", from);
    CHECK(tilesrc_fetch(&local, &r, far, &len, &from) == TILE_NODATA, "card, far tile");

    /* Not gzip, and nothing at all. */
    CHECK(maprender_points(&r, 0, "pois", collect, &got) == TILE_NODATA, "no bytes");
    memcpy(r.tile, "not gzip at all, not even close", 31);
    CHECK(maprender_points(&r, 31, "pois", collect, &got) == TILE_ERROR, "not gzip");

    maparchive_close(&card);
    fclose(cf);
    tilecache_close(&c);
    tilecache_remove(dir, "20261009");
    rmdir(dir);
    maprender_free(&r);
    maparchive_close(&remote);
    fclose(net.f);
}

int main(void)
{
    blocks();
    indexes();
    picking();
    fixture();
    printf("\nplacestest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

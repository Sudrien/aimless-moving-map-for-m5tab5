/*
 * tilesrctest.c -- main/tilecache.c and main/tilesrc.c on the fixture.
 *
 * The fixture stands in for the remote archive, through a read callback
 * that counts its calls and can be told to fail, as a network would. The
 * cache lives in a temporary directory.
 *
 * SPDX-License-Identifier: MIT
 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "style.h"
#include "tilesrc.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define Z   14
#define CX  4823
#define CY  6160
#define PX  256

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

static tile_id_t tid(uint32_t x, uint32_t y) { tile_id_t t = { Z, x, y }; return t; }

int main(void)
{
    char dir[] = "/tmp/tilesrctest.XXXXXX";
    if (!mkdtemp(dir)) { printf("mkdtemp failed\n"); return 1; }

    net_t net = { fopen("fixture.pmtiles", "rb"), 0, 0 };
    if (!net.f) { printf("run from test/: fixture.pmtiles not found\n"); return 1; }
    maparchive_t remote;
    CHECK(maparchive_open(&remote, net_read, &net, &MEM) == PMT_OK, "remote open");

    maprender_t r;
    CHECK(maprender_init(&r, PX, &MEM) == 0, "render scratch");
    style_init(PX, 0);

    uint16_t *px = malloc((size_t)PX * PX * 2), *ref = malloc((size_t)PX * PX * 2);
    CHECK(maprender_tile(&r, &remote, tid(CX, CY), ref, 0) == TILE_READY, "reference");

    /* ---- the cache on its own ---- */
    tilecache_t c;
    memset(&c, 0, sizeof(c));
    CHECK(tilecache_open(&c, dir, "20261008", 64, big, free), "cache open");
    uint8_t buf[64];
    uint32_t n = sizeof(buf);
    CHECK(!tilecache_get(&c, 3, 1, 2, buf, &n), "empty cache hit");
    CHECK(tilecache_put(&c, 3, 1, 2, (const uint8_t *)"hello", 5), "put");
    CHECK(tilecache_put(&c, 3, 1, 3, NULL, 0), "put marker");
    n = sizeof(buf);
    CHECK(tilecache_get(&c, 3, 1, 2, buf, &n) && n == 5 && !memcmp(buf, "hello", 5), "get");
    CHECK(tilecache_is_empty(&c, 3, 1, 3) && !tilecache_is_empty(&c, 3, 1, 2), "marker");
    n = 2;
    CHECK(!tilecache_get(&c, 3, 1, 2, buf, &n), "payload over the buffer is a miss");
    tilecache_close(&c);

    CHECK(tilecache_open(&c, dir, "20261008", 64, big, free), "reopen");
    CHECK(c.n == 2 && c.st.rescans == 0, "index reloaded: %u entries, %u rescans",
          c.n, c.st.rescans);
    tilecache_close(&c);

    char p[160];
    snprintf(p, sizeof(p), "%s/20261008.idx", dir);
    remove(p);
    CHECK(tilecache_open(&c, dir, "20261008", 64, big, free), "reopen, no index");
    n = sizeof(buf);
    CHECK(c.st.rescans == 1 && c.n == 2 &&
          tilecache_get(&c, 3, 1, 2, buf, &n) && n == 5, "rescanned");
    tilecache_close(&c);
    tilecache_remove(dir, "20261008");

    /* Same files as the original's: 16-byte header, then the payload. */
    CHECK(tilecache_open(&c, dir, "b", 64, big, free), "fresh");
    tilecache_put(&c, 0, 0, 0, (const uint8_t *)"xy", 2);
    CHECK(c.blob_len == 18, "record is 16 + 2, got %u", c.blob_len);
    tilecache_close(&c);
    tilecache_remove(dir, "b");

    /* ---- the chain ---- */
    CHECK(tilecache_open(&c, dir, "net", 64, big, free), "chain cache");
    mapset_t none = { 0 };
    tilesrc_t s = { &none, &c, &remote, { 0 } };
    tilesrc_from_t from;

    net.reads = 0;
    CHECK(tilesrc_draw(&s, &r, tid(CX, CY), px, 0, &from) == TILE_READY &&
          from == TILESRC_NET, "from the network: %d", from);
    CHECK(net.reads > 0, "the network was read");
    CHECK(!memcmp(px, ref, (size_t)PX * PX * 2), "network pixels match");

    net.reads = 0;
    memset(px, 0, (size_t)PX * PX * 2);
    CHECK(tilesrc_draw(&s, &r, tid(CX, CY), px, 0, &from) == TILE_READY &&
          from == TILESRC_CACHE, "from the cache: %d", from);
    CHECK(net.reads == 0, "cache hit read the network %d times", net.reads);
    CHECK(!memcmp(px, ref, (size_t)PX * PX * 2), "cached pixels match");

    /* Outside the fixture: asked once, then a marker. */
    CHECK(tilesrc_draw(&s, &r, tid(CX + 5, CY), px, 0, &from) == TILE_NODATA,
          "outside is nodata");
    CHECK(tilecache_is_empty(&c, Z, CX + 5, CY), "marker stored");
    net.reads = 0;
    CHECK(tilesrc_draw(&s, &r, tid(CX + 5, CY), px, 0, &from) == TILE_NODATA &&
          net.reads == 0, "marker kept the network out: %d reads", net.reads);

    /* The network failing: an error, nothing cached, asked again later. */
    net.fail = 1;
    const tile_state_t t = tilesrc_draw(&s, &r, tid(CX + 1, CY), px, 0, &from);
    CHECK(t == TILE_ERROR || t == TILE_NODATA, "failing network: %d", t);
    uint32_t big_n = r.tile_cap;
    CHECK(!tilecache_get(&c, Z, CX + 1, CY, r.tile, &big_n), "failure not cached");
    net.fail = 0;
    CHECK(tilesrc_draw(&s, &r, tid(CX + 1, CY), px, 0, &from) == TILE_READY &&
          from == TILESRC_NET, "recovered");

    /* Offline: the cache still serves; the rest is nodata. */
    s.remote = NULL;
    CHECK(tilesrc_draw(&s, &r, tid(CX, CY), px, 0, &from) == TILE_READY &&
          from == TILESRC_CACHE, "offline cache");
    CHECK(tilesrc_draw(&s, &r, tid(CX - 1, CY), px, 0, &from) == TILE_NODATA,
          "offline uncached");

    /* The card before the network. */
    FILE *lf = fopen("fixture.pmtiles", "rb");
    net_t lnet = { lf, 0, 0 };
    maparchive_t local;
    CHECK(maparchive_open(&local, net_read, &lnet, &MEM) == PMT_OK, "local open");
    mapset_t set = { 0 };
    mapset_add(&set, &local);
    s.local = &set;
    s.remote = &remote;
    net.reads = 0;
    CHECK(tilesrc_draw(&s, &r, tid(CX - 1, CY - 1), px, 0, &from) == TILE_READY &&
          from == TILESRC_LOCAL && net.reads == 0, "local first: from %d, %d net reads",
          from, net.reads);
    CHECK(s.st.cache_hits == 2 && s.st.net_hits == 2 && s.st.local_hits == 1,
          "stats %u/%u/%u", s.st.cache_hits, s.st.net_hits, s.st.local_hits);

    /* ---- tilesrc_store(): the area cache's step (0026) ---- */
    {
        tilecache_t c2;
        memset(&c2, 0, sizeof(c2));
        CHECK(tilecache_open(&c2, dir, "store", 64, big, free), "store cache");
        mapset_t empty = { 0 };
        tilesrc_t st = { &empty, &c2, &remote, { 0 } };
        net.reads = 0;
        CHECK(tilesrc_store(&st, &r, tid(CX + 1, CY + 1), 0, &from) == TILE_READY &&
              from == TILESRC_NET && net.reads > 0, "stored from the network: %d", from);
        uint32_t m = r.tile_cap;
        CHECK(tilecache_get(&c2, Z, CX + 1, CY + 1, r.tile, &m) && m > 0, "in the cache");
        /* And it draws from there, as the network's would. */
        uint16_t *pa = malloc((size_t)PX * PX * 2), *pb = malloc((size_t)PX * PX * 2);
        CHECK(maprender_tile(&r, &remote, tid(CX + 1, CY + 1), pa, 0) == TILE_READY, "ref");
        CHECK(tilesrc_draw(&st, &r, tid(CX + 1, CY + 1), pb, 0, &from) == TILE_READY &&
              from == TILESRC_CACHE && !memcmp(pa, pb, (size_t)PX * PX * 2), "drawn from it");
        free(pa);
        free(pb);
        net.reads = 0;
        CHECK(tilesrc_store(&st, &r, tid(CX + 1, CY + 1), 0, &from) == TILE_READY &&
              from == TILESRC_CACHE && net.reads == 0, "held: no network");
        CHECK(tilesrc_store(&st, &r, tid(CX + 9, CY), 0, &from) == TILE_NODATA &&
              tilecache_is_empty(&c2, Z, CX + 9, CY), "no data: a marker");
        net.reads = 0;
        CHECK(tilesrc_store(&st, &r, tid(CX + 9, CY), 0, &from) == TILE_NODATA &&
              from == TILESRC_CACHE && net.reads == 0, "marker held: no network");
        net.fail = 1;
        CHECK(tilesrc_store(&st, &r, tid(CX - 1, CY + 1), 0, &from) == TILE_ERROR, "failure");
        net.fail = 0;
        m = r.tile_cap;
        CHECK(!tilecache_get(&c2, Z, CX - 1, CY + 1, r.tile, &m), "failure not cached");
        /* Covered by the card: not read, not copied. */
        st.local = &set;
        net.reads = 0;
        lnet.reads = 0;
        CHECK(tilesrc_store(&st, &r, tid(CX, CY - 1), 0, &from) == TILE_READY &&
              from == TILESRC_LOCAL && net.reads == 0 && lnet.reads == 0, "local: nothing read");
        m = r.tile_cap;
        CHECK(!tilecache_get(&c2, Z, CX, CY - 1, r.tile, &m), "local tile copied to the cache");
        st.local = &empty;
        st.remote = NULL;
        CHECK(tilesrc_store(&st, &r, tid(CX - 1, CY), 0, &from) == TILE_ERROR, "offline");
        tilecache_close(&c2);
        st.cache = &c2;
        CHECK(tilesrc_store(&st, &r, tid(CX - 1, CY), 0, &from) == TILE_ERROR, "no cache");
        /* Counted as tilesrc_draw() counts (0027): the status line's
         * "tiles N cache, N card, N network" saw none of the area cache's.
         * One payload from the network and one from the cache (and the
         * draw above, another from the cache); a no-data
         * answer and the marker it left, both misses; one covered by the
         * card; the failed fetch. Offline and no cache are not tiles
         * tried, as tilesrc_draw() does not count them either. */
        CHECK(st.st.net_hits == 1 && st.st.cache_hits == 2 && st.st.local_hits == 1 &&
              st.st.misses == 2 && st.st.errors == 1,
              "store stats net %u cache %u local %u misses %u errors %u",
              st.st.net_hits, st.st.cache_hits, st.st.local_hits, st.st.misses, st.st.errors);
        tilecache_remove(dir, "store");
    }

    tilecache_close(&c);
    tilecache_remove(dir, "net");
    rmdir(dir);
    maparchive_close(&local);
    maparchive_close(&remote);
    maprender_free(&r);
    fclose(lf);
    fclose(net.f);
    free(px);
    free(ref);
    printf("tilesrctest: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

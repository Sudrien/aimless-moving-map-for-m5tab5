/*
 * tilecache.c -- see tilecache.h. original/tilecache.cpp in C.
 *
 * The files are the original's, byte for byte, so a card that cached
 * tiles under the original keeps them: records of
 *   u32 magic 'PMTC' (0x43544D50), u64 tile id, u32 length, payload
 * and an index of
 *   u32 magic, u32 count, u64 blob length, count x {u64 id, u32 off, u32 len}
 * all little-endian.
 *
 * SPDX-License-Identifier: MIT
 */
#include "tilecache.h"

#include <stdlib.h>
#include <string.h>

#include "pmtiles.h"            /* pmt_zxy_to_tileid() */

/* src: original/tilecache.cpp REC_MAGIC. */
#define REC_MAGIC   0x43544D50u
#define REC_HDR     16u

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint64_t get64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

/* src: original/tilecache.cpp flush_interval(): every write while the
 * cache is tiny, since then a few tiles are the whole map; wider later. */
static uint32_t flush_interval(const tilecache_t *c)
{
    if (c->n < 64)  return 1;
    if (c->n < 512) return 16;
    return 256;
}

static int find_slot(const tilecache_t *c, uint64_t id, bool *found)
{
    int lo = 0, hi = (int)c->n - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        if (c->idx[mid].tile_id == id) { *found = true; return mid; }
        if (c->idx[mid].tile_id < id) lo = mid + 1;
        else hi = mid - 1;
    }
    *found = false;
    return lo;
}

static bool idx_insert(tilecache_t *c, uint64_t id, uint32_t off, uint32_t len)
{
    bool found;
    const int at = find_slot(c, id, &found);
    if (found) { c->idx[at].offset = off; c->idx[at].len = len; return true; }
    if (c->n >= c->cap) return false;
    memmove(&c->idx[at + 1], &c->idx[at], (c->n - (uint32_t)at) * sizeof(c->idx[0]));
    c->idx[at].tile_id = id;
    c->idx[at].offset = off;
    c->idx[at].len = len;
    c->n++;
    return true;
}

static void idx_save(tilecache_t *c)
{
    FILE *f = fopen(c->idx_path, "wb");
    if (!f) return;
    uint8_t h[16];
    put32(h, REC_MAGIC);
    put32(h + 4, c->n);
    put64(h + 8, c->blob_len);
    bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h);
    for (uint32_t i = 0; ok && i < c->n; i++) {
        uint8_t e[16];
        put64(e, c->idx[i].tile_id);
        put32(e + 8, c->idx[i].offset);
        put32(e + 12, c->idx[i].len);
        ok = fwrite(e, 1, sizeof(e), f) == sizeof(e);
    }
    fclose(f);
    if (!ok) { remove(c->idx_path); return; }   /* a rescan beats a bad index */
    c->st.index_flushes++;
    c->since_flush = 0;
}

static bool idx_load(tilecache_t *c)
{
    FILE *f = fopen(c->idx_path, "rb");
    if (!f) return false;
    uint8_t h[16];
    bool ok = fread(h, 1, sizeof(h), f) == sizeof(h) && get32(h) == REC_MAGIC;
    const uint32_t n = ok ? get32(h + 4) : 0;
    const uint64_t blob_len = ok ? get64(h + 8) : 0;
    if (n > c->cap) ok = false;
    for (uint32_t i = 0; ok && i < n; i++) {
        uint8_t e[16];
        ok = fread(e, 1, sizeof(e), f) == sizeof(e);
        if (ok) {
            c->idx[i].tile_id = get64(e);
            c->idx[i].offset = get32(e + 8);
            c->idx[i].len = get32(e + 12);
        }
    }
    fclose(f);
    if (!ok) { c->n = 0; return false; }
    c->n = n;
    /* The blob grew after the last flush: rescan to recover the tiles
     * appended since, rather than orphan them. */
    return blob_len == c->blob_len;
}

/* src: original/tilecache.cpp rescan(). Walk the records from the start;
 * stop at the first that does not hold together. */
static void rescan(tilecache_t *c)
{
    c->n = 0;
    uint32_t pos = 0;
    while (pos + REC_HDR <= c->blob_len && c->n < c->cap) {
        uint8_t h[REC_HDR];
        if (fseek(c->blob, (long)pos, SEEK_SET) != 0 ||
            fread(h, 1, sizeof(h), c->blob) != sizeof(h)) break;
        if (get32(h) != REC_MAGIC) break;
        const uint32_t len = get32(h + 12);
        const uint32_t payload = pos + REC_HDR;
        if (len > c->blob_len - payload) break;
        idx_insert(c, get64(h + 4), payload, len);
        pos = payload + len;
    }
    c->st.rescans++;
    /* Nothing found in a blob with content means the read path is broken,
     * not the data: saving an empty index would make that permanent. */
    if (c->n == 0 && c->blob_len > REC_HDR) return;
    idx_save(c);
}

bool tilecache_is_open(const tilecache_t *c) { return c->blob && c->idx; }

bool tilecache_open(tilecache_t *c, const char *dir, const char *build,
                    uint32_t max_entries,
                    void *(*alloc)(size_t), void (*release)(void *))
{
    /* The same build again is not a reopen: tearing down a populated index
     * to rebuild it from a blob not yet flushed is how the original lost
     * entries while the blob grew. */
    if (tilecache_is_open(c) && strcmp(c->build, build) == 0) return true;
    tilecache_close(c);
    memset(c, 0, sizeof(*c));
    if (!dir || !build || !*build || !alloc || !release || !max_entries) return false;

    snprintf(c->build, sizeof(c->build), "%s", build);
    if (snprintf(c->blob_path, sizeof(c->blob_path), "%s/%s.dat", dir, c->build) >=
            (int)sizeof(c->blob_path) ||
        snprintf(c->idx_path, sizeof(c->idx_path), "%s/%s.idx", dir, c->build) >=
            (int)sizeof(c->idx_path)) return false;

    c->release = release;
    c->cap = max_entries;
    c->idx = alloc((size_t)max_entries * sizeof(c->idx[0]));
    if (!c->idx) return false;

    /* "r+b", then "w+b" to create: read-write without truncating. The
     * original's FILE_APPEND made the cache write-only. */
    c->blob = fopen(c->blob_path, "r+b");
    if (!c->blob) c->blob = fopen(c->blob_path, "w+b");
    if (!c->blob) { tilecache_close(c); return false; }

    if (fseek(c->blob, 0, SEEK_END) != 0) { tilecache_close(c); return false; }
    const long end = ftell(c->blob);
    c->blob_len = end > 0 ? (uint32_t)end : 0;

    if (!idx_load(c)) rescan(c);
    c->st.entries = c->n;
    c->st.blob_bytes = c->blob_len;
    return true;
}

void tilecache_close(tilecache_t *c)
{
    if (c->blob) {
        fflush(c->blob);
        if (c->idx) idx_save(c);
        fclose(c->blob);
        c->blob = NULL;
    }
    if (c->idx && c->release) c->release(c->idx);
    c->idx = NULL;
    c->n = c->cap = 0;
}

bool tilecache_get(tilecache_t *c, uint8_t z, uint32_t x, uint32_t y,
                   uint8_t *dst, uint32_t *len)
{
    if (!tilecache_is_open(c)) return false;
    bool found;
    const int at = find_slot(c, pmt_zxy_to_tileid(z, x, y), &found);
    if (!found) { c->st.misses++; return false; }
    const uint32_t n = c->idx[at].len;
    if (n == 0) { *len = 0; c->st.hits++; return true; }
    if (n > *len ||
        fseek(c->blob, (long)c->idx[at].offset, SEEK_SET) != 0 ||
        fread(dst, 1, n, c->blob) != n) { c->st.misses++; return false; }
    *len = n;
    c->st.hits++;
    return true;
}

bool tilecache_is_empty(tilecache_t *c, uint8_t z, uint32_t x, uint32_t y)
{
    if (!tilecache_is_open(c)) return false;
    bool found;
    const int at = find_slot(c, pmt_zxy_to_tileid(z, x, y), &found);
    return found && c->idx[at].len == 0;
}

bool tilecache_put(tilecache_t *c, uint8_t z, uint32_t x, uint32_t y,
                   const uint8_t *src, uint32_t len)
{
    if (!tilecache_is_open(c)) return false;
    const uint64_t id = pmt_zxy_to_tileid(z, x, y);
    bool found;
    find_slot(c, id, &found);
    if (found) return true;
    if (c->n >= c->cap) return false;
    const uint32_t off = c->blob_len;
    if (len > TILECACHE_BLOB_MAX - REC_HDR || off > TILECACHE_BLOB_MAX - REC_HDR - len)
        return false;

    uint8_t h[REC_HDR];
    put32(h, REC_MAGIC);
    put64(h + 4, id);
    put32(h + 12, len);
    if (fseek(c->blob, (long)off, SEEK_SET) != 0 ||
        fwrite(h, 1, sizeof(h), c->blob) != sizeof(h) ||
        (len && fwrite(src, 1, len, c->blob) != len)) {
        /* Part of a record may be on the card. The length is not advanced,
         * so the next put writes over it. */
        return false;
    }
    c->blob_len = off + REC_HDR + len;
    idx_insert(c, id, off + REC_HDR, len);
    c->st.writes++;
    c->st.entries = c->n;
    c->st.blob_bytes = c->blob_len;
    if (++c->since_flush >= flush_interval(c)) { fflush(c->blob); idx_save(c); }
    return true;
}

void tilecache_flush(tilecache_t *c)
{
    if (!tilecache_is_open(c)) return;
    fflush(c->blob);
    idx_save(c);
}

void tilecache_remove(const char *dir, const char *build)
{
    char p[96];
    if (snprintf(p, sizeof(p), "%s/%s.dat", dir, build) < (int)sizeof(p)) remove(p);
    if (snprintf(p, sizeof(p), "%s/%s.idx", dir, build) < (int)sizeof(p)) remove(p);
}

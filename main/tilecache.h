/*
 * tilecache.h -- tiles fetched over the network, kept on the card.
 *
 * original/tilecache.cpp in C, unchanged in shape: one append-only blob
 * of records, each with its own header, and a sorted index of them held
 * in PSRAM and written out every so often. Not a file per tile -- the
 * original's header says why: thousands of small creates is how a FAT32
 * card is lost to a power cut, and 32 KB clusters waste most of each
 * 3 KB tile. If the index is lost or stale, the blob is rescanned.
 *
 * A zero-length record is a negative marker: the archive has no tile
 * there, so the network is not asked again.
 *
 * Free of ESP-IDF: plain stdio, and the caller's allocator for the
 * index. stdio offsets are a `long`, 32 bits on the P4, so the blob
 * stops growing at TILECACHE_BLOB_MAX; puts past it are refused. Not
 * thread-safe; one caller holds a lock around it (tilesrc.c).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* src: original/mapconfig.h CACHE_MAX_ENTRIES_CFG. 16 bytes each, so
 * 1.3 MB of PSRAM. */
#define TILECACHE_ENTRIES_DEFAULT  (80000u)

/* src: chosen. Below 2^31 so every offset fits stdio's 32-bit `long`. */
#define TILECACHE_BLOB_MAX  (0x7F000000u)

typedef struct {
    uint64_t tile_id;
    uint32_t offset;            /* of the payload, past its header */
    uint32_t len;
} tilecache_entry_t;

typedef struct {
    uint32_t entries, hits, misses, writes;
    uint32_t blob_bytes;
    uint32_t index_flushes, rescans;
} tilecache_stats_t;

typedef struct {
    FILE              *blob;
    tilecache_entry_t *idx;
    uint32_t           n, cap;
    uint32_t           blob_len;     /* append offset, tracked not asked */
    uint32_t           since_flush;
    char               build[16];
    char               blob_path[96], idx_path[96];
    void             (*release)(void *p);
    tilecache_stats_t  st;
} tilecache_t;

/*
 * Open, or create, the cache for archive build `build` under directory
 * `dir` (which must exist): `<dir>/<build>.dat` and `.idx`. The index
 * comes from `alloc` (max_entries x 16 bytes) and goes back through
 * `release`. False on failure, with the cache closed.
 */
bool tilecache_open(tilecache_t *c, const char *dir, const char *build,
                    uint32_t max_entries,
                    void *(*alloc)(size_t), void (*release)(void *));

/* Flush the index and close. Safe on a closed cache. */
void tilecache_close(tilecache_t *c);

bool tilecache_is_open(const tilecache_t *c);

/*
 * A hit: true, and *len set -- to 0 for a negative marker, otherwise the
 * payload copied into dst (whose size *len gives on entry). A payload
 * larger than *len is a miss.
 */
bool tilecache_get(tilecache_t *c, uint8_t z, uint32_t x, uint32_t y,
                   uint8_t *dst, uint32_t *len);

/* Whether z/x/y is cached as a negative marker. */
bool tilecache_is_empty(tilecache_t *c, uint8_t z, uint32_t x, uint32_t y);

/* Append a tile; len 0 records a negative marker. A tile already cached
 * is left as it is and counts as success. */
bool tilecache_put(tilecache_t *c, uint8_t z, uint32_t x, uint32_t y,
                   const uint8_t *src, uint32_t len);

/* Write the index out now. */
void tilecache_flush(tilecache_t *c);

/* Remove both files of a closed build's cache. */
void tilecache_remove(const char *dir, const char *build);

#ifdef __cplusplus
}
#endif

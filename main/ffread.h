/*
 * ffread.h -- a file on the card read through FatFs directly.
 *
 * original/bigfile.cpp, without the exFAT half. Why FatFs and not stdio:
 * fseek() takes a long, 32 bits here, so stdio stops at 2 GB, and
 * feckless-storage's storage_io_read_at() refuses past that for the same
 * reason. plan-extracts.py sizes bands to fit under FAT32's 4 GB, so an
 * extract is routinely between the two. f_lseek() takes FSIZE_t, which
 * on FAT32 is 32 unsigned bits: the whole of any file FAT32 can hold.
 *
 * A file over 4 GB needs exFAT, and exFAT needs the patched FatFs the
 * player vendors (its cmake/exfat.cmake). Not this milestone.
 *
 * Fast seek (CONFIG_FATFS_USE_FASTSEEK) builds a cluster map at open, as
 * bigfile.cpp does; without it every seek walks the FAT chain, which the
 * original measured at ~500 ms on a large archive.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ffread_s ffread_t;

/* `drive` is FatFs's volume number (storage_ff_drive()), `path` from the
 * volume's root. NULL if it will not open. */
ffread_t *ffread_open(int drive, const char *path);
void      ffread_close(ffread_t *f);
uint64_t  ffread_size(const ffread_t *f);

/* pmt_read_fn: `len` bytes at `off` into dst, 0 on success. Each call
 * holds feckless-storage's arbiter, a chunk at a time. */
int ffread_read(void *ctx, uint64_t off, uint32_t len, uint8_t *dst);

#ifdef __cplusplus
}
#endif

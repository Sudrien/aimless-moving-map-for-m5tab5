/*
 * ffread.c -- see ffread.h. original/bigfile.cpp, on FAT32 or exFAT.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ffread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ff.h"

#include "storage_io.h"

static const char *TAG = "ffread";

struct ffread_s {
    FIL      fp;
    uint64_t size;
    char     path[80];
    DWORD   *clmt;
};

/* The cluster map for fast seek. src: original/bigfile.cpp clmt_build():
 * 256 items first, then whatever FatFs says it needs, up to 16384. */
static void clmt_build(ffread_t *f)
{
#if FF_USE_FASTSEEK
    uint32_t items = 256;
    for (int attempt = 0; attempt < 2; attempt++) {
        DWORD *tbl = heap_caps_malloc(items * sizeof(DWORD), MALLOC_CAP_SPIRAM);
        if (!tbl) tbl = malloc(items * sizeof(DWORD));
        if (!tbl) break;
        f->fp.cltbl = tbl;
        tbl[0] = items;
        const FRESULT r = f_lseek(&f->fp, CREATE_LINKMAP);
        if (r == FR_OK) {
            f->clmt = tbl;
            f_lseek(&f->fp, 0);
            ESP_LOGI(TAG, "%s: fast seek, %u of %u table items", f->path,
                     (unsigned)tbl[0], (unsigned)items);
            return;
        }
        const uint32_t need = (r == FR_NOT_ENOUGH_CORE) ? (uint32_t)tbl[0] : 0;
        f->fp.cltbl = NULL;
        free(tbl);
        if (!need || need > 16384 || attempt == 1) break;
        items = need;
    }
    ESP_LOGW(TAG, "%s: no fast seek; every seek walks the FAT chain", f->path);
#else
    (void)f;
    ESP_LOGW(TAG, "built without CONFIG_FATFS_USE_FASTSEEK; seeks walk the FAT chain");
#endif
}

ffread_t *ffread_open(int drive, const char *path)
{
    if (drive < 0 || !path || !*path) return NULL;
    ffread_t *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    const int n = snprintf(f->path, sizeof(f->path), "%d:%s%s", drive,
                           path[0] == '/' ? "" : "/", path);
    if (n < 0 || (size_t)n >= sizeof(f->path) ||
        f_open(&f->fp, f->path, FA_READ) != FR_OK) {
        free(f);
        return NULL;
    }
    f->size = (uint64_t)f_size(&f->fp);
    clmt_build(f);
    return f;
}

void ffread_close(ffread_t *f)
{
    if (!f) return;
    f_close(&f->fp);
#if FF_USE_FASTSEEK
    f->fp.cltbl = NULL;
#endif
    free(f->clmt);
    free(f);
}

uint64_t ffread_size(const ffread_t *f) { return f ? f->size : 0; }

int ffread_read(void *ctx, uint64_t off, uint32_t len, uint8_t *dst)
{
    ffread_t *f = ctx;
    if (!f || off + len > f->size) return -1;
    int rc = 0;
    storage_io_acquire(STORAGE_IO_PLAYBACK);
    if (f_lseek(&f->fp, (FSIZE_t)off) != FR_OK || f_tell(&f->fp) != (FSIZE_t)off) rc = -1;
    uint32_t done = 0;
    while (rc == 0 && done < len) {
        const UINT want = (UINT)((len - done) > STORAGE_IO_CHUNK ? STORAGE_IO_CHUNK : (len - done));
        UINT got = 0;
        if (f_read(&f->fp, dst + done, want, &got) != FR_OK || got == 0) rc = -1;
        done += got;
    }
    storage_io_release();
    return rc;
}

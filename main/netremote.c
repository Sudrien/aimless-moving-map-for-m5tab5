/*
 * netremote.c -- see netremote.h. original/netsource.cpp's network half.
 *
 * SPDX-License-Identifier: MIT
 */
#include "netremote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "ethernet.h"           /* net_online() */
#include "wifi.h"               /* wifi_ntp_synced() */

#include "builddate.h"
#include "tilecache.h"

static const char *TAG = "netremote";

/* src: original/netsource.cpp REFRESH_DAYS. */
#define REFRESH_DAYS        (30)
/* src: original/netsource.cpp MAX_PROBE_DAYS: Protomaps keep about a
 * week of dailies, so nothing older answers. */
#define MAX_PROBE_DAYS      (8)
/* src: original/netsource.cpp RANGE_CHUNK: large ranges arrived slowly
 * or not at all; requests this size were reliable. */
#define RANGE_CHUNK         (32 * 1024)
/* src: original/netsource.cpp NET_REQUEST_GAP_MS -- polite spacing
 * against a bucket that asks not to be hotlinked. */
#define REQUEST_GAP_MS      (150)
/* src: original/netsource.cpp NET_KEEPALIVE_IDLE_MS. */
#define KEEPALIVE_IDLE_MS   (10000)
/* src: original/netsource.cpp g_http.setTimeout(15000). */
#define HTTP_TIMEOUT_MS     (15000)
/* src: chosen. How long a failed open or probe waits before the next. */
#define RETRY_MS            (30000)

static char             s_dir[64];
static maptile_alloc_t  s_mem;
static int32_t          s_today;
static char             s_build[16];
static int32_t          s_adopted;          /* the day it was adopted */
static maparchive_t     s_arc;
static bool             s_arc_open;
static tilecache_t      s_cache;
static int64_t          s_retry_at;
static bool             s_said_no_date;

static esp_http_client_handle_t s_http;
static char             s_url[160];
static int64_t          s_last_req;
static netremote_stats_t s_st;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ---- the pooled connection ----
 *
 * src: original/netsource.cpp's pooled HTTPClient. One socket kept
 * between requests, because the TLS handshake is most of a small tile's
 * cost. It survives only a request that went exactly as asked: anything
 * else and unread bytes may be sitting in it, which the next request
 * would read as its own reply. */

static void pool_drop(void)
{
    if (!s_http) return;
    esp_http_client_cleanup(s_http);
    s_http = NULL;
}

static bool pool_ready(const char *url)
{
    if (s_http && now_ms() - s_last_req > KEEPALIVE_IDLE_MS) pool_drop();
    if (s_http) return true;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };
    s_http = esp_http_client_init(&cfg);
    if (s_http) s_st.fresh++;
    return s_http != NULL;
}

/* One range request on the pooled connection. *reused says whether it
 * went out on a socket already open, which decides whether a failure is
 * worth one more try on a fresh one. */
static int range_once(const char *url, uint64_t off, uint32_t len, uint8_t *dst,
                      bool *reused)
{
    *reused = s_http != NULL;
    if (!pool_ready(url)) return -1;
    if (esp_http_client_set_url(s_http, url) != ESP_OK) { pool_drop(); return -1; }
    char range[64];
    snprintf(range, sizeof(range), "bytes=%llu-%llu",
             (unsigned long long)off, (unsigned long long)(off + len - 1));
    esp_http_client_set_header(s_http, "Range", range);

    const int64_t t0 = now_ms();
    if (esp_http_client_open(s_http, 0) != ESP_OK) { pool_drop(); return -1; }
    const int64_t clen = esp_http_client_fetch_headers(s_http);
    const int code = esp_http_client_get_status_code(s_http);
    /* 206 or nothing: a 200 is the whole archive on its way. */
    if (code != 206) {
        if (code == 200) ESP_LOGW(TAG, "server ignored Range; refusing the body");
        else ESP_LOGW(TAG, "range %llu+%u: HTTP %d", (unsigned long long)off,
                      (unsigned)len, code);
        pool_drop();
        return -1;
    }
    if (clen >= 0 && clen != (int64_t)len) {
        ESP_LOGW(TAG, "asked %u bytes, told %lld", (unsigned)len, (long long)clen);
        pool_drop();
        return -1;
    }
    uint32_t got = 0;
    while (got < len) {
        const int n = esp_http_client_read(s_http, (char *)dst + got, (int)(len - got));
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    s_last_req = now_ms();
    s_st.last_ms = (uint32_t)(s_last_req - t0);
    if (got != len || !esp_http_client_is_complete_data_received(s_http)) {
        ESP_LOGW(TAG, "range %llu+%u: got %u", (unsigned long long)off,
                 (unsigned)len, (unsigned)got);
        pool_drop();
        return -1;
    }
    s_st.requests++;
    s_st.bytes += got;
    return 0;
}

static int range_read(const char *url, uint64_t off, uint32_t len, uint8_t *dst)
{
    if (!net_online()) { pool_drop(); return -1; }
    const int64_t since = now_ms() - s_last_req;
    if (s_last_req && since < REQUEST_GAP_MS)
        vTaskDelay(pdMS_TO_TICKS(REQUEST_GAP_MS - since));
    bool reused = false;
    int r = range_once(url, off, len, dst, &reused);
    if (r != 0 && reused) {
        /* A keep-alive the far end closed looks like any other failure;
         * a fresh connection tells them apart. */
        ESP_LOGI(TAG, "pooled connection failed, retrying fresh");
        pool_drop();
        r = range_once(url, off, len, dst, &reused);
    }
    if (r != 0) s_st.failed++;
    return r;
}

/* The archive's read callback. */
static int net_read(void *ctx, uint64_t off, uint32_t len, uint8_t *dst)
{
    (void)ctx;
    for (uint32_t done = 0; done < len; ) {
        const uint32_t n = (len - done) > RANGE_CHUNK ? RANGE_CHUNK : (len - done);
        if (range_read(s_url, off + done, n, dst + done) != 0) return -1;
        done += n;
    }
    return 0;
}

/* ---- which build ---- */

static void set_url(const char *build)
{
    snprintf(s_url, sizeof(s_url), "%s%s.pmtiles", CONFIG_AIMLESS_TILE_BASE, build);
}

/* The first 16 bytes of a candidate: "PMTiles" and version 3. */
static bool probe(const char *build)
{
    set_url(build);
    uint8_t hdr[16];
    const bool ok = range_read(s_url, 0, sizeof(hdr), hdr) == 0 &&
                    memcmp(hdr, "PMTiles", 7) == 0 && hdr[7] == 3;
    ESP_LOGI(TAG, "probing build %s: %s", build, ok ? "ok" : "no");
    return ok;
}

static bool discover(char *out)
{
    for (int back = 0; back < MAX_PROBE_DAYS; back++) {
        char name[9];
        if (!bd_name(s_today - back, name)) return false;
        if (probe(name)) { snprintf(out, 16, "%s", name); return true; }
    }
    ESP_LOGW(TAG, "no build found in the last %d days", MAX_PROBE_DAYS);
    return false;
}

static void manifest_path(char *p, size_t n) { snprintf(p, n, "%s/build.txt", s_dir); }

static void manifest_save(void)
{
    char p[96];
    manifest_path(p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (!f) return;
    /* The original's format: the build, then the day it was adopted. Its
     * day count is from year 0, not 1970; it is only ever compared with
     * itself, and this file's is with this file's. */
    fprintf(f, "%s\n%ld\n", s_build, (long)s_adopted);
    fclose(f);
}

static void manifest_load(void)
{
    char p[96];
    manifest_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f) return;
    char a[32] = "", b[32] = "";
    if (fgets(a, sizeof(a), f) && fgets(b, sizeof(b), f)) {
        a[strcspn(a, "\r\n")] = 0;
        if (strlen(a) > 0 && strlen(a) < sizeof(s_build)) {
            snprintf(s_build, sizeof(s_build), "%s", a);
            s_adopted = (int32_t)strtol(b, NULL, 10);
            /* An adoption day from the original's epoch is centuries
             * ahead of this one's; read it as "adopted today", so the
             * build is used and refreshed on schedule. */
            if (s_adopted > 100000) s_adopted = 0;
        }
    }
    fclose(f);
}

static void today_from_clock(void)
{
    if (s_today || !wifi_ntp_synced()) return;
    const time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    if (!bd_year_plausible(tm.tm_year + 1900)) return;
    s_today = bd_days(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    ESP_LOGI(TAG, "date %04d-%02d-%02d from SNTP", tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday);
}

/* Close the remote archive; and its cache too, removing both files, when
 * the build is being replaced. */
static void close_remote(bool replaced)
{
    if (s_arc_open) { maparchive_close(&s_arc); s_arc_open = false; }
    pool_drop();
    if (replaced && tilecache_is_open(&s_cache)) {
        char old[16];
        snprintf(old, sizeof(old), "%s", s_cache.build);
        tilecache_close(&s_cache);
        tilecache_remove(s_dir, old);
    }
}

static bool open_build(void)
{
    set_url(s_build);
    if (!tilecache_open(&s_cache, s_dir, s_build, TILECACHE_ENTRIES_DEFAULT,
                        s_mem.big, s_mem.release))
        ESP_LOGW(TAG, "no tile cache for %s; tiles will be fetched every time", s_build);
    else
        ESP_LOGI(TAG, "cache %s: %u tiles, %u KB%s", s_build, (unsigned)s_cache.n,
                 (unsigned)(s_cache.blob_len / 1024),
                 s_cache.st.rescans ? " (rescanned)" : "");
    const pmt_err_t e = maparchive_open(&s_arc, net_read, NULL, &s_mem);
    if (e != PMT_OK) {
        ESP_LOGW(TAG, "remote %s: %s", s_build, pmt_strerror(e));
        maparchive_close(&s_arc);
        return false;
    }
    s_arc_open = true;
    ESP_LOGI(TAG, "remote build %s open, z%u-%u", s_build,
             s_arc.pmt.hdr.min_zoom, s_arc.pmt.hdr.max_zoom);
    return true;
}

/* ---- public ---- */

void netremote_init(const char *dir, const maptile_alloc_t *mem)
{
    snprintf(s_dir, sizeof(s_dir), "%s", dir);
    s_mem = *mem;
    mkdir(s_dir, 0775);
    manifest_load();
    if (s_build[0]) {
        ESP_LOGI(TAG, "last build %s", s_build);
        /* Cached tiles are good offline, before any network. */
        if (tilecache_open(&s_cache, s_dir, s_build, TILECACHE_ENTRIES_DEFAULT,
                           s_mem.big, s_mem.release))
            ESP_LOGI(TAG, "cache %s: %u tiles, %u KB", s_build, (unsigned)s_cache.n,
                     (unsigned)(s_cache.blob_len / 1024));
    }
    ESP_LOGI(TAG, "tiles from %s%s", CONFIG_AIMLESS_TILE_BASE,
             CONFIG_AIMLESS_PINNED_BUILD[0] ? CONFIG_AIMLESS_PINNED_BUILD ".pmtiles"
                                            : "<date>.pmtiles");
}

void netremote_set_today(int32_t days)
{
    if (days > 0 && days != s_today) s_today = days;
}

void netremote_update(tilesrc_t *s)
{
    s->cache = tilecache_is_open(&s_cache) ? &s_cache : NULL;
    if (!net_online()) {
        if (s->remote) ESP_LOGI(TAG, "offline");
        s->remote = NULL;
        pool_drop();
        return;
    }
    today_from_clock();

    const bool pinned = CONFIG_AIMLESS_PINNED_BUILD[0] != 0;
    const bool aged = !pinned && s_arc_open && s_today && s_adopted &&
                      s_today - s_adopted >= REFRESH_DAYS;
    if (s_arc_open && !aged) { s->remote = &s_arc; return; }

    s->remote = NULL;
    if (now_ms() < s_retry_at) return;
    s_retry_at = now_ms() + RETRY_MS;

    char want[16] = "";
    if (pinned) {
        snprintf(want, sizeof(want), "%s", CONFIG_AIMLESS_PINNED_BUILD);
    } else if (s_build[0] && !aged &&
               (!s_today || !s_adopted || s_today - s_adopted < REFRESH_DAYS)) {
        snprintf(want, sizeof(want), "%s", s_build);
    } else if (!s_today) {
        if (!s_said_no_date) {
            ESP_LOGI(TAG, "no date yet (GNSS or SNTP); the network waits for one");
            s_said_no_date = true;
        }
        return;
    } else if (!discover(want)) {
        return;
    }

    if (aged) ESP_LOGI(TAG, "build %s is %ld days old; looking again", s_build,
                       (long)(s_today - s_adopted));
    const bool changed = strcmp(want, s_build) != 0;
    if (changed && s_build[0]) ESP_LOGI(TAG, "build %s -> %s, old cache removed", s_build, want);
    close_remote(changed);
    snprintf(s_build, sizeof(s_build), "%s", want);
    if (changed || !s_adopted || aged) {
        s_adopted = s_today;
        manifest_save();
    }
    if (open_build()) {
        s_retry_at = 0;
        s->remote = &s_arc;
    }
    s->cache = tilecache_is_open(&s_cache) ? &s_cache : NULL;
}

void netremote_stats(netremote_stats_t *out)
{
    *out = s_st;
    snprintf(out->build, sizeof(out->build), "%s", s_build);
    out->open = s_arc_open;
}

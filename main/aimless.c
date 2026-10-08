/*
 * aimless.c -- the map from the card and the network, with the
 * receiver's position on it. Milestones 1 and 2.
 *
 * What runs:
 *
 *   tab5io        the I2C bus and expanders (LCD_RST, CHG_EN)
 *   lcd, gfx      the panel, turned to landscape (GFX_ROT_270, the way
 *                 up lothesome-audio-analyzer found for the original's
 *                 setRotation(3))
 *   storage       the card and a USB drive, through feckless-storage;
 *                 every .pmtiles in either root is opened (mapset.h)
 *   gnss          the M135 on GPIO6/7, its own task
 *   network       Wi-Fi through the C6 and USB Ethernet, through
 *                 feckless-network; saved networks are defeatist's
 *   render task   core 1: one tile at a time, from the cache, the card
 *                 or the network (tilesrc.h, netremote.h)
 *   this loop     follow the fix, draw
 *
 * What the original did that this does not yet: labels, place names,
 * zoom levels other than z14, the compass, waypoints, the setup portal
 * (so no way to add a network here yet), Wi-Fi location, the world map
 * floor, the night palette's automatic switch. ARCHITECTURE.md has the
 * milestones.
 *
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "gfx.h"
#include "lcd.h"
#include "storage.h"
#include "storage_io.h"
#include "tab5io.h"
#include "usbhost.h"

#include "ethernet.h"
#include "feckless_net.h"
#include "wifi.h"
#include "wifistore.h"

#include "ffread.h"
#include "builddate.h"
#include "gnss.h"
#include "mapconfig.h"
#include "mapset.h"
#include "maptile.h"
#include "mapview.h"
#include "style.h"
#include "netremote.h"
#include "tilesrc.h"

static const char *TAG = "aimless";

/* The way up for the original's setRotation(3); see the top of this file. */
#define VIEW_ROTATION   (GFX_ROT_270)
#define BACKLIGHT_PCT   (80)

/* The status strip along the top. */
#define STATUS_H        (36)
#define TEXT_SCALE      (2)

/* The zoom everything is drawn at this milestone.
 * src: original/mapconfig.h Z_FLOOR. */
#define VIEW_ZOOM       (Z_FLOOR)

/* src: original/mapengine.cpp draw_marker(): blue with a good 3D fix,
 * grey with a coarse one. */
#define MARKER_R        (12)
#define COL_FINE        RGB(30, 120, 230)
#define COL_COARSE      RGB(130, 130, 130)
#define COL_RING        RGB(255, 255, 255)
#define COL_STATUS_BG   RGB(0, 0, 0)
#define COL_STATUS_FG   RGB(255, 255, 255)
#define COL_WAIT        RGB(240, 180, 60)

/* The NVS namespace saved networks are read from: defeatist's, so a
 * network joined there is joined here (0007 shares the partition). */
#define NETS_NVS_NS     "defeatist"

/* Where the network's tile cache lives: the original's /t, on the card.
 * src: original/tilecache.cpp CACHE_DIR. */
#define CACHE_SUBDIR    "/t"

/* The render task. src: original/mapengine.cpp's worker, pinned to
 * core 1 so a slow network fetch never holds up the screen or GNSS on
 * core 0. Its stack has the TLS handshake on it: mbedTLS's is several KB.
 * src: chosen, with headroom; the high-water mark is logged. */
#define RENDER_CORE     (1)
#define RENDER_PRIO     (4)
#define RENDER_STACK    (16384)

/* src: chosen. How often tiles that failed are tried again. */
#define REDO_ERRORS_US  (30 * 1000000LL)

static maparchive_t s_arc[MAPSET_MAX];
static ffread_t    *s_file[MAPSET_MAX];
static mapset_t     s_set;
static maprender_t  s_render;
static mapview_t    s_view;
static tilesrc_t    s_src = { .local = &s_set };
/* Everything in s_view, between the render task and this loop. The
 * draw itself runs outside it (mapview_take()). */
static SemaphoreHandle_t s_lock;
static volatile bool s_dirty;
static volatile bool s_online;

/* ---- memory: PSRAM for the big buffers, internal RAM for the hot ones,
 * as original/mapengine.cpp alloc_all() ---- */
static void *mem_big(size_t n)  { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void *mem_fast(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }
static const maptile_alloc_t MEM = { mem_big, mem_fast, heap_caps_free };

/* ---- archives ---- */

/* Every *.pmtiles in one volume's root. */
static void scan_volume(storage_id_t id)
{
    const int drv = storage_ff_drive(id);
    if (drv < 0) return;
    char root[16];   /* "%d:/" for any int: GCC checks the full range */
    snprintf(root, sizeof(root), "%d:/", drv);
    FF_DIR d;
    FILINFO fi;
    if (f_opendir(&d, root) != FR_OK) return;
    while (s_set.n < MAPSET_MAX && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & AM_DIR) continue;
        const size_t n = strlen(fi.fname);
        if (n < 9 || strcasecmp(fi.fname + n - 8, ".pmtiles") != 0) continue;
        const int i = s_set.n;
        s_file[i] = ffread_open(drv, fi.fname);
        if (!s_file[i]) { ESP_LOGW(TAG, "%s: will not open", fi.fname); continue; }
        const pmt_err_t e = maparchive_open(&s_arc[i], ffread_read, s_file[i], &MEM);
        if (e != PMT_OK) {
            ESP_LOGW(TAG, "%s: %s", fi.fname, pmt_strerror(e));
            maparchive_close(&s_arc[i]);
            ffread_close(s_file[i]);
            s_file[i] = NULL;
            continue;
        }
        const pmt_header_t *h = &s_arc[i].pmt.hdr;
        ESP_LOGI(TAG, "%s %s: %.1f MB, z%u-%u, %.2f..%.2f E, %.2f..%.2f N",
                 storage_label(id), fi.fname, ffread_size(s_file[i]) / 1048576.0,
                 h->min_zoom, h->max_zoom, h->min_lon_e7 / 1e7, h->max_lon_e7 / 1e7,
                 h->min_lat_e7 / 1e7, h->max_lat_e7 / 1e7);
        mapset_add(&s_set, &s_arc[i]);
    }
    f_closedir(&d);
}

/* The view's draw callback: every tile through the source chain. The
 * render task calls tilesrc_draw() itself, through mapview_take(), so
 * this is mapview_step()'s and unused here. */
static tile_state_t src_draw(void *ctx, tile_id_t id, uint16_t *px, int split)
{
    (void)ctx;
    return tilesrc_draw(&s_src, &s_render, id, px, split, NULL);
}

/* ---- drawing ---- */

static void draw_status(const gnss_fix_t *fix, int pending)
{
    char line[160];
    if (gnss_coarse(fix)) {
        snprintf(line, sizeof(line), "%.5f %c  %.5f %c   z%d   %d sats   HDOP %.1f   %.0f km/h   %.6sZ   %s%s",
                 fabs(fix->lat), fix->lat < 0 ? 'S' : 'N',
                 fabs(fix->lon), fix->lon < 0 ? 'W' : 'E',
                 VIEW_ZOOM, fix->sats, fix->hdop, fix->speed_kmh, fix->utc,
                 s_online ? "online" : "offline", pending ? "   drawing" : "");
    } else {
        snprintf(line, sizeof(line), "waiting for a fix   %u sentences   %d sats in view   %s%s",
                 (unsigned)gnss_sentences(),
                 fix->cons[0].visible + fix->cons[1].visible + fix->cons[2].visible +
                 fix->cons[3].visible, s_online ? "online" : "offline",
                 pending ? "   drawing" : "");
    }
    gfx_fill_rect(0, 0, gfx_w(), STATUS_H, COL_STATUS_BG);
    gfx_draw_text(10, (STATUS_H - GFX_GLYPH_H(TEXT_SCALE)) / 2, line, TEXT_SCALE,
                  gfx_w() - 20, gnss_coarse(fix) ? COL_STATUS_FG : COL_WAIT);
}

static void draw_message(const char *a, const char *b)
{
    gfx_fill_rect(0, 0, gfx_w(), gfx_h(), COL_STATUS_BG);
    gfx_draw_text(40, gfx_h() / 2 - 40, a, 3, gfx_w() - 80, COL_STATUS_FG);
    if (b) gfx_draw_text(40, gfx_h() / 2 + 10, b, 2, gfx_w() - 80, COL_WAIT);
    gfx_blit(0, gfx_h());
}

static void draw(const gnss_fix_t *fix)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mapview_compose(&s_view, gfx_fb(), gfx_w(), gfx_h(), gfx_w());
    const int pending = mapview_pending(&s_view);
    xSemaphoreGive(s_lock);
    /* The marker is at the window's centre: mapview centres on the
     * position. No marker without a fix -- a remembered or seeded
     * position is a claim, not a placeholder (original/mapengine.cpp). */
    if (gnss_coarse(fix)) {
        const int cx = gfx_w() / 2, cy = gfx_h() / 2;
        gfx_fill_circle(cx, cy, MARKER_R + 3, COL_RING);
        gfx_fill_circle(cx, cy, MARKER_R, gnss_fine(fix) ? COL_FINE : COL_COARSE);
    }
    draw_status(fix, pending);
    gfx_blit(0, gfx_h());
}

/* ---- the render task ---- */

static const char *from_name(tilesrc_from_t f)
{
    switch (f) {
    case TILESRC_CACHE: return "cache";
    case TILESRC_LOCAL: return "card";
    case TILESRC_NET:   return "network";
    default:            return "nowhere";
    }
}

static void render_task(void *arg)
{
    (void)arg;
    bool had_remote = false;
    int64_t last_redo = esp_timer_get_time();
    uint32_t tiles = 0;
    for (;;) {
        /* Outside the lock: this may be seconds of HTTP. */
        netremote_update(&s_src);
        const bool remote = s_src.remote != NULL;
        s_online = remote;

        render_job_t job;
        uint16_t *px = NULL;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const int64_t now = esp_timer_get_time();
        if (remote && !had_remote) {
            /* The network has arrived: everything the card did not have
             * is worth asking it for. */
            const int n = mapview_redo(&s_view, true);
            if (n) ESP_LOGI(TAG, "network up: %d tile%s to try again", n, n == 1 ? "" : "s");
            last_redo = now;
        } else if (now - last_redo >= REDO_ERRORS_US) {
            mapview_redo(&s_view, false);
            last_redo = now;
        }
        had_remote = remote;
        const bool took = mapview_take(&s_view, &job, &px);
        xSemaphoreGive(s_lock);

        if (!took) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        const int64_t t0 = esp_timer_get_time();
        tilesrc_from_t from;
        const tile_state_t t = tilesrc_draw(&s_src, &s_render, job.id, px, SUBTILE_SPLIT, &from);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        mapview_commit(&s_view, &job, t);
        xSemaphoreGive(s_lock);
        s_dirty = true;

        ESP_LOGI(TAG, "tile %u/%u/%u in %u ms from %s: %s (%u -> %u bytes)",
                 job.id.z, (unsigned)job.id.x, (unsigned)job.id.y,
                 (unsigned)((esp_timer_get_time() - t0) / 1000), from_name(from),
                 t == TILE_READY ? "drawn" : t == TILE_NODATA ? "no data" : "FAILED",
                 (unsigned)s_render.last_bytes, (unsigned)s_render.last_inflated);
        if ((++tiles % 16) == 0)
            ESP_LOGI(TAG, "render task stack: %u bytes never used",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

/* ---- the network ---- */

/* The radio only when there is somewhere to join; the cable regardless. */
static bool want_wifi(void) { return wifistore_count() > 0; }
static bool want_ntp(void)  { return true; }

static void net_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        /* Not erased: a program that wipes the saved networks because it
         * could not read them is worse than one that runs without them. */
        ESP_LOGE(TAG, "nvs_flash_init: %s; no saved networks this boot",
                 esp_err_to_name(err));
    }
    wifistore_init(NETS_NVS_NS);
    ESP_LOGI(TAG, "%d saved network%s (from %s's list)", wifistore_count(),
             wifistore_count() == 1 ? "" : "s", NETS_NVS_NS);
    static const feckless_net_hooks_t hooks = {
        .ntp_enabled        = want_ntp,
        .wifi_enabled       = want_wifi,
        .usb_register_class = usbhost_register_class,
    };
    feckless_net_set_hooks(&hooks);
    wifi_init(tab5io_exp2());
    /* Before usbhost_start(), like every class on the port. */
    if (ethernet_init() != ESP_OK)
        ESP_LOGW(TAG, "no USB Ethernet this boot");
}

/* ---- boot ---- */

void app_main(void)
{
    {
        const esp_app_desc_t *d = esp_app_get_description();
        ESP_LOGW(TAG, "=== Aimless Moving Map === %s, IDF %s, built %s %s",
                 d ? d->version : "?", d ? d->idf_ver : "?",
                 d ? d->date : "?", d ? d->time : "?");
    }

    ESP_ERROR_CHECK(tab5io_init());
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(lcd_init(&panel));
    ESP_ERROR_CHECK(gfx_init(panel, LCD_H_RES, LCD_V_RES));
    gfx_set_rotation(VIEW_ROTATION);
    draw_message("Aimless Moving Map", "looking for maps");
    ESP_ERROR_CHECK(lcd_backlight_set(BACKLIGHT_PCT));

    /* The card, and a USB drive on the USB-A port, which feckless-storage
     * registers with the drivers' USB host before the port comes up. */
    ESP_ERROR_CHECK(usbhost_init(tab5io_exp2()));
    storage_io_init();
    static const storage_usb_t usb = {
        .register_class = usbhost_register_class,
        .set_power      = usbhost_set_power,
        .powered        = usbhost_powered,
    };
    ESP_ERROR_CHECK(storage_init(&usb));
    net_start();
    usbhost_start();
    /* The join runs on the network's own worker; this returns at once. */
    wifi_request_apply();

    /* The receiver searches while everything else comes up. */
    if (!gnss_start(GNSS_P4_RX_PIN, GNSS_P4_TX_PIN, GNSS_BAUD, GNSS_PPS_PIN, 0, 5))
        ESP_LOGE(TAG, "GNSS did not start");

    /* Archives: the card is mounted by storage_init(); a USB drive takes
     * a few seconds to enumerate, so it is looked for a few times. None
     * is no longer the end: the network may have the tiles. */
    for (int tries = 0; tries < 4 && s_set.n == 0; tries++) {
        if (tries) vTaskDelay(pdMS_TO_TICKS(2000));
        scan_volume(STORAGE_SD);
        scan_volume(STORAGE_USB);
    }
    ESP_LOGI(TAG, "%d archive%s", s_set.n, s_set.n == 1 ? "" : "s");
    if (s_set.n == 0)
        draw_message("No maps on the card",
                     "Tiles will come from the network once there is one, and a fix.");

    /* The network's tile cache, on the card if there is one, else the
     * drive; without either, tiles are fetched every time. */
    {
        const storage_id_t where = storage_present(STORAGE_SD) ? STORAGE_SD : STORAGE_USB;
        char dir[48] = "";
        if (storage_present(where))
            snprintf(dir, sizeof(dir), "%s" CACHE_SUBDIR, storage_mount_path(where));
        netremote_init(dir[0] ? dir : "/nowhere", &MEM);
    }

    style_init(SUBTILE_PX, 0);
    if (maprender_init(&s_render, SUBTILE_PX, &MEM) != 0) {
        draw_message("Out of memory", "render scratch");
        return;
    }
    uint16_t *bufs[GRID_COUNT];
    for (int i = 0; i < GRID_COUNT; i++) {
        bufs[i] = mem_big((size_t)SUBTILE_PX * SUBTILE_PX * sizeof(uint16_t));
        if (!bufs[i]) { draw_message("Out of memory", "tile buffers"); return; }
    }
    mapview_init(&s_view, src_draw, NULL, bufs, VIEW_ZOOM, style_background());
    ESP_LOGI(TAG, "%d tiles of %d px in PSRAM; %u KB PSRAM, %u KB internal left",
             GRID_COUNT, SUBTILE_PX,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    s_lock = xSemaphoreCreateMutex();

    /* Before a fix, the first archive's centre, so there is a map to look
     * at; without a marker, since it is not where anyone is. With no
     * archive there is nowhere to look until the fix. */
    double lat, lon;
    if (mapset_centre(&s_set, &lat, &lon)) mapview_centre(&s_view, lat, lon);

    if (xTaskCreatePinnedToCore(render_task, "render", RENDER_STACK, NULL,
                                RENDER_PRIO, NULL, RENDER_CORE) != pdPASS) {
        draw_message("Out of memory", "render task");
        return;
    }

    gnss_fix_t fix;
    int64_t last_draw = 0, last_log = 0;
    bool had_fix = false;
    for (;;) {
        gnss_get(&fix);
        if (gnss_coarse(&fix)) {
            if (!had_fix) {
                ESP_LOGI(TAG, "first fix after %u ms", (unsigned)gnss_first_coarse_ms());
                had_fix = true;
            }
            xSemaphoreTake(s_lock, portMAX_DELAY);
            mapview_centre(&s_view, fix.lat, fix.lon);
            xSemaphoreGive(s_lock);
        }
        /* Today's date, for finding a daily build without SNTP. */
        if (fix.status == 'A') netremote_set_today(bd_from_ddmmyy(fix.date));

        const int64_t now = esp_timer_get_time();
        if (s_dirty || now - last_draw >= 1000000) {
            s_dirty = false;
            draw(&fix);
            last_draw = now;
        }
        if (now - last_log >= 10000000) {
            netremote_stats_t ns;
            netremote_stats(&ns);
            char route[64];
            net_route_describe(route, sizeof(route));
            ESP_LOGI(TAG, "fix %c mode %d, %d sats, HDOP %.1f, %u sentences, PPS %u",
                     fix.status, fix.mode, fix.sats, fix.hdop,
                     (unsigned)gnss_sentences(), (unsigned)gnss_pps_count());
            ESP_LOGI(TAG, "net %s, build %s%s, %u requests (%u connections, %u failed), "
                          "%u KB, last %u ms; tiles %u cache, %u card, %u network, %u errors",
                     route, ns.build[0] ? ns.build : "none", ns.open ? " open" : "",
                     (unsigned)ns.requests, (unsigned)ns.fresh, (unsigned)ns.failed,
                     (unsigned)(ns.bytes / 1024), (unsigned)ns.last_ms,
                     (unsigned)s_src.st.cache_hits, (unsigned)s_src.st.local_hits,
                     (unsigned)s_src.st.net_hits, (unsigned)s_src.st.errors);
            last_log = now;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

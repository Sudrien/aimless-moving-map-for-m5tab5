/*
 * aimless.c -- milestone 1: the offline map from the card, with the
 * receiver's position on it.
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
 *   this loop     follow the fix, render one tile at a time, draw
 *
 * What the original did that this does not yet: the network and its
 * tile cache, labels, place names, zoom levels other than z14, the
 * compass, waypoints, the setup portal, Wi-Fi location, the world map
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
#include "freertos/task.h"

#include "gfx.h"
#include "lcd.h"
#include "storage.h"
#include "storage_io.h"
#include "tab5io.h"
#include "usbhost.h"

#include "ffread.h"
#include "gnss.h"
#include "mapconfig.h"
#include "mapset.h"
#include "maptile.h"
#include "mapview.h"
#include "style.h"
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

static maparchive_t s_arc[MAPSET_MAX];
static ffread_t    *s_file[MAPSET_MAX];
static mapset_t     s_set;
static maprender_t  s_render;
static mapview_t    s_view;
static tilesrc_t    s_src = { .local = &s_set };

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

/* The view's draw callback: every tile through the source chain. */
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
        snprintf(line, sizeof(line), "%.5f %c  %.5f %c   z%d   %d sats   HDOP %.1f   %.0f km/h   %.6sZ%s",
                 fabs(fix->lat), fix->lat < 0 ? 'S' : 'N',
                 fabs(fix->lon), fix->lon < 0 ? 'W' : 'E',
                 VIEW_ZOOM, fix->sats, fix->hdop, fix->speed_kmh, fix->utc,
                 pending ? "   drawing" : "");
    } else {
        snprintf(line, sizeof(line), "waiting for a fix   %u sentences   %d sats in view%s",
                 (unsigned)gnss_sentences(),
                 fix->cons[0].visible + fix->cons[1].visible + fix->cons[2].visible +
                 fix->cons[3].visible, pending ? "   drawing" : "");
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
    mapview_compose(&s_view, gfx_fb(), gfx_w(), gfx_h(), gfx_w());
    /* The marker is at the window's centre: mapview centres on the
     * position. No marker without a fix -- a remembered or seeded
     * position is a claim, not a placeholder (original/mapengine.cpp). */
    if (gnss_coarse(fix)) {
        const int cx = gfx_w() / 2, cy = gfx_h() / 2;
        gfx_fill_circle(cx, cy, MARKER_R + 3, COL_RING);
        gfx_fill_circle(cx, cy, MARKER_R, gnss_fine(fix) ? COL_FINE : COL_COARSE);
    }
    draw_status(fix, mapview_pending(&s_view));
    gfx_blit(0, gfx_h());
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
    usbhost_start();

    /* The receiver searches while everything else comes up. */
    if (!gnss_start(GNSS_P4_RX_PIN, GNSS_P4_TX_PIN, GNSS_BAUD, GNSS_PPS_PIN, 0, 5))
        ESP_LOGE(TAG, "GNSS did not start");

    /* Archives: the card is mounted by storage_init(); a USB drive takes
     * a few seconds to enumerate, so it is looked for again until
     * something is found. */
    for (int tries = 0; s_set.n == 0; tries++) {
        scan_volume(STORAGE_SD);
        scan_volume(STORAGE_USB);
        if (s_set.n) break;
        if (tries == 3)
            draw_message("No maps found",
                         "Put one or more .pmtiles files in the root of the card or a USB drive.");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "%d archive%s", s_set.n, s_set.n == 1 ? "" : "s");

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

    /* Before a fix, the first archive's centre, so there is a map to look
     * at; without a marker, since it is not where anyone is. */
    double lat, lon;
    if (mapset_centre(&s_set, &lat, &lon)) mapview_centre(&s_view, lat, lon);

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
            mapview_centre(&s_view, fix.lat, fix.lon);
        }

        bool changed = false;
        if (mapview_pending(&s_view)) {
            const int64_t t0 = esp_timer_get_time();
            mapview_step(&s_view);
            ESP_LOGI(TAG, "tile in %u ms (%u -> %u bytes)",
                     (unsigned)((esp_timer_get_time() - t0) / 1000),
                     (unsigned)s_render.last_bytes, (unsigned)s_render.last_inflated);
            changed = true;
        }

        const int64_t now = esp_timer_get_time();
        if (changed || now - last_draw >= 1000000) {
            draw(&fix);
            last_draw = now;
        }
        if (now - last_log >= 10000000) {
            ESP_LOGI(TAG, "fix %c mode %d, %d sats, HDOP %.1f, %u sentences, PPS %u",
                     fix.status, fix.mode, fix.sats, fix.hdop,
                     (unsigned)gnss_sentences(), (unsigned)gnss_pps_count());
            last_log = now;
        }
        if (!changed) vTaskDelay(pdMS_TO_TICKS(100));
    }
}

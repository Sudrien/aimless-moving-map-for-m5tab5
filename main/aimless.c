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
 *   touch         the panel's controller, turned with the picture
 *   setup         M5Launcher's saved networks imported (launcher_import.h),
 *                 or the setup portal (portal.h): with nothing saved, or
 *                 with a touch in the first two seconds, as the original
 *   this loop     follow the fix (or a pan), draw, the button row, the
 *                 settings and saved points panels, the setup box over
 *                 the map, saved points and the guide to one, touch
 *
 * What the original did that this does not yet: zoom levels other than z14, the compass, waypoints, Wi-Fi location,
 * the world map floor, the night palette's automatic switch.
 * ARCHITECTURE.md has the milestones.
 *
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

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
#include "touch.h"
#include "usbhost.h"

#include "ethernet.h"
#include "feckless_net.h"
#include "wifi.h"
#include "wifistore.h"

#include "ffread.h"
#include "builddate.h"
#include "gnss.h"
#include "launcher_import.h"
#include "mapconfig.h"
#include "mapset.h"
#include "maptile.h"
#include "mapview.h"
#include "motion.h"
#include "places.h"
#include "style.h"
#include "sun.h"
#include "netremote.h"
#include "portal.h"
#include "tilesrc.h"
#include "uirow.h"
#include "waypoints.h"
#include "worldtile.h"
#include "lastfix.h"
#include "area.h"
#include "aop.h"
#include "mercator.h"

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

/* Where the network's tile cache lives. The original's was /t
 * (original/tilecache.cpp CACHE_DIR); dotted and hidden since 0023, as
 * the other files this writes, and an existing /t is renamed to it so
 * its tiles are kept. */
#define CACHE_SUBDIR    "/.aimless.tiles"
#define CACHE_OLD       "/t"

/* The render task. src: original/mapengine.cpp's worker, pinned to
 * core 1 so a slow network fetch never holds up the screen or GNSS on
 * core 0. Its stack has the TLS handshake on it: mbedTLS's is several KB.
 * src: chosen, with headroom; the high-water mark is logged. */
#define RENDER_CORE     (1)
#define RENDER_PRIO     (4)
#define RENDER_STACK    (16384)

/* src: chosen. How often tiles that failed are tried again. */
#define REDO_ERRORS_US  (30 * 1000000LL)
/* How long a place block that would not read waits to be tried again.
 * src: original/mapengine.cpp ensure_place_blocks(), 20 s. */
#define PLACES_RETRY_US (20 * 1000000LL)

/* The backlight by daylight (0015). Day is what it always was here; the
 * other two are the original's levels on its 0-255 scale as percent.
 * src: original/tab5_map.cpp BRIGHT_NIGHT 60 and BRIGHT_DUSK 140, both
 * judgements there (original/PROVENANCE.md): 60/255 and 140/255. */
#define BRIGHT_NIGHT_PCT (24)
#define BRIGHT_DUSK_PCT  (55)
/* Either side of sunrise and sunset, the dusk step.
 * src: original/tab5_map.cpp DUSK_HALFWIDTH_MIN, civil twilight's rough
 * length at mid latitudes. */
#define DUSK_HALF_MIN    (30.0)

/* A touch this long after the screen comes up asks for Wi-Fi setup.
 * src: original/tab5_map.cpp wantsSetup(), two seconds. */
#define SETUP_WINDOW_US (2 * 1000000LL)
/* src: original/tab5_map.cpp wantsSetup(): it polled every 20 ms. */
#define SETUP_POLL_MS   (20)
/* How long the radio may take to come up for the portal before setup
 * gives up. src: chosen; wifi_start() is about two seconds (wifi.h). */
#define SETUP_RADIO_US  (30 * 1000000LL)
/* How long the result stays in the box after setup ends. src: chosen. */
#define SETUP_NOTE_US   (8 * 1000000LL)
/* The setup box, along the bottom of the map. src: chosen, three lines
 * of TEXT_SCALE text with room between. */
#define SETUP_MARGIN    (40)
#define SETUP_PAD       (14)
#define SETUP_LINE      (GFX_GLYPH_H(TEXT_SCALE) + 10)
#define SETUP_H         (SETUP_PAD * 2 + SETUP_LINE * 3)
#define COL_SETUP_BG    RGB(20, 20, 20)
#define COL_SETUP_EDGE  RGB(240, 180, 60)

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
/* The palette this loop wants, and the one the render task has drawn
 * with. The style is global and the render task is what uses it, so the
 * render task changes it, between tiles (0015). */
static volatile bool s_want_dark;
static bool s_dark;

/* The controls (0016). This loop's own; nothing else reads them. */
static ui_theme_t  s_theme;         /* the palette override */
static ui_bright_t s_bright;        /* the backlight override */
static bool        s_sun_dark;      /* what the sun alone says, for the panel */
static int         s_backlight = -1;    /* percent in force; -1 to reapply */
static bool        s_panel;         /* the settings panel is open */
static bool        s_screen_off;
/* Names on the map (0029). src: original/mapengine.cpp g_labels_on: on
 * until turned off, and not kept across a restart, as the other rows. */
static bool        s_labels = true;
/* The parked dim (0031): when the screen was last touched, in ms, and
 * whether the backlight is dimmed for it now. */
static uint32_t    s_touch_ms;
static bool        s_idle_dim;

/*
 * Place names (0030). Two indexes, the z12 block's localities and
 * neighbourhoods and the z6 block's regions and countries, each with a
 * spare the render task fills and swaps in under s_lock, as the
 * original's g_place_idx and w_place_idx: the status line reads one
 * while the other is built. The position they are for, published by
 * the main loop under s_lock. [0] fine, [1] coarse.
 */
static places_index_t *s_pidx[2], *s_pidx_w[2];
static bool        s_pidx_ok[2];
static tile_id_t   s_pidx_have[2];
static int64_t     s_pidx_retry[2];
static bool        s_place_at;
static double      s_place_wx, s_place_wy;
static places_t    s_places;
/* Pan: the view follows an anchor, which is the marker until a pan moves
 * it (original/mapengine.cpp g_anchor_wx). In tiles at VIEW_ZOOM. */
static bool        s_panning;
static double      s_anchor_x, s_anchor_y;
static bool        s_mark_ok;       /* a measured position, to draw */
static double      s_mark_x, s_mark_y;

/* The area cache (0026): the walk, under s_lock, stepped by the render
 * task between the tiles the screen wants; and the button's confirm. */
static area_t      s_area;
static int64_t     s_area_armed_until;

/* Saved points (0017): the list, the panel and its page. */
static wp_list_t   s_wp;
static bool        s_pins;          /* the panel is open */
static int         s_pins_scroll;

/*
 * Wi-Fi setup, as this loop moves through it. Read by the network's
 * hooks from its own task, so a single word.
 *
 *   SETUP_NONE     nothing to do, or done
 *   SETUP_IMPORT   reading M5Launcher's networks; the portal waits on it
 *   SETUP_WANT     the radio is wanted for the portal and coming up
 *   SETUP_ACTIVE   the portal is running
 */
typedef enum { SETUP_NONE = 0, SETUP_IMPORT, SETUP_WANT, SETUP_ACTIVE } setup_t;
static volatile setup_t s_setup;
static int64_t s_setup_since;       /* when WANT began, for SETUP_RADIO_US */
/* What happened, shown in the box for SETUP_NOTE_US after setup ends. */
static char    s_setup_note[96];
static int64_t s_setup_note_until;

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

/* NMEA sentences a minute from the receiver, over the last ten seconds;
 * -1 until there have been ten (0024). A count since boot only ever
 * grows and says nothing at a glance; a rate says the receiver is
 * talking, and how much -- about 800 a minute is normal here, 0 is a
 * receiver that is not. */
static int s_sent_per_min = -1;

static void sentence_rate(void)
{
    static int64_t at;
    static uint32_t was;
    const int64_t now = esp_timer_get_time();
    const uint32_t n = gnss_sentences();
    if (!at) { at = now; was = n; return; }
    if (now - at < 10000000) return;
    s_sent_per_min = (int)((uint64_t)(n - was) * 60000000ULL / (uint64_t)(now - at));
    at = now;
    was = n;
}

static void draw_status(const gnss_fix_t *fix, int pending)
{
    /* With a target, where it is goes first: the rest of the line is
     * longer than the screen and is cut at the right (0017). */
    /* 0030: where you are in words leads, as the original's: the one
     * part of the bar worth reading at a glance. */
    char nav[64] = "";
    /* Static: with the place, past CLAUDE.md's few hundred bytes of stack. */
    static char line[384], place[4 * MAPLABEL_TEXT_MAX + 8];
    if (gnss_coarse(fix)) wp_target_text(&s_wp, fix->lat, fix->lon, nav, sizeof(nav));
    if (!gnss_coarse(fix) || !places_text(&s_places, place, sizeof(place))) place[0] = '\0';
    if (gnss_coarse(fix)) {
        snprintf(line, sizeof(line), "%s%s%s%s%.5f %c  %.5f %c   z%d   %d sats   HDOP %.1f   %.0f km/h   %.6sZ   %s%s",
                 place, place[0] ? "   " : "", nav, nav[0] ? "   " : "",
                 fabs(fix->lat), fix->lat < 0 ? 'S' : 'N',
                 fabs(fix->lon), fix->lon < 0 ? 'W' : 'E',
                 VIEW_ZOOM, fix->sats, fix->hdop, fix->speed_kmh, fix->utc,
                 s_online ? "online" : "offline", pending ? "   drawing" : "");
    } else {
        char rate[32];   /* GCC sizes %d for any int: 25 with the words */
        if (s_sent_per_min < 0) snprintf(rate, sizeof(rate), "listening");
        else snprintf(rate, sizeof(rate), "%d sentences/min", s_sent_per_min);
        snprintf(line, sizeof(line), "waiting for a fix   %s   %d sats in view   %s%s",
                 rate,
                 fix->cons[0].visible + fix->cons[1].visible + fix->cons[2].visible +
                 fix->cons[3].visible, s_online ? "online" : "offline",
                 pending ? "   drawing" : "");
    }
    gfx_fill_rect(0, 0, gfx_w(), STATUS_H, COL_STATUS_BG);
    gfx_draw_text(10, (STATUS_H - GFX_GLYPH_H(TEXT_SCALE)) / 2, line, TEXT_SCALE,
                  gfx_w() - 20, gnss_coarse(fix) ? COL_STATUS_FG : COL_WAIT);
}

/* ---- the world, at boot (0020) ---- */

/* Tile z0/0/0, fetched when the firmware was built (tools/
 * fetch_worldtile.py, main/CMakeLists.txt) and embedded; empty when the
 * build could not fetch it. */
extern const uint8_t world_z0_start[] asm("_binary_world_z0_mvt_gz_start");
extern const uint8_t world_z0_end[]   asm("_binary_world_z0_mvt_gz_end");

/* The world drawn, in a buffer of its own: on the screen from boot until
 * the map has something of its own to show where it is placed -- the
 * last known position's tiles, or the fix's -- then freed (0021). NULL
 * once freed, or if there is no world. */
static uint16_t *s_world;

static void world_draw(void)
{
    const size_t len = (size_t)(world_z0_end - world_z0_start);
    if (len == 0) {
        ESP_LOGI(TAG, "world: none embedded (the build could not fetch it)");
        return;
    }
    uint16_t *px = heap_caps_malloc((size_t)SUBTILE_PX * SUBTILE_PX * sizeof(uint16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!px) {
        ESP_LOGW(TAG, "world: no PSRAM for it");
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    const tile_state_t t = worldtile_draw(&s_render, world_z0_start, len, px);
    if (t != TILE_READY) {
        ESP_LOGW(TAG, "world: %u bytes embedded, but it would not draw", (unsigned)len);
        heap_caps_free(px);
        return;
    }
    s_world = px;
    ESP_LOGI(TAG, "world: %u bytes, %u inflated, drawn in %u ms", (unsigned)len,
             (unsigned)s_render.last_inflated,
             (unsigned)((esp_timer_get_time() - t0) / 1000));
}

/* A boot message: over the world when there is one, with a band behind
 * the words so they read over land and sea alike; on black otherwise. */
static void draw_message(const char *a, const char *b)
{
    if (s_world) {
        worldtile_compose(s_world, SUBTILE_PX, gfx_fb(), gfx_w(), gfx_h(), gfx_w());
        gfx_fill_rect(0, gfx_h() / 2 - 60, gfx_w(), 110, COL_STATUS_BG);
    } else {
        gfx_fill_rect(0, 0, gfx_w(), gfx_h(), COL_STATUS_BG);
    }
    gfx_draw_text(40, gfx_h() / 2 - 40, a, 3, gfx_w() - 80, COL_STATUS_FG);
    if (b) gfx_draw_text(40, gfx_h() / 2 + 10, b, 2, gfx_w() - 80, COL_WAIT);
    gfx_blit(0, gfx_h());
}

/* ---- the setup box ---- */

static void setup_note(const char *msg)
{
    snprintf(s_setup_note, sizeof(s_setup_note), "%s", msg);
    s_setup_note_until = esp_timer_get_time() + SETUP_NOTE_US;
    ESP_LOGI(TAG, "setup: %s", msg);
}

static void setup_box_rect(int *x, int *y, int *w, int *h)
{
    *x = SETUP_MARGIN;
    *w = gfx_w() - 2 * SETUP_MARGIN;
    *h = SETUP_H;
    /* Above the button row's touch zone (0016), not over it. */
    *y = ui_map_bottom(gfx_h()) - SETUP_H - 10;
}

/* What the box says, three lines; false when there is no box. The SSIDs
 * are the portal's copies, never borrowed (portal.h). */
static bool setup_lines(char a[96], char b[128], char c[96])
{
    a[0] = b[0] = c[0] = '\0';
    const setup_t st = s_setup;
    if (st == SETUP_IMPORT) {
        snprintf(a, 96, "Wi-Fi: reading M5Launcher's saved networks");
        return true;
    }
    if (st == SETUP_WANT) {
        snprintf(a, 96, "Wi-Fi setup: starting the radio");
        return true;
    }
    if (st == SETUP_ACTIVE) {
        portal_state_t ps;
        portal_state(&ps);
        switch (ps.status) {
        case PORTAL_OFF:
        case PORTAL_STARTING:
            snprintf(a, 96, "Wi-Fi setup: looking for networks");
            break;
        case PORTAL_TRYING:
            snprintf(a, 96, "Wi-Fi setup: trying %s", ps.last_ssid);
            snprintf(b, 128, "This takes up to fifteen seconds.");
            break;
        case PORTAL_SAVED:
            snprintf(a, 96, "Wi-Fi setup: saved %s", ps.last_ssid);
            break;
        default:
            snprintf(a, 96, "Wi-Fi setup: on a phone, join the network %s", ps.ap_ssid);
            if (ps.status == PORTAL_FAILED)
                snprintf(b, 128, "That did not work for %s. Try again on the phone.   %u:%02u left",
                         ps.last_ssid, ps.seconds_left / 60u, ps.seconds_left % 60u);
            else
                snprintf(b, 128, "Open http://%s/ if no page appears.   %u phone%s joined   %u:%02u left",
                         ps.url_ip, ps.clients, ps.clients == 1 ? "" : "s",
                         ps.seconds_left / 60u, ps.seconds_left % 60u);
            break;
        }
        snprintf(c, 96, "Tap here to close Wi-Fi setup");
        return true;
    }
    if (s_setup_note[0] && esp_timer_get_time() < s_setup_note_until) {
        snprintf(a, 96, "%s", s_setup_note);
        return true;
    }
    return false;
}

static void draw_setup(void)
{
    /* Statics: 320 bytes is past CLAUDE.md's few hundred on a stack. */
    static char a[96], b[128], c[96];
    if (!setup_lines(a, b, c)) return;
    int x, y, w, h;
    setup_box_rect(&x, &y, &w, &h);
    gfx_fill_rect(x, y, w, h, COL_SETUP_EDGE);
    gfx_fill_rect(x + 2, y + 2, w - 4, h - 4, COL_SETUP_BG);
    const int tx = x + SETUP_PAD, tw = w - 2 * SETUP_PAD;
    gfx_draw_text(tx, y + SETUP_PAD, a, TEXT_SCALE, tw, COL_STATUS_FG);
    if (b[0]) gfx_draw_text(tx, y + SETUP_PAD + SETUP_LINE, b, TEXT_SCALE, tw, COL_STATUS_FG);
    if (c[0]) gfx_draw_text(tx, y + SETUP_PAD + 2 * SETUP_LINE, c, TEXT_SCALE, tw, COL_WAIT);
}

/* ---- saved points (0017) ---- */

/* The original's WP_PATH was /waypoints.bin. Not .bin here (0022):
 * M5Launcher lists every .bin on the card as firmware to install, and
 * this runs under it. And dotted and FAT-hidden (0023), as defeatist's
 * .defeatist.dat: an undotted file in the root is the first thing anyone
 * sees on plugging the card into a computer. Contents unchanged; the
 * older names are renamed to this on first read (file_adopt()). */
#define WP_PATH     STORAGE_SD_MOUNT "/.aimless.waypoints.dat"
#define WP_TMP      STORAGE_SD_MOUNT "/.aimless.waypoints.tmp"
#define WP_OLD      STORAGE_SD_MOUNT "/waypoints.bin"   /* the original's */
#define WP_0022     STORAGE_SD_MOUNT "/waypoints.dat"   /* 0022's */

/* If `path` is not on the card and an older name `old` is, rename the
 * one to the other -- same bytes, a name M5Launcher will not offer to
 * flash -- and hide it (0022, 0023). A directory is adopted the same
 * way. */
static void file_adopt(const char *old, const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0 || stat(old, &st) != 0) return;
    if (rename(old, path) == 0) {
        storage_mark_hidden(path);
        ESP_LOGI(TAG, "renamed %s to %s", old, path);
    } else {
        ESP_LOGW(TAG, "could not rename %s to %s", old, path);
    }
}

static uint8_t s_wp_file[WP_FILE_MAX];

static void wp_read(void)
{
    wp_init(&s_wp);
    file_adopt(WP_OLD, WP_PATH);
    file_adopt(WP_0022, WP_PATH);
    FILE *f = fopen(WP_PATH, "rb");
    if (!f) return;
    const size_t n = fread(s_wp_file, 1, sizeof(s_wp_file), f);
    fclose(f);
    ESP_LOGI(TAG, "saved points: %d", wp_load(&s_wp, s_wp_file, n));
}

/* Written whole, to a second file renamed over the first, so a power cut
 * mid-write leaves the old list rather than half a new one. FatFs will
 * not rename over a file, so the old one goes first; a cut between the
 * two leaves the new list in waypoints.tmp, which is not lost, only not
 * read. */
static void wp_write(void)
{
    const size_t n = wp_save(&s_wp, s_wp_file, sizeof(s_wp_file));
    FILE *f = fopen(WP_TMP, "wb");
    if (!f) { ESP_LOGW(TAG, "saved points: cannot write the card"); return; }
    const bool ok = fwrite(s_wp_file, 1, n, f) == n;
    if (fclose(f) != 0 || !ok) { ESP_LOGW(TAG, "saved points: write failed"); return; }
    remove(WP_PATH);
    if (rename(WP_TMP, WP_PATH) != 0) {
        ESP_LOGW(TAG, "saved points: rename failed");
        return;
    }
    /* After the rename: the attribute goes with the name (storage.h). */
    storage_mark_hidden(WP_PATH);
    ESP_LOGI(TAG, "saved points: wrote %d", s_wp.n);
}

/* Seconds since 1970 from RMC's date and time, or the clock, for the
 * name a point gets; 0 when neither is known. src: the days-from-civil
 * algorithm, Howard Hinnant, "chrono-Compatible Low-Level Date
 * Algorithms". */
static int64_t utc_now(const gnss_fix_t *fix)
{
    int y, m, d;
    double min;
    if (fix->status == 'A' && sun_from_nmea(fix->date, fix->utc, &y, &m, &d, &min)) {
        y -= m <= 2;
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const int64_t yoe = y - era * 400;
        const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return (era * 146097 + doe - 719468) * 86400 + (int64_t)(min * 60.0);
    }
    return wifi_ntp_synced() ? (int64_t)time(NULL) : 0;
}

/* ---- the last known position (0021) ---- */

/* The original's LASTFIX_PATH was /lastfix.bin; this name for the
 * reasons WP_PATH gives (0022, 0023). */
#define LASTFIX_PATH    STORAGE_SD_MOUNT "/.aimless.lastfix.dat"
#define LASTFIX_OLD     STORAGE_SD_MOUNT "/lastfix.bin"     /* the original's */
#define LASTFIX_0022    STORAGE_SD_MOUNT "/lastfix.dat"     /* 0022's */
/* src: original/tab5_map.cpp: written on a good fix at most every ten
 * minutes -- a boot position does not need to be fresher, and the card
 * does not need the writes. */
#define LASTFIX_EVERY_US (600 * 1000000LL)

static bool lastfix_read(double *lat, double *lon)
{
    file_adopt(LASTFIX_OLD, LASTFIX_PATH);
    file_adopt(LASTFIX_0022, LASTFIX_PATH);
    FILE *f = fopen(LASTFIX_PATH, "rb");
    if (!f) return false;
    uint8_t b[LASTFIX_BYTES];
    const size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    return lastfix_decode(b, n, lat, lon);
}

static void lastfix_keep(const gnss_fix_t *fix)
{
    static int64_t last;
    const int64_t now = esp_timer_get_time();
    if (!gnss_fine(fix) || (last && now - last < LASTFIX_EVERY_US)) return;
    last = now;
    uint8_t b[LASTFIX_BYTES];
    lastfix_encode(fix->lat, fix->lon, utc_now(fix), b);
    FILE *f = fopen(LASTFIX_PATH, "wb");
    if (!f) return;
    const bool ok = fwrite(b, 1, sizeof(b), f) == sizeof(b);
    if (fclose(f) != 0 || !ok) {
        ESP_LOGW(TAG, "last position: write failed");
        return;
    }
    /* Every time: FatFs resets a file's attributes when it is opened for
     * writing over. */
    storage_mark_hidden(LASTFIX_PATH);
}

/* ---- AssistNow Autonomous (0028) ---- */

/* The original's AOP_PATH was /aopdb.bin; this name for the reasons
 * WP_PATH gives (0022, 0023). */
#define AOP_PATH    STORAGE_SD_MOUNT "/.aimless.aopdb.dat"
#define AOP_TMP     STORAGE_SD_MOUNT "/.aimless.aopdb.tmp"
#define AOP_OLD     STORAGE_SD_MOUNT "/aopdb.bin"       /* the original's */
/* src: original aopSaveTask(), 8192 with Arduino's Print in it; the
 * buffers here are on the heap and this calls stdio and the UART. */
#define AOP_STACK   (4096)

static volatile bool s_aop_busy;
static volatile bool s_aop_pushed;      /* a database went in this boot */
static volatile int  s_aop_age_h = -1;  /* its age, whole hours; -1 unknown */
static uint32_t      s_aop_saved_ms;
static int64_t       s_aop_utc;         /* the time to stamp the next save */

/* Push the saved database back, if it is fresh enough
 * (original aopRestore()). */
static void aop_restore(void)
{
    file_adopt(AOP_OLD, AOP_PATH);
    FILE *f = fopen(AOP_PATH, "rb");
    if (!f) { ESP_LOGI(TAG, "aop: no saved database"); return; }
    uint8_t head[AOP_HEAD_BYTES];
    uint32_t bytes;
    int64_t written;
    if (fread(head, 1, sizeof(head), f) != sizeof(head) ||
        !aop_head_decode(head, sizeof(head), &bytes, &written)) {
        ESP_LOGW(TAG, "aop: saved database not recognised; not using it");
        fclose(f);
        return;
    }
    /* The clock is rarely set this early -- no fix, no SNTP yet -- so
     * this is usually "age unknown", and pushed anyway, as the original
     * did: the receiver checks what it is given. */
    double age_h;
    const aop_age_t age = aop_age(written, wifi_ntp_synced() ? (int64_t)time(NULL) : 0, &age_h);
    if (age == AOP_STALE) {
        ESP_LOGI(TAG, "aop: saved database is %.1f h old, past %d h; not using it",
                 age_h, AOP_MAX_AGE_H);
        fclose(f);
        return;
    }
    uint8_t *buf = malloc(bytes);
    const bool ok = buf && fread(buf, 1, bytes, f) == bytes;
    fclose(f);
    if (ok) {
        if (age == AOP_FRESH)
            ESP_LOGI(TAG, "aop: saved database %.1f h old, %u bytes", age_h, (unsigned)bytes);
        else
            ESP_LOGI(TAG, "aop: saved database %u bytes, age unknown", (unsigned)bytes);
        if (gnss_dbd_write(buf, bytes)) {
            s_aop_age_h = age == AOP_FRESH ? (int)age_h : -1;
            s_aop_pushed = true;
        }
    } else {
        ESP_LOGW(TAG, "aop: saved database unreadable");
    }
    free(buf);
}

/* At boot, on its own task, so the receiver's 1.5 s to answer and the
 * push do not hold up the screen. In this order deliberately (original
 * setup()): ack-aiding is set with AOP and the push wants it, and
 * assistance is worth most before the search has got far. */
static void aop_boot_task(void *arg)
{
    (void)arg;
    /* src: original setup(): let the module finish talking after reset. */
    vTaskDelay(pdMS_TO_TICKS(200));
    gnss_enable_aop();
    aop_restore();
    vTaskDelete(NULL);
}

/* Poll the database and write it, to a second file renamed over the
 * first so a cut write cannot leave half a database that looks whole
 * (original aopSave()). On its own task: the poll can take 8 s. */
static void aop_save_task(void *arg)
{
    (void)arg;
    uint8_t *buf = malloc(AOP_CAP);
    const size_t n = buf ? gnss_dbd_read(buf, AOP_CAP) : 0;
    bool ok = false;
    if (n) {
        uint8_t head[AOP_HEAD_BYTES];
        aop_head_encode((uint32_t)n, s_aop_utc, head);
        FILE *f = fopen(AOP_TMP, "wb");
        if (f) {
            ok = fwrite(head, 1, sizeof(head), f) == sizeof(head) && fwrite(buf, 1, n, f) == n;
            if (fclose(f) != 0) ok = false;
            if (ok) {
                remove(AOP_PATH);       /* FatFs will not rename over a file */
                ok = rename(AOP_TMP, AOP_PATH) == 0;
                if (ok) storage_mark_hidden(AOP_PATH);
            } else {
                remove(AOP_TMP);
            }
        }
        ESP_LOGI(TAG, "aop: %s database, %u bytes", ok ? "saved" : "could not save", (unsigned)n);
    }
    free(buf);
    s_aop_busy = false;
    vTaskDelete(NULL);
}

static void aop_keep(const gnss_fix_t *fix)
{
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!aop_save_due(gnss_fine(fix), s_aop_busy, now, s_aop_saved_ms)) return;
    /* Stamped before the attempt, so a receiver that never answers is
     * asked again in thirty minutes, not on the next turn of the loop. */
    s_aop_saved_ms = now ? now : 1;
    s_aop_utc = utc_now(fix);
    s_aop_busy = true;
    /* src: original aopMaintain(): low priority on the reader's core; the
     * reader must keep draining the UART for the poll to see anything. */
    if (xTaskCreatePinnedToCore(aop_save_task, "aopsave", AOP_STACK, NULL, 1, NULL, 0) != pdPASS) {
        s_aop_busy = false;
        ESP_LOGW(TAG, "aop: could not start the save");
    }
}

/* Time to first fix from when the receiver was started, with what
 * assistance went in -- a TTFF means nothing without knowing which case
 * produced it (original ttffReport()). */
static void ttff_report(const gnss_fix_t *fix)
{
    static bool coarse, fine;
    if (fine) return;
    const uint32_t t0 = gnss_start_ms();
    char how[40];
    if (!s_aop_pushed) snprintf(how, sizeof(how), "no assistance");
    else if (s_aop_age_h < 0) snprintf(how, sizeof(how), "assisted, age unknown");
    else snprintf(how, sizeof(how), "assisted, %d h old", (int)s_aop_age_h);
    if (!coarse && gnss_coarse(fix)) {
        coarse = true;
        ESP_LOGI(TAG, "first fix %.1f s after the receiver started (%s), %d sats",
                 (double)(int32_t)(gnss_first_coarse_ms() - t0) / 1000.0, how, fix->sats);
    }
    if (coarse && gnss_fine(fix)) {
        fine = true;
        ESP_LOGI(TAG, "3D fix, HDOP %.1f, %.1f s after the receiver started (%s)", fix->hdop,
                 (double)(int32_t)(gnss_first_fine_ms() - t0) / 1000.0, how);
    }
}

/* ---- the button row and the settings panel (0016) ---- */

/* src: original/tab5_map.cpp drawFooter() and setRowText()'s colours. */
#define COL_BTN         RGB(70, 70, 70)
#define COL_BTN_LIT     RGB(150, 60, 30)    /* recentre: noticed across a dashboard */
#define COL_BTN_EDGE    RGB(255, 255, 255)
#define COL_PANEL_BG    RGB(20, 20, 26)
#define COL_ROW_BG      RGB(45, 45, 55)
#define COL_CHIP_AUTO   RGB(60, 90, 60)     /* at its automatic default */
#define COL_CHIP_SET    RGB(60, 80, 110)    /* held by hand */
#define COL_CHIP_NET    RGB(40, 70, 150)
#define COL_NOTE        RGB(190, 190, 190)
#define COL_DIM         RGB(128, 128, 128)

/* A box: a white edge two pixels wide round a fill. The original's were
 * rounded; gfx has no rounded rectangle and this is not the place to
 * add one. */
static void box(int x, int y, int w, int h, uint16_t fill, uint16_t edge)
{
    gfx_fill_rect(x, y, w, h, edge);
    gfx_fill_rect(x + 2, y + 2, w - 4, h - 4, fill);
}

static void text_centred(int cx, int cy, const char *s, int max_w, uint16_t c)
{
    int tw = gfx_text_w(s, TEXT_SCALE);
    if (tw > max_w) tw = max_w;
    gfx_draw_text(cx - tw / 2, cy - GFX_GLYPH_H(TEXT_SCALE) / 2, s, TEXT_SCALE, max_w, c);
}

/* src: original drawFooter(): green once held, orange while armed. */
#define COL_CACHE_HELD  RGB(30, 90, 50)
#define COL_CACHE_ARMED RGB(255, 165, 0)
/* src: original handleTouch(): a second tap within 5 s starts it. */
#define AREA_CONFIRM_US (5 * 1000000LL)

/* Where the square would be: the grid's middle tile, as the original's
 * prefetch_centre() -- the grid rather than the fix, so it works if the
 * fix has dropped for a moment. False before the grid is placed. */
static bool area_centre(int32_t *cx, int32_t *cy, double *lat)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool placed = s_view.grid.initialised;
    const tile_id_t o = s_view.grid.origin;
    xSemaphoreGive(s_lock);
    if (!placed) return false;
    const int32_t n = 1 << VIEW_ZOOM;
    *cx = ((o.x + GRID_N / 2) % n + n) % n;
    *cy = o.y + GRID_N / 2;
    double lon;
    const merc_pt_t mid = { *cx + 0.5, *cy + 0.5, VIEW_ZOOM };
    merc_to_ll(mid, lat, &lon);
    return true;
}

/* For the button: busy and how far, held offline already, and how wide
 * the square is. "Held" asks every archive's header about 250 tiles, so
 * it is memoised for 2 s, as the original's held_memo. */
static void area_state(bool *busy, int *pct, bool *held, double *km)
{
    static int64_t memo_at;
    static bool memo_held;
    static double memo_km = 27.0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *busy = s_area.active;
    *pct = area_progress(&s_area);
    xSemaphoreGive(s_lock);
    const int64_t now = esp_timer_get_time();
    if (!*busy && (!memo_at || now - memo_at > 2000000)) {
        int32_t cx, cy;
        double lat;
        memo_held = false;
        if (area_centre(&cx, &cy, &lat)) {
            memo_held = area_pending(&s_set, VIEW_ZOOM, cx, cy) == 0;
            /* The square's width at this latitude; the original used a
             * fixed cos 42 (0.74). src: the equator, 40075 km. */
            memo_km = (2 * AREA_RADIUS + 1) * 40075.0 * cos(lat * M_PI / 180.0) /
                      (double)(1 << VIEW_ZOOM);
        }
        memo_at = now;
    }
    *held = !*busy && memo_held;
    *km = memo_km;
}

static void setup_want(bool apply);    /* below, with the rest of setup */

/* The cache button, as the original's handleTouch(). */
static void area_tap(void)
{
    bool busy, held;
    int pct;
    double km;
    area_state(&busy, &pct, &held, &km);
    if (busy) return;
    if (held) {
        ESP_LOGI(TAG, "area: already offline on the card");
        return;
    }
    if (!s_online) {
        /* Checked after "held": with the area on the card there is
         * nothing to download, and asking for a network to get it would
         * be solving a problem nobody has. */
        if (s_setup == SETUP_NONE) {
            ESP_LOGI(TAG, "setup: asked for by the cache button");
            setup_want(true);
        }
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now >= s_area_armed_until) {
        s_area_armed_until = now + AREA_CONFIRM_US;
        ESP_LOGI(TAG, "area: tap again within 5 s to fetch %.0f km around here", km);
        return;
    }
    s_area_armed_until = 0;
    int32_t cx, cy;
    double lat;
    if (!area_centre(&cx, &cy, &lat)) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    area_start(&s_area, VIEW_ZOOM, cx, cy);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "area: %d tiles around %u/%d/%d", area_total(), VIEW_ZOOM, cx, cy);
}

static void draw_footer(void)
{
    for (int i = 0; i < UI_BTN_COUNT; i++) {
        if (!ui_button_present(i)) continue;
        ui_rect_t r;
        ui_button_rect(i, gfx_w(), gfx_h(), &r);
        const char *label = "";
        uint16_t bg = COL_BTN;
        switch (i) {
        case UI_BTN_HOME:
            /* src: original drawFooter(): lit only while the view is
             * somewhere the device is not. */
            label = s_panning ? "recentre" : "centred";
            bg = s_panning ? COL_BTN_LIT : COL_BTN;
            break;
        case UI_BTN_PINS: {
            /* src: original drawFooter(): the button names the target
             * while guiding, so the one control that ends guidance also
             * says it is on. */
            static char pl[40];
            const int t = wp_target(&s_wp);
            if (t >= 0) snprintf(pl, sizeof(pl), "to %s", s_wp.p[t].name);
            else snprintf(pl, sizeof(pl), "points (%d)", s_wp.n);
            label = pl;
            bg = t >= 0 ? COL_BTN_LIT : COL_BTN;
            break;
        }
        case UI_BTN_CACHE: {
            /* src: original drawFooter()'s cache button: its progress while
             * busy, "offline" once the cards hold the whole square, a
             * confirm armed by the first tap, "wifi set" with no network,
             * and otherwise how wide the square is. */
            static char cl[40];
            int pct;
            bool busy, held;
            double km;
            area_state(&busy, &pct, &held, &km);
            const bool armed = esp_timer_get_time() < s_area_armed_until;
            if (busy)        snprintf(cl, sizeof(cl), "cache %d%%", pct);
            else if (held)   snprintf(cl, sizeof(cl), "offline");
            else if (armed)  snprintf(cl, sizeof(cl), "confirm?");
            else if (!s_online) snprintf(cl, sizeof(cl), "wifi set");
            else             snprintf(cl, sizeof(cl), "cache %d km", (int)lround(km));
            label = cl;
            bg = busy ? COL_BTN : held ? COL_CACHE_HELD : armed ? COL_CACHE_ARMED
               : s_online ? COL_CHIP_NET : COL_BTN;
            break;
        }
        case UI_BTN_SET:   label = "settings";   break;
        case UI_BTN_SLEEP: label = "screen off"; break;
        default: break;
        }
        box(r.x, r.y, r.w, r.h, bg, COL_BTN_EDGE);
        text_centred(r.x + r.w / 2, r.y + r.h / 2, label, r.w - 12, COL_STATUS_FG);
    }
}

/* One row's text, as the original's setRowText(): a name, the answer as a
 * chip, and a line saying what the answer means. */
static void row_text(int i, char *name, char *value, char *note, uint16_t *chip)
{
    switch (i) {
    case UI_SET_THEME:
        snprintf(name, 40, "palette");
        snprintf(value, 40, "%s", s_theme == UI_THEME_DAY ? "day"
                                : s_theme == UI_THEME_NIGHT ? "night" : "auto");
        if (s_theme == UI_THEME_AUTO) {
            snprintf(note, 80, "following the sun - %s right now", s_sun_dark ? "night" : "day");
            *chip = COL_CHIP_AUTO;
        } else {
            snprintf(note, 80, "held %s until you change it",
                     s_theme == UI_THEME_DAY ? "light" : "dark");
            *chip = COL_CHIP_SET;
        }
        break;
    case UI_SET_BRIGHT:
        snprintf(name, 40, "brightness");
        snprintf(value, 40, "%s", s_bright == UI_BRIGHT_AUTO ? "auto"
                                : s_bright == UI_BRIGHT_LOW ? "low"
                                : s_bright == UI_BRIGHT_MED ? "medium" : "high");
        snprintf(note, 80, "backlight at %d%%%s", s_backlight < 0 ? BACKLIGHT_PCT : s_backlight,
                 s_bright == UI_BRIGHT_AUTO ? ", set by the sun" : "");
        *chip = s_bright == UI_BRIGHT_AUTO ? COL_CHIP_AUTO : COL_CHIP_SET;
        break;
    case UI_SET_LABELS:
        /* src: original/tab5_map.cpp setRowText()'s SET_LABELS, but not
         * "place names": that is the status line's readout, still to come. */
        snprintf(name, 40, "labels");
        snprintf(value, 40, "%s", s_labels ? "on" : "off");
        snprintf(note, 80, "names drawn over the map - instant either way");
        *chip = s_labels ? COL_CHIP_SET : COL_BTN;
        break;
    case UI_SET_WIFI: {
        char ssid[33];
        const bool joined = wifi_sta_ssid(ssid, sizeof(ssid));
        snprintf(name, 40, "wifi network");
        if (s_setup != SETUP_NONE) {
            snprintf(value, 40, "setting up");
            snprintf(note, 80, "the box below says what to do");
            *chip = COL_CHIP_SET;
        } else if (joined) {
            snprintf(value, 40, "%.32s", ssid);
            snprintf(note, 80, "tap to join a different network");
            *chip = COL_CHIP_NET;
        } else {
            snprintf(value, 40, "offline");
            snprintf(note, 80, "tap to open the setup portal");
            *chip = COL_BTN;
        }
        break;
    }
    default:
        name[0] = value[0] = note[0] = '\0';
        *chip = COL_BTN;
        break;
    }
}

static void draw_panel(void)
{
    if (!s_panel) return;
    ui_rect_t p;
    ui_panel_rect(gfx_w(), gfx_h(), &p);
    box(p.x, p.y, p.w, p.h, COL_PANEL_BG, COL_BTN_EDGE);
    gfx_draw_text(p.x + 18, p.y + 18, "settings", TEXT_SCALE, 200, COL_STATUS_FG);
    gfx_draw_text(p.x + 150, p.y + 20, "tap a row to change it", TEXT_SCALE, p.w - 170, COL_DIM);
    /* Statics: three strings a row are past CLAUDE.md's few hundred. */
    static char n[40], v[40], t[80];
    for (int i = 0; i < UI_SET_COUNT; i++) {
        const int ry = p.y + UI_SP_HEAD_H + i * UI_SP_ROW_H;
        uint16_t chip;
        row_text(i, n, v, t, &chip);
        gfx_fill_rect(p.x + 14, ry, p.w - 28, UI_SP_ROW_H - 8, COL_ROW_BG);
        int cw = gfx_text_w(v, TEXT_SCALE) + 28;
        if (cw < 90) cw = 90;
        const int cx = p.x + p.w - 24 - cw;
        gfx_draw_text(p.x + 30, ry + 6, n, TEXT_SCALE, cx - p.x - 40, COL_STATUS_FG);
        gfx_draw_text(p.x + 30, ry + 30, t, TEXT_SCALE, cx - p.x - 40, COL_NOTE);
        gfx_fill_rect(cx, ry + 8, cw, UI_SP_ROW_H - 24, chip);
        text_centred(cx + cw / 2, ry + 8 + (UI_SP_ROW_H - 24) / 2, v, cw - 8, COL_STATUS_FG);
    }
    box(p.x + 14, p.y + p.h - 56, 150, 44, COL_BTN, COL_BTN_EDGE);
    text_centred(p.x + 89, p.y + p.h - 34, "close", 140, COL_STATUS_FG);
}

/* src: original drawPinPanel()'s and draw_pins()' colours. */
#define COL_SAVE        RGB(40, 110, 60)
#define COL_ROW_TARGET  RGB(110, 45, 25)
#define COL_DEL         RGB(235, 120, 120)
#define COL_PIN         RGB(200, 40, 140)
#define COL_PIN_TARGET  RGB(230, 80, 40)

static void draw_pins_panel(const gnss_fix_t *fix)
{
    if (!s_pins) return;
    ui_rect_t p;
    ui_pins_rect(gfx_w(), gfx_h(), &p);
    const int rows = ui_pins_rows(gfx_w(), gfx_h());
    const bool can_save = gnss_coarse(fix) && s_wp.n < WP_MAX;
    const int target = wp_target(&s_wp);
    box(p.x, p.y, p.w, p.h, COL_PANEL_BG, COL_BTN_EDGE);
    gfx_draw_text(p.x + 18, p.y + 18, "saved points", TEXT_SCALE, 300, COL_STATUS_FG);
    gfx_fill_rect(p.x + p.w - 200, p.y + 12, 186, 44, can_save ? COL_SAVE : COL_BTN);
    text_centred(p.x + p.w - 107, p.y + 34,
                 !gnss_coarse(fix) ? "no fix" : s_wp.n >= WP_MAX ? "list full" : "save here",
                 180, can_save ? COL_STATUS_FG : COL_NOTE);

    static char line[96];
    for (int r = 0; r < rows; r++) {
        const int i = s_pins_scroll + r;
        if (i >= s_wp.n) break;
        const wp_point_t *w = &s_wp.p[i];
        const int ry = p.y + UI_PP_HEAD_H + r * UI_PP_ROW_H;
        gfx_fill_rect(p.x + 14, ry, p.w - 28, UI_PP_ROW_H - 8, i == target ? COL_ROW_TARGET : COL_ROW_BG);
        if (gnss_coarse(fix)) {
            const double m = wp_distance_m(fix->lat, fix->lon, w->lat, w->lon);
            const int b = (int)wp_bearing_deg(fix->lat, fix->lon, w->lat, w->lon);
            if (m < 1000.0) snprintf(line, sizeof(line), "%s   %d m   %03d deg", w->name, (int)m, b);
            else snprintf(line, sizeof(line), "%s   %.1f km   %03d deg", w->name, m / 1000.0, b);
        } else {
            snprintf(line, sizeof(line), "%s   %.5f %.5f", w->name, w->lat, w->lon);
        }
        const int cy = ry + (UI_PP_ROW_H - 8) / 2;
        gfx_draw_text(p.x + 30, cy - GFX_GLYPH_H(TEXT_SCALE) / 2, line, TEXT_SCALE, p.w - 150, COL_STATUS_FG);
        text_centred(p.x + p.w - 50, cy, "del", 60, COL_DEL);
    }
    if (s_wp.n == 0)
        text_centred(p.x + p.w / 2, p.y + p.h / 2, "none yet - save here drops one", p.w - 40, COL_NOTE);

    box(p.x + 14, p.y + p.h - 56, 150, 44, COL_BTN, COL_BTN_EDGE);
    text_centred(p.x + 89, p.y + p.h - 34, "close", 140, COL_STATUS_FG);
    if (target >= 0) {
        box(p.x + 178, p.y + p.h - 56, 210, 44, COL_ROW_TARGET, COL_BTN_EDGE);
        text_centred(p.x + 283, p.y + p.h - 34, "stop guiding", 200, COL_STATUS_FG);
    }
    if (s_wp.n > rows) {
        box(p.x + p.w - 154, p.y + p.h - 56, 64, 44, COL_BTN, COL_BTN_EDGE);
        text_centred(p.x + p.w - 122, p.y + p.h - 34, "up", 60, COL_STATUS_FG);
        box(p.x + p.w - 80, p.y + p.h - 56, 64, 44, COL_BTN, COL_BTN_EDGE);
        text_centred(p.x + p.w - 48, p.y + p.h - 34, "down", 60, COL_STATUS_FG);
    }
}

/* ---- labels (0029) ---- */

/* What the last compose laid out, drawn after the lock is let go. In
 * BSS: forty of them are past CLAUDE.md's few hundred bytes of stack. */
static maplabel_placed_t s_placed[MAPLABEL_ON_SCREEN];
static int               s_nplaced;

/* Under s_lock, after mapview_compose(): the READY tiles' names laid out
 * over the map band, as the original's draw_labels() did its layout. */
static void labels_layout(void)
{
    static maplabel_src_t src[GRID_COUNT];
    s_nplaced = 0;
    if (!s_labels) return;
    const int n = mapview_label_srcs(&s_view, gfx_w(), gfx_h(), src);
    s_nplaced = maplabel_layout(src, n, SUBTILE_PX, gfx_w(), STATUS_H, ui_map_bottom(gfx_h()),
                                ARK12_H, gfx_text_w, s_placed, MAPLABEL_ON_SCREEN);
}

/*
 * src: original/mapengine.cpp draw_poi_dot(), draw_one_label(),
 * label_ink(), label_halo(). The ink is the style table's own colour for
 * the point, so a label matches what the tile would have drawn; the halo
 * is near-white by day and near-black by night, the only pair that
 * separates from water, park and building fills alike, eight ways round
 * at 2 px so the diagonals of an A or a V are not left bare. A POI gets
 * a ringed dot under its name: the rasteriser does not draw one
 * (style.c, has_fill 0) so that turning labels off takes the dots too.
 * Under the saved points and the marker, as there.
 */
static void draw_labels(void)
{
    const uint16_t halo = s_dark ? RGB(0, 0, 0) : RGB(255, 255, 255);
    for (int i = 0; i < s_nplaced; i++) {
        const maplabel_placed_t *p = &s_placed[i];
        const uint16_t ink = STYLES[p->style].fill;
        if (p->style == S_POI) {
            gfx_fill_circle(p->ax, p->ay, MAPLABEL_DOT_R, halo);
            gfx_fill_circle(p->ax, p->ay, MAPLABEL_DOT_R - 1, ink);
            gfx_fill_circle(p->ax, p->ay, 2, halo);
        }
        for (int dy = -2; dy <= 2; dy += 2)
            for (int dx = -2; dx <= 2; dx += 2)
                if (dx || dy) gfx_draw_text(p->tx + dx, p->ty + dy, p->text, p->scale, p->tw, halo);
        gfx_draw_text(p->tx, p->ty, p->text, p->scale, p->tw, ink);
    }
}

/* A thick line, as a quadrilateral: gfx has no line. */
static void thick_line(double x0, double y0, double x1, double y1, double half, uint16_t c)
{
    double dx = x1 - x0, dy = y1 - y0;
    const double len = sqrt(dx * dx + dy * dy);
    if (len < 0.5) return;
    const double px = -dy / len * half, py = dx / len * half;
    const int xy[8] = { (int)lround(x0 + px), (int)lround(y0 + py), (int)lround(x1 + px), (int)lround(y1 + py),
                        (int)lround(x1 - px), (int)lround(y1 - py), (int)lround(x0 - px), (int)lround(y0 - py) };
    gfx_fill_poly(xy, 4, c);
}

/* Where a world point is in the window, which is centred on the view. */
static void to_screen(double vx, double vy, double lat, double lon, double *sx, double *sy)
{
    const merc_pt_t p = merc_from_ll(lat, lon, VIEW_ZOOM);
    *sx = gfx_w() / 2 + (p.x - vx) * SUBTILE_PX;
    *sy = gfx_h() / 2 + (p.y - vy) * SUBTILE_PX;
}

/*
 * The points on the map, as the original's draw_pins(): a teardrop whose
 * tip is the position (a circle centred there would sit half a diameter
 * off), haloed in the palette's opposite so it separates from water,
 * park and building fills alike, and the name above it. The target is
 * orange and ringed. Under the marker: the position dot is the one
 * thing that must always be findable.
 */
static void draw_pins_on_map(double vx, double vy)
{
    const int top = STATUS_H, bot = ui_map_bottom(gfx_h());
    const uint16_t halo = s_dark ? RGB(0, 0, 0) : RGB(255, 255, 255);
    const int target = wp_target(&s_wp);
    for (int i = 0; i < s_wp.n; i++) {
        double fx, fy;
        to_screen(vx, vy, s_wp.p[i].lat, s_wp.p[i].lon, &fx, &fy);
        const int sx = (int)lround(fx), sy = (int)lround(fy);
        /* The map band only: not under the status bar or the row. */
        if (sx < -60 || sx > gfx_w() + 60 || sy < top + 50 || sy > bot) continue;
        const uint16_t ink = i == target ? COL_PIN_TARGET : COL_PIN;
        gfx_fill_rect(sx - 1, sy - 16, 3, 17, halo);
        if (i == target) gfx_fill_circle(sx, sy - 22, 13, ink);
        gfx_fill_circle(sx, sy - 22, 10, halo);
        gfx_fill_circle(sx, sy - 22, 8, ink);
        gfx_fill_circle(sx, sy, 2, ink);
        const int tw = gfx_text_w(s_wp.p[i].name, TEXT_SCALE);
        const int ty = sy - 42 - GFX_GLYPH_H(TEXT_SCALE) / 2;
        for (int dy = -2; dy <= 2; dy += 2)
            for (int dx = -2; dx <= 2; dx += 2)
                if (dx || dy)
                    gfx_draw_text(sx - tw / 2 + dx, ty + dy, s_wp.p[i].name, TEXT_SCALE, tw + 4, halo);
        gfx_draw_text(sx - tw / 2, ty, s_wp.p[i].name, TEXT_SCALE, tw + 4, ink);
    }
}

/*
 * A fixed-length arrow from the marker toward the target, as the
 * original's draw_target_guide(): GUIDE_R long, haloed, starting clear of
 * the marker's ring. Nothing when standing on it -- a bearing from a metre
 * of receiver noise spins, and a spinning arrow reads as a fault.
 */
static void draw_guide(double vx, double vy, int mx, int my)
{
    const int t = wp_target(&s_wp);
    if (t < 0) return;
    double tx, ty;
    to_screen(vx, vy, s_wp.p[t].lat, s_wp.p[t].lon, &tx, &ty);
    double dx = tx - mx, dy = ty - my;
    const double len = sqrt(dx * dx + dy * dy);
    if (len < 12.0) return;
    dx /= len;
    dy /= len;
    const uint16_t halo = s_dark ? RGB(0, 0, 0) : RGB(255, 255, 255);
    const double x0 = mx + dx * 16, y0 = my + dy * 16;
    const double x1 = mx + dx * (GUIDE_R - 16), y1 = my + dy * (GUIDE_R - 16);
    const double hx = mx + dx * GUIDE_R, hy = my + dy * GUIDE_R;
    const double px = -dy, py = dx;
    const int head_halo[6] = { (int)lround(hx + dx * 3), (int)lround(hy + dy * 3),
                               (int)lround(x1 + px * 13), (int)lround(y1 + py * 13),
                               (int)lround(x1 - px * 13), (int)lround(y1 - py * 13) };
    const int head[6] = { (int)lround(hx), (int)lround(hy),
                          (int)lround(x1 + px * 10), (int)lround(y1 + py * 10),
                          (int)lround(x1 - px * 10), (int)lround(y1 - py * 10) };
    thick_line(x0, y0, x1, y1, 3.0, halo);
    gfx_fill_poly(head_halo, 3, halo);
    thick_line(x0, y0, x1, y1, 1.5, COL_PIN_TARGET);
    gfx_fill_poly(head, 3, COL_PIN_TARGET);
}

static void draw(const gnss_fix_t *fix)
{
    if (s_screen_off) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* The world until the map has a picture of its own (0021). */
    if (s_world && mapview_has_picture(&s_view)) {
        heap_caps_free(s_world);
        s_world = NULL;
        ESP_LOGI(TAG, "world: the map has a picture; backdrop freed");
    }
    if (s_world) {
        worldtile_compose(s_world, SUBTILE_PX, gfx_fb(), gfx_w(), gfx_h(), gfx_w());
        s_nplaced = 0;
    } else {
        mapview_compose(&s_view, gfx_fb(), gfx_w(), gfx_h(), gfx_w());
        labels_layout();
    }
    const int pending = mapview_pending(&s_view);
    const double vx = s_view.fx, vy = s_view.fy;
    /* Where the device is, not where the view is: a pan does not move
     * you. Each rank keeps its name with nothing near (places.h). */
    if (gnss_coarse(fix) && s_mark_ok)
        places_pick(&s_places, s_pidx_ok[0] ? s_pidx[0] : NULL, s_pidx_ok[1] ? s_pidx[1] : NULL,
                    VIEW_ZOOM, s_mark_x, s_mark_y);
    xSemaphoreGive(s_lock);
    /* The marker is where the device is, which is the window's centre
     * unless a pan has moved the view; off the screen it is not drawn.
     * No marker without a fix -- a remembered or seeded position is a
     * claim, not a placeholder (original/mapengine.cpp). */
    draw_labels();
    draw_pins_on_map(vx, vy);
    if (gnss_coarse(fix) && s_mark_ok) {
        const double ox = (s_mark_x - vx) * SUBTILE_PX, oy = (s_mark_y - vy) * SUBTILE_PX;
        if (fabs(ox) < gfx_w() && fabs(oy) < gfx_h()) {
            const int cx = gfx_w() / 2 + (int)lround(ox), cy = gfx_h() / 2 + (int)lround(oy);
            draw_guide(vx, vy, cx, cy);
            /* src: original/mapengine.cpp draw_marker(): above 3 km/h, a
             * needle along the course, 26 px from the middle of a 9 px
             * dot -- 17 px past it, here past this dot's ring. Below that
             * a parked receiver's course is noise, or empty and 0. */
            if (fix->speed_kmh > 3.0) {
                const double a = fix->course * M_PI / 180.0;
                const double len = MARKER_R + 3 + 17;
                const double ex = cx + len * sin(a), ey = cy - len * cos(a);
                thick_line(cx, cy, ex, ey, 3.0, COL_RING);
                thick_line(cx, cy, ex, ey, 1.5, gnss_fine(fix) ? COL_FINE : COL_COARSE);
            }
            gfx_fill_circle(cx, cy, MARKER_R + 3, COL_RING);
            gfx_fill_circle(cx, cy, MARKER_R, gnss_fine(fix) ? COL_FINE : COL_COARSE);
        }
    }
    draw_status(fix, pending);
    draw_footer();
    draw_setup();
    draw_panel();
    draw_pins_panel(fix);
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

/*
 * One tile of the area cache, when the screen wants nothing (0026). Only
 * with a network: offline it waits where it is rather than burn through
 * the square marking everything failed. False when there was nothing to
 * do.
 */
static bool area_step(void)
{
    if (!s_src.remote) return false;
    tile_id_t id;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool was = s_area.active;
    const bool any = was && area_next(&s_area, &id);
    const area_t a0 = s_area;
    xSemaphoreGive(s_lock);
    /* The walk only knows it is over when asked for one more tile, so the
     * summary is here, on the call that finds none (0027: it was on the
     * last tile's, which still saw the walk active, and never printed). */
    if (!any) {
        if (was)
            ESP_LOGI(TAG, "area: %d tiles done -- %d from the network, %d already cached, "
                          "%d on the card, %d empty, %d failed",
                     a0.done, a0.fetched, a0.cached, a0.offline, a0.empty, a0.failed);
        return false;
    }

    tilesrc_from_t from;
    const tile_state_t t = tilesrc_store(&s_src, &s_render, id, 0, &from);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (t == TILE_READY) {
        if (from == TILESRC_NET) s_area.fetched++;
        else if (from == TILESRC_LOCAL) s_area.offline++;
        else s_area.cached++;
    } else if (t == TILE_NODATA) {
        s_area.empty++;
    } else {
        s_area.failed++;
    }
    const area_t a = s_area;
    xSemaphoreGive(s_lock);
    s_dirty = true;     /* the button's percentage */

    if (a.done % 25 == 0)
        ESP_LOGI(TAG, "area: %d of %d", a.done, a.total);
    return true;
}

/*
 * One place block, if one is wanted and not held: original/mapengine.cpp
 * ensure_place_blocks() and load_place_block(), on the render task when
 * it has no tile to draw. Nine tiles, each from the cache, the card or
 * the network, their places layer decoded into the spare index, which
 * is swapped in if any tile read. One that did not is tried again after
 * PLACES_RETRY_US. True if a block was read.
 */
static bool places_step(void)
{
    static const uint8_t ZOOM[2] = { PLACES_FINE_Z, PLACES_COARSE_Z };
    if (!s_pidx[0]) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool at = s_place_at;
    const double wx = s_place_wx, wy = s_place_wy;
    xSemaphoreGive(s_lock);
    if (!at) return false;
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < 2; i++) {
        const tile_id_t c = places_centre(VIEW_ZOOM, wx, wy, ZOOM[i]);
        if (s_pidx_ok[i] && s_pidx_have[i].x == c.x && s_pidx_have[i].y == c.y) continue;
        if (now < s_pidx_retry[i]) continue;

        const int64_t t0 = esp_timer_get_time();
        places_index_t *idx = s_pidx_w[i];
        places_begin(idx, ZOOM[i]);
        tile_id_t block[9];
        const int n = places_block(c, block);
        int read = 0;
        for (int k = 0; k < n && !idx->full; k++) {
            uint32_t len;
            if (tilesrc_fetch(&s_src, &s_render, block[k], &len, NULL) != TILE_READY) continue;
            places_sink_t sink = { idx, block[k] };
            if (maprender_points(&s_render, len, "places", places_part, &sink) == TILE_READY) read++;
        }
        if (read) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_pidx_w[i] = s_pidx[i];
            s_pidx[i] = idx;
            s_pidx_have[i] = c;
            s_pidx_ok[i] = true;
            xSemaphoreGive(s_lock);
            s_dirty = true;
        } else {
            s_pidx_retry[i] = now + PLACES_RETRY_US;
        }
        ESP_LOGI(TAG, "places: z%u/%u/%u block, %d of %d tiles read, %u places%s, in %u ms",
                 c.z, (unsigned)c.x, (unsigned)c.y, read, n, (unsigned)idx->n,
                 idx->full ? " (index full)" : "",
                 (unsigned)((esp_timer_get_time() - t0) / 1000));
        return true;
    }
    return false;
}

static void render_task(void *arg)
{
    (void)arg;
    bool had_remote = false;
    int64_t last_redo = esp_timer_get_time();
    uint32_t tiles = 0;
    for (;;) {
        /* The palette, between tiles, so no tile is drawn half in each. */
        if (s_want_dark != s_dark) {
            s_dark = s_want_dark;
            style_init(SUBTILE_PX, s_dark ? 1 : 0);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            mapview_restyle(&s_view, style_background());
            xSemaphoreGive(s_lock);
            s_dirty = true;
            ESP_LOGI(TAG, "palette: %s", s_dark ? "night" : "day");
        }

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
        /* The overview first: until it lands the screen has nothing at
         * all, and once it has, every tile still to come shows soft
         * rather than blank. The original put the first one behind the
         * grid because its boot screen already had a picture; this has
         * none (0014). */
        tile_id_t cz_id;
        uint16_t *cz_px = NULL;
        const bool cz_took = mapview_coarse_take(&s_view, &cz_id, &cz_px);
        const bool took = !cz_took && mapview_take(&s_view, &job, &px);
        /* The names go with the pixels: this buffer's set, and only for
         * a grid tile -- the overview and the area cache want none. */
        s_render.labels = took ? mapview_labels_for(&s_view, px) : NULL;
        xSemaphoreGive(s_lock);

        if (cz_took) {
            const int64_t t0 = esp_timer_get_time();
            tilesrc_from_t from;
            maprender_resize(&s_render, COARSE_PX);
            const tile_state_t t = tilesrc_draw(&s_src, &s_render, cz_id, cz_px, 0, &from);
            maprender_resize(&s_render, SUBTILE_PX);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            mapview_coarse_commit(&s_view, cz_id, t);
            xSemaphoreGive(s_lock);
            s_dirty = true;
            ESP_LOGI(TAG, "overview %u/%u/%u in %u ms from %s: %s",
                     cz_id.z, (unsigned)cz_id.x, (unsigned)cz_id.y,
                     (unsigned)((esp_timer_get_time() - t0) / 1000), from_name(from),
                     t == TILE_READY ? "drawn" : t == TILE_NODATA ? "no data" : "FAILED");
            continue;
        }

        if (!took) {
            if (!places_step() && !area_step()) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        tilesrc_from_t from;
        const tile_state_t t = tilesrc_draw(&s_src, &s_render, job.id, px, SUBTILE_SPLIT, &from);
        s_render.labels = NULL;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        mapview_commit(&s_view, &job, t);
        xSemaphoreGive(s_lock);
        s_dirty = true;

        /* Sizes only for a tile that drew: otherwise they are whatever
         * the scratch last held (0024: a "no data" tile was logged with
         * the world tile's sizes). */
        if (t == TILE_READY)
            ESP_LOGI(TAG, "tile %u/%u/%u in %u ms from %s: drawn (%u -> %u bytes)",
                     job.id.z, (unsigned)job.id.x, (unsigned)job.id.y,
                     (unsigned)((esp_timer_get_time() - t0) / 1000), from_name(from),
                     (unsigned)s_render.last_bytes, (unsigned)s_render.last_inflated);
        else
            ESP_LOGI(TAG, "tile %u/%u/%u in %u ms from %s: %s",
                     job.id.z, (unsigned)job.id.x, (unsigned)job.id.y,
                     (unsigned)((esp_timer_get_time() - t0) / 1000), from_name(from),
                     t == TILE_NODATA ? "no data" : "FAILED");
        if ((++tiles % 16) == 0)
            ESP_LOGI(TAG, "render task stack: %u bytes never used",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

/* ---- the network ---- */

/* The radio when there is somewhere to join, or while setup wants it
 * for the portal; the cable regardless. */
static bool setup_owns_radio(void)
{
    return s_setup == SETUP_WANT || s_setup == SETUP_ACTIVE;
}
static bool want_wifi(void) { return wifistore_count() > 0 || setup_owns_radio(); }
static bool want_ntp(void)  { return true; }
/* The network's background join stays out of the way from the moment
 * setup asks for the radio, not only once the AP is up: otherwise its
 * scan and joins race the portal's own scan. */
static bool hook_portal_running(void) { return setup_owns_radio() || portal_running(); }

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
        .portal_running     = hook_portal_running,
        .portal_stop        = portal_stop,
        .usb_register_class = usbhost_register_class,
    };
    feckless_net_set_hooks(&hooks);
    wifi_init(tab5io_exp2());
    /* Before usbhost_start(), like every class on the port. */
    if (ethernet_init() != ESP_OK)
        ESP_LOGW(TAG, "no USB Ethernet this boot");
}

/* ---- day and night ---- */

/*
 * The original's rule (original/README.md "Day and night"): the palette
 * by whether the sun is up where the receiver is, and the backlight in
 * three steps, dim at night, full by day, and between for half an hour
 * either side of a crossing. Asked once a second.
 *
 * The time is the fix's, or the clock once SNTP has set it. The place is
 * the fix's, or the last one: a fix that drops to 'V' under a bridge is
 * not news about the sun, and the original's palette flickered night /
 * day / night at walking pace until it stopped treating it as such.
 * Without either, nothing changes.
 */
/* The backlight: 0 with the screen off, else `pct`, set only when it
 * changes. s_backlight -1 makes the next call set it whatever it is. */
static void backlight(int pct)
{
    if (s_screen_off) pct = 0;
    if (pct == s_backlight) return;
    lcd_backlight_set(pct);
    s_backlight = pct;
}

/* The overrides over what the sun says (0016): a held palette, and a
 * fixed backlight at one of the three automatic levels -- the original's
 * brightnessWanted(), which reused them rather than adding a scale. A
 * fixed level overrides the palette's dimming too: forcing the night
 * palette and asking for a bright screen is a coherent thing to want. */
static void apply_light(bool sun_dark, int sun_pct)
{
    s_sun_dark = sun_dark;
    const bool dark = s_theme == UI_THEME_DAY ? false
                    : s_theme == UI_THEME_NIGHT ? true : sun_dark;
    if (dark != s_want_dark) s_want_dark = dark;
    const int pct = s_bright == UI_BRIGHT_LOW  ? BRIGHT_NIGHT_PCT
                  : s_bright == UI_BRIGHT_MED  ? BRIGHT_DUSK_PCT
                  : s_bright == UI_BRIGHT_HIGH ? BACKLIGHT_PCT
                  : s_theme == UI_THEME_NIGHT  ? BRIGHT_NIGHT_PCT
                  : s_theme == UI_THEME_DAY    ? BACKLIGHT_PCT
                  : sun_pct;
    /* Parked and untouched: a step down from whatever is in force, the
     * overrides included -- the original's applyIdleDim() on
     * brightnessWanted(). */
    backlight(s_idle_dim ? motion_dim_pct(pct) : pct);
}

/* Where the sun is asked about: the fix, or the last known position
 * until there is one (0021), so a boot in the dark starts dark. */
static bool   s_sun_have;
static double s_sun_lat, s_sun_lon;

static void daylight(const gnss_fix_t *fix)
{
    static bool have_sun;
    static bool sun_dark;
    static int sun_pct = BACKLIGHT_PCT;
    if (gnss_coarse(fix)) {
        s_sun_lat = fix->lat;
        s_sun_lon = fix->lon;
        s_sun_have = true;
    }
    const double lat = s_sun_lat, lon = s_sun_lon;
    /* Without a position or a time the sun's answer is the last one, or
     * day; the overrides still apply. */
    if (!s_sun_have) { apply_light(sun_dark, sun_pct); return; }

    int y, m, d;
    double now;
    if (!(fix->status == 'A' && sun_from_nmea(fix->date, fix->utc, &y, &m, &d, &now))) {
        if (!wifi_ntp_synced()) { apply_light(sun_dark, sun_pct); return; }
        const time_t t = time(NULL);
        struct tm tm;
        gmtime_r(&t, &tm);
        y = tm.tm_year + 1900;
        m = tm.tm_mon + 1;
        d = tm.tm_mday;
        now = tm.tm_hour * 60.0 + tm.tm_min + tm.tm_sec / 60.0;
    }

    sun_day_t sd;
    sun_day(lat, lon, y, m, d, &sd);
    const bool dark = !sun_up(&sd, now);
    if (!have_sun || dark != sun_dark) {
        if (sd.kind == SUN_CROSSES)
            ESP_LOGI(TAG, "sun: rise %02d:%02dZ set %02d:%02dZ at %.3f,%.3f; now %s",
                     (int)sd.rise_min / 60, (int)sd.rise_min % 60,
                     (int)sd.set_min / 60, (int)sd.set_min % 60, lat, lon,
                     dark ? "night" : "day");
        else
            ESP_LOGI(TAG, "sun: %s all day at %.3f,%.3f",
                     sd.kind == SUN_ALWAYS_UP ? "up" : "down", lat, lon);
        sun_dark = dark;
        have_sun = true;
    }

    const double near = sun_to_crossing(&sd, now);
    sun_pct = (near >= 0 && near < DUSK_HALF_MIN) ? BRIGHT_DUSK_PCT
            : dark ? BRIGHT_NIGHT_PCT : BACKLIGHT_PCT;
    apply_light(sun_dark, sun_pct);
}

/* ---- setup ---- */

/* Touch at boot, as the original's wantsSetup(): any touch within the
 * window asks for the portal even with networks saved. */
static bool setup_asked(void)
{
    if (!touch_present()) return false;
    draw_message("Aimless Moving Map", "Touch the screen now to set up Wi-Fi.");
    const int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < SETUP_WINDOW_US) {
        int x, y;
        if (touch_get(&x, &y)) {
            touch_swallow();
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(SETUP_POLL_MS));
    }
    return false;
}

/* Ask for the radio for the portal. setup_begin() runs before boot's
 * first wifi_request_apply(), so from there this is that call's to
 * make; from setup_step() it makes its own. */
static void setup_want(bool apply)
{
    s_setup = SETUP_WANT;
    s_setup_since = esp_timer_get_time();
    if (apply) wifi_request_apply();
}

/* Which setup this boot needs. The original's rule: the portal when
 * nothing is saved or a touch asked for it. Before the portal, a
 * Launcher install is asked for its networks, so a password typed into
 * Launcher is not asked for twice. */
static void setup_begin(bool asked)
{
    if (asked) {
        ESP_LOGI(TAG, "setup: asked for by touch");
        setup_want(false);
    } else if (wifistore_count() > 0) {
        s_setup = SETUP_NONE;
    } else if (launcher_present() && launcher_import_request() == ESP_OK) {
        ESP_LOGI(TAG, "setup: nothing saved; asking M5Launcher");
        s_setup = SETUP_IMPORT;
    } else {
        ESP_LOGI(TAG, "setup: nothing saved; starting the portal");
        setup_want(false);
    }
}

/* One step, from the loop. */
static void setup_step(void)
{
    switch (s_setup) {
    case SETUP_IMPORT: {
        li_status_t li;
        launcher_import_status(&li);
        if (li.phase == LI_RUNNING) return;
        if (wifistore_count() > 0) {
            setup_note(li.msg);
            s_setup = SETUP_NONE;
            wifi_request_apply();       /* the radio, and a join */
        } else {
            ESP_LOGI(TAG, "setup: %s", li.msg);
            setup_want(true);
        }
        s_dirty = true;
        return;
    }
    case SETUP_WANT:
        if (wifi_up()) {
            if (portal_start() == ESP_OK) s_setup = SETUP_ACTIVE;
        } else if (esp_timer_get_time() - s_setup_since >= SETUP_RADIO_US) {
            setup_note("Wi-Fi setup: the radio did not come up");
            s_setup = SETUP_NONE;
            wifi_request_apply();
        }
        return;
    case SETUP_ACTIVE:
        if (portal_running()) return;
        {
            portal_state_t ps;
            portal_state(&ps);
            char msg[96];
            switch (ps.status) {
            case PORTAL_SAVED:
                snprintf(msg, sizeof(msg), "Wi-Fi: saved %s", ps.last_ssid);
                break;
            case PORTAL_TIMEDOUT:
                snprintf(msg, sizeof(msg), "Wi-Fi setup closed after %d minutes with nothing saved",
                         PORTAL_TIMEOUT_S / 60);
                break;
            case PORTAL_ERROR:
                snprintf(msg, sizeof(msg), "Wi-Fi setup stopped: the radio failed");
                break;
            default:
                snprintf(msg, sizeof(msg), "Wi-Fi setup closed");
                break;
            }
            setup_note(msg);
        }
        s_setup = SETUP_NONE;
        /* Back to what the saved list says: off with nothing saved, and
         * the background join otherwise. */
        wifi_request_apply();
        s_dirty = true;
        return;
    default:
        return;
    }
}

/* ---- pan, screen off, touch (0016) ---- */

/* One step of a pan: a third of the map's width or height, the original's
 * MARKER_BAND, which is also how far its follow band let the marker go. */
static void pan_step(int dx, int dy)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_view.grid.initialised) {
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "pan: nothing to pan yet");
        return;
    }
    if (!s_panning) {
        s_anchor_x = s_view.fx;
        s_anchor_y = s_view.fy;
        s_panning = true;
    }
    const double vis_h = ui_map_bottom(gfx_h()) - STATUS_H;
    s_anchor_x += dx * (gfx_w() * MARKER_BAND) / SUBTILE_PX;
    s_anchor_y += dy * (vis_h * MARKER_BAND) / SUBTILE_PX;
    mapview_centre_tiles(&s_view, s_anchor_x, s_anchor_y);
    xSemaphoreGive(s_lock);
}

/* Back to following the device: at once, rather than at the next fix,
 * which at the idle rate can be seconds away (original map_pan_reset()).
 * Without a fix the view stays where it is and stops being a pan. */
static void pan_reset(void)
{
    if (!s_panning) return;
    s_panning = false;
    if (!s_mark_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mapview_centre_tiles(&s_view, s_mark_x, s_mark_y);
    xSemaphoreGive(s_lock);
}

/*
 * The original's screenOff(): the wake target drawn first, so where to
 * press is not a secret, then the backlight to 0. A pan is dropped:
 * waking to a map of somewhere the device is not, hours later, is the
 * one way a pan can mislead. GNSS and the render task carry on, so the
 * grid is drawn when the screen comes back; the original stopped
 * rasterising instead, which saves power this does not.
 */
static void screen_off(void)
{
    const int W = gfx_w(), H = gfx_h();
    gfx_fill_rect(0, 0, W, H, RGB(0, 0, 0));
    box(W / 3, H / 3, W / 3, H / 3, RGB(0, 0, 0), COL_DIM);
    text_centred(W / 2, H / 2, "touch here to wake", W / 3 - 20, COL_DIM);
    gfx_blit(0, H);
    /* src: original screenOff(): the target shown for 700 ms. */
    vTaskDelay(pdMS_TO_TICKS(700));
    pan_reset();
    s_panel = false;
    s_pins = false;
    s_screen_off = true;
    backlight(0);
    gfx_fill_rect(0, 0, W, H, RGB(0, 0, 0));
    gfx_blit(0, H);
    ESP_LOGI(TAG, "screen: off (GNSS and tiles continue)");
}

static void screen_on(void)
{
    s_screen_off = false;
    s_backlight = -1;       /* whatever the light now calls for */
    s_dirty = true;
    ESP_LOGI(TAG, "screen: on");
}

static void row_tap(int row)
{
    switch (row) {
    case UI_SET_THEME:
        s_theme = ui_theme_next(s_theme);
        ESP_LOGI(TAG, "palette: %s", s_theme == UI_THEME_AUTO ? "auto"
                                   : s_theme == UI_THEME_DAY ? "day" : "night");
        break;
    case UI_SET_BRIGHT:
        s_bright = ui_bright_next(s_bright);
        ESP_LOGI(TAG, "brightness: %d", (int)s_bright);
        break;
    case UI_SET_LABELS:
        /* A repaint, not a render: the names are drawn over the tiles. */
        s_labels = !s_labels;
        ESP_LOGI(TAG, "labels: %s", s_labels ? "on" : "off");
        break;
    case UI_SET_WIFI:
        /* The portal, as at boot; the panel closes so the box shows. */
        s_panel = false;
        if (s_setup == SETUP_NONE) {
            ESP_LOGI(TAG, "setup: asked for from settings");
            setup_want(true);
        }
        break;
    default:
        break;
    }
}

/*
 * One tap, in the original's order: the wake zone alone while the screen
 * is off; the panel, which takes every tap while it is open and closes on
 * one outside it; the setup box; the pan squares; the buttons.
 * Edge-triggered, one press one tap, and the press is swallowed after
 * anything that changes what is under the finger (touch.h).
 */
/* A tap on the saved points panel, as the original's pinPanelTouch(). */
static void pins_tap(int x, int y, const gnss_fix_t *fix)
{
    int row;
    const int rows = ui_pins_rows(gfx_w(), gfx_h());
    switch (ui_pins_at(x, y, gfx_w(), gfx_h(), &row)) {
    case UI_PINS_OUTSIDE:
    case UI_PINS_CLOSE:
        s_pins = false;
        break;
    case UI_PINS_SAVE:
        /* src: original wp_add_fix(): no fix, no point -- saving 0,0
         * because the receiver had not locked is the one failure that
         * looks like success. */
        if (!gnss_coarse(fix)) break;
        if (wp_add(&s_wp, fix->lat, fix->lon, NULL, utc_now(fix)) >= 0) {
            ESP_LOGI(TAG, "saved points: added %s at %.5f,%.5f",
                     s_wp.p[s_wp.n - 1].name, fix->lat, fix->lon);
            wp_write();
        }
        break;
    case UI_PINS_STOP:
        if (wp_target(&s_wp) >= 0) {
            wp_set_target(&s_wp, -1);
            ESP_LOGI(TAG, "saved points: guidance off");
        }
        break;
    case UI_PINS_UP:
        if (s_pins_scroll > 0) s_pins_scroll--;
        break;
    case UI_PINS_DOWN:
        if (s_pins_scroll + rows < s_wp.n) s_pins_scroll++;
        break;
    case UI_PINS_PICK: {
        const int i = s_pins_scroll + row;
        if (i >= s_wp.n) break;
        /* Tapping the target again stops guiding: the row is its own
         * toggle. */
        wp_set_target(&s_wp, i == wp_target(&s_wp) ? -1 : i);
        ESP_LOGI(TAG, "saved points: %s%s", wp_target(&s_wp) >= 0 ? "guiding to " : "guidance off",
                 wp_target(&s_wp) >= 0 ? s_wp.p[i].name : "");
        s_pins = false;
        break;
    }
    case UI_PINS_DELETE: {
        const int i = s_pins_scroll + row;
        if (i >= s_wp.n) break;
        ESP_LOGI(TAG, "saved points: removed %s", s_wp.p[i].name);
        wp_remove(&s_wp, i);
        wp_write();
        /* The list got shorter under the page. */
        while (s_pins_scroll > 0 && s_pins_scroll + rows > s_wp.n) s_pins_scroll--;
        break;
    }
    default:
        break;
    }
}

static void ui_touch(const gnss_fix_t *fix)
{
    static bool was_down;
    int x, y;
    const bool down = touch_get(&x, &y);
    const bool tap = down && !was_down;
    was_down = down;
    if (!tap) return;
    const int W = gfx_w(), H = gfx_h();
    /* Any tap, the wake included: original handleTouch(). */
    s_touch_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_idle_dim) {
        s_idle_dim = false;
        s_dirty = true;
    }

    if (s_screen_off) {
        if (ui_wake_zone(x, y, W, H)) { screen_on(); touch_swallow(); }
        return;
    }
    s_dirty = true;
    if (s_pins) {
        pins_tap(x, y, fix);
        touch_swallow();
        return;
    }
    if (s_panel) {
        const int hit = ui_panel_at(x, y, W, H);
        if (hit == UI_PANEL_OUTSIDE || hit == UI_PANEL_CLOSE) s_panel = false;
        else if (hit >= 0) row_tap(hit);
        touch_swallow();
        return;
    }
    if (s_setup == SETUP_ACTIVE) {
        int bx, by, bw, bh;
        setup_box_rect(&bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            ESP_LOGI(TAG, "setup: closed by touch");
            portal_request_stop();
            touch_swallow();
            return;
        }
    }
    int dx, dy;
    if (ui_pan_cell(x, y, W, H, STATUS_H, &dx, &dy)) {
        if (dx || dy) pan_step(dx, dy);
        return;
    }
    switch (ui_button_at(x, y, W, H)) {
    case UI_BTN_CACHE:
        area_tap();
        break;
    case UI_BTN_HOME:
        /* Harmless when already following, and deliberately still live:
         * it is what you press when unsure (original handleTouch()). */
        pan_reset();
        break;
    case UI_BTN_PINS:
        s_pins = true;
        s_pins_scroll = 0;
        touch_swallow();
        break;
    case UI_BTN_SET:
        s_panel = true;
        touch_swallow();
        break;
    case UI_BTN_SLEEP:
        screen_off();
        touch_swallow();
        break;
    default:
        break;
    }
}

/*
 * The receiver's rate and the parked dim, from the fix (0031):
 * original/tab5_map.cpp gnssRatePolicy() and applyIdleDim(), as
 * motion.h has them. The rate is FAST while the area cache walks, as
 * there while prefetching.
 */
static void motion_step(const gnss_fix_t *fix)
{
    static motion_rate_t rate;
    static motion_idle_t idle;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = s_area.active;
    xSemaphoreGive(s_lock);

    const uint16_t want = motion_rate_want(gnss_coarse(fix), fix->mode, fix->speed_kmh, busy);
    const uint16_t set = motion_rate_step(&rate, want, gnss_rate_ms(), now);
    if (set) {
        gnss_set_rate_ms(set);
        ESP_LOGI(TAG, "gnss: rate %u ms (%.1f km/h, mode %d)", set, fix->speed_kmh, fix->mode);
    }

    const bool dim = !s_screen_off &&
                     motion_idle(&idle, now, s_touch_ms, gnss_coarse(fix) && fix->mode == 3,
                                 fix->lat, fix->lon, gnss_rate_ms());
    if (dim != s_idle_dim) {
        s_idle_dim = dim;
        s_dirty = true;
        ESP_LOGI(TAG, "bright: idle dim %s", dim ? "on" : "off");
    }
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
    s_backlight = BACKLIGHT_PCT;
    /* After the panel: the controller's reset is released with the
     * panel's (touch.h). Without touch the map runs; only setup by touch
     * is lost. */
    if (touch_init(tab5io_bus(), LCD_H_RES, LCD_V_RES) == ESP_OK)
        touch_set_rotation(VIEW_ROTATION);
    else
        ESP_LOGW(TAG, "no touch this boot");

    /* The render scratch and the grid's buffers, now rather than after
     * the card and the network, so the world can be drawn into one of
     * them while those come up (0020). */
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
    world_draw();
    draw_message("Aimless Moving Map", "looking for maps");

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
    wp_read();
    portal_init();
    net_start();
    usbhost_start();

    /* The receiver searches while everything else comes up. */
    if (!gnss_start(GNSS_P4_RX_PIN, GNSS_P4_TX_PIN, GNSS_BAUD, GNSS_PPS_PIN, 0, 5))
        ESP_LOGE(TAG, "GNSS did not start");
    else if (xTaskCreatePinnedToCore(aop_boot_task, "aop", AOP_STACK, NULL, 1, NULL, 0) != pdPASS)
        ESP_LOGW(TAG, "aop: could not start; a cold start, as without it");

    /* The two seconds to ask for Wi-Fi setup, while the receiver and a
     * USB drive come up anyway. Before the radio is asked for, so the
     * first apply already knows whether the portal wants it, and the
     * background join does not start a scan the portal's would race. */
    setup_begin(setup_asked());
    draw_message("Aimless Moving Map", "looking for maps");
    /* The join runs on the network's own worker; this returns at once. */
    wifi_request_apply();

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
        if (storage_present(where)) {
            char old[48];
            snprintf(dir, sizeof(dir), "%s" CACHE_SUBDIR, storage_mount_path(where));
            snprintf(old, sizeof(old), "%s" CACHE_OLD, storage_mount_path(where));
            file_adopt(old, dir);
        }
        netremote_init(dir[0] ? dir : "/nowhere", &MEM);
        /* netremote_init() made it if it was not there. */
        if (dir[0]) storage_mark_hidden(dir);
    }

    mapview_init(&s_view, src_draw, NULL, bufs, VIEW_ZOOM, style_background());
    /* The overview's two buffers, 512 KB each. Without them the map runs
     * as before, blank where a tile is missing. */
    {
        uint16_t *cz_a = mem_big((size_t)COARSE_PX * COARSE_PX * sizeof(uint16_t));
        uint16_t *cz_b = mem_big((size_t)COARSE_PX * COARSE_PX * sizeof(uint16_t));
        if (cz_a && cz_b) mapview_set_coarse(&s_view, cz_a, cz_b);
        else ESP_LOGW(TAG, "no overview: out of PSRAM");
    }
    /* The place indexes, 8 KB each (0030). Without them, no place names. */
    {
        bool ok = true;
        for (int i = 0; i < 2; i++) {
            ok &= (s_pidx[i] = mem_big(sizeof(places_index_t))) != NULL;
            ok &= (s_pidx_w[i] = mem_big(sizeof(places_index_t))) != NULL;
        }
        if (!ok) {
            s_pidx[0] = NULL;
            ESP_LOGW(TAG, "no place names: out of PSRAM");
        }
    }
    /* A label set for each tile buffer, 2.5 KB each (0029). Without
     * them the map runs as before, with no names. */
    {
        maplabel_set_t *sets[GRID_COUNT];
        bool ok = true;
        for (int i = 0; i < GRID_COUNT; i++) ok &= (sets[i] = mem_big(sizeof(maplabel_set_t))) != NULL;
        if (ok) mapview_set_labels(&s_view, sets);
        else ESP_LOGW(TAG, "no labels: out of PSRAM");
    }
    ESP_LOGI(TAG, "%d tiles of %d px in PSRAM; %u KB PSRAM, %u KB internal left",
             GRID_COUNT, SUBTILE_PX,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    s_lock = xSemaphoreCreateMutex();

    /* Before a fix, the last known position, so the map that comes up is
     * the one around where the device was (original map_seed_position());
     * without a marker, since it is not where anyone is now. Without one,
     * nowhere: the world stays up until the fix. 0021: this was the
     * first archive's centre, which put a stranger's city on the screen
     * between the world and the fix. */
    {
        double lat, lon;
        if (lastfix_read(&lat, &lon)) {
            mapview_centre(&s_view, lat, lon);
            s_sun_lat = lat;
            s_sun_lon = lon;
            s_sun_have = true;
            ESP_LOGI(TAG, "seeded at %.4f,%.4f from the last known position", lat, lon);
        }
    }

    if (xTaskCreatePinnedToCore(render_task, "render", RENDER_STACK, NULL,
                                RENDER_PRIO, NULL, RENDER_CORE) != pdPASS) {
        draw_message("Out of memory", "render task");
        return;
    }

    gnss_fix_t fix;
    int64_t last_draw = 0, last_log = 0;
    for (;;) {
        gnss_get(&fix);
        if (gnss_coarse(&fix)) {
            const merc_pt_t p = merc_from_ll(fix.lat, fix.lon, VIEW_ZOOM);
            s_mark_x = p.x;
            s_mark_y = p.y;
            s_mark_ok = true;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_place_wx = p.x;
            s_place_wy = p.y;
            s_place_at = true;
            xSemaphoreGive(s_lock);
            /* While panned the view stays where the pan put it; the
             * marker moves on its own (original/README.md). */
            if (!s_panning) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                mapview_centre_tiles(&s_view, p.x, p.y);
                xSemaphoreGive(s_lock);
            }
        }
        /* Today's date, for finding a daily build without SNTP. */
        if (fix.status == 'A') netremote_set_today(bd_from_ddmmyy(fix.date));

        sentence_rate();
        setup_step();
        ui_touch(&fix);
        motion_step(&fix);
        lastfix_keep(&fix);
        ttff_report(&fix);
        aop_keep(&fix);

        const int64_t now = esp_timer_get_time();
        if (s_dirty || now - last_draw >= 1000000) {
            daylight(&fix);
            s_dirty = false;
            draw(&fix);
            last_draw = now;
        }
        if (now - last_log >= 10000000) {
            netremote_stats_t ns;
            netremote_stats(&ns);
            char route[64];
            net_route_describe(route, sizeof(route));
            char rate[32];   /* GCC sizes %d for any int: 25 with the words */
            if (s_sent_per_min < 0) snprintf(rate, sizeof(rate), "? sentences/min");
            else snprintf(rate, sizeof(rate), "%d sentences/min", s_sent_per_min);
            ESP_LOGI(TAG, "fix %c mode %d, %d sats, HDOP %.1f, %s, PPS %u",
                     fix.status, fix.mode, fix.sats, fix.hdop, rate,
                     (unsigned)gnss_pps_count());
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

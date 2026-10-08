/*
 * launcher_import.c -- see launcher_import.h.
 *
 * defeatist-music-player-for-m5tab5's main/launcher_import.c at 74535ca:
 * its li_run(), config.conf reading and partition walk, around
 * launcherkey.c in place of its own crypto. Two changes beyond that:
 *
 *   - Every buffer is on the heap. Defeatist's marker search kept a 4 KB
 *     window on a 6 KB task stack, which CLAUDE.md does not allow here.
 *   - config.conf is read through storage_io_fread(), so the read takes
 *     the card's arbiter like every other read in this program.
 *
 * Config shape, from bmorcelli/Launcher by way of defeatist:
 *   [ { "wifi": [ { "ssid": ..., "pwd": base64, "secure": bool }, ... ] }, ... ]
 * or the same object without the outer array.
 *
 * SPDX-License-Identifier: MIT
 */
#include "launcher_import.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "storage.h"
#include "storage_io.h"
#include "wifistore.h"

#include "launcherkey.h"

static const char *TAG = "limport";

/* Launcher's config.conf is at the card's root.
 * src: defeatist launcher_import.c LI_CONFIG_PATH. */
#define LI_CONFIG_PATH  STORAGE_SD_MOUNT "/config.conf"
/* src: defeatist launcher_import.c, li_load_config(): a config larger
 * than this is not Launcher's. */
#define LI_CONFIG_MAX   (256 * 1024)
/* src: defeatist launcher_import.c LI_READ_CHUNK. */
#define LI_READ_CHUNK   (4096)
/* src: defeatist launcher_import_request(): 6 KB, with every buffer on
 * the heap -- here that is true of the marker window as well. */
#define LI_STACK        (6144)
#define LI_PRIO         (4)

static SemaphoreHandle_t s_lock;
static li_status_t s_state;

static void set_status(li_phase_t ph, int imported, int skipped, const char *msg)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.phase = ph;
    s_state.imported = imported;
    s_state.skipped = skipped;
    snprintf(s_state.msg, sizeof(s_state.msg), "%s", msg ? msg : "");
    xSemaphoreGive(s_lock);
}

void launcher_import_status(li_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

/* ---- the partition ------------------------------------------------------ */

/* Whether `p` holds Launcher's IV, which is how its partition is told
 * from any other app's. The window overlaps by the marker's length less
 * one, so a marker across two reads is seen. */
static bool has_marker(const esp_partition_t *p, uint8_t *win)
{
    const size_t overlap = LKEY_MARKER_LEN - 1;
    for (size_t off = 0; off < p->size; off += LI_READ_CHUNK - overlap) {
        const size_t want = (p->size - off < LI_READ_CHUNK) ? p->size - off : LI_READ_CHUNK;
        if (esp_partition_read(p, off, win, want) != ESP_OK) return false;
        for (size_t i = 0; i + LKEY_MARKER_LEN <= want; i++)
            if (memcmp(win + i, LKEY_MARKER, LKEY_MARKER_LEN) == 0) return true;
        if (want < LI_READ_CHUNK) break;
        /* src: defeatist li_part_has_marker(): yield a tick a read, so a
         * whole-partition pass does not starve IDLE and trip the task
         * watchdog. */
        vTaskDelay(1);
    }
    return false;
}

static bool find_key(const esp_partition_t *p, const lkey_oracle_t *o,
                     uint8_t *buf, lkey_scan_t *scan, uint8_t key[16])
{
    lkey_scan_init(scan, o);
    for (size_t off = 0; off < p->size; off += LI_READ_CHUNK) {
        const size_t want = (p->size - off < LI_READ_CHUNK) ? p->size - off : LI_READ_CHUNK;
        if (esp_partition_read(p, off, buf, want) != ESP_OK) break;
        if (lkey_scan(scan, buf, want)) break;
        vTaskDelay(1);   /* as has_marker() */
    }
    if (!lkey_scan_end(scan)) return false;
    memcpy(key, scan->key, 16);
    return true;
}

/* ---- config.conf -------------------------------------------------------- */

static cJSON *load_config(cJSON **wifi_out)
{
    FILE *f = fopen(LI_CONFIG_PATH, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > LI_CONFIG_MAX) { fclose(f); return NULL; }
    char *txt = malloc((size_t)sz + 1);
    if (!txt) { fclose(f); return NULL; }
    const size_t rd = storage_io_fread(txt, (size_t)sz, f, STORAGE_IO_BACKGROUND);
    fclose(f);
    txt[rd] = 0;
    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) return NULL;

    cJSON *wifi = NULL;
    if (cJSON_IsArray(root)) {
        cJSON *el;
        cJSON_ArrayForEach(el, root) {
            cJSON *w = cJSON_GetObjectItemCaseSensitive(el, "wifi");
            if (cJSON_IsArray(w)) { wifi = w; break; }
        }
    } else {
        cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "wifi");
        if (cJSON_IsArray(w)) wifi = w;
    }
    if (!wifi) { cJSON_Delete(root); return NULL; }
    *wifi_out = wifi;
    return root;
}

static void fill_oracle(cJSON *wifi, lkey_oracle_t *o)
{
    memset(o, 0, sizeof(*o));
    cJSON *e;
    cJSON_ArrayForEach(e, wifi) {
        if (o->n >= 2) break;
        cJSON *sec = cJSON_GetObjectItemCaseSensitive(e, "secure");
        cJSON *pwd = cJSON_GetObjectItemCaseSensitive(e, "pwd");
        if (sec && cJSON_IsFalse(sec)) continue;
        if (!cJSON_IsString(pwd)) continue;
        (void)lkey_oracle_add(o, pwd->valuestring);
    }
}

/* ---- the job ------------------------------------------------------------ */

/* Everything the job holds that is more than a few bytes, in one heap
 * block: the oracle and the scan are about 400 bytes together. */
typedef struct {
    uint8_t       buf[LI_READ_CHUNK];
    lkey_oracle_t oracle;
    lkey_scan_t   scan;
    uint8_t       ct[LKEY_CT_MAX];
    uint8_t       pt[LKEY_CT_MAX + 1];
    uint8_t       key[16];
} li_work_t;

static void run(li_work_t *w)
{
    cJSON *wifi = NULL;
    cJSON *root = load_config(&wifi);
    if (!root) {
        set_status(LI_FAILED, 0, 0, "No M5Launcher config.conf on the card");
        return;
    }

    fill_oracle(wifi, &w->oracle);
    if (w->oracle.n == 0) {
        cJSON_Delete(root);
        set_status(LI_FAILED, 0, 0, "No saved networks in M5Launcher");
        return;
    }

    /* The key, from an app partition that carries the marker and is not
     * this program. */
    bool found = false;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_partition_iterator_t it =
        esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it && !found) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p != running && has_marker(p, w->buf) &&
            find_key(p, &w->oracle, w->buf, &w->scan, w->key)) {
            found = true;
            ESP_LOGI(TAG, "key recovered from partition '%s'", p->label);
        }
        it = esp_partition_next(it);
    }
    if (it) esp_partition_iterator_release(it);
    if (!found) {
        cJSON_Delete(root);
        set_status(LI_FAILED, 0, 0, "M5Launcher's key was not found");
        return;
    }

    /* Decrypt and save. The SSID may be logged; the password never is. */
    int imported = 0, skipped = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, wifi) {
        cJSON *jssid = cJSON_GetObjectItemCaseSensitive(e, "ssid");
        cJSON *jpwd  = cJSON_GetObjectItemCaseSensitive(e, "pwd");
        cJSON *jsec  = cJSON_GetObjectItemCaseSensitive(e, "secure");
        if (!cJSON_IsString(jssid) || !jssid->valuestring[0]) { skipped++; continue; }
        const bool secure = !(jsec && cJSON_IsFalse(jsec));
        /* Open networks: wifistore keeps a secret for every record and
         * has nowhere to put none (portalweb.h). */
        if (!secure || !cJSON_IsString(jpwd) || !jpwd->valuestring[0]) { skipped++; continue; }

        const int ctl = lkey_b64_decode(jpwd->valuestring, w->ct, sizeof(w->ct));
        const int plen = ctl > 0 ? lkey_decrypt(w->key, w->ct, (size_t)ctl, w->pt, sizeof(w->pt)) : -1;
        bool is_psk = false;
        if (plen < 0 || !lkey_classify((const char *)w->pt, &is_psk)) {
            ESP_LOGW(TAG, "%.32s: not a usable password; skipped", jssid->valuestring);
            skipped++;
        } else if (wifistore_save(jssid->valuestring, (const char *)w->pt, is_psk) == ESP_OK) {
            ESP_LOGI(TAG, "%.32s: saved as %s", jssid->valuestring, is_psk ? "PSK" : "passphrase");
            imported++;
        } else {
            skipped++;
        }
        memset(w->pt, 0, sizeof(w->pt));
    }
    cJSON_Delete(root);

    /* Room for both counts at their widest: GCC's format-truncation
     * check sizes %d for any int (0006). */
    char msg[64];
    snprintf(msg, sizeof(msg), "Imported %d from M5Launcher, skipped %d", imported, skipped);
    set_status(LI_DONE, imported, skipped, msg);
    ESP_LOGI(TAG, "%s", msg);
}

static void li_task(void *arg)
{
    (void)arg;
    li_work_t *w = calloc(1, sizeof(*w));
    if (w) {
        run(w);
        memset(w, 0, sizeof(*w));   /* the key and the last plaintext */
        free(w);
    } else {
        set_status(LI_FAILED, 0, 0, "Out of memory");
    }
    vTaskDelete(NULL);
}

esp_err_t launcher_import_request(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    li_status_t st;
    launcher_import_status(&st);
    if (st.phase == LI_RUNNING) return ESP_ERR_INVALID_STATE;
    set_status(LI_RUNNING, 0, 0, "Reading M5Launcher's networks");
    if (xTaskCreate(li_task, "limport", LI_STACK, NULL, LI_PRIO, NULL) != pdPASS) {
        set_status(LI_IDLE, 0, 0, "");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool launcher_present(void)
{
    const esp_partition_t *fac = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    return fac && fac != esp_ota_get_running_partition();
}

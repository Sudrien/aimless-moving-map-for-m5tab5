/*
 * gnss.c -- see gnss.h. original/gnss.cpp, on ESP-IDF.
 *
 * SPDX-License-Identifier: MIT
 */

#include "gnss.h"

#include "aop.h"
#include "ubx.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "gnss";

#define GNSS_UART       (UART_NUM_1)
/* src: original/gnss.cpp Serial1.setRxBufferSize(2048). */
#define GNSS_RX_BUF     (2048)
#define GNSS_TASK_STACK (4096)

/* Gap above which two pulses belong to different runs rather than being
 * one very long interval. src: original/gnss.cpp PPS_RESTART_MS. */
#define PPS_RESTART_MS  (2000)

static SemaphoreHandle_t s_lock;
static gnss_fix_t s_pub;                /* published copy, under s_lock */
static uint32_t   s_sentences;
static volatile uint32_t s_first_coarse_ms, s_first_fine_ms;
static volatile uint32_t s_pps_count, s_pps_last, s_pps_interval;
static uint16_t   s_rate_ms = 1000;

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void IRAM_ATTR pps_isr(void *arg)
{
    (void)arg;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t d = now - s_pps_last;
    if (s_pps_last && d < PPS_RESTART_MS) s_pps_interval = d;
    s_pps_last = now;
    s_pps_count = s_pps_count + 1;      /* sole writer; see the original */
}

/*
 * UBX arrives in the middle of the NMEA stream; ubx.c recognises it, so a
 * binary reply never lands in the line buffer, and hands each complete
 * frame to whatever exchange is waiting for one (0028).
 */
static ubx_parser_t s_ubx;
static ubx_capture_t *volatile s_cap;  /* set by ubx_exchange(), under s_ubx_lock */
static SemaphoreHandle_t s_ubx_lock;    /* one exchange at a time */
static uint32_t s_start_ms;

static void gnss_task(void *arg)
{
    (void)arg;
    static uint8_t rx[256];
    char line[128];
    int pos = 0;
    gnss_fix_t local;
    gnss_fix_init(&local);
    for (;;) {
        const int n = uart_read_bytes(GNSS_UART, rx, sizeof(rx), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            const char c = (char)rx[i];
            const ubx_byte_t u = ubx_feed(&s_ubx, (uint8_t)c);
            if (u == UBX_FRAME) ubx_capture_offer(s_cap, s_ubx.frame, s_ubx.flen, now_ms());
            if (u != UBX_NOT) continue;
            if (c == '\n') {
                line[pos] = 0;
                const uint32_t t = now_ms();
                if (nmea_parse(line, &local, t)) s_sentences++;
                pos = 0;
                if (!s_first_coarse_ms && gnss_coarse(&local)) s_first_coarse_ms = t;
                if (!s_first_fine_ms && gnss_fine(&local))     s_first_fine_ms = t;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_pub = local;
                xSemaphoreGive(s_lock);
            } else if (c != '\r' && pos < (int)sizeof(line) - 1) {
                line[pos++] = c;
            }
        }
    }
}

static void ubx_send(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[UBX_OVERHEAD + 64];
    const size_t n = ubx_frame(cls, id, payload, len, frame, sizeof(frame));
    if (!n) return;
    uart_write_bytes(GNSS_UART, frame, n);
    uart_wait_tx_done(GNSS_UART, pdMS_TO_TICKS(100));
}

bool gnss_start(int rx_pin, int tx_pin, uint32_t baud, int pps_pin,
                int core, int priority)
{
    s_lock = xSemaphoreCreateMutex();
    s_ubx_lock = xSemaphoreCreateMutex();
    if (!s_lock || !s_ubx_lock) return false;
    ubx_parser_init(&s_ubx);
    s_start_ms = now_ms();
    gnss_fix_init(&s_pub);
    s_pub.last_sentence_ms = now_ms();

    const uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(GNSS_UART, GNSS_RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(GNSS_UART, &cfg);
    if (err == ESP_OK) err = uart_set_pin(GNSS_UART, tx_pin, rx_pin,
                                          UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART: %s", esp_err_to_name(err));
        return false;
    }

    if (pps_pin >= 0) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << pps_pin,
            .mode = GPIO_MODE_INPUT,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_POSEDGE,
        };
        err = gpio_config(&io);
        if (err == ESP_OK) {
            err = gpio_install_isr_service(0);
            if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;    /* already installed */
        }
        if (err == ESP_OK) err = gpio_isr_handler_add(pps_pin, pps_isr, NULL);
        if (err != ESP_OK) ESP_LOGW(TAG, "PPS on GPIO%d: %s; counting none", pps_pin,
                                    esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "UART%d at %u baud, RX GPIO%d, TX GPIO%d, PPS GPIO%d",
             GNSS_UART, (unsigned)baud, rx_pin, tx_pin, pps_pin);
    return xTaskCreatePinnedToCore(gnss_task, "gnss", GNSS_TASK_STACK, NULL,
                                   priority, NULL, core) == pdPASS;
}

void gnss_get(gnss_fix_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_pub;
    xSemaphoreGive(s_lock);
}

/* UBX-CFG-RATE. src: original/gnss.cpp gnss_set_rate_ms(). */
bool gnss_set_rate_ms(uint16_t ms)
{
    if (ms < 200)   ms = 200;
    if (ms > 10000) ms = 10000;
    const uint8_t payload[6] = {
        (uint8_t)(ms & 0xFF), (uint8_t)(ms >> 8),
        0x01, 0x00,                     /* navRate: one solution per measurement */
        0x01, 0x00,                     /* timeRef: GPS */
    };
    ubx_send(0x06, 0x08, payload, sizeof(payload));
    s_rate_ms = ms;
    return true;
}

uint16_t gnss_rate_ms(void)         { return s_rate_ms; }
uint32_t gnss_first_coarse_ms(void) { return s_first_coarse_ms; }
uint32_t gnss_first_fine_ms(void)   { return s_first_fine_ms; }
uint32_t gnss_sentences(void)       { return s_sentences; }
uint32_t gnss_pps_count(void)       { return s_pps_count; }
uint32_t gnss_pps_interval(void)    { return s_pps_interval; }
uint32_t gnss_start_ms(void)         { return s_start_ms; }

/* ---- AssistNow Autonomous (0028) ---- */

/* src: u-blox M8 protocol specification (UBX-13003221), "UBX Class IDs"
 * and the message sections; the same in the M9/M10 documents.
 * original/gnss.cpp's names. */
#define UBX_CLS_CFG     (0x06)
#define UBX_ID_NAVX5    (0x23)      /* UBX-CFG-NAVX5, the AOP enable */
#define UBX_CLS_MGA     (0x13)
#define UBX_ID_DBD      (0x80)      /* UBX-MGA-DBD, the navigation database */
#define UBX_ID_MGA_ACK  (0x60)      /* UBX-MGA-ACK, which ends a DBD poll */

/*
 * Send a poll and collect the frames `c` asks for, until its terminating
 * frame, `timeout_ms`, or `idle_ms` of quiet after the first one -- so a
 * receiver with ack-aiding off still finishes rather than burning the
 * whole timeout (original ubx_exchange()). The reader task does the
 * collecting; it is the only thing draining the UART.
 */
static bool ubx_exchange(uint8_t cls, uint8_t id, ubx_capture_t *c,
                         uint32_t timeout_ms, uint32_t idle_ms)
{
    if (!s_ubx_lock || xSemaphoreTake(s_ubx_lock, pdMS_TO_TICKS(2000)) != pdTRUE)
        return false;
    c->len = 0;
    c->frames = 0;
    c->done = false;
    c->last_ms = now_ms();
    s_cap = c;
    ubx_send(cls, id, NULL, 0);
    const uint32_t t0 = now_ms();
    while (!c->done && now_ms() - t0 < timeout_ms) {
        if (c->frames && idle_ms && now_ms() - c->last_ms > idle_ms) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_cap = NULL;
    /* The reader may still be inside ubx_capture_offer() with the old
     * pointer. The captures are static, so that is never a dangling one;
     * this is so it is out before the next exchange resets it. */
    vTaskDelay(pdMS_TO_TICKS(30));
    xSemaphoreGive(s_ubx_lock);
    return c->frames > 0 || c->done;
}

bool gnss_enable_aop(void)
{
    static uint8_t buf[128];    /* the reply frame: 44 bytes of payload at most */
    static ubx_capture_t c;     /* static: the reader holds a pointer to it */
    c = (ubx_capture_t){ .cls = UBX_CLS_CFG, .id = UBX_ID_NAVX5,
                        .ack_cls = 0xFF, .ack_id = 0xFF,
                        .buf = buf, .cap = sizeof(buf) };
    /* src: original gnss_enable_aop(): 1.5 s for the reply, done 250 ms
     * after it. */
    if (!ubx_exchange(UBX_CLS_CFG, UBX_ID_NAVX5, &c, 1500, 250)) {
        ESP_LOGW(TAG, "CFG-NAVX5 poll got no reply; no AssistNow Autonomous");
        return false;
    }
    const uint16_t plen = (uint16_t)(buf[4] | (uint16_t)buf[5] << 8);
    uint8_t *p = buf + UBX_HEAD;
    if (c.len < (size_t)plen + UBX_OVERHEAD || !aop_navx5_edit(p, plen)) {
        ESP_LOGW(TAG, "CFG-NAVX5 reply too short (%u bytes)", (unsigned)c.len);
        return false;
    }
    ubx_send(UBX_CLS_CFG, UBX_ID_NAVX5, p, plen);
    ESP_LOGI(TAG, "AssistNow Autonomous on (NAVX5 v%u, %u bytes)", (unsigned)p[0], (unsigned)plen);
    return true;
}

size_t gnss_dbd_read(uint8_t *dst, size_t cap)
{
    static ubx_capture_t c;
    c = (ubx_capture_t){ .cls = UBX_CLS_MGA, .id = UBX_ID_DBD,
                        .ack_cls = UBX_CLS_MGA, .ack_id = UBX_ID_MGA_ACK,
                        .buf = dst, .cap = cap };
    /* src: original gnss_dbd_read(): 8 s, done 500 ms after the last
     * record if the MGA-ACK never comes. */
    if (!ubx_exchange(UBX_CLS_MGA, UBX_ID_DBD, &c, 8000, 500)) return 0;
    ESP_LOGI(TAG, "navigation database: %u records, %u bytes%s", (unsigned)c.frames,
             (unsigned)c.len, c.done ? "" : " (no MGA-ACK; ended on quiet)");
    return c.len;
}

bool gnss_dbd_write(const uint8_t *src, size_t len)
{
    int frames = 0;
    const size_t good = ubx_frames(src, len, &frames);
    if (!frames) return false;
    if (!s_ubx_lock || xSemaphoreTake(s_ubx_lock, pdMS_TO_TICKS(2000)) != pdTRUE)
        return false;
    size_t off = 0;
    while (off < good) {
        const size_t flen = (size_t)(src[off + 4] | (size_t)src[off + 5] << 8) + UBX_OVERHEAD;
        uart_write_bytes(GNSS_UART, src + off, flen);
        uart_wait_tx_done(GNSS_UART, pdMS_TO_TICKS(100));
        off += flen;
        /* src: original gnss_dbd_write(), u-blox's reference spacing: the
         * receiver drops assistance it is too busy to take. */
        vTaskDelay(pdMS_TO_TICKS(7));
    }
    xSemaphoreGive(s_ubx_lock);
    ESP_LOGI(TAG, "navigation database pushed: %d records, %u of %u bytes",
             frames, (unsigned)good, (unsigned)len);
    return true;
}

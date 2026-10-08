/*
 * gnss.c -- see gnss.h. original/gnss.cpp, on ESP-IDF.
 *
 * SPDX-License-Identifier: MIT
 */

#include "gnss.h"

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
 * UBX frames are skipped. The original captured them for the AssistNow
 * calls, which are not ported (gnss.h); what is kept is recognising one,
 * so that a binary reply never lands in the NMEA line buffer. Returns
 * true while the byte belongs to a UBX frame. As original/gnss.cpp
 * ubx_feed(), without the capture.
 */
static bool ubx_skip(uint8_t c)
{
    static int st;
    static uint16_t len, got;
    switch (st) {
    case 0: if (c == 0xB5) { st = 1; return true; } return false;
    case 1: if (c == 0x62) { st = 2; return true; } st = 0; return false;
    case 2: st = 3; return true;                            /* class */
    case 3: st = 4; return true;                            /* id */
    case 4: len = c; st = 5; return true;
    case 5: len |= (uint16_t)c << 8; got = 0; st = len ? 6 : 7; return true;
    case 6: if (++got >= len) st = 7; return true;
    case 7: st = 8; return true;                            /* checksum A */
    case 8: st = 0; return true;                            /* checksum B */
    }
    st = 0;
    return false;
}

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
            if (ubx_skip((uint8_t)c)) continue;
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
    uint8_t frame[6 + 64 + 2];
    if (len > 64) return;
    frame[0] = 0xB5; frame[1] = 0x62;
    frame[2] = cls;  frame[3] = id;
    frame[4] = (uint8_t)(len & 0xFF); frame[5] = (uint8_t)(len >> 8);
    if (len) memcpy(frame + 6, payload, len);
    ubx_checksum(frame + 2, 4u + len, &frame[6 + len], &frame[7 + len]);
    uart_write_bytes(GNSS_UART, frame, 8u + len);
    uart_wait_tx_done(GNSS_UART, pdMS_TO_TICKS(100));
}

bool gnss_start(int rx_pin, int tx_pin, uint32_t baud, int pps_pin,
                int core, int priority)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;
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

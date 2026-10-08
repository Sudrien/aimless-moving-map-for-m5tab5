/*
 * gnss.h -- the M135 GNSS module on the Tab5's expansion port.
 *
 * original/gnss.cpp on ESP-IDF's UART driver instead of Arduino's
 * Serial1: a task reads the module, nmea.h parses each line into a
 * private fix, and the whole fix is published under a lock. The PPS line
 * is edge-counted in an interrupt. UBX is sent for the measurement rate,
 * and for AssistNow Autonomous (0028): ubx.c frames and captures it,
 * aop.h is what it is for.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nmea.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The Tab5 wiring, from original/tab5_map.cpp:
 *
 *   src: original/tab5_map.cpp PIN_GNSS_TX 7, PIN_GNSS_RX 6, PIN_PPS 51,
 *        GNSS_BAUD 38400. Note the names there are the module's: "TX 7"
 *        is the module transmitting, which is the P4's receive pin.
 */
#define GNSS_P4_RX_PIN   (7)
#define GNSS_P4_TX_PIN   (6)
#define GNSS_PPS_PIN     (51)
#define GNSS_BAUD        (38400)

/* Open the UART, attach PPS (pps_pin < 0: none) and start the reader on
 * `core` at `priority`. False if any of that fails. */
bool gnss_start(int rx_pin, int tx_pin, uint32_t baud, int pps_pin,
                int core, int priority);

/* The latest published fix. */
void gnss_get(gnss_fix_t *out);

/* When the first coarse and first fine fix were parsed, in ms since boot;
 * 0 until then. */
uint32_t gnss_first_coarse_ms(void);
uint32_t gnss_first_fine_ms(void);

/* One solution every `ms`, 200..10000 (UBX-CFG-RATE). */
bool     gnss_set_rate_ms(uint16_t ms);
uint16_t gnss_rate_ms(void);

/* When gnss_start() opened the UART, in ms since boot: what time to first
 * fix is measured from (the original measured from the receiver's power,
 * which here is the same moment to within the UART's setup). */
uint32_t gnss_start_ms(void);

/* AssistNow Autonomous (0028; aop.h). original/gnss.h's three.
 *
 * Turn it on, with ack-aiding, which ends a database poll; once, after
 * gnss_start(). Blocks up to 1.5 s for the receiver's reply. */
bool   gnss_enable_aop(void);
/* Poll the navigation database into `dst`: whole UBX frames, their
 * length, 0 on failure. Blocks up to 8 s. */
size_t gnss_dbd_read(uint8_t *dst, size_t cap);
/* Push a saved database back, its good frames spaced as u-blox ask.
 * False if there was nothing in it to push. */
bool   gnss_dbd_write(const uint8_t *src, size_t len);

uint32_t gnss_sentences(void);
uint32_t gnss_pps_count(void);
uint32_t gnss_pps_interval(void);   /* ms between the last two pulses */

#ifdef __cplusplus
}
#endif

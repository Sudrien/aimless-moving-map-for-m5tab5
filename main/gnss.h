/*
 * gnss.h -- the M135 GNSS module on the Tab5's expansion port.
 *
 * original/gnss.cpp on ESP-IDF's UART driver instead of Arduino's
 * Serial1: a task reads the module, nmea.h parses each line into a
 * private fix, and the whole fix is published under a lock. The PPS line
 * is edge-counted in an interrupt. UBX is sent for the measurement rate.
 *
 * Not ported yet, and why:
 *   gnss_enable_aop(), gnss_dbd_read(), gnss_dbd_write() -- AssistNow
 *   Autonomous and saving the navigation database across power-off.
 *   They are a warm-start optimisation that needs the card and the UBX
 *   capture machinery; milestone 3 (ARCHITECTURE.md).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
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

uint32_t gnss_sentences(void);
uint32_t gnss_pps_count(void);
uint32_t gnss_pps_interval(void);   /* ms between the last two pulses */

#ifdef __cplusplus
}
#endif

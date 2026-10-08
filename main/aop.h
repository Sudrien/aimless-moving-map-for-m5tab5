/*
 * aop.h -- AssistNow Autonomous: the receiver's own orbit predictions,
 * kept on the card so they outlive its backup supply (0028).
 *
 * The receiver predicts where the satellites will be from ephemeris it
 * has already received itself, good for about three days. No server, no
 * token, no network. The predictions live in its battery-backed RAM,
 * held by a supercap that lasts hours, so they are polled out over UBX
 * (UBX-MGA-DBD), written to the card, and pushed back at the next boot
 * (original/gnss.cpp and original/tab5_map.cpp aopSave(), aopRestore(),
 * aopMaintain()). A receiver that has never had a fix has nothing to
 * predict from; the first cold start is not helped.
 *
 * This is the part with no UART and no file in it: the CFG-NAVX5 edit
 * that turns it on, the file's header, whether a saved database is still
 * worth pushing, and when to save. Tested on the host.
 *
 * THE FILE is the original's /aopdb.bin, byte for byte -- a 16-byte
 * header ("AOP1" as a little-endian u32, the byte count as a u32, the
 * time written as an int64, 0 when the clock was not trusted) and the
 * MGA-DBD frames as the receiver sent them -- named .aimless.aopdb.dat
 * and hidden, as 0022 and 0023 named the others.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* src: original/tab5_map.cpp struct AopHeader, as the ESP32 lays it out. */
#define AOP_HEAD_BYTES  (16)
/* src: original/tab5_map.cpp AOP_CAP: room for the database. */
#define AOP_CAP         (16 * 1024)
/* src: original/tab5_map.cpp AOP_MAX_AGE_H: the predictions are good for
 * up to three days (u-blox M8 receiver description, AssistNow
 * Autonomous), so an older database has nothing left in it. */
#define AOP_MAX_AGE_H   (72)
/* src: original/tab5_map.cpp AOP_SAVE_INTERVAL_MS, and aopMaintain()'s
 * five minutes before the first: the predictions improve as the
 * receiver sees more, and constant writes would wear the card for
 * nothing. */
#define AOP_SAVE_EVERY_MS   (30u * 60u * 1000u)
#define AOP_FIRST_SAVE_MS   (5u * 60u * 1000u)

/* The header as file bytes. */
void aop_head_encode(uint32_t bytes, int64_t written_utc, uint8_t out[AOP_HEAD_BYTES]);

/* A file's first bytes; false unless it is the original's header with a
 * byte count from 1 to AOP_CAP. */
bool aop_head_decode(const uint8_t *buf, size_t len, uint32_t *bytes, int64_t *written_utc);

typedef enum {
    AOP_FRESH,          /* both times known, under AOP_MAX_AGE_H */
    AOP_AGE_UNKNOWN,    /* a time missing: pushed anyway, as the original */
    AOP_STALE,          /* over AOP_MAX_AGE_H: not pushed */
} aop_age_t;

/*
 * Whether a database written at `written_utc` is still worth pushing at
 * `now_utc` (either 0 when unknown); *age_h gets its age in hours, or -1.
 * With no trustworthy clock at either end it is pushed anyway: the
 * receiver checks what it is given, and the alternative is a cold start
 * (original aopRestore()).
 */
aop_age_t aop_age(int64_t written_utc, int64_t now_utc, double *age_h);

/*
 * Edit a CFG-NAVX5 payload, as the receiver reported it, to turn on
 * AssistNow Autonomous and ack-aiding -- the latter is what ends a
 * database poll with an MGA-ACK. NAVX5 has three versions of different
 * lengths, so the receiver's own is edited rather than one made (as
 * u-blox's drivers do). False if `plen` is too short to be one.
 */
bool aop_navx5_edit(uint8_t *p, uint16_t plen);

/* Whether to poll and save now: a good fix, no save running, five
 * minutes after boot for the first and thirty between the rest.
 * `last_ms` 0 for never. */
bool aop_save_due(bool fine, bool busy, uint32_t now_ms, uint32_t last_ms);

#ifdef __cplusplus
}
#endif

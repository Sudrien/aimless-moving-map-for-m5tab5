/*
 * lastfix.h -- where the receiver last had a good fix, kept on the card
 * so the next boot can start there (0021).
 *
 * The original's /lastfix.bin, byte for byte (original/tab5_map.cpp
 * lastFixSave() and lastFixLoad()), so a card that held one under it is
 * read here: "LFX1" as a little-endian u32, the latitude and longitude as
 * floats, four bytes of padding, and the time written as an int64 (0 when
 * the clock was not set) -- 24 bytes, as the ESP32 laid out its struct.
 * A float is good to about a metre here, which is far finer than this
 * is used for.
 *
 * What it is for: the map is drawn from it before the receiver has a
 * fix -- a cold start is thirty to ninety seconds -- and the palette
 * starts on the right side of sunset. No marker is drawn there: a
 * remembered position is a claim, not a measurement.
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

/* src: original/tab5_map.cpp struct LastFix. */
#define LASTFIX_BYTES   (24)

/* The record as file bytes. */
void lastfix_encode(double lat, double lon, int64_t utc, uint8_t out[LASTFIX_BYTES]);

/* A file's bytes; false unless it is the original's record with a
 * position that is one. */
bool lastfix_decode(const uint8_t *buf, size_t len, double *lat, double *lon);

#ifdef __cplusplus
}
#endif

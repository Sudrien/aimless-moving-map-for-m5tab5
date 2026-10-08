/*
 * ubx.h -- u-blox's binary protocol, as much of it as AssistNow
 * Autonomous needs (0028).
 *
 * The receiver talks NMEA, which is all the map needs, but assistance is
 * UBX only, and its replies arrive in the middle of the NMEA stream on
 * the same wire. This frames a message, recognises one arriving a byte
 * at a time, captures the ones a caller is waiting for, and walks a run
 * of saved frames. original/gnss.cpp's ubx_send(), ubx_feed() and
 * UbxCapture, as C with no UART and no clock in it, so it is tested on
 * the host; gnss.c owns the wire.
 *
 * One departure: the original never checked a received frame's checksum.
 * Here a frame that fails it is dropped, as is a saved frame that fails
 * it on the way back -- the receiver would reject either, and a
 * database saved from a corrupted read would be pushed into it at every
 * boot for three days.
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

/* src: original/gnss.cpp ubx_feed()'s frame[512]. Bigger than any
 * message this asks for: CFG-NAVX5 is 40 or 44 bytes of payload, an
 * MGA-DBD record under 200 (u-blox M8 protocol specification,
 * UBX-13003221, UBX-MGA-DBD). */
#define UBX_FRAME_MAX   (512)
/* Sync, class, id, length; then the checksum after the payload. */
#define UBX_HEAD        (6)
#define UBX_OVERHEAD    (8)

/* What ubx_feed() made of a byte. */
typedef enum {
    UBX_NOT = 0,    /* not UBX: the NMEA line assembler's */
    UBX_MORE,       /* part of a frame, consumed */
    UBX_FRAME,      /* the last byte of a frame with a good checksum:
                     * it is in p->frame, p->flen bytes, sync to checksum */
} ubx_byte_t;

typedef struct {
    int      st;
    uint16_t len, got;
    size_t   flen;
    uint8_t  frame[UBX_FRAME_MAX];
} ubx_parser_t;

void       ubx_parser_init(ubx_parser_t *p);
ubx_byte_t ubx_feed(ubx_parser_t *p, uint8_t c);

/* A whole frame -- sync, header, payload (NULL for a poll), checksum --
 * into `out`. Its length, or 0 if `cap` is too small. */
size_t ubx_frame(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len,
                 uint8_t *out, size_t cap);

/* How many bytes from the start of `b` are whole frames with good
 * checksums, and how many frames (`*count`, may be NULL). Stops at the
 * first thing that is not one. */
size_t ubx_frames(const uint8_t *b, size_t len, int *count);

/*
 * A capture: frames of class `cls` and id `id` (0xFF matches any) are
 * appended to `buf` until the frame `ack_cls`/`ack_id` arrives, which
 * sets `done`. original/gnss.cpp UbxCapture.
 */
typedef struct {
    uint8_t  cls, id;
    uint8_t  ack_cls, ack_id;
    uint8_t *buf;
    size_t   cap, len;
    uint32_t frames;
    volatile bool done;
    uint32_t last_ms;           /* when the last frame was kept */
} ubx_capture_t;

/* Offer a complete frame to a capture, at time `now_ms`. */
void ubx_capture_offer(ubx_capture_t *c, const uint8_t *frame, size_t flen, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

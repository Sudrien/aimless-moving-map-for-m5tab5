/*
 * ubx.c -- see ubx.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ubx.h"

#include <string.h>

/* src: u-blox M8 protocol specification (UBX-13003221), "UBX Frame
 * Structure": two sync characters, then class, id, little-endian length,
 * payload, and an 8-bit Fletcher checksum over class to payload. */
#define SYNC1   (0xB5)
#define SYNC2   (0x62)

static void checksum(const uint8_t *b, size_t n, uint8_t *a, uint8_t *k)
{
    uint8_t ca = 0, ck = 0;
    for (size_t i = 0; i < n; i++) { ca += b[i]; ck += ca; }
    *a = ca;
    *k = ck;
}

void ubx_parser_init(ubx_parser_t *p)
{
    p->st = 0;
    p->len = p->got = 0;
    p->flen = 0;
}

ubx_byte_t ubx_feed(ubx_parser_t *p, uint8_t c)
{
    switch (p->st) {
    case 0:
        if (c != SYNC1) return UBX_NOT;
        p->st = 1;
        return UBX_MORE;
    case 1:
        /* A false start: hand the byte back. The 0xB5 before it is
         * lost to NMEA, which is printable ASCII and never had one. */
        if (c != SYNC2) { p->st = 0; return UBX_NOT; }
        p->frame[0] = SYNC1;
        p->frame[1] = SYNC2;
        p->flen = 2;
        p->st = 2;
        return UBX_MORE;
    case 2:                                     /* class */
    case 3:                                     /* id */
    case 4:                                     /* length, low */
        p->frame[p->flen++] = c;
        p->st++;
        return UBX_MORE;
    case 5:
        p->frame[p->flen++] = c;
        p->len = (uint16_t)(p->frame[4] | (uint16_t)c << 8);
        p->got = 0;
        /* src: original ubx_feed(): a length this large means the stream
         * is not really UBX; drop it rather than overrun, and let NMEA
         * pick up again at the next '$'. */
        if (p->len > UBX_FRAME_MAX - UBX_OVERHEAD) { p->st = 0; return UBX_MORE; }
        p->st = p->len ? 6 : 7;
        return UBX_MORE;
    case 6:
        p->frame[p->flen++] = c;
        if (++p->got >= p->len) p->st = 7;
        return UBX_MORE;
    case 7:                                     /* checksum A */
        p->frame[p->flen++] = c;
        p->st = 8;
        return UBX_MORE;
    case 8: {                                   /* checksum B */
        p->frame[p->flen++] = c;
        p->st = 0;
        uint8_t a, k;
        checksum(p->frame + 2, 4u + p->len, &a, &k);
        return (a == p->frame[p->flen - 2] && k == c) ? UBX_FRAME : UBX_MORE;
    }
    }
    p->st = 0;
    return UBX_NOT;
}

size_t ubx_frame(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len,
                 uint8_t *out, size_t cap)
{
    const size_t n = (size_t)len + UBX_OVERHEAD;
    if (cap < n || (len && !payload)) return 0;
    out[0] = SYNC1;
    out[1] = SYNC2;
    out[2] = cls;
    out[3] = id;
    out[4] = (uint8_t)(len & 0xFF);
    out[5] = (uint8_t)(len >> 8);
    if (len) memcpy(out + UBX_HEAD, payload, len);
    checksum(out + 2, 4u + len, &out[UBX_HEAD + len], &out[UBX_HEAD + len + 1]);
    return n;
}

size_t ubx_frames(const uint8_t *b, size_t len, int *count)
{
    size_t off = 0;
    int n = 0;
    while (b && off + UBX_OVERHEAD <= len) {
        if (b[off] != SYNC1 || b[off + 1] != SYNC2) break;
        const size_t plen = (size_t)b[off + 4] | (size_t)b[off + 5] << 8;
        const size_t flen = plen + UBX_OVERHEAD;
        if (off + flen > len) break;            /* cut short */
        uint8_t a, k;
        checksum(b + off + 2, 4u + plen, &a, &k);
        if (a != b[off + UBX_HEAD + plen] || k != b[off + UBX_HEAD + plen + 1]) break;
        off += flen;
        n++;
    }
    if (count) *count = n;
    return off;
}

void ubx_capture_offer(ubx_capture_t *c, const uint8_t *frame, size_t flen, uint32_t now_ms)
{
    if (!c || c->done || flen < UBX_OVERHEAD) return;
    const uint8_t cls = frame[2], id = frame[3];
    /* src: original ubx_feed(): the terminating frame ends it and is not
     * kept; anything else must match what is asked for. */
    if (cls == c->ack_cls && id == c->ack_id) { c->done = true; return; }
    if (c->cls != 0xFF && cls != c->cls) return;
    if (c->id != 0xFF && id != c->id) return;
    /* A frame that does not fit is dropped whole: half a frame would make
     * everything saved after it unreadable. */
    if (c->len + flen > c->cap) return;
    memcpy(c->buf + c->len, frame, flen);
    c->len += flen;
    c->frames++;
    c->last_ms = now_ms;
}

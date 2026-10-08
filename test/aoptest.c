/*
 * aoptest.c -- main/ubx.c and main/aop.c: AssistNow Autonomous without
 * the receiver.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aop.h"
#include "ubx.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* original/tab5_map.cpp struct AopHeader, laid out by this host as by
 * the ESP32. */
struct AopHeader { uint32_t magic; uint32_t bytes; int64_t written_utc; };

/* Feed `n` bytes; the NMEA left over goes to `nmea`, frames to `c`. */
static int feed(ubx_parser_t *p, ubx_capture_t *c, const uint8_t *b, size_t n,
                char *nmea, size_t ncap)
{
    int frames = 0;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const ubx_byte_t u = ubx_feed(p, b[i]);
        if (u == UBX_FRAME) { frames++; ubx_capture_offer(c, p->frame, p->flen, 1000 + (uint32_t)i); }
        if (u == UBX_NOT && k + 1 < ncap) nmea[k++] = (char)b[i];
    }
    if (ncap) nmea[k] = 0;
    return frames;
}

int main(void)
{
    /* ---- framing, against a frame u-blox document ---- */
    /* src: u-blox M8 protocol specification: a UBX-CFG-RATE poll is
     * B5 62 06 08 00 00 0E 30 (the checksum the specification's
     * example and every u-center log show). */
    uint8_t f[64];
    CHECK(ubx_frame(0x06, 0x08, NULL, 0, f, sizeof f) == 8 &&
          !memcmp(f, "\xB5\x62\x06\x08\x00\x00\x0E\x30", 8), "CFG-RATE poll");
    /* MGA-DBD poll, its checksum worked by hand: A = 13+80 = 93, B =
     * 13+93+93+93 = CC (mod 100h). */
    CHECK(ubx_frame(0x13, 0x80, NULL, 0, f, sizeof f) == 8 &&
          !memcmp(f, "\xB5\x62\x13\x80\x00\x00\x93\xCC", 8), "MGA-DBD poll");
    const uint8_t rate[6] = { 0xE8, 0x03, 1, 0, 1, 0 };
    CHECK(ubx_frame(0x06, 0x08, rate, 6, f, sizeof f) == 14 &&
          f[12] == 0x01 && f[13] == 0x39, "CFG-RATE 1000 ms: %02X %02X", f[12], f[13]);
    CHECK(ubx_frame(0x06, 0x08, rate, 6, f, 13) == 0, "too small");

    /* ---- frames out of NMEA ---- */
    ubx_parser_t p;
    ubx_parser_init(&p);
    uint8_t cbuf[256];
    ubx_capture_t c = { .cls = 0x13, .id = 0x80, .ack_cls = 0x13, .ack_id = 0x60,
                        .buf = cbuf, .cap = sizeof cbuf };
    uint8_t stream[512];
    size_t n = 0;
    const char *s1 = "$GNRMC,123519,A*00\r\n";
    memcpy(stream + n, s1, strlen(s1)); n += strlen(s1);
    const uint8_t d1[5] = { 1, 2, 3, 4, 5 }, d2[3] = { 9, 8, 7 };
    const size_t r1 = ubx_frame(0x13, 0x80, d1, 5, stream + n, sizeof stream - n); n += r1;
    const char *s2 = "$GNGGA,1*00\r\n";
    memcpy(stream + n, s2, strlen(s2)); n += strlen(s2);
    n += ubx_frame(0x05, 0x01, d2, 2, stream + n, sizeof stream - n);   /* not asked for */
    const size_t r2 = ubx_frame(0x13, 0x80, d2, 3, stream + n, sizeof stream - n); n += r2;
    char nmea[256];
    CHECK(feed(&p, &c, stream, n, nmea, sizeof nmea) == 3, "three frames");
    CHECK(!strcmp(nmea, "$GNRMC,123519,A*00\r\n$GNGGA,1*00\r\n"), "NMEA untouched: %s", nmea);
    CHECK(c.frames == 2 && c.len == r1 + r2 && !c.done, "two captured, %u", c.frames);
    int cnt;
    CHECK(ubx_frames(cbuf, c.len, &cnt) == c.len && cnt == 2, "captured frames walk");
    /* The MGA-ACK ends it and is not kept; nothing after it is. */
    n = ubx_frame(0x13, 0x60, d1, 4, stream, sizeof stream);
    n += ubx_frame(0x13, 0x80, d1, 5, stream + n, sizeof stream - n);
    feed(&p, &c, stream, n, nmea, sizeof nmea);
    CHECK(c.done && c.frames == 2, "ack ends it");

    /* A bad checksum is dropped, and the parser carries on. */
    ubx_capture_t any = { .cls = 0xFF, .id = 0xFF, .ack_cls = 0xFF, .ack_id = 0xFF,
                          .buf = cbuf, .cap = sizeof cbuf };
    n = ubx_frame(0x13, 0x80, d1, 5, stream, sizeof stream);
    stream[n - 1] ^= 1;
    n += ubx_frame(0x13, 0x80, d2, 3, stream + n, sizeof stream - n);
    CHECK(feed(&p, &any, stream, n, nmea, 0) == 1 && any.frames == 1 &&
          any.buf[UBX_HEAD] == 9, "bad checksum dropped");
    /* A length past UBX_FRAME_MAX is not UBX: dropped, and NMEA after it
     * comes through. */
    const uint8_t huge[] = { 0xB5, 0x62, 0x13, 0x80, 0xFF, 0x7F };
    memcpy(stream, huge, sizeof huge);
    memcpy(stream + sizeof huge, "$GP*00\r\n", 8);
    feed(&p, &any, stream, sizeof huge + 8, nmea, sizeof nmea);
    CHECK(!strcmp(nmea, "$GP*00\r\n"), "after an oversized length: %s", nmea);
    /* 0xB5 then not 0x62: the second byte is NMEA's. */
    const uint8_t fs[] = { 0xB5, '$', 'G' };
    feed(&p, &any, fs, 3, nmea, sizeof nmea);
    CHECK(!strcmp(nmea, "$G"), "false start: %s", nmea);
    /* A zero-length frame. */
    n = ubx_frame(0x13, 0x80, NULL, 0, stream, sizeof stream);
    CHECK(feed(&p, &any, stream, n, nmea, 0) == 1, "empty payload");
    /* Full: a frame that does not fit is dropped whole. */
    uint8_t small[20];
    ubx_capture_t sm = { .cls = 0xFF, .id = 0xFF, .ack_cls = 0xFF, .ack_id = 0xFF,
                         .buf = small, .cap = sizeof small };
    n = ubx_frame(0x13, 0x80, d1, 5, stream, sizeof stream);
    n += ubx_frame(0x13, 0x80, d1, 5, stream + n, sizeof stream - n);
    feed(&p, &sm, stream, n, nmea, 0);
    CHECK(sm.frames == 1 && sm.len == 13, "full: %u frames, %zu bytes", sm.frames, sm.len);

    /* ---- walking a saved database ---- */
    n = ubx_frame(0x13, 0x80, d1, 5, stream, sizeof stream);
    n += ubx_frame(0x13, 0x80, d2, 3, stream + n, sizeof stream - n);
    CHECK(ubx_frames(stream, n, &cnt) == n && cnt == 2, "whole");
    CHECK(ubx_frames(stream, n - 1, &cnt) == 13 && cnt == 1, "cut short: the whole ones");
    stream[15] ^= 0x40;     /* inside the second frame's payload */
    CHECK(ubx_frames(stream, n, &cnt) == 13 && cnt == 1, "corrupt: up to it");
    CHECK(ubx_frames(NULL, 10, &cnt) == 0 && cnt == 0, "nothing");
    CHECK(ubx_frames((const uint8_t *)"$GNRMC,,", 8, NULL) == 0, "not UBX");

    /* ---- the original's /aopdb.bin header ---- */
    CHECK(sizeof(struct AopHeader) == AOP_HEAD_BYTES, "struct is %zu", sizeof(struct AopHeader));
    struct AopHeader o = { 0x414F5031u, 4321, 1791500720 };
    uint8_t h[AOP_HEAD_BYTES];
    uint32_t bytes;
    int64_t utc;
    CHECK(aop_head_decode((const uint8_t *)&o, sizeof o, &bytes, &utc) &&
          bytes == 4321 && utc == 1791500720, "the original's");
    aop_head_encode(4321, 1791500720, h);
    CHECK(!memcmp(h, &o, sizeof h), "written as the original wrote it");
    aop_head_encode(10, -5, h);
    CHECK(aop_head_decode(h, sizeof h, &bytes, &utc) && utc == 0, "no clock: 0");
    CHECK(!aop_head_decode(h, sizeof h - 1, &bytes, &utc), "short");
    o.bytes = 0;
    CHECK(!aop_head_decode((const uint8_t *)&o, sizeof o, &bytes, &utc), "empty");
    o.bytes = AOP_CAP + 1;
    CHECK(!aop_head_decode((const uint8_t *)&o, sizeof o, &bytes, &utc), "too big");
    o.bytes = 10; o.magic = 0x314F5041u;
    CHECK(!aop_head_decode((const uint8_t *)&o, sizeof o, &bytes, &utc), "wrong magic");

    /* ---- age ---- */
    double age;
    const int64_t t = 1791500720;
    CHECK(aop_age(t, t + 3600 * 5, &age) == AOP_FRESH && age == 5.0, "5 h");
    CHECK(aop_age(t, t + 3600 * 72, &age) == AOP_FRESH, "72 h exactly");
    CHECK(aop_age(t, t + 3600 * 73, &age) == AOP_STALE && age == 73.0, "73 h");
    CHECK(aop_age(0, t, &age) == AOP_AGE_UNKNOWN && age == -1.0, "not stamped");
    CHECK(aop_age(t, 0, &age) == AOP_AGE_UNKNOWN, "no clock now");
    CHECK(aop_age(t, 50000, &age) == AOP_AGE_UNKNOWN, "clock never set");

    /* ---- NAVX5 ---- */
    /* A v2 payload (40 bytes) with other settings in it. */
    uint8_t nx[44];
    memset(nx, 0xA5, sizeof nx);
    nx[0] = 2; nx[1] = 0;
    CHECK(aop_navx5_edit(nx, 40), "v2");
    CHECK(nx[2] == 0x00 && nx[3] == 0x44, "mask1 aop + ackAid: %02X%02X", nx[3], nx[2]);
    CHECK(nx[4] == 0 && nx[5] == 0 && nx[6] == 0xA5 && nx[7] == 0xA5, "mask2 low half only");
    CHECK(nx[17] == 1 && nx[27] == 1 && nx[30] == 0 && nx[31] == 0, "fields");
    CHECK(nx[0] == 2 && nx[16] == 0xA5 && nx[18] == 0xA5 && nx[26] == 0xA5 &&
          nx[28] == 0xA5 && nx[39] == 0xA5, "the rest untouched");
    CHECK(!aop_navx5_edit(nx, 31) && !aop_navx5_edit(NULL, 40), "too short");

    /* ---- when to save ---- */
    const uint32_t five = 5u * 60u * 1000u, thirty = 30u * 60u * 1000u;
    CHECK(!aop_save_due(true, false, five - 1, 0), "not in the first five minutes");
    CHECK(aop_save_due(true, false, five, 0), "then");
    CHECK(!aop_save_due(false, false, five, 0), "not without a good fix");
    CHECK(!aop_save_due(true, true, five, 0), "not while one runs");
    CHECK(!aop_save_due(true, false, 1000 + thirty - 1, 1000), "not again for thirty");
    CHECK(aop_save_due(true, false, 1000 + thirty, 1000), "then again");
    CHECK(aop_save_due(true, false, 100, 0xFFFFFFFFu - thirty + 50), "across the wrap");

    printf("aoptest: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}

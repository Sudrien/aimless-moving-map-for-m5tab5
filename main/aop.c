/*
 * aop.c -- see aop.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "aop.h"

/* src: original/tab5_map.cpp AOP_MAGIC, ASCII "AOP1" -- the original's
 * own container, not u-blox's. */
#define AOP_MAGIC   (0x414F5031u)

/* src: original/tab5_map.cpp aopRestore(): a clock reading under this is
 * one that was never set. */
#define CLOCK_SET   (100000)

static uint64_t get_le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++) { p[i] = (uint8_t)v; v >>= 8; }
}

void aop_head_encode(uint32_t bytes, int64_t written_utc, uint8_t out[AOP_HEAD_BYTES])
{
    put_le(out, AOP_MAGIC, 4);
    put_le(out + 4, bytes, 4);
    put_le(out + 8, (uint64_t)(written_utc > 0 ? written_utc : 0), 8);
}

bool aop_head_decode(const uint8_t *buf, size_t len, uint32_t *bytes, int64_t *written_utc)
{
    if (!buf || len < AOP_HEAD_BYTES || get_le(buf, 4) != AOP_MAGIC) return false;
    const uint32_t n = (uint32_t)get_le(buf + 4, 4);
    /* src: original aopRestore(): nothing, or more than it would ever
     * have saved, is not a database. */
    if (n == 0 || n > AOP_CAP) return false;
    *bytes = n;
    *written_utc = (int64_t)get_le(buf + 8, 8);
    return true;
}

aop_age_t aop_age(int64_t written_utc, int64_t now_utc, double *age_h)
{
    *age_h = -1.0;
    if (written_utc <= 0 || now_utc <= CLOCK_SET) return AOP_AGE_UNKNOWN;
    const double h = (double)(now_utc - written_utc) / 3600.0;
    if (h > AOP_MAX_AGE_H) { *age_h = h; return AOP_STALE; }
    *age_h = h;
    return AOP_FRESH;
}

bool aop_navx5_edit(uint8_t *p, uint16_t plen)
{
    /* src: original gnss_enable_aop(): the reply is at least 32 bytes of
     * payload in every version, and aopOrbMaxErr is bytes 30-31. */
    if (!p || plen < 32) return false;
    /* src: original gnss_enable_aop(), after u-blox M8 protocol
     * specification UBX-CFG-NAVX5: mask1 selects which fields the
     * receiver applies -- a byte written without its bit changes nothing,
     * which the original found when ackAiding was written and silently
     * ignored. Bit 10 (0x0400) ackAid, bit 14 (0x4000) aop; nothing else.
     * mask1 is bytes 2-3, little-endian; the low half of mask2 (bytes
     * 4-5) is cleared, so nothing it selects is applied either. */
    p[2] = 0x00;
    p[3] = 0x44;
    p[4] = p[5] = 0x00;
    p[17] = 1;          /* ackAiding */
    p[27] = 1;          /* aopCfg: AssistNow Autonomous on */
    p[30] = p[31] = 0;  /* aopOrbMaxErr 0: the firmware's default */
    return true;
}

bool aop_save_due(bool fine, bool busy, uint32_t now_ms, uint32_t last_ms)
{
    if (busy || !fine) return false;
    if (last_ms) return now_ms - last_ms >= AOP_SAVE_EVERY_MS;
    return now_ms >= AOP_FIRST_SAVE_MS;
}

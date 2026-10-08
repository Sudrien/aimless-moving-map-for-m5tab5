/*
 * launcherkey.h -- M5Launcher's saved Wi-Fi passwords, decrypted, with
 * no ESP-IDF in it.
 *
 * Under M5Launcher, this program and Launcher each keep a network list:
 * defeatist's NVS one here (wifistore.h), and an encrypted config.conf on
 * the card for Launcher. launcher_import.c reads Launcher's into ours so a
 * password typed into Launcher is not typed again. This file is the part
 * of that which handles bytes: base64, AES-128-CBC, and finding the key.
 *
 * WHY THE KEY IS FOUND, NOT STORED
 *
 * Launcher encrypts each password with a key fixed at build time from a
 * CI secret, so it is in Launcher's firmware image and not in its source.
 * A captured copy would stop working the day the secret is rotated. So
 * every printable run in Launcher's app partition is a candidate, and
 * the right one is the one that decrypts a config.conf entry to valid
 * PKCS#7 over printable text. That needs no known plaintext and survives
 * a rotation.
 *
 * The shape, from bmorcelli/Launcher src/wifi_crypto.cpp, by way of
 * defeatist-music-player-for-m5tab5's main/launcher_import.c at 74535ca,
 * whose crypto core this is:
 *
 *   key   = the build's key, repeated or cut to 16 bytes
 *   IV    = "LauncherWifiKey!", fixed -- and so also a marker for which
 *           partition is Launcher's
 *   pwd   = base64(AES-128-CBC(passphrase + PKCS#7))
 *
 * WHAT CHANGED FROM DEFEATIST'S
 *
 * Its scan flushed the printable run at the end of every 4 KB read, so a
 * key that crossed a read boundary was tried only in pieces and never
 * found -- about one image in 4096 / the key's length. lkey_scan() keeps
 * the run across calls and flushes it only at a byte that is not
 * printable, or at lkey_scan_end(). The same candidates are tried, in
 * the same order, with the same oracle.
 *
 * And its base64 decoder shifted an int left six bits a character
 * without ever clearing it, which overflows -- undefined -- by the sixth
 * character of every value. lkey_b64_decode() keeps the 24 bits it uses.
 * And its buffers held 64 bytes of ciphertext, so a PSK saved in Launcher
 * as 64 hex digits -- 80 bytes encrypted -- was skipped.
 *
 * AES is decrypt-only and vendored, as defeatist's is: Mbed TLS 4 (IDF
 * 6.1) dropped mbedtls/aes.h, and this cannot break on a version bump.
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

/* src: defeatist launcher_import.c LI_MARKER and LI_IV. Launcher's IV;
 * a partition with these bytes in it is Launcher's. */
#define LKEY_MARKER     "LauncherWifiKey!"
#define LKEY_MARKER_LEN (16)

/* src: defeatist launcher_import.c LI_MIN_CAND / LI_MAX_CAND. The
 * shortest and longest printable run tried as a key. */
#define LKEY_MIN_CAND   (8)
#define LKEY_MAX_CAND   (64)

/* The longest ciphertext: a 64-hex PSK is four blocks, and PKCS#7 adds
 * a fifth whole one to a value that fills its last. src: WPA's 64-digit
 * PSK; defeatist's 64-byte buffers refused it (see above). */
#define LKEY_CT_MAX     (80)

/*
 * Decode base64 into `out`. Characters outside the alphabet are skipped
 * (config.conf may wrap lines), and decoding stops at '='. Returns the
 * number of bytes, or -1 if they do not fit.
 */
int lkey_b64_decode(const char *s, uint8_t *out, size_t cap);

/*
 * Decrypt `ct` (`n` bytes, a whole number of blocks) with `key` and
 * Launcher's IV into `out`, which needs n + 1 bytes. Returns the
 * plaintext's length with a terminator after it, or -1 when the padding
 * is not PKCS#7 or what it covers is not printable ASCII -- which, for a
 * candidate key, is how a wrong one is told from the right one.
 */
int lkey_decrypt(const uint8_t key[16], const uint8_t *ct, size_t n,
                 uint8_t *out, size_t out_cap);

/* Up to two of config.conf's ciphertexts, to try candidate keys on. */
typedef struct {
    uint8_t ct[2][LKEY_CT_MAX];
    size_t  len[2];
    int     n;
} lkey_oracle_t;

/* Add one base64 `pwd` to the oracle. False, and nothing added, if it
 * is full or the value is not a whole number of blocks. */
bool lkey_oracle_add(lkey_oracle_t *o, const char *b64);

/*
 * Whether `key` decrypts the oracle. With two ciphertexts both must
 * decrypt; with one, its plaintext must also be at least 8 characters,
 * WPA's floor, since one block of random padding passes PKCS#7 about one
 * time in 256.
 */
bool lkey_key_ok(const uint8_t key[16], const lkey_oracle_t *o);

/* A key from a printable run: repeated or cut to 16 bytes. */
void lkey_build_key(const uint8_t *raw, size_t len, uint8_t key[16]);

/*
 * The search, fed a partition a piece at a time.
 *
 * lkey_scan() takes the next `n` bytes and returns true as soon as a key
 * is found, with it in `key`; lkey_scan_end() flushes the last run. The
 * state is small and holds the run that is open across calls, so a key
 * that straddles two reads is found.
 */
typedef struct {
    const lkey_oracle_t *o;
    uint8_t run[LKEY_MAX_CAND];
    size_t  rl;
    bool    found;
    uint8_t key[16];
} lkey_scan_t;

void lkey_scan_init(lkey_scan_t *s, const lkey_oracle_t *o);
bool lkey_scan(lkey_scan_t *s, const uint8_t *buf, size_t n);
bool lkey_scan_end(lkey_scan_t *s);

/*
 * What a decrypted value is to wifistore: 64 hex digits are a PSK, 8 to
 * 63 characters a passphrase. False for anything else.
 */
bool lkey_classify(const char *s, bool *is_psk);

#ifdef __cplusplus
}
#endif

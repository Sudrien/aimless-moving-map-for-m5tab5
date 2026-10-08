/*
 * launcherkeytest.c -- main/launcherkey.c: M5Launcher's encrypted
 * passwords, and the search for the key in a firmware image.
 *
 * The ciphertexts were made with OpenSSL, not with this code:
 *
 *   K=$(printf 'ExampleKey12Exam' | od -An -tx1 | tr -d ' \n')
 *   IV=$(printf 'LauncherWifiKey!' | od -An -tx1 | tr -d ' \n')
 *   printf '%s' "$pw" | openssl enc -aes-128-cbc -K $K -iv $IV | base64 -w0
 *
 * "ExampleKey12" is a made-up build key, twelve characters so the
 * repeat-to-16 rule is exercised. No real Launcher key is in this file.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcherkey.h"

static int checks, failures;

#define CHECK(cond, ...) do {                                   \
    checks++;                                                   \
    if (!(cond)) {                                              \
        failures++;                                             \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);           \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

#define BUILD_KEY   "ExampleKey12"
#define CT_HUNTER   "T8HI33bsm9pxeF7/yBd9bw=="                       /* hunter2hunter2 */
#define CT_HORSE    "hHGGOlQOrnCQCi9S/Ksp8iMZxZyIoUaFAAeYaGeYm9k="   /* correct horse battery staple */
#define CT_SHORT    "No9vzR8kilq/oXKka65hwg=="                       /* shortpw! */
#define CT_X        "Pa9WHw6POcI/fdnnNLYxGQ=="                       /* x */
#define CT_PSK      "3F1kj27r37usbNXc9PnNJmsH7cBJ3J+2ksL9iQKgV7ImI0sfQNJZliAj7bgP6GGc" \
                    "1YQnoHlKBIn2Tcn/HqyA1bhhvKDXc+hgTa3DuvnyvPc="
#define PT_PSK      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static uint32_t s_rng = 12345;
static uint8_t rnd(void) { s_rng = s_rng * 1103515245u + 12345u; return (uint8_t)(s_rng >> 16); }

/* Random bytes with no printable run of LKEY_MIN_CAND or more, so the
 * only key candidates are the ones a test puts in. */
static void noise(uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        b[i] = rnd();
        if (i % 6 == 5) b[i] = 0;
    }
}

/* Feed `img` to a scanner in `chunk`-byte pieces, as a partition read. */
static bool scan_in(const lkey_oracle_t *o, const uint8_t *img, size_t n, size_t chunk,
                    uint8_t key[16])
{
    lkey_scan_t s;
    lkey_scan_init(&s, o);
    bool found = false;
    for (size_t off = 0; off < n && !found; off += chunk)
        found = lkey_scan(&s, img + off, n - off < chunk ? n - off : chunk);
    if (!found) found = lkey_scan_end(&s);
    if (found) memcpy(key, s.key, 16);
    return found;
}

static int decrypt_b64(const uint8_t key[16], const char *b64, char *out, size_t cap)
{
    uint8_t ct[LKEY_CT_MAX];
    const int n = lkey_b64_decode(b64, ct, sizeof ct);
    if (n <= 0) return -1;
    return lkey_decrypt(key, ct, (size_t)n, (uint8_t *)out, cap);
}

int main(void)
{
    uint8_t key[16];
    char pt[LKEY_CT_MAX + 1];

    printf("base64: wrapped lines, padding, and nothing past the buffer\n");
    {
        uint8_t o[32];
        CHECK(lkey_b64_decode("aGVsbG8=", o, sizeof o) == 5 && memcmp(o, "hello", 5) == 0, "hello");
        CHECK(lkey_b64_decode("aGVs\nbG8gd29y\r\nbGQ=", o, sizeof o) == 11 &&
              memcmp(o, "hello world", 11) == 0, "wrapped");
        CHECK(lkey_b64_decode("aGVsbG8=", o, 4) == -1, "overflow not refused");
        CHECK(lkey_b64_decode("", o, sizeof o) == 0, "empty");
        /* Long input: the accumulator must not overflow (UBSan checks). */
        static char longb64[4001];
        memset(longb64, '/', 4000);
        uint8_t big[3000];
        CHECK(lkey_b64_decode(longb64, big, sizeof big) == 3000 && big[2999] == 0xff, "long");
    }

    printf("the build key is repeated or cut to sixteen bytes\n");
    lkey_build_key((const uint8_t *)BUILD_KEY, strlen(BUILD_KEY), key);
    CHECK(memcmp(key, "ExampleKey12Exam", 16) == 0, "repeat");
    {
        uint8_t k2[16];
        lkey_build_key((const uint8_t *)"0123456789abcdefXYZ", 19, k2);
        CHECK(memcmp(k2, "0123456789abcdef", 16) == 0, "cut");
    }

    printf("decrypting with the right key matches OpenSSL\n");
    CHECK(decrypt_b64(key, CT_HUNTER, pt, sizeof pt) == 14 && strcmp(pt, "hunter2hunter2") == 0, "hunter");
    CHECK(decrypt_b64(key, CT_HORSE, pt, sizeof pt) == 28 &&
          strcmp(pt, "correct horse battery staple") == 0, "horse");
    CHECK(decrypt_b64(key, CT_PSK, pt, sizeof pt) == 64 && strcmp(pt, PT_PSK) == 0,
          "a 64-hex PSK is five blocks and must fit");

    printf("a wrong key does not decrypt\n");
    {
        uint8_t bad[16];
        lkey_build_key((const uint8_t *)"ExampleKey13", 12, bad);
        CHECK(decrypt_b64(bad, CT_HORSE, pt, sizeof pt) < 0, "wrong key decrypted");
        int passed = 0;
        for (int t = 0; t < 20000; t++) {
            for (int i = 0; i < 16; i++) bad[i] = rnd();
            passed += decrypt_b64(bad, CT_HORSE, pt, sizeof pt) >= 0;
        }
        CHECK(passed == 0, "%d random keys passed a two-block oracle", passed);
        CHECK(decrypt_b64(key, CT_HUNTER, pt, 16) < 0, "no room for the terminator");
    }

    printf("the oracle: two ciphertexts, or one of at least eight characters\n");
    {
        lkey_oracle_t o = { 0 };
        CHECK(lkey_oracle_add(&o, CT_HUNTER) && lkey_oracle_add(&o, CT_HORSE), "add");
        CHECK(!lkey_oracle_add(&o, CT_SHORT), "a third was taken");
        CHECK(lkey_key_ok(key, &o), "right key refused");

        lkey_oracle_t one = { 0 };
        CHECK(lkey_oracle_add(&one, CT_SHORT) && lkey_key_ok(key, &one), "8 characters");
        lkey_oracle_t tiny = { 0 };
        CHECK(lkey_oracle_add(&tiny, CT_X) && !lkey_key_ok(key, &tiny),
              "a one-character value is not enough to trust a key");

        lkey_oracle_t junk = { 0 };
        CHECK(!lkey_oracle_add(&junk, "aGVsbG8="), "not whole blocks");
        CHECK(!lkey_oracle_add(&junk, ""), "empty");
        CHECK(junk.n == 0 && !lkey_key_ok(key, &junk), "empty oracle passes");
    }

    printf("the key is found in an image, wherever the reads fall\n");
    {
        lkey_oracle_t o = { 0 };
        lkey_oracle_add(&o, CT_HUNTER);
        lkey_oracle_add(&o, CT_HORSE);

        enum { N = 3 * 4096 };
        uint8_t *img = malloc(N);
        static const char *const before[] = { "", "printable junk before ", "x" };
        int misses = 0, runs = 0;
        for (size_t b = 0; b < sizeof before / sizeof before[0]; b++) {
            /* Every position across the first read boundary, so the key
             * starts before it, straddles it, and starts after it. */
            for (size_t at = 4096 - 40; at < 4096 + 4; at++) {
                noise(img, N);
                memcpy(img + 100, LKEY_MARKER, LKEY_MARKER_LEN);
                const size_t bl = strlen(before[b]);
                memcpy(img + at - bl, before[b], bl);
                memcpy(img + at, BUILD_KEY, strlen(BUILD_KEY));
                img[at + strlen(BUILD_KEY)] = 0;
                uint8_t got[16];
                runs++;
                if (!scan_in(&o, img, N, 4096, got) || memcmp(got, key, 16) != 0) misses++;
            }
        }
        CHECK(misses == 0, "%d of %d placements missed", misses, runs);

        /* The key as the very last bytes: only lkey_scan_end() sees it. */
        noise(img, N);
        memcpy(img + N - strlen(BUILD_KEY), BUILD_KEY, strlen(BUILD_KEY));
        uint8_t got[16];
        CHECK(scan_in(&o, img, N, 4096, got) && memcmp(got, key, 16) == 0, "at the end");

        /* One byte at a time is the same search. */
        CHECK(scan_in(&o, img, N, 1, got) && memcmp(got, key, 16) == 0, "bytewise");

        /* No key in the image. */
        noise(img, N);
        memcpy(img + 500, "ExampleKey13", 12);
        CHECK(!scan_in(&o, img, N, 4096, got), "found a key that is not there");

        /* A run longer than LKEY_MAX_CAND is cut there and carries on,
         * as defeatist's did: a key after 64 printable bytes is found. */
        noise(img, N);
        memset(img + 200, 'q', LKEY_MAX_CAND);
        memcpy(img + 200 + LKEY_MAX_CAND, BUILD_KEY, strlen(BUILD_KEY));
        img[200 + LKEY_MAX_CAND + strlen(BUILD_KEY)] = 0;
        CHECK(scan_in(&o, img, N, 4096, got) && memcmp(got, key, 16) == 0, "after a long run");
        free(img);
    }

    printf("what wifistore is given: PSK, passphrase, or nothing\n");
    {
        bool psk = true;
        CHECK(lkey_classify("hunter2hunter2", &psk) && !psk, "passphrase");
        CHECK(lkey_classify(PT_PSK, &psk) && psk, "psk");
        CHECK(!lkey_classify("short", &psk) && !psk, "7 characters");
        char g[65];
        memset(g, 'g', 64); g[64] = 0;
        CHECK(!lkey_classify(g, &psk), "64 characters, not hex");
        memset(g, 'a', 63); g[63] = 0;
        CHECK(lkey_classify(g, &psk) && !psk, "63");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

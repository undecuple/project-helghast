/* The player's EBOOT.elf linked into kz3.exe (release step 9s).
 *
 * The release's first-run build (tools/release/firstrun.py) configures with -DKZ3_EMBED_EBOOT=<the EBOOT.elf it
 * lifted>; kz3_eboot_blob.c then carries that file's bytes (.incbin) and the SHA-256 CMake took of it at configure
 * time. At start the exe hashes the bytes it carries and loads them only when the two agree, so the lifted code and
 * the EBOOT it runs against are always the same pair. The exe is built on the player's machine from their own dump
 * and never shared; nothing from the game enters the repository. A developer build has no embedded EBOOT
 * (kz3_embedded_eboot returns NULL) and keeps taking argv[1] / kz3.ini / the EBOOT beside the exe.
 *
 * Header-only: a small SHA-256 (FIPS 180-4) and the known decrypted EBOOT hashes (numbers; the same values as
 * tools/release/eboot_sha256.json, test_kz3_embed.py keeps them equal). */
#ifndef KZ3_EMBED_H
#define KZ3_EMBED_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -> the expected SHA-256 (64 lower-case hex digits) and the embedded bytes, or NULL when this exe has none. */
const char* kz3_embedded_eboot(const unsigned char** data, size_t* size);

#ifdef __cplusplus
}
#endif

typedef struct {
    const char* ver;
    const char* sha256;
} kz3_known_eboot_t;
static const kz3_known_eboot_t kz3_known_eboots[] = {
    {"1.00", "faaa30432b7fec8529c25558ededc0568b6d630ec5c93842ae17b89b6a0bf611"},
    {"1.14", "7ad9e4fe357469c15ad61091a5368035957436bd361f388670fc8b9d808bf7e4"},
};

/* -> "1.00" / "1.14" for a known decrypted EBOOT, else "unknown". */
static inline const char* kz3_eboot_version_of(const char* hex)
{
    for (size_t i = 0; i < sizeof kz3_known_eboots / sizeof kz3_known_eboots[0]; i++)
        if (strcmp(hex, kz3_known_eboots[i].sha256) == 0) return kz3_known_eboots[i].ver;
    return "unknown";
}

static inline uint32_t kz3_sha_ror(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static inline void kz3_sha256_block(uint32_t h[8], const unsigned char* p)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 16; i++)
        w[i] =
            ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = kz3_sha_ror(w[i - 15], 7) ^ kz3_sha_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = kz3_sha_ror(w[i - 2], 17) ^ kz3_sha_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        const uint32_t t1 =
            hh + (kz3_sha_ror(e, 6) ^ kz3_sha_ror(e, 11) ^ kz3_sha_ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        const uint32_t t2 =
            (kz3_sha_ror(a, 2) ^ kz3_sha_ror(a, 13) ^ kz3_sha_ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

/* SHA-256 of data[0..n) -> 64 lower-case hex digits + NUL in hex[65]. */
static inline void kz3_sha256_hex(const void* data, size_t n, char hex[65])
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const unsigned char* p = (const unsigned char*)data;
    size_t left = n;
    for (; left >= 64; left -= 64, p += 64) kz3_sha256_block(h, p);
    unsigned char tail[128] = {0};
    memcpy(tail, p, left);
    tail[left] = 0x80;
    const size_t tl = left < 56 ? 64 : 128;
    const uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (unsigned char)(bits >> (8 * i));
    kz3_sha256_block(h, tail);
    if (tl == 128) kz3_sha256_block(h, tail + 64);
    static const char dig[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        const unsigned char byte = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
        hex[2 * i] = dig[byte >> 4];
        hex[2 * i + 1] = dig[byte & 15];
    }
    hex[64] = 0;
}

#endif /* KZ3_EMBED_H */

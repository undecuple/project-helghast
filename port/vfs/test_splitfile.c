/* Host test for kz3_splitfile.h (public issue 4: FAT32 dump parts data_ps3.psarc.66600...). No game data: small fake
 * parts of uneven size. Build/run: port/vfs/test_splitfile.sh */
#include "kz3_splitfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("[ok]   " __VA_ARGS__); else { fails++; printf("[FAIL] " __VA_ARGS__); } printf("\n"); } while (0)

static void put(const char* path, const unsigned char* d, size_t n)
{
    FILE* f = fopen(path, "wb"); if (!f) { perror(path); exit(2); }
    fwrite(d, 1, n, f); fclose(f);
}

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    enum { N = 5000 };
    static unsigned char all[N], got[N];
    for (int i = 0; i < N; i++) all[i] = (unsigned char)((i * 31u + (i >> 8) * 7u) ^ 0x5a);
    char base[1200], p[1300];
    snprintf(base, sizeof base, "%s/data_ps3.psarc", argv[1]);

    /* A: parts of different sizes (1000, 1500, 64, 2436); the sizes come from the files, not from a part size. */
    const size_t sz[] = { 1000, 1500, 64, 2436 };
    size_t off = 0;
    for (int i = 0; i < 4; i++) { snprintf(p, sizeof p, "%s.%d", base, 66600 + i); put(p, all + off, sz[i]); off += sz[i]; }
    kz3_sf* s = kz3_sf_open(base);
    CHECK(s && s->nparts == 4 && kz3_sf_size(s) == N, "four parts open as one file of %d bytes", N);
    if (!s) return 1;
    CHECK(kz3_sf_read(s, got, N) == N && !memcmp(got, all, N), "whole file read in one call equals the joined bytes");
    CHECK(kz3_sf_read(s, got, 10) == 0, "read at the end returns 0");
    int bad = 0;
    const size_t bounds[] = { 1000, 2500, 2564 };
    for (int b = 0; b < 3; b++)
        for (int back = 1; back <= 40; back += 13)
            for (int len = 2; len <= 3000; len += 997) {
                size_t o = bounds[b] - back;
                if (o + len > N) continue;
                kz3_sf_seek(s, (int64_t)o);
                memset(got, 0, sizeof got);
                if (kz3_sf_read(s, got, len) != (size_t)len || memcmp(got, all + o, len)) bad++;
            }
    CHECK(!bad, "reads crossing each boundary (incl. spanning a whole 64-byte part) match the joined bytes");
    kz3_sf_seek(s, 4990); memset(got, 0, 20);
    CHECK(kz3_sf_read(s, got, 20) == 10 && !memcmp(got, all + 4990, 10), "read past the end is short");
    bad = 0;
    for (int i = 0; i < 200; i++) {                       /* random-access pattern like the archive reader */
        size_t o = (size_t)((i * 7919u) % (N - 100)); size_t len = 1 + (i * 131u) % 100;
        kz3_sf_seek(s, (int64_t)o);
        if (kz3_sf_read(s, got, len) != len || memcmp(got, all + o, len)) bad++;
    }
    CHECK(!bad, "200 random small reads match");
    kz3_sf_close(s);

    /* B: a gap (.66602 removed) ends the set at the first missing number: only 2 parts, never skipping ahead. */
    snprintf(p, sizeof p, "%s.%d", base, 66602); remove(p);
    s = kz3_sf_open(base);
    CHECK(s && s->nparts == 2 && kz3_sf_size(s) == 2500, "a gap ends the set (2 parts, 2500 bytes)");
    kz3_sf_close(s);

    /* C: the plain file wins over parts. */
    put(base, all, 123);
    s = kz3_sf_open(base);
    CHECK(s && s->nparts == 1 && kz3_sf_size(s) == 123, "the single file is used when it exists");
    kz3_sf_close(s);
    remove(base);

    /* D: parts without .66600 (starting at .66601) are not a set; nothing at all is NULL. */
    snprintf(p, sizeof p, "%s.%d", base, 66600); remove(p);
    s = kz3_sf_open(base);
    CHECK(s == NULL, "parts not starting at .66600 are not opened");
    snprintf(p, sizeof p, "%s.%d", base, 66601); remove(p);
    CHECK(kz3_sf_open(base) == NULL, "no file and no parts gives NULL");
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}

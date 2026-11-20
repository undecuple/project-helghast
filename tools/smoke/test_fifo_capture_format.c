/* Host test for KZ3 patch 2164: the FIFO capture file format (libs/video/rsx_fifo_capture.h). Writes a capture holding one
 * record of every type, reads it back and compares every field; then the negative controls, each of which the reader must
 * reject: one flipped payload byte (checksum), a dropped record (sequence break), a truncated file, a file without its END
 * record, a bad magic, a newer version. Also the RANGE byte policy (bytes on first sight and when the digest changes, digest
 * only otherwise and for with_bytes = 0, a NULL pointer flag). No game data, no toolkit sources beyond the header.
 *   usage: test_fifo_capture_format <scratch dir> */
#include "rsx_fifo_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf(" (line %d)\n", __LINE__); fails++; } } while (0)

static uint8_t* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc(*n ? *n : 1);
    if (fread(b, 1, *n, f) != *n) { free(b); fclose(f); return NULL; }
    fclose(f); return b;
}
static void spit(const char* path, const uint8_t* b, size_t n) { FILE* f = fopen(path, "wb"); fwrite(b, 1, n, f); fclose(f); }
/* read a file to its end: returns the code of the first non-record result (0 clean end, <0 error) and the number of records */
static int drain(const char* path, int* nrec)
{
    rsx_fc_reader r; rsx_fc_rec rec; int rc = rsx_fc_r_open(&r, path);
    *nrec = 0;
    if (rc) return rc - 100;                   /* open errors map to -101..-103 */
    while ((rc = rsx_fc_r_next(&r, &rec)) == 1) (*nrec)++;
    rsx_fc_r_close(&r);
    return rc;
}

int main(int argc, char** argv)
{
    char p[512], q[512];
    if (argc < 2) { puts("usage: test_fifo_capture_format <scratch dir>"); return 2; }
    snprintf(p, sizeof p, "%s/good.kz3fc", argv[1]);
    uint8_t mem[256];
    for (int i = 0; i < 256; i++) mem[i] = (uint8_t)(i * 7 + 3);

    /* ---- write ---- */
    rsx_fc_writer w;
    CHECK(rsx_fc_w_open(&w, p, 5, 3) == 0, "open for writing");
    const uint32_t cfg[3] = { 0xC0000000u, 0x0F900000u, 0xDEADBEEFu };
    rsx_fc_w_words(&w, RSX_FC_CONFIG, cfg, 3);
    { uint8_t h[12]; rsx_fc_put32(h, RSX_FC_EA); rsx_fc_put32(h + 4, 0x10000u); rsx_fc_put32(h + 8, 64);
      rsx_fc_w_rec2(&w, RSX_FC_IMAGE, h, sizeof h, mem, 64); }
    { const uint32_t v[4] = { 0x100u, 0x80u, 0u, 1u }; rsx_fc_w_words(&w, RSX_FC_PASS, v, 4); }
    { const uint32_t v[5] = { 0x40u, 0x00040064u, 1u, 0x4000u, 0u }; rsx_fc_w_words(&w, RSX_FC_CMD, v, 4); }
    CHECK(rsx_fc_w_range(&w, RSX_FC_RD_RENDERER, RSX_FC_MAIN, 0x1000, mem, 128, 1) == 1, "first sight writes bytes");
    CHECK(rsx_fc_w_range(&w, RSX_FC_RD_RENDERER, RSX_FC_MAIN, 0x1000, mem, 128, 1) == 0, "same bytes again: digest only");
    mem[5] ^= 0xFF;
    CHECK(rsx_fc_w_range(&w, RSX_FC_RD_RENDERER, RSX_FC_MAIN, 0x1000, mem, 128, 1) == 1, "changed bytes: written again");
    CHECK(rsx_fc_w_range(&w, RSX_FC_WR_WALKER, RSX_FC_EA, 0x2000, mem, 64, 0) == 0, "with_bytes 0: digest only");
    CHECK(rsx_fc_w_range(&w, RSX_FC_RD_WALKER, RSX_FC_EA, 0x2000, mem, 64, 1) == 1, "a digest-only record must not hide the first bytes");
    CHECK(rsx_fc_w_range(&w, RSX_FC_RD_RENDERER, RSX_FC_LOCAL, 0x9000, NULL, 32, 1) == 0, "NULL pointer: no bytes");
    { const uint32_t v[4] = { 1u, 3u, 0x00D00400u, 0xC0DE0001u }; rsx_fc_w_words(&w, RSX_FC_WORD, v, 4); }
    { const uint32_t v[12] = { 0u, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }; rsx_fc_w_words(&w, RSX_FC_COPY, v, 12); }
    { const uint32_t v[3] = { 1u, 0u, 7u }; rsx_fc_w_words(&w, RSX_FC_FLIP, v, 3); }
    { const uint32_t v[5] = { 1u, 0x2000u, 0x100u, 1u, 77u }; rsx_fc_w_words(&w, RSX_FC_SPU_NOTE, v, 5); }
    { const uint32_t v[3] = { 4u, 9u, 0u }; rsx_fc_w_words(&w, RSX_FC_DRAIN, v, 3); }
    { uint8_t e[24] = {0}; rsx_fc_put32(e, 0x120u); strcpy((char*)e + 8, "caught-up"); rsx_fc_w_rec(&w, RSX_FC_PASS_END, e, sizeof e); }
    { const uint32_t v[2] = { 7u, 0u }; rsx_fc_w_words(&w, RSX_FC_FRAME, v, 2); }
    const unsigned long long want_recs = w.seq;
    CHECK(rsx_fc_w_close(&w) == 0, "close");

    /* ---- read back every record ---- */
    rsx_fc_reader r; rsx_fc_rec rec; int rc, k = 0;
    CHECK(rsx_fc_r_open(&r, p) == 0, "open for reading");
    CHECK(r.version == RSX_FC_VERSION && r.frame_start == 5 && r.frame_count == 3, "header v%u %u,%u", r.version, r.frame_start, r.frame_count);
    mem[5] ^= 0xFF;   /* back to the first-sight content for the first RANGE, the changed one is checked below */
    unsigned n_range = 0, n_bytes = 0, n_null = 0;
    while ((rc = rsx_fc_r_next(&r, &rec)) == 1) {
        CHECK(rec.seq == (uint64_t)k, "seq %llu at %d", (unsigned long long)rec.seq, k);
        switch (k) {
        case 0: CHECK(rec.type == RSX_FC_CONFIG && rec.len == 12 && rsx_fc_get32(rec.p + 8) == 0xDEADBEEFu, "CONFIG"); break;
        case 1: CHECK(rec.type == RSX_FC_IMAGE && rec.len == 12 + 64 && rsx_fc_get32(rec.p + 4) == 0x10000u && rec.p[12 + 1] == (uint8_t)(1 * 7 + 3), "IMAGE"); break;
        case 2: CHECK(rec.type == RSX_FC_PASS && rec.len == 16 && rsx_fc_get32(rec.p + 12) == 1u, "PASS"); break;
        case 3: CHECK(rec.type == RSX_FC_CMD && rec.len == 16 && rsx_fc_get32(rec.p) == 0x40u && rsx_fc_get32(rec.p + 4) == 0x00040064u && rsx_fc_get32(rec.p + 12) == 0x4000u, "CMD"); break;
        default: break;
        }
        if (rec.type == RSX_FC_RANGE) {
            const uint32_t fl = rsx_fc_get32(rec.p + 16), len = rsx_fc_get32(rec.p + 12);
            n_range++;
            if (fl & RSX_FC_F_NULLPTR) { n_null++; CHECK(rec.len == RSX_FC_RANGE_HEAD && rsx_fc_get64(rec.p + 24) == 0, "NULL range"); }
            else if (fl & RSX_FC_F_BYTES) {
                n_bytes++;
                CHECK(rec.len == RSX_FC_RANGE_HEAD + len, "range with bytes: len %u", rec.len);
                CHECK(rsx_fc_get64(rec.p + 24) == rsx_fc_fnv(RSX_FC_FNV_SEED, rec.p + RSX_FC_RANGE_HEAD, len), "range digest must match its own bytes");
            } else CHECK(rec.len == RSX_FC_RANGE_HEAD && rsx_fc_get64(rec.p + 24) != 0, "digest-only range");
        }
        if (rec.type == RSX_FC_PASS_END) CHECK(!strcmp((const char*)rec.p + 8, "caught-up"), "PASS_END why");
        if (rec.type == RSX_FC_WORD) CHECK(rsx_fc_get32(rec.p) == 1 && rsx_fc_get32(rec.p + 12) == 0xC0DE0001u, "WORD");
        if (rec.type == RSX_FC_COPY) CHECK(rec.len == 48 && rsx_fc_get32(rec.p + 44) == 11u, "COPY");
        if (rec.type == RSX_FC_SPU_NOTE) CHECK(rec.len == 20 && rsx_fc_get32(rec.p + 16) == 77u, "SPU_NOTE");
        k++;
    }
    CHECK(rc == 0, "clean end, got %d", rc);
    CHECK((unsigned long long)k == want_recs, "read %d records, wrote %llu", k, want_recs);
    CHECK(n_range == 6 && n_bytes == 3 && n_null == 1, "ranges %u bytes %u null %u", n_range, n_bytes, n_null);
    rsx_fc_r_close(&r);

    /* ---- negative controls: every one must be rejected ---- */
    size_t n = 0; uint8_t* b = slurp(p, &n);
    CHECK(b != NULL && n > 200, "slurp");
    int nrec;
    snprintf(q, sizeof q, "%s/bad.kz3fc", argv[1]);
    /* 1. one flipped payload byte in record 3 (the CMD): offset of record 3 = header + sizes of records 0..2 */
    { size_t off = RSX_FC_HEADER_SIZE; for (int i = 0; i < 3; i++) { off += RSX_FC_REC_HEAD + rsx_fc_get32(b + off + 4); }
      uint8_t* c = (uint8_t*)malloc(n); memcpy(c, b, n); c[off + RSX_FC_REC_HEAD + 5] ^= 0x01; spit(q, c, n); free(c);
      CHECK(drain(q, &nrec) == -2 && nrec == 3, "flipped payload byte: rc %d after %d records (want -2 after 3)", drain(q, &nrec), nrec); }
    /* 2. record 2 cut out: record 3 then carries the wrong sequence number */
    { size_t o2 = RSX_FC_HEADER_SIZE; for (int i = 0; i < 2; i++) o2 += RSX_FC_REC_HEAD + rsx_fc_get32(b + o2 + 4);
      size_t l2 = RSX_FC_REC_HEAD + rsx_fc_get32(b + o2 + 4);
      uint8_t* c = (uint8_t*)malloc(n); memcpy(c, b, o2); memcpy(c + o2, b + o2 + l2, n - o2 - l2); spit(q, c, n - l2); free(c);
      CHECK(drain(q, &nrec) == -3, "dropped record: rc %d (want -3)", drain(q, &nrec)); }
    /* 3. truncated in the middle of a payload */
    { spit(q, b, n / 2); CHECK(drain(q, &nrec) == -1, "truncated file: rc %d (want -1)", drain(q, &nrec)); }
    /* 4. a complete file minus its END record reads as truncated, never as a short capture */
    { size_t end_len = RSX_FC_REC_HEAD + 16; spit(q, b, n - end_len); CHECK(drain(q, &nrec) == -1, "missing END: rc %d (want -1)", drain(q, &nrec)); }
    /* 5. END counts that disagree (payload byte total altered, checksum recomputed by hand) */
    { uint8_t* c = (uint8_t*)malloc(n); memcpy(c, b, n);
      size_t eo = n - (RSX_FC_REC_HEAD + 16);
      rsx_fc_put64(c + eo + RSX_FC_REC_HEAD + 8, rsx_fc_get64(c + eo + RSX_FC_REC_HEAD + 8) + 1);
      uint64_t h = rsx_fc_fnv(RSX_FC_FNV_SEED, c + eo, 16); h = rsx_fc_fnv(h, c + eo + RSX_FC_REC_HEAD, 16);
      rsx_fc_put64(c + eo + 16, h); spit(q, c, n); free(c);
      CHECK(drain(q, &nrec) == -4, "END mismatch: rc %d (want -4)", drain(q, &nrec)); }
    /* 6. bad magic, 7. newer version */
    { uint8_t* c = (uint8_t*)malloc(n); memcpy(c, b, n); c[0] = 'X'; spit(q, c, n);
      CHECK(drain(q, &nrec) == -102, "bad magic: rc %d (want -102)", drain(q, &nrec));
      memcpy(c, b, n); rsx_fc_put32(c + 8, RSX_FC_VERSION + 1); spit(q, c, n);
      CHECK(drain(q, &nrec) == -103, "newer version: rc %d (want -103)", drain(q, &nrec)); free(c); }
    free(b);
    /* a clean copy must still read clean (the controls above were not a reader that rejects everything) */
    CHECK(drain(p, &nrec) == 0 && (unsigned long long)nrec == want_recs, "the good file reads clean");

    printf("[test_fifo_capture_format] %s (%d records, 7 negative controls)\n", fails ? "FAIL" : "PASS", k);
    return fails ? 1 : 0;
}

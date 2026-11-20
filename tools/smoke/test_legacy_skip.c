/* Host test for KZ3 patch 2189 (command-thread overhaul O1): the pure core of rsx_legacy_skip.h (mode parse, effect list,
 * reader accounting, [legacy-skip] line). Pure C, no game data. Built twice by test_legacy_skip.sh: plain, and with
 * -DRSX_LGS_NEG_DROP=0x1D74 (the negative control: that method must drop off the list). */
#include <stdio.h>
#include <string.h>
#include "rsx_legacy_skip.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("[FAIL] %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
#ifdef RSX_LGS_NEG_DROP
    CHECK(!rsx_lgs_effect(RSX_LGS_NEG_DROP), "the negative-control build must drop 0x%X", (unsigned)RSX_LGS_NEG_DROP);
    printf("[test_legacy_skip] neg build: 0x%X dropped\n", (unsigned)RSX_LGS_NEG_DROP);
    return fails ? 1 : 0;
#else
    CHECK(rsx_lgs_mode(NULL) == RSX_LGS_SKIP && rsx_lgs_mode("") == RSX_LGS_SKIP, "unset = skip (default)");
    CHECK(rsx_lgs_mode("1") == RSX_LGS_SKIP && rsx_lgs_mode("0") == RSX_LGS_OFF && rsx_lgs_mode("verify") == RSX_LGS_VERIFY, "modes");
    CHECK(rsx_lgs_mode("2") == RSX_LGS_SKIP && rsx_lgs_mode("verifyx") == RSX_LGS_OFF, "other values: nonzero = skip, text = atoi 0 = off");
    /* the exact list, scanned over the whole method space (a method added or dropped without this test failing is the bug) */
    static const unsigned want[] = { 0x0000, 0x0050, 0x0104, 0x0180, 0x01A4, 0x01A8, 0x1D6C, 0x1D70, 0x1D74, 0xE920, 0xE924 };
    unsigned n = 0;
    for (unsigned m = 0; m < 0x10000; m += 4) if (rsx_lgs_effect(m)) {
        int ok = 0; for (unsigned i = 0; i < sizeof want / sizeof *want; i++) ok |= want[i] == m;
        CHECK(ok, "unexpected method on the list: 0x%04X", m); n++;
    }
    CHECK(n == sizeof want / sizeof *want, "list size %u", n);
    CHECK(!rsx_lgs_effect(0x1808) && !rsx_lgs_effect(0x1D94) && !rsx_lgs_effect(0x1800), "draw, clear and report-get are state-only/walker-answered, not on the list");
    /* reader accounting */
    rsx_lgs s; memset(&s, 0, sizeof s);
    CHECK(rsx_lgs_note(&s, 0x1808, 0, 0, 0, 0) == 0 && s.ran == 1 && s.readers == 0, "no callbacks, no reader");
    CHECK(rsx_lgs_note(&s, 0x1808, 0, 3, 1, 1) == 0 && s.readers == 0 && s.null_state == 3 && s.null_param == 1, "null backend calls are counted apart, never readers");
    CHECK(rsx_lgs_note(&s, 0x1808, 0, 2, 1, 0) == 1 && s.readers == 3 && s.rd_cnt == 1, "a real backend callback after a method outside the list is a reader (new method)");
    CHECK(rsx_lgs_note(&s, 0x1808, 0, 1, 0, 0) == 0 && s.readers == 4 && s.rd_cnt == 1 && s.rd_n[0] == 4, "same method again: counted, not new");
    CHECK(rsx_lgs_note(&s, 0x1D94, 0, 0, 1, 0) == 1 && s.rd_cnt == 2, "second reader method");
    CHECK(rsx_lgs_note(&s, 0x0050, 1, 5, 5, 0) == 0 && s.readers == 5, "a method already on the list is not reported");
    for (unsigned m = 0x100; m < 0x100 + 4 * 40; m += 4) rsx_lgs_note(&s, m, 0, 1, 0, 0);
    CHECK(s.rd_cnt == RSX_LGS_RD_MAX, "the reader table is capped (%d)", s.rd_cnt);
    /* line format */
    rsx_lgs c, p, z; memset(&z, 0, sizeof z); memset(&c, 0, sizeof c); p = z;
    c.m3d = 1000; c.ran = 40; c.skipped = 960; c.verify = 1; c.null_state = 2; c.null_param = 7;
    char ln[512]; rsx_lgs_fmt(ln, sizeof ln, 12.0, &c, &p);
    CHECK(!strcmp(ln, "[legacy-skip] t=12 mode=verify m3d=1000 ran=40 skipped=960 readers=0 null_state=2 null_param=7 by_method=-"), "line: %s", ln);
    c.rd_cnt = 2; c.rd_m[0] = 0x1808; c.rd_n[0] = 12; c.rd_m[1] = 0x1D94; c.rd_n[1] = 3; c.readers = 15; c.verify = 0;
    p.m3d = 400; p.skipped = 400; rsx_lgs_fmt(ln, sizeof ln, 13.0, &c, &p);
    CHECK(!strcmp(ln, "[legacy-skip] t=13 mode=skip m3d=600 ran=40 skipped=560 readers=15 null_state=2 null_param=7 by_method=0x1808:12,0x1D94:3"), "line 2: %s", ln);
    char tiny[40]; rsx_lgs_fmt(tiny, sizeof tiny, 1.0, &c, &p); CHECK(strlen(tiny) < sizeof tiny, "never writes past n");
    printf("[test_legacy_skip] core %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
#endif
}

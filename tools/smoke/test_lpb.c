/* Host test for KZ3 patch 2208: libs/video/rsx_lpb.h, the [o0-lpb] LightProbe 8x8 block probe core. Synthetic flips built the way
 * the walker feeds it: binds (with the draw's VP/FP and the unit's filter), new entries, hashes, redecodes, then the flip.
 * Built and mutated by test_lpb.sh. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsx_lpb.h"

rsx_lpb g_rsx_lpb; /* the extern of the header; the test uses its own instances */
static rsx_lpb L;
static int fails;
#define CHECK(name, cond)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s (line %d)\n", name, __LINE__);                                                             \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

#define FPOINT ((1u << 16) | (1u << 24))  /* min NEAREST, mag NEAREST */
#define FLIN ((2u << 16) | (2u << 24))    /* min LINEAR, mag LINEAR */
#define FMAGLIN ((1u << 16) | (2u << 24)) /* min NEAREST, mag LINEAR: not point */
#define FNN ((3u << 16) | (1u << 24))     /* NEAREST_NEAREST, mag NEAREST: point */
#define SPAN 512u

static void bind(uint32_t off, uint64_t vp, uint32_t fpo, uint32_t filter)
{
    rsx_lpb_bind(&L, off, SPAN, vp, 1, fpo, 0x200, 0xABCD0000ull + fpo, 1, filter, 0, 1);
}
static char line[512], wb[8192];
static size_t wl;
static size_t flip(void)
{
    return rsx_lpb_flip(&L, line, sizeof line, wb, sizeof wb, &wl);
}
static unsigned long long F(const char* line_, const char* name) /* value of " name=N" in a flip line */
{
    char key[40];
    snprintf(key, sizeof key, " %s=", name);
    const char* p = strstr(line_, key);
    return p ? strtoull(p + strlen(key), NULL, 10) : ~0ull;
}

int main(void)
{
    /* descriptor classification */
    CHECK("block-yes", rsx_lpb_is_block(1, 0x9A, 8, 8, 2, 0));
    CHECK("block-fmt-mask", rsx_lpb_is_block(1, 0x9A | 0x60, 8, 8, 2, 0)); /* LN/UN bits are outside the 0x9F mask */
    CHECK("block-not-loc0", !rsx_lpb_is_block(0, 0x9A, 8, 8, 2, 0));
    CHECK("block-not-fmt", !rsx_lpb_is_block(1, 0x85, 8, 8, 2, 0));
    CHECK("block-not-w", !rsx_lpb_is_block(1, 0x9A, 16, 8, 2, 0));
    CHECK("block-not-h", !rsx_lpb_is_block(1, 0x9A, 8, 16, 2, 0));
    CHECK("block-not-cube", !rsx_lpb_is_block(1, 0x9A, 8, 8, 2, 1));
    CHECK("block-not-dim", !rsx_lpb_is_block(1, 0x9A, 8, 8, 3, 0));
    CHECK("point-nearest", rsx_lpb_point(FPOINT));
    CHECK("point-nn", rsx_lpb_point(FNN));
    CHECK("point-linear-is-not", !rsx_lpb_point(FLIN));
    CHECK("point-maglinear-is-not", !rsx_lpb_point(FMAGLIN));
    CHECK("point-minlinear-is-not", !rsx_lpb_point((2u << 16) | (1u << 24)));
    CHECK("point-miplinear-is-not", !rsx_lpb_point((5u << 16) | (1u << 24)));

    /* flip 1: 10 contiguous blocks at 0x5C180 + i*512, each bound twice by VP A / FP 0x100 (point, then NEAREST_NEAREST);
     * one extra bind of block 0 by VP B / FP 0x300 with a linear filter; blocks 0,1,2 are new (0,1 recycled); blocks 3..6 were
     * hashed (3,4,5 changed; 3 and 5 redecoded in place, 4 replaced); the rest were only bound. */
    rsx_lpb_init(&L, 30, 2);
    for (int r = 0; r < 2; r++)
        for (uint32_t i = 0; i < 10; i++) bind(0x5C180u + i * SPAN, 0xAAAA, 0x100, r ? FNN : FPOINT);
    bind(0x5C180u, 0xBBBB, 0x300, FLIN);
    rsx_lpb_new(&L, 0x5C180u, SPAN, 1);
    rsx_lpb_new(&L, 0x5C180u + SPAN, SPAN, 1);
    rsx_lpb_new(&L, 0x5C180u + 2 * SPAN, SPAN, 0);
    for (uint32_t i = 3; i < 7; i++) rsx_lpb_hash(&L, 0x5C180u + i * SPAN, SPAN, i < 6);
    rsx_lpb_redecode(&L, 0x5C180u + 3 * SPAN, 1);
    rsx_lpb_redecode(&L, 0x5C180u + 4 * SPAN, 0);
    rsx_lpb_redecode(&L, 0x5C180u + 5 * SPAN, 1);
    size_t n = flip();
    CHECK("line-printed", n > 0 && strncmp(line, "[o0-lpb] flip=1 ", 16) == 0);
    CHECK("binds", F(line, "binds") == 21);
    CHECK("distinct", F(line, "distinct") == 10);
    CHECK("new", F(line, "new") == 3);
    CHECK("recycled", F(line, "recycled") == 2);
    CHECK("hashed", F(line, "hashed") == 4);
    CHECK("changed", F(line, "changed") == 3);
    CHECK("inplace", F(line, "inplace") == 2);
    CHECK("replaced", F(line, "replaced") == 1);
    CHECK("boundKB", F(line, "boundKB") == 5); /* 10 blocks x 512 = 5120 */
    CHECK("decKB", F(line, "decKB") == 3);     /* 6 dirty blocks (3 new + 3 changed) x 512 = 3072 -> 3 KiB */
    CHECK("dpages", F(line, "dpages") == 1);   /* dirty blocks 0..5 are 0x5C180..0x5CD80: all in page 0x5C */
    CHECK("dspans", F(line, "dspans") == 1);
    CHECK("dspans16", F(line, "dspans16") == 1);
    CHECK("nonpoint", F(line, "nonpoint") == 1);
    CHECK("users", F(line, "users") == 2);
    CHECK("users-table", L.n_use == 2 && L.use[0].binds == 20 && L.use[0].nonpoint == 0 && L.use[1].binds == 1 &&
                             L.use[1].nonpoint == 1);
    CHECK("filters-table", L.n_filt == 3); /* point, nn, linear */
    {
        char ub[2048];
        const size_t un = rsx_lpb_fmt_use(&L, ub, sizeof ub);
        CHECK("use-lines", un > 0 && strstr(ub, "[o0-lpb-use] vp=0000aaaa fp=1:0x00000100/512") &&
                               strstr(ub, "binds=20 nonpoint=0\n") && strstr(ub, "vp=0000bbbb") &&
                               strstr(ub, "binds=1 nonpoint=1\n"));
        CHECK("filt-lines", strstr(ub, "[o0-lpb-filt] min=2 mag=2 mips=1 aniso=0 point=0 binds=1\n") &&
                                strstr(ub, "min=1 mag=1 mips=1 aniso=0 point=1 binds=10\n") &&
                                strstr(ub, "min=3 mag=1 mips=1 aniso=0 point=1 binds=10\n"));
    }
    CHECK("no-window-yet", wl == 0);

    /* flip 2: a fresh generation (distinct restarts). A block bound twice counts once. Dirty pages: new/changed blocks in pages 1, 2
     * (adjacent), 100, 105 (hash-changed) and one block straddling the 300/301 boundary. Window closes here (win = 2 flips). */
    bind(0x5C180u, 0xAAAA, 0x100, FPOINT);
    bind(0x5C180u, 0xAAAA, 0x100, FPOINT);
    rsx_lpb_new(&L, 0x1000u, SPAN, 0);
    rsx_lpb_new(&L, 0x2000u, SPAN, 0);
    rsx_lpb_new(&L, 100u * 4096u, SPAN, 0);
    rsx_lpb_hash(&L, 105u * 4096u + 0x800u, SPAN, 1);
    rsx_lpb_new(&L, 300u * 4096u + 4096u - 256u, SPAN, 0);
    n = flip();
    CHECK("flip2-line", n > 0 && strncmp(line, "[o0-lpb] flip=2 ", 16) == 0);
    CHECK("gen-distinct", F(line, "distinct") == 6); /* 0x5C180 (bound only) + 5 dirty blocks */
    CHECK("gen-binds", F(line, "binds") == 2);
    CHECK("gen-new", F(line, "new") == 4);
    CHECK("gen-dpages", F(line, "dpages") == 6);     /* 1, 2, 100, 105, 300, 301 (a bound-only block adds none) */
    CHECK("gen-dspans", F(line, "dspans") == 4);     /* {1,2} {100} {105} {300,301} */
    CHECK("gen-dspans16", F(line, "dspans16") == 3); /* {1,2} {100..105} {300,301} */
    CHECK("window-closed", wl > 0 && strstr(wb, "[o0-lpb] win flips=2 active=2 ") != NULL);
    CHECK("window-avg-max", strstr(wb, " binds=11.5/21 ") != NULL); /* (21 + 2) / 2, max 21 */
    CHECK("window-use-lines", strstr(wb, "[o0-lpb-use] vp=0000aaaa") != NULL);
    n = flip(); /* nothing happened: no line, no window (win_flips restarted at 0, now 1) */
    CHECK("inactive-no-line", n == 0 && wl == 0);
    /* a window with no active flip prints nothing; the next active flip's window is only its own flips (window reset) */
    n = flip();
    CHECK("inactive-window-silent", n == 0 && wl == 0);
    bind(0x7000u, 0xAAAA, 0x100, FPOINT);
    n = flip();
    CHECK("flip5-line", n > 0 && strncmp(line, "[o0-lpb] flip=5 ", 16) == 0);
    n = flip();
    CHECK("window2", wl > 0 && strstr(wb, "[o0-lpb] win flips=2 active=1 ") != NULL &&
                         strstr(wb, " binds=0.5/1 ") != NULL); /* flips 5,6: 1 bind over 2 flips */

    /* per-flip line cap: max_lines = 1 prints the first active flip only; windows still run */
    rsx_lpb_init(&L, 1, 100);
    bind(0x100u, 1, 0x100, FPOINT);
    CHECK("cap-first", flip() > 0);
    bind(0x100u, 1, 0x100, FPOINT);
    CHECK("cap-second-silent", flip() == 0 && L.tot[RSX_LPB_N_BINDS] == 2);

    /* switch off per-flip lines entirely: max_lines = 0 */
    rsx_lpb_init(&L, 0, 100);
    bind(0x100u, 1, 0x100, FPOINT);
    CHECK("cap-zero", flip() == 0 && L.tot[RSX_LPB_N_BINDS] == 1);

    /* generation wrap: an old entry stamped with the wrapped generation must not read as part of the new flip */
    rsx_lpb_init(&L, 30, 100);
    bind(0x5000u, 1, 0x100, FPOINT);
    L.gen = 0xFFFFFFFFu; /* the next flip wraps */
    memset(L.tab, 0, sizeof L.tab);
    L.tab[3].gen = 1;
    L.tab[3].off = 0x9000u;
    L.tab[3].flags = RSX_LPB_F_NEW;
    L.tab[3].span = SPAN; /* stale entry that a wrap without clearing would revive */
    L.n_distinct = 0;
    (void)flip();
    CHECK("wrap-gen", L.gen == 1);
    bind(0x6000u, 1, 0x100, FPOINT);
    n = flip();
    CHECK("wrap-no-stale", F(line, "distinct") == 1 && F(line, "dpages") == 0);

    /* more distinct blocks than the table keeps: counted as overflow, no crash, binds still counted */
    rsx_lpb_init(&L, 30, 100);
    for (uint32_t i = 0; i < RSX_LPB_MAXB + 50u; i++) bind(0x10000u + i * SPAN, 1, 0x100, FPOINT);
    n = flip();
    CHECK("ovf-binds",
          F(line, "binds") == RSX_LPB_MAXB + 50u && F(line, "distinct") == RSX_LPB_MAXB && F(line, "ovf") == 50);

    /* consumer table overflow */
    rsx_lpb_init(&L, 30, 100);
    for (uint32_t i = 0; i < RSX_LPB_MAXU + 3u; i++) bind(0x100u, 0x1000u + i, 0x100, FPOINT);
    CHECK("use-ovf", L.n_use == RSX_LPB_MAXU && L.ovf_use == 3);
    /* a draw without a built pipeline is a separate consumer, never merged with a built one */
    rsx_lpb_init(&L, 30, 100);
    rsx_lpb_bind(&L, 0x100u, SPAN, 5, 1, 0x100, 0x200, 0, 0, FPOINT, 0, 1);
    rsx_lpb_bind(&L, 0x100u, SPAN, 5, 1, 0x100, 0x200, 1, 1, FPOINT, 0, 1);
    CHECK("nopso-separate", L.n_use == 2);
    /* a block bound by the same consumer in two flips counts as a consumer once per flip */
    rsx_lpb_init(&L, 30, 100);
    bind(0x100u, 1, 0x100, FPOINT);
    bind(0x200u, 1, 0x100, FPOINT);
    (void)flip();
    bind(0x100u, 1, 0x100, FPOINT);
    n = flip();
    CHECK("users-per-flip", F(line, "users") == 1 && L.n_use == 1 && L.use[0].binds == 3);

    /* two programs that differ only in the fragment-program offset are separate consumers (T2 reads each consuming program) */
    rsx_lpb_init(&L, 30, 100);
    rsx_lpb_bind(&L, 0x100u, SPAN, 5, 1, 0x100, 0x200, 7, 1, FPOINT, 0, 1);
    rsx_lpb_bind(&L, 0x100u, SPAN, 5, 1, 0x400, 0x200, 7, 1, FPOINT, 0, 1);
    CHECK("fpoff-separate", L.n_use == 2 && L.use[0].binds == 1 && L.use[1].binds == 1);
    /* the anisotropy field is bits 4-6 of control0 (bits 0-3 and 7+ must not leak in) and keeps filter states apart */
    rsx_lpb_init(&L, 30, 100);
    rsx_lpb_bind(&L, 0x100u, SPAN, 5, 1, 0x100, 0x200, 7, 1, FLIN, 5u << 4, 1);
    rsx_lpb_bind(&L, 0x100u, SPAN, 5, 1, 0x100, 0x200, 7, 1, FLIN, (5u << 4) | 0x8Fu | 0x700u, 1);
    rsx_lpb_bind(&L, 0x100u, SPAN, 5, 1, 0x100, 0x200, 7, 1, FLIN, 2u << 4, 1);
    CHECK("aniso-field", L.n_filt == 2 && L.filt[0].aniso == 5 && L.filt[0].binds == 2 && L.filt[1].aniso == 2);
    {
        char ub[2048];
        (void)rsx_lpb_fmt_use(&L, ub, sizeof ub);
        CHECK("aniso-line", strstr(ub, "[o0-lpb-filt] min=2 mag=2 mips=1 aniso=5 point=0 binds=2\n") != NULL);
    }

    if (fails) {
        printf("test_lpb: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_lpb: all checks pass\n");
    return 0;
}

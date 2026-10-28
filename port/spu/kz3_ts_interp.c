/* KZ3 SPU pass 55: KZ3_TS_INTERP=1 runs the Gfx.TextureStreaming job (module 42, image job_01181700) in the SPU
 * interpreter instead of its lifted code (diagnostic, default off). Installed as the module's entry hook (patch
 * 1044): at the policy module's call of the module entry the hook interprets from the entry pc up to the link
 * (image id switched to one with no lifted functions and no resident overlay, so the interpreter never rejoins
 * lifted code), then returns 1 so the runtime returns to the link with the interpreter's registers. Question it
 * answers: does the job's request list (LS 0x9610 -> 0x13F50A0, count at +0xC) stay empty because of the lift
 * or because of its inputs? Logs the first 8 runs and every run whose request count is non-zero (up to 40). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kz3_cat.h"
#include "spu_context.h"
#include "spu_interp.h"
#include "kz3_spu_skip.h"

extern void spu_overlay_set_entry_hook(int image_id, int (*fn)(struct spu_context*, int, void*), void* user);
extern uint64_t g_spu_interp_steps;
extern uint32_t g_spu_interp_last_pc;
extern _Thread_local uint32_t* g_spu_interp_pc_hist; /* patch 1070 */
extern _Thread_local uint32_t g_spu_interp_pc_hist_lo, g_spu_interp_pc_hist_n;

/* Patch 1070 histogram of one run over the module (LS 0x4000..0x7400): prints runs of consecutive pcs with the
 * same count as "lo-hi:count" (pcs never run are left out), 12 runs per line. */
static uint32_t s_hist[0xD00];
static void hist_dump(long run)
{
    char b[400];
    int k = kz3_cat(b, sizeof b, 0, "[ts-hist] run %ld:", run), n = 0;
    for (uint32_t i = 0; i < 0xD00;) {
        uint32_t j = i;
        while (j + 1 < 0xD00 && s_hist[j + 1] == s_hist[i]) j++;
        if (s_hist[i]) {
            k = kz3_cat(b, sizeof b, k, " %X-%X:%u", 0x4000 + 4 * i, 0x4000 + 4 * j, s_hist[i]);
            if (++n == 12) {
                fprintf(stderr, "%s\n", b);
                k = kz3_cat(b, sizeof b, 0, "[ts-hist] run %ld:", run);
                n = 0;
            }
        }
        i = j + 1;
    }
    if (n) fprintf(stderr, "%s\n", b);
    fflush(stderr);
}

static uint32_t ls32(const spu_context* c, uint32_t a)
{
    a &= SPU_LS_MASK;
    return ((uint32_t)c->ls[a] << 24) | ((uint32_t)c->ls[a + 1] << 16) | ((uint32_t)c->ls[a + 2] << 8) | c->ls[a + 3];
}

static int ts_hook(struct spu_context* ctx, int img, void* user)
{
    (void)user;
    static long runs, lines;
    const uint32_t entry = ctx->pc & SPU_LS_MASK, link = ctx->gpr[0]._u32[0] & SPU_LS_MASK;
    const int save_img = ctx->image_id, save_ovl = ctx->resident_ovl;
    ctx->image_id = 499; /* KZ3_OVL_SKIP_MAX - 13: no lifted functions registered there */
    ctx->resident_ovl = 0;
    const int hist = (runs + 1 == 200 || runs + 1 == 400);
    if (hist) {
        memset(s_hist, 0, sizeof s_hist);
        g_spu_interp_pc_hist_lo = 0x4000;
        g_spu_interp_pc_hist_n = 0xD00;
        g_spu_interp_pc_hist = s_hist;
    }
    const uint32_t end = spu_interp_run_until(ctx, entry, link);
    g_spu_interp_pc_hist = 0;
    if (hist) hist_dump(runs + 1);
    const uint64_t steps = g_spu_interp_steps;
    ctx->image_id = save_img;
    ctx->resident_ovl = save_ovl;
    runs++;
    const uint32_t req = ls32(ctx, 0x961C), ev = ls32(ctx, 0x9640);
    if (lines < 40 && (runs <= 8 || req || end != link)) {
        lines++;
        fprintf(stderr,
                "[ts-interp] run %ld img=%d entry=0x%05X link=0x%05X end=0x%05X last=0x%05X pc=0x%05X stop=0x%X "
                "r0=0x%05X steps=%llu req=%u [%08X %08X %08X] ev=%u\n",
                runs, img, entry, link, end, g_spu_interp_last_pc, ctx->pc & SPU_LS_MASK, ctx->stop_code,
                ctx->gpr[0]._u32[0] & SPU_LS_MASK, (unsigned long long)steps, req, ls32(ctx, 0x9610), ls32(ctx, 0x9614),
                ls32(ctx, 0x9618), ev);
        fflush(stderr);
    }
    ctx->pc = link;
    return 1;
}

void kz3_ts_interp_install(void)
{
    const char* e = getenv("KZ3_TS_INTERP");
    if (!e || *e != '1') return;
    for (unsigned i = 0; i < kz3_spu_image_names_n; i++)
        if (!strcmp(kz3_spu_image_names[i].name, "job_01181700")) {
            spu_overlay_set_entry_hook(kz3_spu_image_names[i].image_id, ts_hook, NULL);
            fprintf(stderr, "[ts-interp] job_01181700 (image %d) runs interpreted\n", kz3_spu_image_names[i].image_id);
            return;
        }
    fprintf(stderr, "[ts-interp] job_01181700 not in the image table\n");
}

/* KZ3 SPU pass 31: the KZ3_CULL_AB=empty44 entry hook (runtime patch 1044). Job 44 (Gfx.Occlusion.Raster,
 * job_0112af00 on 1.00) reads its parameters at LS[jobctx+0x20] (three qwords copied to its stack at 0x4440);
 * qword 0 is {hw0: output size/16 | ..., w1: output EA (block+0x778 raster), w2: ?, hw7: flags<<14 | occluder
 * count}. A zero count takes the module's own empty path (0x45EC -> 0x5060: no 1.0f clear, no rasterization, the
 * output filled with 0xFF bytes by 0x6168, then the common tail 0x4EE8: PUT to w1 and the job-chain epilogue), so
 * the PPU's completion counter (block+0x8A0, +1 by lwarx at 0x1D8A1C) still comes back to 0. Facts from the
 * disassembly in the RE notes Pass 31; the parameter meaning is ASSUMPTION until a run shows the count.
 * Pass 32 (RE pass 65): KZ3_CULL_AB=empty51. Job 51 (Gfx.Occluders.Scene, job_011a8280 on 1.00, image 250;
 * 1.14 jobmod_01224c8c) GETs its 64-byte output descriptor from param w1 (view data + 0x290) after entry; the u32 at
 * descriptor +0x20 is the output capacity (limit = LS 0xA1D0 + (min(cap, 1024) & ~3) * 4). With 0 the pick emits
 * nothing, the PUT at 0x7924 has size 0, the count PUT writes 0 and the job still reaches the AddJob chain epilogue.
 * The hook zeroes that u32 in main memory (an LS edit would be overwritten by the job's own GET); nothing rewrites
 * it, so the zero persists for the session. Host test: tools/spu/test_spu_cull_empty.sh. */
#include <stdint.h>
#include <stdio.h>
#include "spu_context.h"
#include "kz3_spu_skip.h"
extern uint8_t* vm_base; /* runtime/spu/spu_dma.h: guest EA 0 on the host */

static int empty51_entry(struct spu_context* ctx, int image_id, uint32_t jc, uint32_t prm)
{
    static unsigned s_calls, s_bad;
    static uint32_t s_cap0;
    uint32_t w1 = spu_ls_read32(ctx, prm + 4), ea = w1 + 0x20u;
    if (!vm_base || w1 < 0x10000u || w1 >= 0xF0000000u || (w1 & 3u)) {
        if (s_bad++ < 2)
            fprintf(
                stderr,
                "[spu-cull] image %d job 51: unexpected descriptor EA 0x%08X (ctx 0x%05X params 0x%05X), left alone\n",
                image_id, w1, jc, prm);
        return 0;
    }
    uint8_t* d = vm_base + ea;
    uint32_t cap = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
    if (s_calls == 0) s_cap0 = cap;
    d[0] = d[1] = d[2] = d[3] = 0;
    s_calls++;
    if (s_calls <= 2 || (s_calls & 2047u) == 0)
        fprintf(stderr,
                "[spu-cull] image %d job 51 entry %u: output capacity at 0x%08X %u -> 0 (first seen %u, params LS "
                "0x%05X)\n",
                image_id, s_calls, ea, cap, s_cap0, prm);
    return 0; /* run the module */
}

int kz3_cull_empty_entry(struct spu_context* ctx, int image_id, void* user)
{
    static unsigned s_calls, s_bad;
    static unsigned long long s_zeroed;
    int job = (int)(intptr_t)user;
    uint32_t jc = ctx->gpr[3]._u32[0] & SPU_LS_MASK;                                 /* job context (LS 0x1330) */
    uint32_t prm = jc < 0x4000u ? (spu_ls_read32(ctx, jc + 0x20) & SPU_LS_MASK) : 0; /* parameter block */
    if ((job != 44 && job != 51) || jc >= 0x4000u || prm == 0 || prm >= 0x4000u) {
        if (s_bad++ < 2)
            fprintf(stderr,
                    "[spu-cull] image %d job %d: unexpected entry state (ctx 0x%05X params 0x%05X), left alone\n",
                    image_id, job, jc, prm);
        return 0;
    }
    if (job == 51) return empty51_entry(ctx, image_id, jc, prm);
    uint8_t* p = spu_ls_ptr(ctx, prm + 14);
    unsigned hw = ((unsigned)p[0] << 8) | p[1];
    unsigned count = hw & 0x3FFFu;
    p[0] = (uint8_t)(p[0] & 0xC0u);
    p[1] = 0;
    s_zeroed += count;
    s_calls++;
    if (s_calls <= 2 || (s_calls & 2047u) == 0)
        fprintf(stderr,
                "[spu-cull] image %d job 44 entry %u: occluder count %u -> 0 (flags 0x%X kept, params LS 0x%05X, "
                "%llu occluders dropped so far)\n",
                image_id, s_calls, count, hw >> 14, prm, s_zeroed);
    return 0; /* run the module */
}

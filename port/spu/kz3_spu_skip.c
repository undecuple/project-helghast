/* KZ3 spu rounds 30-31: KZ3_SPU_SKIP_OVL / KZ3_CULL_AB -> spu_overlay_set_skip (runtime patch 1043) or
 * spu_overlay_set_entry_hook (patch 1044, KZ3_CULL_AB=empty44). See kz3_spu_skip.h.
 * Host test: tools/spu/test_spu_skip_env.sh (stub name table, stub runtime entry points, stub hook). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kz3_spu_skip.h"

extern void spu_overlay_set_skip(int image_id, int on);
extern void spu_overlay_set_entry_hook(int image_id, int (*fn)(struct spu_context*, int, void*), void* user);

/* KZ3's gfx visibility jobs (the RE notes section 8): job id, module VA in the 1.00 and 1.14 EBOOT,
 * whether the "empty" entry hook knows the job's parameter layout (the RE notes Pass 31). */
static const struct {
    int job;
    uint32_t ea100, ea114;
    int has_empty;
} CULL_JOBS[] = {
    {43, 0x01130800u, 0x011ACC80u, 0}, /* Gfx.Occlusion.Rendering */
    {44, 0x0113AF00u, 0x011B7380u, 1}, /* Gfx.Occlusion.Raster (occluder rasterizer): count = param qword 0 hw 7 */
    {45, 0x0113DD00u, 0x011BA180u, 0}, /* Gfx.Occlusion.Filter */
    {46, 0x011A6D80u, 0x01223180u, 0}, /* Gfx.Scene.KdTree */
    {47, 0x011AD200u, 0x01229900u, 0}, /* Gfx.Scene.Portals */
    {48, 0x011BE480u, 0x0123AF80u, 0}, /* Gfx.Occlusion.Scene */
    {49, 0x011C7200u, 0x01244080u, 0}, /* Gfx.Occlusion.Scene.KdTree */
    {50, 0x011CC280u, 0x01249100u, 0}, /* Gfx.Occlusion.Scene.Setups */
    {51, 0x011B8280u, 0x01234C80u,
     1}, /* Gfx.Occluders.Scene (occluder pick): capacity u32 at param w1 + 0x20 (main memory) */
};

static int id_by_name(const char* s, size_t n)
{
    for (unsigned i = 0; i < kz3_spu_image_names_n; i++)
        if (strlen(kz3_spu_image_names[i].name) == n && memcmp(kz3_spu_image_names[i].name, s, n) == 0)
            return kz3_spu_image_names[i].image_id;
    return 0;
}

static int id_by_ea(uint32_t ea)
{
    for (unsigned i = 0; i < kz3_spu_image_names_n; i++)
        if (ea && kz3_spu_image_names[i].ea == ea) return kz3_spu_image_names[i].image_id;
    return 0;
}

static void add_id(int id, int mode, int* ids, uint8_t* modes, unsigned* n, unsigned max_ids)
{
    if (id <= 0) return;
    if (mode != KZ3_CULL_MODE_SKIP && !modes) return; /* caller without modes: skips only */
    for (unsigned i = 0; i < *n; i++)
        if (ids[i] == id) {
            if (modes && modes[i] != mode)
                fprintf(stderr, "[spu-ovl-skip] image %d named twice with different modes: keeping the first\n", id);
            return;
        }
    if (*n < max_ids) {
        if (modes) modes[*n] = (uint8_t)mode;
        ids[(*n)++] = id;
    }
}

unsigned kz3_spu_skip_resolve(const char* skip_env, const char* cull_env, int* ids, uint8_t* modes, unsigned max_ids)
{
    unsigned n = 0;
    const char* p = skip_env;
    while (p && *p) {
        while (*p == ',' || *p == ' ') p++;
        const char* e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if (e > p) {
            char* end = NULL;
            long v = strtol(p, &end, 0);
            int id = (end == e) ? (int)v : id_by_name(p, (size_t)(e - p));
            if (id <= 0) fprintf(stderr, "[spu-ovl-skip] KZ3_SPU_SKIP_OVL: unknown image '%.*s'\n", (int)(e - p), p);
            add_id(id, KZ3_CULL_MODE_SKIP, ids, modes, &n, max_ids);
        }
        p = e;
    }
    p = cull_env;
    while (p && *p) {
        while (*p == ',' || *p == ' ') p++;
        const char* e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if (e > p) {
            int id = 0, mode = 0, job = 0;
            if (e - p >= 5 && memcmp(p, "skip", 4) == 0) {
                mode = KZ3_CULL_MODE_SKIP;
                job = atoi(p + 4);
            } else if (e - p >= 6 && memcmp(p, "empty", 5) == 0) {
                mode = KZ3_CULL_MODE_EMPTY;
                job = atoi(p + 5);
            }
            for (unsigned i = 0; mode && i < sizeof CULL_JOBS / sizeof CULL_JOBS[0]; i++)
                if (CULL_JOBS[i].job == job) {
                    if (mode == KZ3_CULL_MODE_EMPTY && !CULL_JOBS[i].has_empty) {
                        fprintf(stderr,
                                "[spu-ovl-skip] KZ3_CULL_AB: '%.*s' not implemented (parameter layout "
                                "unknown; only empty44 and empty51)\n",
                                (int)(e - p), p);
                        job = 0;
                        break;
                    }
                    id = id_by_ea(CULL_JOBS[i].ea100);
                    if (!id) id = id_by_ea(CULL_JOBS[i].ea114);
                }
            if (id <= 0 && job)
                fprintf(stderr,
                        "[spu-ovl-skip] KZ3_CULL_AB: unknown or unlifted '%.*s' (want skip43..skip51, "
                        "empty44, empty51)\n",
                        (int)(e - p), p);
            add_id(id, mode, ids, modes, &n, max_ids);
        }
        p = e;
    }
    return n;
}

void kz3_ts_interp_install(void);  /* kz3_ts_interp.c, KZ3_TS_INTERP=1 (SPU pass 55) */
void kz3_stall_diag_install(void); /* kz3_stall_diag.c, KZ3_STALL_DIAG=1 (SPU pass 63) */
void kz3_frame_trace_install(void); /* kz3_frame_trace.c, KZ3_FRAME_TRACE=FROM,TO (SPU pass 66) */
void kz3_spu_apply_skip_env(void)
{
    kz3_ts_interp_install();
    kz3_stall_diag_install();
    kz3_frame_trace_install();
    const char* a = getenv("KZ3_SPU_SKIP_OVL");
    const char* b = getenv("KZ3_CULL_AB");
    if ((!a || !*a) && (!b || !*b)) return;
    int ids[32];
    uint8_t modes[32];
    unsigned n = kz3_spu_skip_resolve(a, b, ids, modes, 32);
    for (unsigned i = 0; i < n; i++) {
        const char* name = "?";
        for (unsigned j = 0; j < kz3_spu_image_names_n; j++)
            if (kz3_spu_image_names[j].image_id == ids[i]) {
                name = kz3_spu_image_names[j].name;
                break;
            }
        if (modes[i] == KZ3_CULL_MODE_EMPTY) {
            int job = 0;
            for (unsigned k = 0; k < sizeof CULL_JOBS / sizeof CULL_JOBS[0]; k++)
                if (id_by_ea(CULL_JOBS[k].ea100) == ids[i] || id_by_ea(CULL_JOBS[k].ea114) == ids[i])
                    job = CULL_JOBS[k].job;
            fprintf(stderr, "[spu-ovl-skip] image %d (%s) runs with its work count zeroed at entry (empty%d)\n", ids[i],
                    name, job);
            spu_overlay_set_entry_hook(ids[i], kz3_cull_empty_entry, (void*)(intptr_t)job);
        } else {
            fprintf(stderr, "[spu-ovl-skip] image %d (%s) will return at its entry\n", ids[i], name);
            spu_overlay_set_skip(ids[i], 1);
        }
    }
}

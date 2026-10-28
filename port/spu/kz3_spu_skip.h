/* KZ3 spu rounds 30-31: skip or neutralise chosen SPU job modules at their entry (runtime patches 1043/1044) from
 * the environment.
 *   KZ3_SPU_SKIP_OVL=<image id | lifted image name>[,...]   e.g. 220 or job_0112af00
 *   KZ3_CULL_AB=skipNN[,...]    NN = KZ3 gfx job id 43..51 (the RE notes section 8), resolved to the
 *                               module's lifted image for the lifted EBOOT version (1.00 or 1.14 job EA). The module
 *                               returns at its entry (patch 1043); it never queues its successor, so skip44/skip51
 *                               hang the level load (pass 30).
 *   KZ3_CULL_AB=empty44         pass 31: job 44 (occluder rasterizer) runs, but the entry hook (patch 1044) zeroes
 *                               the occluder count in its parameter block, so the module takes its own
 *                               "nothing to rasterize" path (output filled with 0xFF = far, completion as usual).
 * The generated work/spu/gen/kz3_spu_register.c provides the name table and calls kz3_spu_apply_skip_env()
 * after registering the images. */
#ifndef KZ3_SPU_SKIP_H
#define KZ3_SPU_SKIP_H
#include <stdint.h>

typedef struct kz3_spu_image_name {
    const char* name; /* lifted image directory name, e.g. "job_0112af00" */
    int image_id;     /* runtime image id (spu_begin_image) */
    uint32_t ea;      /* module / ELF EA in the EBOOT image, 0 if none */
} kz3_spu_image_name;

extern const kz3_spu_image_name kz3_spu_image_names[];
extern const unsigned kz3_spu_image_names_n;

enum { KZ3_CULL_MODE_SKIP = 1, KZ3_CULL_MODE_EMPTY = 2 };

/* Resolve the two environment strings (either may be NULL) into image ids and modes (modes may be NULL: then
 * the "empty" entries are dropped); returns the count written. The first mention of an image wins. */
unsigned kz3_spu_skip_resolve(const char* skip_env, const char* cull_env, int* ids, uint8_t* modes, unsigned max_ids);
/* getenv both, resolve, call spu_overlay_set_skip(id, 1) or spu_overlay_set_entry_hook(id, hook, job) for each,
 * print one line per id. */
void kz3_spu_apply_skip_env(void);

/* Entry hook (patch 1044) behind KZ3_CULL_AB=emptyNN; user = (void*)(intptr_t)NN. Only 44 is implemented: it
 * zeroes the low 14 bits of halfword 7 of the job's first parameter qword (the occluder count; the two flag bits
 * stay) and returns 0 so the module runs. Defined in kz3_spu_cull.c (needs the runtime's spu_context.h). */
struct spu_context;
int kz3_cull_empty_entry(struct spu_context* ctx, int image_id, void* user);

#endif

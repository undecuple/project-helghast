/* kz3_gpost.h -- GPU post plan P1: the pure core of the
 * wrapper around the post-chain batch runner (1.14 @0x0020C010, 1.00 @0x002006C0), no ppu_context and no host
 * memory access of its own (a kz3_gpost_mem accessor), so host tests drive it over a fake guest memory.
 *
 * What the runner does (read from the lifted bodies, not guessed): for each batch object B in the renderer's list
 * (u32 pointers at [R+0x18D8], count [R+0x18D4]) it runs the batch when ([B+0xC] & S) != 0 and ([B+0x10] & S) == 0,
 * with S = [item + 0x258 + 4*[B+0x14]] (item = its r3, the post item), and queues the batch's 8-byte job entries
 * (u32 count at B+4, u32 pointer at B+8) plus a barrier. Zeroing [B+0xC] therefore makes the runner skip the batch.
 *
 * begin(): identify the steps, decide the claimed set, zero the masks of the claimed batches.
 * end():   restore the masks, snapshot the claimed batches' job descriptors into the per-frame plan, publish it.
 * With nothing claimed begin() writes nothing, so the runner's job list is the original's (host test).
 */
#ifndef KZ3_GPOST_H
#define KZ3_GPOST_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* guest layout (both builds, the RE notes "0x2006C0"; offsets identical in 1.00 and 1.14) */
#define KZ3_GPOST_R_LIST 0x18D8u   /* renderer: u32 pointer to the batch pointer array */
#define KZ3_GPOST_R_COUNT 0x18D4u  /* renderer: number of batches */
#define KZ3_GPOST_R_FRAME 0x2300u  /* renderer: frame counter (plan s.3.1 step 5; ASSUMPTION: not re-read by the runner) */
#define KZ3_GPOST_ITEM_S 0x258u    /* item: per-viewport feature flag words, 4 bytes each */
#define KZ3_GPOST_B_COUNT 0x4u     /* batch: u32 number of 8-byte entries */
#define KZ3_GPOST_B_ENTRIES 0x8u   /* batch: u32 pointer to the entries */
#define KZ3_GPOST_B_MASK 0xCu
#define KZ3_GPOST_B_EXCL 0x10u
#define KZ3_GPOST_B_CTX 0x14u

#define KZ3_GPOST_MAXB 160
#define KZ3_GPOST_BLOB 65536
#define KZ3_GPOST_NSEQ 37
#define KZ3_GPOST_VALID_STEPS 0x3FFFFFFFFEull /* bit n = sequence number n, n = 1..37 */

typedef struct {
    void* u;
    uint32_t (*r32)(void* u, uint32_t ea);
    void (*w32)(void* u, uint32_t ea, uint32_t v);
} kz3_gpost_mem;

typedef enum { KZ3_GP_BUILD_114 = 0, KZ3_GP_BUILD_100 = 1 } kz3_gpost_build;

/* the expected 720p, one-viewport, host-AA-on chain: one row per batch, in list order (flat table, s.3.1) */
typedef struct {
    uint8_t seq;      /* sequence number 6..36 (docs table, re/data/post_jobs.tsv "CHAIN") */
    uint8_t job;      /* Gfx.Post.* job id (22..37) */
    uint32_t mask;    /* batch +0xC */
    uint32_t excl;    /* batch +0x10 */
    uint8_t optional; /* the row may be absent (S13 MotionBlur when Options > Display > Motion blur is off) */
} kz3_gpost_row;

extern const kz3_gpost_row kz3_gpost_rows[];
extern const int kz3_gpost_nrows;

typedef struct {
    uint32_t ea, count, entries, mask, excl, ctx;
    uint32_t w0, w1;    /* first 8 bytes of the first job descriptor (G3 log; w0 is the module VA, ASSUMPTION) */
    uint8_t job;        /* Gfx.Post.* id from w0 via the module table, 0 = unknown */
    uint8_t seq;        /* 0 = not matched */
    uint8_t runs;       /* predicate (mask&S) && !(excl&S) with this frame's S */
    uint8_t claimed;
    uint8_t
        rowjob; /* job id of the table row this batch matched (P2 checks the module in the command list against it) */
} kz3_gpost_batch;

/* ---- P2: PPU-side snapshot of the claimed steps' command lists, user blocks and parameter blocks (plan P2;
 * layout: V = read from instructions, A = ASSUMPTION there). Read-only on guest
 * memory; runs only inside kz3_gpost_end for CLAIMED batches, so with nothing claimed it costs and writes nothing.
 * Every followed pointer/size is validated; a failed step is recorded with its reject reason and counted (ok = 0), never
 * trusted. Rejected steps still have their masks zeroed by P1 (P3 decides what to do with them, plan s.5 M8). */
#define KZ3_GPOST_SURF_BASE 0xA44u /* renderer: surface table, 21 records of 0x94 bytes (V) */
#define KZ3_GPOST_SURF_STRIDE 0x94u
#define KZ3_GPOST_NSURF 21
#define KZ3_GPOST_P2_MAXSTEP 64
#define KZ3_GPOST_P2_MAXPARAM 16  /* list elements incl. the pad; ILRSharp at 720p has 5 + pad */
#define KZ3_GPOST_P2_MAXTILE 1024 /* tiles parsed per batch; more is a reject */
#define KZ3_GPOST_P2_BLOB 4096    /* parameter-block bytes per frame (720p chain needs ~0x600) */
#define KZ3_GPOST_P2_BLK_MAX                                                                                           \
    0x220u /* largest copied block (BloomCapture); bigger records (ILRSharp chunks) are EA/size only */
#define KZ3_GPOST_EA_LO 0x00010000u /* followed pointers (command lists, lists, blocks) must lie in main memory */
#define KZ3_GPOST_EA_HI 0xC0000000u

typedef enum {
    KZ3_GP_REJ_NONE = 0,
    KZ3_GP_REJ_TYPE, /* entry type != 1 or the flag nibble is not 0 */
    KZ3_GP_REJ_SIZE, /* entry size outside 0x60..0x100, not a multiple of 16, or != align16(8*ncmd)+0x30 */
    KZ3_GP_REJ_EA, /* command-list pointer 0, unaligned or out of range, or the batch's entry array (8*count bytes) out of range */
    KZ3_GP_REJ_CMD,    /* unknown/duplicate/misordered command, no RUN at the end, count formula violated */
    KZ3_GP_REJ_MODULE, /* no module GET (U0), or its EA is not a known Gfx.Post module of this build */
    KZ3_GP_REJ_JOB,    /* module's job id differs from the table row the batch matched */
    KZ3_GP_REJ_D,      /* user block D inconsistent (zero tile size, flags vs commands, column/row out of range) */
    KZ3_GP_REJ_SHAPE,  /* nIn / nParams / block sizes differ from the job's layout row (s.8) */
    KZ3_GP_REJ_PLIST,  /* parameter list pointer/size/pad invalid */
    KZ3_GP_REJ_PBLK,   /* parameter block pointer or size out of range */
    KZ3_GP_REJ_INLIST, /* input list size/count/first element disagrees with the input word of D */
    KZ3_GP_REJ_RMW,    /* read-modify-write list disagrees with D */
    KZ3_GP_REJ_TILES,  /* entry count != cols*rows, tile order, shared fields or destination EA arithmetic differ */
    KZ3_GP_REJ_BLOB,   /* parameter-block capacity or step capacity exhausted */
    KZ3_GP_REJ_N
} kz3_gpost_rej;

/* off/copied: bytes in p2.blob, 0 = not copied. Only element 0 (the parameter block proper, at most 0x220 bytes) is range-checked
 * (ea_ok) and copied. The ILRSharp chunk elements (j >= 1, nparam-1 of them) are validated only for non-zero size, a multiple of
 * 16 and at most 0x4000, and a 16-byte aligned non-zero EA: they are NOT range-checked against main memory and nothing is read
 * through them. The only cross-check is ilrs_chunks (EA = idx9.EA + k*0x4000 over pitch*h of surface 9, when the surface table
 * was valid). P3 must range-check (or derive from idx9) the chunk EAs itself before it reads or writes through them. */
typedef struct {
    uint32_t size, ea;
    uint16_t off, copied;
} kz3_gpost_pelem;
typedef struct {
    uint32_t list_ea, list_bytes, nel, first_size, first_ea;
    int8_t surf;
} kz3_gpost_inlist; /* surf: surface idx or -1 */

typedef struct {
    uint32_t batch;  /* index into plan.b */
    uint8_t ok, rej; /* rej = first kz3_gpost_rej */
    uint8_t job, nin, nparam, rmw, extra, mode;
    int8_t ilrs_chunks; /* ILRSharp: 1 chunk EAs/sizes equal idx9.EA + k*0x4000 over pitch*h, 0 differs, -1 n/a */
    uint32_t module_ea, module_bytes, tmask, brec;
    uint32_t ntiles, entry_size;
    uint32_t inw[4], dst_ea, tw, th, pitch, flags, cols, rows, bpp; /* tile 0's D, plain numbers */
    kz3_gpost_inlist in[4], rmwl;
    uint32_t plist_ea, plist_bytes;
    kz3_gpost_pelem pe[KZ3_GPOST_P2_MAXPARAM];
} kz3_gpost_pstep;

typedef struct {
    uint32_t frame, nstep, nok, nrej;
    uint32_t rej[KZ3_GP_REJ_N];
    uint32_t blob_used;
    uint32_t surf[KZ3_GPOST_NSURF][7]; /* +0 EA, +4 w, +8 h, +0xC pitch, +0x10 padded h, +0x14 fmt, +0x18 bytes/px */
    uint8_t surf_valid;
    kz3_gpost_pstep s[KZ3_GPOST_P2_MAXSTEP];
    unsigned char blob[KZ3_GPOST_P2_BLOB];
} kz3_gpost_p2;

typedef struct {
    uint32_t frame, S, nb;
    uint32_t item, renderer;
    uint8_t build;             /* kz3_gpost_build of the identify call */
    int match;                 /* 1 = the whole list matched the table */
    int nomatch_at;            /* batch index of the first divergence, -1 = none */
    const char* reason;        /* "claim" | "none" | "nomatch" | "viewports" | "aa-off" | "s0x100" | "toomany" | "off" */
    uint64_t want;             /* KZ3_GPOST_STEPS, bit n = seq n */
    uint64_t claim;            /* seq bits actually claimed this frame */
    uint64_t pred_skip;        /* wanted, but the predicate says the step does not run (e.g. S9 under 0x10000) */
    uint32_t nclaimed;         /* batches whose mask was zeroed */
    uint32_t blob_used, blob_trunc;
    kz3_gpost_batch b[KZ3_GPOST_MAXB];
    unsigned char blob[KZ3_GPOST_BLOB]; /* per claimed batch: {u32 batch index, u32 n, then n x (u64 entry, descriptor bytes)} */
    kz3_gpost_p2 p2;                    /* P2 snapshot of the claimed batches (below) */
} kz3_gpost_plan;

typedef struct {
    uint32_t ea[KZ3_GPOST_MAXB];
    uint32_t mask[KZ3_GPOST_MAXB];
    uint32_t n;
} kz3_gpost_saved;

/* The switch value: hex string -> bits (n = seq n), masked to 1..37; NULL or "" = 0. */
uint64_t kz3_gpost_parse_steps(const char* s);

/* Identify the batch list against the table. Fills p->b[].seq/job, p->match, p->nomatch_at, p->reason.
 * Reads only (never writes guest memory). */
void kz3_gpost_identify(const kz3_gpost_mem* m, kz3_gpost_build build, uint32_t item, uint32_t renderer, kz3_gpost_plan* p);

/* identify + claim decision + mask zeroing (writes only the claimed batches' +0xC). Returns the zeroed count. */
uint32_t kz3_gpost_begin(const kz3_gpost_mem* m, kz3_gpost_build build, uint32_t item, uint32_t renderer, uint64_t want,
                         kz3_gpost_plan* p, kz3_gpost_saved* sv);

/* restore every saved mask, snapshot the claimed batches' descriptors (by EA, after the runner). */
void kz3_gpost_end(const kz3_gpost_mem* m, kz3_gpost_plan* p, const kz3_gpost_saved* sv);

/* P2 accessors on a published plan: the parsed step of batch index i, and the bytes of a copied parameter block (NULL when
 * not copied). kz3_gpost_p2_step returns NULL for a batch that is not claimed AND for a claimed batch that was never parsed
 * because the 64-step cap (KZ3_GPOST_P2_MAXSTEP) was already reached: such a batch is counted in p2.nrej / rej[KZ3_GP_REJ_BLOB]
 * but has no entry in p2.s[], so nstep == nok + nrej holds only below the cap. P3 must treat NULL for a claimed batch as a
 * reject. */
const kz3_gpost_pstep* kz3_gpost_p2_step(const kz3_gpost_plan* p, uint32_t batch);
const uint32_t* kz3_gpost_p2_block(const kz3_gpost_plan* p, const kz3_gpost_pelem* e);
/* Parse one batch (tile 0 in full, every other tile for consistency) into *st; bumps p->p2.blob_used for copied blocks.
 * Returns the reject reason (0 = ok). Exposed for host tests; kz3_gpost_end calls it for each claimed batch.
 * Writes into the plan: p->p2.blob_used (restored on a reject) and *st. kz3_gpost_end (p2_snapshot) additionally writes
 * p->b[i].job of an accepted step when P1 left it 0 (the exact job id from the module GET, which P1's w0 heuristic cannot
 * give): this is the one P2 write into the P1 part of the plan; the identity check of P1 (rowjob) is unaffected, but a consumer
 * of plan.b[i].job sees 0 for a batch whose step was rejected or capped. */
kz3_gpost_rej kz3_gpost_p2_parse(const kz3_gpost_mem* m, kz3_gpost_build build, kz3_gpost_plan* p, uint32_t batch,
                                 kz3_gpost_pstep* st);
const char* kz3_gpost_rej_name(kz3_gpost_rej r);
/* "[gpost-p2] frame=.. steps=.. ok=.. rej=.. <per-reason counts>" one line, no newline */
int kz3_gpost_format_p2(const kz3_gpost_plan* p, char* out, int n);
/* one step: "[gpost-p2-step] batch=.. seq=.. job=.. ok=.. rej=.. tiles=.. nin=.. np=.. D=..." */
int kz3_gpost_format_p2_step(const kz3_gpost_plan* p, uint32_t i, char* out, int n);

/* G3 line: one line, no newline. Returns the length written. */
int kz3_gpost_format(const kz3_gpost_plan* p, char* out, int n);
/* the full list dump (one batch per call): "[gpost-list] i=.. ea=.. job=.. seq=.. mask=.. excl=.. ctx=.. n=.. w0=.. w1=.." */
int kz3_gpost_format_batch(const kz3_gpost_plan* p, uint32_t i, char* out, int n);

/* The per-frame plan ring (4 slots, frame & 3). kz3_gpost_publish stores a copy; readers (P3: the render thread at the
 * park) call kz3_gpost_plan_get(frame): the slot if its frame matches, else NULL. */
const kz3_gpost_plan* kz3_gpost_plan_get(uint32_t frame);
void kz3_gpost_publish(const kz3_gpost_plan* p);

#ifdef __cplusplus
}
#endif
#endif

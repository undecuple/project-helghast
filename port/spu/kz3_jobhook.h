/* kz3_jobhook.h -- SPU job I/O capture and native-job hook (SPU pass 120, runtime patch 1141).
 *
 * Engine-generic core (port/spu/kz3_jobhook.c) over the runtime's job-call bracket: when the WWS job manager policy
 * module branches into a job module's code, the runtime asks g_spu_jhook_open which function to run and calls
 * g_spu_jhook_close when the job has returned to the policy module. The core
 *   - captures chosen job calls (KZ3_JOB_CAPTURE): local store + registers at entry and exit, every channel op the job
 *     makes (reads with their values, writes, channel counts), every DMA element with its bytes, every atomic with its
 *     line, then the policy module's own traffic after the job (its output stores and completion) until its next job;
 *   - dispatches chosen job modules to a registered host function (KZ3_NATIVE_JOB) instead of the lifted entry, with
 *     the job's channel/DMA/completion protocol unchanged (the host function issues the same channel ops).
 * The per-title part is the table kz3_jobhook_title (where the job manager keeps its code buffer and job number) and
 * the native registration table kz3_native_jobs[] (kz3_native_jobs.c). tools/spu/job_replay.py replays a capture
 * against the lifted job (must be exact) or a native candidate.
 *
 * Capture file (cap.bin, host byte order for the header and registers, LS bytes as the guest has them):
 *   struct kz3_jcap_hdr; LS at entry (256 KiB); 128 registers at entry (16 bytes each, the runtime's u128 layout);
 *   LS at exit; registers at exit; then nevents records: struct kz3_jcap_ev followed by `size` data bytes padded to 8
 *   (XFER: the bytes moved, read from LS after the transfer; ATOM: the 128-byte LS line after the command).
 */
#ifndef KZ3_JOBHOOK_H
#define KZ3_JOBHOOK_H
#include <stdint.h>

#define KZ3_JCAP_MAGIC "SPUJCAP1"
#define KZ3_JCAP_LS 0x40000u

enum { KZ3_JEV_RD = 1, KZ3_JEV_CNT = 2, KZ3_JEV_WR = 3, KZ3_JEV_XFER = 4, KZ3_JEV_ATOM = 5, KZ3_JEV_KERN = 6 };
/* KERN (version 2): a SPURS kernel service the runtime answered without channel ops (selectWorkload at LS 0x9D0):
 * lsa = service LS address, cmd = link (r0), v = r3 after the call, size 0 */
#define KZ3_KERN_SELECT_WORKLOAD_LS 0x9D0u
/* IRQ_ENTER (version 2, phase IRQ): an SPU interrupt was taken inside the job call; lsa = srr0, size 0.
 * IRQ_LS (version 2, phase IRQ): at its iret, what the handler changed in LS: runs of {u32 lsa, u32 len, len bytes
 * padded to 4} (little-endian header words), size = total bytes; lsa = the resume pc */
enum { KZ3_JEV_IRQ_ENTER = 7, KZ3_JEV_IRQ_LS = 8 };
/* phases: JOB = the job's own work; IRQ = inside the job call while an SPU interrupt is being handled (ctx->irq_saved: the
 * policy module's interrupt handler, which claims and prefetches the next jobs while this one runs); POST = after the
 * job returned, until the policy module's next job call. Records keep their order across phases. */
enum { KZ3_JPH_JOB = 0, KZ3_JPH_POST = 1, KZ3_JPH_IRQ = 2 };

struct kz3_jcap_hdr {
    char magic[8];
    uint32_t version;               /* 2 (pass 121: KERN, IRQ_ENTER, IRQ_LS records); 1 = pass 120 */
    uint32_t image;                 /* job module image id (the title's lift numbering) */
    uint32_t jobnum;                /* the job manager's run job number (title table) */
    uint32_t codebase;              /* LS address of the job code buffer */
    uint32_t entry_pc;              /* LS address the job call entered */
    uint32_t lr;                    /* the policy module's link: the job call ends when control is back here */
    uint32_t call;                  /* this module's call index since start */
    uint32_t nevents;               /* all records */
    uint32_t njob;                  /* records inside the job call (phases JOB and IRQ; the rest: POST) */
    uint32_t flags;                 /* bit 0: the job ended at lr (close seen); bit 1: post phase cut by the size cap */
    uint64_t t_open_ns, t_close_ns; /* monotonic */
    uint32_t spu;                   /* the SPU context's spu_id */
    uint32_t pad[3];
};

struct kz3_jcap_ev {
    uint8_t kind, phase;
    uint16_t ch;   /* channel (RD/CNT/WR); unused for XFER; ATOM: 0 */
    uint32_t cmd;  /* XFER/ATOM: the MFC command */
    uint32_t lsa;  /* XFER/ATOM */
    uint32_t size; /* data bytes following the record (XFER: transfer size, ATOM: 128) */
    uint64_t ea;   /* XFER/ATOM */
    uint32_t v[4]; /* RD/WR: the 16-byte channel value; CNT: count in v[0] */
};

/* Native jobs. `lifted` is the function the job call would have run (the lifted entry): an identity wrapper calls
 * it; a real native job ignores it and issues the job's channel ops itself. SPU_ABI is the runtime's lifted-code
 * calling convention (sysv on Windows x64 since patch 1099): native functions are called through the same pointer
 * type, so declare them with KZ3_SPU_ABI. */
#if defined(SPU_ABI)
#define KZ3_SPU_ABI SPU_ABI /* include spu_context.h first to get the runtime's exact choice */
#elif defined(_WIN64) && defined(__GNUC__)
#define KZ3_SPU_ABI __attribute__((sysv_abi))
#else
#define KZ3_SPU_ABI
#endif
struct spu_context;
typedef void(KZ3_SPU_ABI* kz3_spu_fn)(struct spu_context*);
typedef void (*kz3_native_job_fn)(struct spu_context* ctx, kz3_spu_fn lifted);
struct kz3_native_job {
    int image;        /* job module image id */
    const char* name; /* KZ3_NATIVE_JOB=<image>:<name> picks it; the first entry of an image is the default */
    kz3_native_job_fn fn;
};
extern const struct kz3_native_job kz3_native_jobs[]; /* terminated by {0, 0, 0} */

/* Per-title layout of the job manager policy module (KZ3: WWS jobmanager, image 2). */
struct kz3_jobhook_title {
    uint32_t codebuf_ls; /* LS word holding the job code buffer's LS address (KZ3 0x1320) */
    uint32_t jobnum_ls;  /* LS word holding the run job number (KZ3 0x12E0) */
};
extern const struct kz3_jobhook_title kz3_jobhook_title;

/* Install from the env (KZ3_JOB_CAPTURE, KZ3_NATIVE_JOB); idempotent; called from kz3_spu_apply_skip_env. */
void kz3_jobhook_install(void);
/* The env parsers, exposed for the host test. */
int kz3_jobhook_parse_capture(const char* s);
int kz3_jobhook_parse_native(const char* s);
#endif

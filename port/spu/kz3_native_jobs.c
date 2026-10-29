/* kz3_native_jobs.c -- KZ3's per-title table for the job hook (kz3_jobhook.c, SPU pass 120): the WWS job manager's
 * LS layout and the registered native jobs. KZ3_NATIVE_JOB=<img>[:<name>] picks an entry; `<img>:identity` is
 * built into the core (calls the lifted entry: the hook's A/B control) and needs no row here.
 *
 * Adding a native job: a row {image, "name", fn} here, a host function with the kz3_native_job_fn
 * signature that issues the job's channel ops (the same DMA-outs, atomics and completion the lifted job makes, in an
 * order the policy module accepts), and its equality result from tools/spu/job_replay.py --native on captures of
 * the job (byte-exact; a float tolerance only with sign-off for that job). Default-on only as a milestone:
 * the equality result plus a 3-pair turn/walk A/B. Empty this round: no job is replaced yet. */
#include "kz3_jobhook.h"
#include "kz3_native_jobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* WWS jobmanager policy module (image 2) LS layout: the job code buffer's LS address at 0x1320 (the runtime's
 * job-call bracket, patch 1085, reads the same word) and the run job number at 0x12E0 (patch 1114's job bracket). */
const struct kz3_jobhook_title kz3_jobhook_title = {0x1320u, 0x12E0u};

const struct kz3_native_job kz3_native_jobs[] = {
    {0, 0, 0},
};

/* ---- per-function native gates (SPU pass 122, D88-; KZ2's gate design, credited in kz3_native_jobs.h) ----
 * The lifted functions listed in re/spu/native_gates.tsv start with
 *   if (g_kz3_native_gate[slot] && g_kz3_native_gate[slot](ctx)) SPU_RET(ctx);
 * g_kz3_native_gate is defined here, once. A slot is filled when its entry is on (a switch =1, or no switch set and the entry default-on: only the Gfx.LightProbes
 * entries of the 1.14 job, SPU pass 127; every switch =0 turns an entry off), by the
 * trampoline that runs the call through kz3_native_run (count, debug gate, constant pool, kernel). Switches:
 * KZ3_NATIVE_JOB_POST=1 (every Gfx.Post gate), KZ3_NATIVE_JOB_POSTMIX=1 (blend), KZ3_NATIVE_JOB_DOF=1,
 * KZ3_NATIVE_JOB_UPSAMPLE=1, KZ3_NATIVE_JOB_MOTIONBLUR=1,
 * KZ3_NATIVE_JOB_LIGHTPROBES=1/0 (all Gfx.LightProbes kernels; default on for 1.14, off for the 1.00 twins; _LP_KD / _LP_BAKE / _LP_BLEND / _LP_ATTEN / _LP_LIGHT one each). KZ3_NATIVE_REC=DIR records real gate inputs instead
 * (KZ3_NATIVE_REC_N per slot, default 24; KZ3_NATIVE_REC_EVERY calls between records, default 61;
 * KZ3_NATIVE_REC_AFTER seconds before the first, default 120); the lifted body then runs. Records hold game data:
 * under work/ only, never committed. */
int (*g_kz3_native_gate[64])(spu_context*);
volatile long g_kz3_native_calls[64], g_kz3_native_fallback[64];

static const kz3_native_entry* s_ent[64];

/* Calls and fallbacks per 10-second window since the process start (SPU pass 125; pass 124 only had totals, so "bake
 * and lights run in the load phase" was an assumption). The series shows which windows each gate is busy in: the load
 * phase is the burst at the start, the steady state the flat tail; tools/spu/native_counts.py splits a log at a
 * given second. The last window collects everything past 64 x 10 s. */
#define KZ3_NATIVE_WINS 64
#define KZ3_NATIVE_WIN_S 10
static volatile long s_win_calls[64][KZ3_NATIVE_WINS], s_win_fb[64][KZ3_NATIVE_WINS];
static unsigned long long s_tick0;

static unsigned long long tick_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
#endif
}

static unsigned win_now(void)
{
    const unsigned long long w = (tick_ms() - s_tick0) / (1000ull * KZ3_NATIVE_WIN_S);
    return w >= KZ3_NATIVE_WINS ? KZ3_NATIVE_WINS - 1 : (unsigned)w;
}

static void kz3_native_exit_print(void);
static int s_last_win = -1;

void kz3_native_win_hit(int slot)
{
    const unsigned w = win_now();
    __atomic_fetch_add(&s_win_calls[slot][w], 1, __ATOMIC_RELAXED);
    /* the first call in a new window prints every slot's series (the process is usually ended without atexit) */
    if (__builtin_expect((int)w != s_last_win, 0) && __atomic_exchange_n(&s_last_win, (int)w, __ATOMIC_RELAXED) != (int)w)
        kz3_native_exit_print();
}

void kz3_native_count_print(int slot, long calls)
{
    char buf[900];
    size_t n = 0;
    for (unsigned w = 0; w < KZ3_NATIVE_WINS && n + 40 < sizeof buf; w++) {
        const long c = s_win_calls[slot][w], f = s_win_fb[slot][w];
        if (c || f) n += (size_t)snprintf(buf + n, sizeof buf - n, f ? " %u:%ld/%ld" : " %u:%ld", w, c, f);
    }
    buf[n] = 0;
    fprintf(stderr, "[native] slot %d %s 0x%05X calls=%ld fallback=%ld win%ds(window:calls[/fallback]):%s\n", slot,
            s_ent[slot] ? s_ent[slot]->image : "?", s_ent[slot] ? s_ent[slot]->addr : 0u, calls,
            g_kz3_native_fallback[slot], KZ3_NATIVE_WIN_S, buf);
}

static void kz3_native_exit_print(void)
{
    for (int s = 0; s < 64; s++)
        if (g_kz3_native_calls[s]) kz3_native_count_print(s, g_kz3_native_calls[s]);
}

const kz3_native_entry* kz3_native_find(int slot)
{
    for (unsigned i = 0; i < kz3_native_post_n; i++)
        if (kz3_native_post[i].slot == slot) return &kz3_native_post[i];
    for (unsigned i = 0; i < kz3_native_dof_n; i++)
        if (kz3_native_dof[i].slot == slot) return &kz3_native_dof[i];
    for (unsigned i = 0; i < kz3_native_lp_n; i++)
        if (kz3_native_lp[i].slot == slot) return &kz3_native_lp[i];
    return NULL;
}

static uint64_t pool_hash(const uint8_t* ls, uint32_t lo, uint32_t hi)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t a = lo; a < hi; a += 8) {
        uint64_t v;
        memcpy(&v, ls + a, 8);
        h = (h ^ v) * 0x100000001b3ull;
    }
    return h;
}

/* The pool is a few dozen quadwords: hashed on every call (tens of ns against a kernel that moves thousands of
 * pixels). A job never writes its own constant pool. */
int kz3_native_run(spu_context* ctx, const kz3_native_entry* e)
{
    kz3_native_count(e->slot);
    if (!spu_ls_dbg_armed() && e->pool_hi > e->pool_lo && e->pool_hi <= SPU_LS_SIZE &&
        pool_hash(ctx->ls, e->pool_lo, e->pool_hi) == e->pool_hash && e->fn(ctx))
        return 1;
    __atomic_fetch_add(&g_kz3_native_fallback[e->slot], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_win_fb[e->slot][win_now()], 1, __ATOMIC_RELAXED);
    return 0;
}

static const char* s_rec_dir;
static long s_rec_n = 24, s_rec_every = 61;
static double s_rec_after = 120.0;
static volatile long s_rec_calls[64], s_rec_done[64];
static time_t s_t0;

static long env_long(const char* k, long d)
{
    const char* e = getenv(k);
    return (e && *e) ? strtol(e, NULL, 0) : d;
}

static int rec(spu_context* ctx, int slot)
{
    kz3_native_count(slot);
    if (difftime(time(NULL), s_t0) < s_rec_after) return 0;
    const long c = __atomic_fetch_add(&s_rec_calls[slot], 1, __ATOMIC_RELAXED);
    if (c % s_rec_every) return 0;
    const long n = __atomic_fetch_add(&s_rec_done[slot], 1, __ATOMIC_RELAXED);
    if (n >= s_rec_n) return 0;
    char path[1024];
    snprintf(path, sizeof path, "%s/s%02d_%03ld.kz3nj", s_rec_dir, slot, n);
    FILE* f = fopen(path, "wb");
    if (!f) {
        if (n == 0) fprintf(stderr, "[native] record: cannot write %s\n", path);
        return 0;
    }
    uint32_t hdr[4] = { (uint32_t)slot, s_ent[slot] ? s_ent[slot]->addr : 0u, (uint32_t)ctx->image_id, 0u };
    fwrite(KZ3_NJ_MAGIC, 1, 8, f);
    fwrite(hdr, 4, 4, f);
    fwrite(ctx->gpr, 16, 128, f);
    fwrite(ctx->ls, 1, SPU_LS_SIZE, f);
    fclose(f);
    fprintf(stderr, "[native] recorded slot %d #%ld (call %ld) -> %s\n", slot, n, c, path);
    return 0;   /* the lifted body runs */
}

#define T1(a, b) static int gate_##a##b(spu_context* c) { return kz3_native_run(c, s_ent[8 * a + b]); } \
                 static int rec_##a##b(spu_context* c) { return rec(c, 8 * a + b); }
#define T8(a) T1(a, 0) T1(a, 1) T1(a, 2) T1(a, 3) T1(a, 4) T1(a, 5) T1(a, 6) T1(a, 7)
T8(0) T8(1) T8(2) T8(3) T8(4) T8(5) T8(6) T8(7)
#define P8(f, a) f##a##0, f##a##1, f##a##2, f##a##3, f##a##4, f##a##5, f##a##6, f##a##7
static int (*const s_gate_fn[64])(spu_context*) = { P8(gate_, 0), P8(gate_, 1), P8(gate_, 2), P8(gate_, 3),
                                                     P8(gate_, 4), P8(gate_, 5), P8(gate_, 6), P8(gate_, 7) };
static int (*const s_rec_fn[64])(spu_context*) = { P8(rec_, 0), P8(rec_, 1), P8(rec_, 2), P8(rec_, 3),
                                                    P8(rec_, 4), P8(rec_, 5), P8(rec_, 6), P8(rec_, 7) };

/* Which switch decides an entry (SPU pass 127). The entry's flags are '|'-separated, general first, the single kernel
 * last; the last one that is set (not empty) decides: "1" on, "0" off, so KZ3_NATIVE_JOB_LP_KD=1 beats
 * KZ3_NATIVE_JOB_LIGHTPROBES=0. A Gfx.Post job also honours KZ3_NATIVE_JOB_POST=1 (before its own flags). Nothing set:
 * the entry's def_on (re/spu/native_gates.tsv column "default"). Returns 1 on, 0 off; *by names the switch or is
 * "default". */
static int native_decide(const kz3_native_entry* e, const char** by)
{
    static char buf[64];
    int on = e->def_on ? 1 : 0;
    *by = "default";
    const char* v = getenv("KZ3_NATIVE_JOB_POST");
    if (v && *v && !strncmp(e->job, "Gfx.Post.", 9)) {
        on = *v == '1';
        *by = "KZ3_NATIVE_JOB_POST";
    }
    const char* p = e->env;
    while (p && *p) {
        const char* q = strchr(p, '|');
        const size_t n = q ? (size_t)(q - p) : strlen(p);
        if (n < sizeof buf) {
            memcpy(buf, p, n);
            buf[n] = 0;
            v = getenv(buf);
            if (v && *v) {
                on = *v == '1';
                *by = buf;
            }
        }
        p = q ? q + 1 : NULL;
    }
    return on;
}

static void gate_add(const kz3_native_entry* t, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        const kz3_native_entry* e = &t[i];
        if (e->slot < 0 || e->slot >= 64) continue;
        s_ent[e->slot] = e;
        if (s_rec_dir) {
            g_kz3_native_gate[e->slot] = s_rec_fn[e->slot];
            continue;
        }
        const char* by;
        if (native_decide(e, &by)) {
            g_kz3_native_gate[e->slot] = s_gate_fn[e->slot];
            fprintf(stderr, "[native] %s %s 0x%05X %s native (slot %d, %s%s)\n", e->job, e->image, e->addr, e->kind,
                    e->slot, by, strcmp(by, "default") ? "=1" : "");
        } else if (e->def_on)
            fprintf(stderr, "[native] %s %s 0x%05X %s lifted (slot %d, %s=0)\n", e->job, e->image, e->addr, e->kind,
                    e->slot, by);
    }
}

__attribute__((constructor)) static void kz3_native_gates_init(void)
{
    s_t0 = time(NULL);
    s_tick0 = tick_ms();
    atexit(kz3_native_exit_print);
    const char* d = getenv("KZ3_NATIVE_REC");
    if (d && *d) {
        s_rec_dir = d;
        s_rec_n = env_long("KZ3_NATIVE_REC_N", 24);
        s_rec_every = env_long("KZ3_NATIVE_REC_EVERY", 61);
        if (s_rec_every < 1) s_rec_every = 1;
        s_rec_after = (double)env_long("KZ3_NATIVE_REC_AFTER", 120);
        fprintf(stderr, "[native] recording gate inputs to %s (%ld per slot, every %ld calls, after %.0f s)\n", d,
                s_rec_n, s_rec_every, s_rec_after);
    }
    gate_add(kz3_native_post, kz3_native_post_n);
    gate_add(kz3_native_dof, kz3_native_dof_n);
    gate_add(kz3_native_lp, kz3_native_lp_n);
}

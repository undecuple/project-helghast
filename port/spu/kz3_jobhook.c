/* kz3_jobhook.c -- SPU job I/O capture and native-job hook (SPU pass 120, runtime patch 1141). See kz3_jobhook.h.
 *
 * KZ3_JOB_CAPTURE=<img>[,<img>...][,n=N][,skip=K][,t=SECONDS][,post=P]
 *   captures job calls of the listed job module images (the lift's image ids, e.g. 255 = Anim.Skinning): the first
 *   N (default 8) calls of each image after skipping K (default 0), counted from T seconds after the hook was
 *   installed (default 0), into KZ3_JOB_CAPTURE_OUT (default "jobcap", the working directory)/ovl<img>/<call>/:
 *   cap.bin (kz3_jobhook.h) and index.json. After the job returns, up to P (default 256) channel/DMA records of the
 *   policy module's own work (output stores, completion) are kept as the post phase; it ends at the policy module's
 *   next job call on that SPU thread or at the cap. The capture hook is installed only while a capture can still
 *   happen and removed when every image has its N captures. Captures are written on the SPU thread (the run slows
 *   during them: a measurement switch, not for timing runs).
 * KZ3_NATIVE_JOB=<img>[:<name>][,...]
 *   the listed images' job calls run the registered host function (kz3_native_jobs[], first entry of the image or
 *   the one named) instead of the lifted entry. `<img>:identity` = a wrapper that calls the lifted entry (the hook's
 *   own cost, A/B control). An image with no registered function is reported and ignored.
 * Both off (unset) = nothing installed: the runtime's hooks stay null (patch 1141's cost when off is one global test
 * per channel op and per DMA element).
 */
#include "spu_context.h"
#include "kz3_jobhook.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <direct.h>
#define kz3_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define kz3_mkdir(p) mkdir((p), 0777)
#endif

/* runtime patch 1141 */
extern void (*g_spu_jcap_ch)(spu_context*, int kind, uint32_t ch, const u128* v);
extern void (*g_spu_jcap_xfer)(spu_context*, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd);
extern void (*g_spu_jcap_irq)(spu_context*, int enter, uint32_t pc);
extern int g_spu_jhook_on;
extern void(SPU_ABI* (*g_spu_jhook_open)(spu_context*, uint32_t lr, void(SPU_ABI* fn)(spu_context*)))(spu_context*);
extern void (*g_spu_jhook_close)(spu_context*);
extern int spu_overlay_match_sig(const uint8_t hdr[16]);

#define JH_MAXIMG 512
#define JH_EVCAP (192u << 20) /* event bytes per capture; a job past it is dropped (reported) */

#if defined(_WIN32)
#include <windows.h>
static uint64_t jh_now_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
static uint64_t jh_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#endif
static uint32_t be32(const uint8_t* p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* ---- configuration ---- */
static uint8_t s_cap_want[JH_MAXIMG];
static unsigned s_cap_n = 8, s_cap_skip = 0, s_cap_post = 256;
static double s_cap_t = 0;
static const char* s_cap_out = "jobcap";
static volatile unsigned s_cap_seen[JH_MAXIMG], s_cap_done[JH_MAXIMG];
static volatile int s_cap_left; /* images still owed captures */
static volatile int s_cap_live; /* captures open now (job or post phase) */
static uint64_t s_t0;
static kz3_native_job_fn s_native[JH_MAXIMG];
static int s_native_any;

int kz3_jobhook_parse_capture(const char* s)
{
    memset(s_cap_want, 0, sizeof s_cap_want);
    int n = 0;
    for (const char* q = s; q && *q;) {
        char* end;
        if (!strncmp(q, "n=", 2))
            s_cap_n = (unsigned)strtoul(q + 2, &end, 0);
        else if (!strncmp(q, "skip=", 5))
            s_cap_skip = (unsigned)strtoul(q + 5, &end, 0);
        else if (!strncmp(q, "post=", 5))
            s_cap_post = (unsigned)strtoul(q + 5, &end, 0);
        else if (!strncmp(q, "t=", 2))
            s_cap_t = strtod(q + 2, &end);
        else {
            if (!strncmp(q, "ovl", 3)) q += 3;
            const long img = strtol(q, &end, 0);
            if (end == q) return -1;
            if (img > 0 && img < JH_MAXIMG && !s_cap_want[img]) {
                s_cap_want[img] = 1;
                n++;
            }
        }
        q = end;
        if (*q == ',')
            q++;
        else if (*q)
            return -1;
    }
    s_cap_left = s_cap_n ? n : 0;
    return n;
}

static void jh_identity(spu_context* ctx, kz3_spu_fn lifted)
{
    lifted(ctx);
}
int kz3_jobhook_parse_native(const char* s)
{
    memset(s_native, 0, sizeof s_native);
    s_native_any = 0;
    int n = 0;
    for (const char* q = s; q && *q;) {
        char* end;
        if (!strncmp(q, "ovl", 3)) q += 3;
        const long img = strtol(q, &end, 0);
        if (end == q) return -1;
        q = end;
        const char* name = NULL;
        size_t nl = 0;
        if (*q == ':') {
            name = ++q;
            while (*q && *q != ',') q++;
            nl = (size_t)(q - name);
        }
        kz3_native_job_fn fn = NULL;
        if (name && nl == 8 && !strncmp(name, "identity", 8))
            fn = jh_identity;
        else
            for (const struct kz3_native_job* j = kz3_native_jobs; j->fn; j++)
                if (j->image == img && (!name || (strlen(j->name) == nl && !strncmp(j->name, name, nl)))) {
                    fn = j->fn;
                    break;
                }
        if (img > 0 && img < JH_MAXIMG && fn) {
            if (!s_native[img]) n++;
            s_native[img] = fn;
            s_native_any = 1;
        } else
            fprintf(stderr, "[jobhook] KZ3_NATIVE_JOB: image %ld%s%.*s has no registered host function: ignored\n", img,
                    name ? ":" : "", (int)nl, name ? name : "");
        if (*q == ',')
            q++;
        else if (*q)
            return -1;
    }
    return n;
}

/* ---- per-SPU-thread capture state ---- */
struct jh_cap {
    int on;  /* 1 job phase, 2 post phase */
    spu_context* ctx;
    struct kz3_jcap_hdr h;
    uint8_t* ls_in;
    u128 regs_in[128], regs_out[128];
    uint8_t* ls_out;
    uint8_t* ls_irq; /* LS when an interrupt was taken inside the job call (version 2) */
    int in_irq;
    uint8_t* ev;
    size_t evn, evcap;
    unsigned post_n;
    int overflow;
};
static _Thread_local struct jh_cap t_cap;
static _Thread_local kz3_native_job_fn t_native;
static _Thread_local kz3_spu_fn t_lifted;

static void jh_ev_put(struct jh_cap* c, const struct kz3_jcap_ev* e, const uint8_t* data)
{
    const size_t pad = ((size_t)e->size + 7u) & ~(size_t)7u;
    const size_t need = c->evn + sizeof *e + pad;
    if (need > JH_EVCAP) {
        c->overflow = 1;
        return;
    }
    if (need > c->evcap) {
        size_t nc = c->evcap ? c->evcap * 2 : (1u << 20);
        while (nc < need) nc *= 2;
        uint8_t* p = (uint8_t*)realloc(c->ev, nc);
        if (!p) {
            c->overflow = 1;
            return;
        }
        c->ev = p;
        c->evcap = nc;
    }
    memcpy(c->ev + c->evn, e, sizeof *e);
    if (e->size) memcpy(c->ev + c->evn + sizeof *e, data, e->size);
    if (pad > e->size) memset(c->ev + c->evn + sizeof *e + e->size, 0, pad - e->size);
    c->evn = need;
    c->h.nevents++;
    if (c->on == 1)
        c->h.njob++; /* job and interrupt-handler records */
    else
        c->post_n++;
}

static void jh_finish(struct jh_cap* c);

static void jh_ch(spu_context* ctx, int kind, uint32_t ch, const u128* v)
{
    struct jh_cap* c = &t_cap;
    if (!c->on || c->ctx != ctx) return;
    struct kz3_jcap_ev e;
    memset(&e, 0, sizeof e);
    e.kind = (uint8_t)kind;
    e.phase = c->on == 2 ? KZ3_JPH_POST : ctx->irq_saved ? KZ3_JPH_IRQ : KZ3_JPH_JOB;
    if (kind == KZ3_JEV_KERN) { /* selectWorkload answered: ch = link, v = r3 */
        e.lsa = KZ3_KERN_SELECT_WORKLOAD_LS;
        e.cmd = ch;
        if (v) memcpy(e.v, v, 16);
        jh_ev_put(c, &e, NULL);
    } else if (kind == KZ3_JEV_ATOM) {
        const uint32_t lsa = ctx->mfc_lsa & (KZ3_JCAP_LS - 1);
        e.cmd = ch;
        e.lsa = lsa;
        e.ea = ctx->mfc_eal & ~127u;
        e.size = lsa + 128 <= KZ3_JCAP_LS ? 128 : KZ3_JCAP_LS - lsa;
        jh_ev_put(c, &e, &ctx->ls[lsa]);
    } else {
        e.ch = (uint16_t)ch;
        if (v) memcpy(e.v, v, 16);
        jh_ev_put(c, &e, NULL);
    }
    if (c->on == 2 && c->post_n >= s_cap_post) {
        c->h.flags |= 2;
        jh_finish(c);
    }
}

static void jh_xfer(spu_context* ctx, uint32_t lsa, uint64_t ea, uint32_t size, uint32_t cmd)
{
    struct jh_cap* c = &t_cap;
    if (!c->on || c->ctx != ctx) return;
    struct kz3_jcap_ev e;
    memset(&e, 0, sizeof e);
    e.kind = KZ3_JEV_XFER;
    e.phase = c->on == 2 ? KZ3_JPH_POST : ctx->irq_saved ? KZ3_JPH_IRQ : KZ3_JPH_JOB;
    e.cmd = cmd;
    e.lsa = lsa & (KZ3_JCAP_LS - 1);
    e.ea = ea;
    e.size = e.lsa + size <= KZ3_JCAP_LS ? size : KZ3_JCAP_LS - e.lsa;
    jh_ev_put(c, &e, &ctx->ls[e.lsa]);
}

/* interrupt taken (enter) / iret inside a captured job call: record the handler's net LS change */
static void jh_irq(spu_context* ctx, int enter, uint32_t pc)
{
    struct jh_cap* c = &t_cap;
    if (c->on != 1 || c->ctx != ctx) return;
    struct kz3_jcap_ev e;
    memset(&e, 0, sizeof e);
    e.phase = KZ3_JPH_IRQ;
    e.lsa = pc;
    if (enter) {
        if (!c->ls_irq) c->ls_irq = (uint8_t*)malloc(KZ3_JCAP_LS);
        if (!c->ls_irq) return;
        /* a take with no iret seen since the last one (the runtime takes the next pending event at the resume pc
         * before the restore; K4 spu121-capk4: list stall-and-notify handled twice) keeps the first snapshot, so
         * the record at the iret holds both handlers' changes */
        if (!c->in_irq) memcpy(c->ls_irq, ctx->ls, KZ3_JCAP_LS);
        c->in_irq = 1;
        e.kind = KZ3_JEV_IRQ_ENTER;
        jh_ev_put(c, &e, NULL);
        return;
    }
    if (!c->in_irq) return;
    c->in_irq = 0;
    size_t cap = 4096, n = 0;
    uint8_t* d = (uint8_t*)malloc(cap);
    if (!d) return;
    for (uint32_t i = 0; i < KZ3_JCAP_LS;) {
        if (ctx->ls[i] == c->ls_irq[i]) {
            i++;
            continue;
        }
        uint32_t j = i + 1, same = 0;
        for (; j < KZ3_JCAP_LS && same < 16; j++) same = ctx->ls[j] == c->ls_irq[j] ? same + 1 : 0;
        const uint32_t len = j - i - same;
        const size_t need = n + 8 + ((len + 3u) & ~3u);
        if (need > cap) {
            while (need > cap) cap *= 2;
            uint8_t* nd = (uint8_t*)realloc(d, cap);
            if (!nd) {
                free(d);
                return;
            }
            d = nd;
        }
        memcpy(d + n, &i, 4);
        memcpy(d + n + 4, &len, 4);
        memcpy(d + n + 8, &ctx->ls[i], len);
        memset(d + n + 8 + len, 0, ((len + 3u) & ~3u) - len);
        n = need;
        i += len;
    }
    e.kind = KZ3_JEV_IRQ_LS;
    e.size = (uint32_t)n;
    jh_ev_put(c, &e, d);
    free(d);
}
static void jh_json_list(FILE* f, const struct jh_cap* c, const char* key, int phase, int want)
{
    /* want: 1 GET-like XFER, 2 PUT-like XFER, 3 ATOM */
    fprintf(f, ",\n \"%s\": [", key);
    unsigned k = 0;
    for (size_t o = 0; o < c->evn;) {
        const struct kz3_jcap_ev* e = (const struct kz3_jcap_ev*)(c->ev + o);
        const uint32_t op = e->cmd & 0xFF;
        const int cls = e->kind == KZ3_JEV_ATOM   ? 3
                        : e->kind == KZ3_JEV_XFER ? ((op >= 0x40 && op <= 0x4F)   ? 1
                                                     : (op >= 0x20 && op <= 0x3F) ? 2
                                                                                  : 0)
                                                  : 0;
        if (cls == want && e->phase == phase && k < 512) {
            fprintf(f, "%s[\"0x%02X\",\"0x%08llX\",\"0x%05X\",%u]", k ? "," : "", e->cmd & 0xFFFF,
                    (unsigned long long)e->ea, e->lsa, e->size);
            k++;
        }
        o += sizeof *e + ((e->size + 7u) & ~7u);
    }
    fprintf(f, "]");
}

static void jh_finish(struct jh_cap* c)
{
    const int img = (int)c->h.image;
    if (c->overflow) {
        fprintf(stderr, "[jobcap] image %d call %u: more than %u MB of records, dropped\n", img, c->h.call,
                JH_EVCAP >> 20);
    } else {
        char dir[512], path[600];
        snprintf(dir, sizeof dir, "%s", s_cap_out);
        kz3_mkdir(dir);
        snprintf(dir, sizeof dir, "%s/ovl%d", s_cap_out, img);
        kz3_mkdir(dir);
        snprintf(dir, sizeof dir, "%s/ovl%d/%u", s_cap_out, img, c->h.call);
        kz3_mkdir(dir);
        snprintf(path, sizeof path, "%s/cap.bin", dir);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(&c->h, sizeof c->h, 1, f);
            fwrite(c->ls_in, 1, KZ3_JCAP_LS, f);
            fwrite(c->regs_in, sizeof c->regs_in, 1, f);
            fwrite(c->ls_out, 1, KZ3_JCAP_LS, f);
            fwrite(c->regs_out, sizeof c->regs_out, 1, f);
            fwrite(c->ev, 1, c->evn, f);
            fclose(f);
        }
        snprintf(path, sizeof path, "%s/index.json", dir);
        f = fopen(path, "w");
        if (f) {
            fprintf(f,
                    "{\"image\": %d, \"call\": %u, \"jobnum\": \"0x%08X\", \"codebase\": \"0x%05X\", "
                    "\"entry\": \"0x%05X\", \"lr\": \"0x%05X\", \"spu\": %u, \"ended\": %u, \"post_cut\": %u,\n"
                    " \"t_open_ms\": %.3f, \"dur_us\": %.1f, \"events\": %u, \"job_events\": %u",
                    img, c->h.call, c->h.jobnum, c->h.codebase, c->h.entry_pc, c->h.lr, c->h.spu, c->h.flags & 1,
                    (c->h.flags >> 1) & 1, (double)(c->h.t_open_ns - s_t0) / 1e6,
                    (double)(c->h.t_close_ns - c->h.t_open_ns) / 1e3, c->h.nevents, c->h.njob);
            jh_json_list(f, c, "job_dma_in", 0, 1);
            jh_json_list(f, c, "job_dma_out", 0, 2);
            jh_json_list(f, c, "job_atomic", 0, 3);
            jh_json_list(f, c, "irq_dma_in", 2, 1);
            jh_json_list(f, c, "irq_dma_out", 2, 2);
            jh_json_list(f, c, "irq_atomic", 2, 3);
            jh_json_list(f, c, "post_dma_in", 1, 1);
            jh_json_list(f, c, "post_dma_out", 1, 2);
            jh_json_list(f, c, "post_atomic", 1, 3);
            fprintf(f, "\n}\n");
            fclose(f);
        }
        fprintf(stderr, "[jobcap] image %d call %u: %u records (%u job), %.1f us -> %s\n", img, c->h.call, c->h.nevents,
                c->h.njob, (double)(c->h.t_close_ns - c->h.t_open_ns) / 1e3, dir);
    }
    c->on = 0;
    c->evn = 0;
    c->overflow = 0;
    if (__atomic_sub_fetch(&s_cap_live, 1, __ATOMIC_ACQ_REL) == 0 &&
        __atomic_load_n(&s_cap_left, __ATOMIC_ACQUIRE) <= 0) {
        g_spu_jcap_ch = 0;
        g_spu_jcap_xfer = 0;
        g_spu_jcap_irq = 0;
        if (!s_native_any) g_spu_jhook_on = 0;
    }
}

static void KZ3_SPU_ABI jh_native_tramp(spu_context* ctx)
{
    t_native(ctx, t_lifted);
}

static void(SPU_ABI* jh_open(spu_context* ctx, uint32_t lr, void(SPU_ABI* fn)(spu_context*)))(spu_context*)
{
    const uint32_t cb = be32(&ctx->ls[kz3_jobhook_title.codebuf_ls]);
    const int img = cb >= 0x4000 && cb + 16 <= KZ3_JCAP_LS ? spu_overlay_match_sig(&ctx->ls[cb]) : 0;
    if (img <= 0 || img >= JH_MAXIMG) return fn;
    struct jh_cap* c = &t_cap;
    if (c->on) jh_finish(c); /* the previous job's post phase ends at this job call */
    if (s_cap_want[img] && s_cap_left > 0) {
        const unsigned seen = __atomic_fetch_add(&s_cap_seen[img], 1, __ATOMIC_RELAXED);
        const uint64_t now = jh_now_ns();
        if (seen >= s_cap_skip && (double)(now - s_t0) >= s_cap_t * 1e9) {
            const unsigned k = __atomic_fetch_add(&s_cap_done[img], 1, __ATOMIC_RELAXED);
            if (k < s_cap_n) {
                if (k + 1 == s_cap_n) __atomic_sub_fetch(&s_cap_left, 1, __ATOMIC_ACQ_REL);
                if (!c->ls_in) {
                    c->ls_in = (uint8_t*)malloc(KZ3_JCAP_LS);
                    c->ls_out = (uint8_t*)malloc(KZ3_JCAP_LS);
                }
                if (c->ls_in && c->ls_out) {
                    memset(&c->h, 0, sizeof c->h);
                    memcpy(c->h.magic, KZ3_JCAP_MAGIC, 8);
                    c->h.version = 2;
                    c->h.image = (uint32_t)img;
                    c->h.jobnum = be32(&ctx->ls[kz3_jobhook_title.jobnum_ls]);
                    c->h.codebase = cb;
                    c->h.entry_pc = ctx->pc & (KZ3_JCAP_LS - 1);
                    c->h.lr = lr;
                    c->h.call = k;
                    c->h.spu = ctx->spu_id;
                    c->h.t_open_ns = now;
                    memcpy(c->ls_in, ctx->ls, KZ3_JCAP_LS);
                    memcpy(c->regs_in, ctx->gpr, sizeof c->regs_in);
                    c->ctx = ctx;
                    c->evn = 0;
                    c->post_n = 0;
                    c->overflow = 0;
                    c->in_irq = 0;
                    __atomic_add_fetch(&s_cap_live, 1, __ATOMIC_ACQ_REL);
                    c->on = 1;
                }
            }
        }
    }
    if (s_native[img]) {
        t_native = s_native[img];
        t_lifted = (kz3_spu_fn)fn;
        return jh_native_tramp;
    }
    return fn;
}

static void jh_close(spu_context* ctx)
{
    struct jh_cap* c = &t_cap;
    if (c->on != 1 || c->ctx != ctx) return;
    c->h.t_close_ns = jh_now_ns();
    c->h.flags |= 1;
    memcpy(c->ls_out, ctx->ls, KZ3_JCAP_LS);
    memcpy(c->regs_out, ctx->gpr, sizeof c->regs_out);
    c->on = 2;
    if (!s_cap_post) jh_finish(c);
}

void kz3_jobhook_install(void)
{
    static int done;
    if (done) return;
    done = 1;
    s_t0 = jh_now_ns();
    const char* cs = getenv("KZ3_JOB_CAPTURE");
    const char* ns = getenv("KZ3_NATIVE_JOB");
    const char* o = getenv("KZ3_JOB_CAPTURE_OUT");
    if (o && *o) s_cap_out = o;
    int nc = 0, nn = 0;
    if (cs && *cs) {
        nc = kz3_jobhook_parse_capture(cs);
        if (nc < 0) fprintf(stderr, "[jobhook] KZ3_JOB_CAPTURE=%s: malformed, off\n", cs);
    }
    if (ns && *ns) {
        nn = kz3_jobhook_parse_native(ns);
        if (nn < 0) fprintf(stderr, "[jobhook] KZ3_NATIVE_JOB=%s: malformed, off\n", ns);
    }
    if (nc <= 0 && nn <= 0) return;
    if (nc > 0 && s_cap_n) {
        g_spu_jcap_xfer = jh_xfer;
        g_spu_jcap_irq = jh_irq;
        g_spu_jcap_ch = jh_ch;
        fprintf(stderr,
                "[jobhook] capture: %d image(s), %u call(s) each after %u, from t=%.1f s, post %u -> %s/ "
                "(KZ3_JOB_CAPTURE, KZ3 patch 1141)\n",
                nc, s_cap_n, s_cap_skip, s_cap_t, s_cap_post, s_cap_out);
    }
    if (nn > 0) fprintf(stderr, "[jobhook] native: %d image(s) dispatch to host functions (KZ3_NATIVE_JOB)\n", nn);
    g_spu_jhook_close = jh_close;
    g_spu_jhook_open = jh_open;
    g_spu_jhook_on = 1;
}

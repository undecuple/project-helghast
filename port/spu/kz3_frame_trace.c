/*
 * KZ3_FRAME_TRACE=FROM,TO (SPU pass 66, opt-in diagnostic; needs KZ3_WAIT_ACCT on): record every event the wait
 * accounting sees between FROM and TO seconds (KZ3_PROF's time base) and write them to KZ3_FRAME_TRACE_OUT
 * (default frametrace.bin, the process's working directory) when the window ends. Runtime patch 1085 passes the
 * events: accounted brackets and notes (HLE calls, syscalls, usleeps, label waits, kernel sleeps, policy-module runs,
 * GCM parks, kernel gaps), job-module changes, flips and WWS job calls. tools/spu/frame_trace.py reads the file
 * and prints the per-frame timeline (SPU frame chain, walker park, label 75, hand-off latencies).
 *
 * File: "KZ3FTRC1", u32 thread-name count, u32 event count, u64 base ns; names (40 bytes each); events (24 bytes:
 * u64 t0, u64 t1 relative to base, u32 site, u8 kind, u8 thread, u16 0). Little endian (the host's).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void (*g_kz3_wtrace)(const char* thr, int kind, uint32_t site, uint64_t t0, uint64_t t1); /* patch 1085 */
extern uint64_t kz3_wacct_base(void);                                                            /* patch 1085 */
extern int g_kz3_wacct;
int kz3_wacct_init(void);

#define FT_NAMES 96
#define FT_NAME_LEN 40

struct ft_ev {
    uint64_t t0, t1;
    uint32_t site;
    uint8_t kind, thr;
    uint16_t pad;
};

static struct ft_ev* s_ev;
static size_t s_cap;
static volatile size_t s_n;
static volatile int s_state;  /* 0 recording, 1 dumping, 2 done */
static uint64_t s_from, s_to; /* absolute ns */
static const char* s_names_p[FT_NAMES];
static char s_names[FT_NAMES][FT_NAME_LEN];
static volatile int s_nnames;
static volatile int s_name_lock;
static const char* s_out = "frametrace.bin";

static int ft_thread(const char* name)
{
    const int n = __atomic_load_n(&s_nnames, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (s_names_p[i] == name) return i;
    while (__atomic_exchange_n(&s_name_lock, 1, __ATOMIC_ACQUIRE)) {
    }
    int i, m = s_nnames;
    for (i = 0; i < m && s_names_p[i] != name; i++) {
    }
    if (i == m && m < FT_NAMES) {
        strncpy(s_names[m], name ? name : "", FT_NAME_LEN - 1);
        s_names_p[m] = name;
        __atomic_store_n(&s_nnames, m + 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s_name_lock, 0, __ATOMIC_RELEASE);
    return i < FT_NAMES ? i : FT_NAMES - 1;
}

static void ft_dump(void)
{
    const size_t n = s_n < s_cap ? s_n : s_cap;
    FILE* f = fopen(s_out, "wb");
    if (!f) {
        fprintf(stderr, "[frame-trace] cannot write %s\n", s_out);
        return;
    }
    /* names may still be updated by threads that see the window closed only after this point: copy what is here */
    const uint32_t nn = (uint32_t)s_nnames, ne = (uint32_t)n;
    const uint64_t base = kz3_wacct_base();
    fwrite("KZ3FTRC1", 1, 8, f);
    fwrite(&nn, 4, 1, f);
    fwrite(&ne, 4, 1, f);
    fwrite(&base, 8, 1, f);
    fwrite(s_names, FT_NAME_LEN, nn, f);
    fwrite(s_ev, sizeof *s_ev, n, f);
    fclose(f);
    fprintf(stderr, "[frame-trace] wrote %u events, %u threads to %s%s\n", ne, nn, s_out,
            s_n > s_cap ? " (buffer full: the window was cut short)" : "");
}

static void ft_hook(const char* thr, int kind, uint32_t site, uint64_t t0, uint64_t t1)
{
    if (s_state) return;
    if (t1 < s_from) return;
    if (t1 >= s_to) {
        if (__atomic_exchange_n(&s_state, 1, __ATOMIC_ACQ_REL) == 0) {
            ft_dump();
            __atomic_store_n(&s_state, 2, __ATOMIC_RELEASE);
        }
        return;
    }
    const size_t i = __atomic_fetch_add(&s_n, 1, __ATOMIC_RELAXED);
    if (i >= s_cap) return;
    const uint64_t b = kz3_wacct_base();
    struct ft_ev* e = &s_ev[i];
    e->t0 = t0 > b ? t0 - b : 0;
    e->t1 = t1 - b;
    e->site = site;
    e->kind = (uint8_t)kind;
    e->thr = (uint8_t)ft_thread(thr);
    e->pad = 0;
}

void kz3_frame_trace_install(void)
{
    const char* e = getenv("KZ3_FRAME_TRACE");
    double from, to;
    if (!e || sscanf(e, "%lf,%lf", &from, &to) != 2 || to <= from) return;
    if ((g_kz3_wacct < 0 ? kz3_wacct_init() : g_kz3_wacct) <= 0) {
        fprintf(stderr, "[frame-trace] KZ3_FRAME_TRACE needs KZ3_WAIT_ACCT: off\n");
        return;
    }
    const char* o = getenv("KZ3_FRAME_TRACE_OUT");
    if (o && *o) s_out = o;
    const char* c = getenv("KZ3_FRAME_TRACE_CAP");
    s_cap = c && atol(c) > 0 ? (size_t)atol(c) : (size_t)4 << 20;
    s_ev = (struct ft_ev*)malloc(s_cap * sizeof *s_ev);
    if (!s_ev) {
        fprintf(stderr, "[frame-trace] no memory for %zu events: off\n", s_cap);
        return;
    }
    s_from = kz3_wacct_base() + (uint64_t)(from * 1e9);
    s_to = kz3_wacct_base() + (uint64_t)(to * 1e9);
    g_kz3_wtrace = ft_hook;
    fprintf(stderr, "[frame-trace] on (KZ3_FRAME_TRACE, SPU pass 66): %.1f-%.1f s, up to %zu events -> %s\n", from, to,
            s_cap, s_out);
}

/* Host test for port/spu/kz3_stall_diag.c (KZ3_STALL_DIAG, runtime patch 1081, SPU pass 63): built and run by
 * tools/spu/test_stall_diag.sh. Self-contained: guest memory is a host buffer, no game data. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "spu_context.h"

uint8_t* vm_base;
extern uint64_t g_kz3_sd_guest_size;
void (*g_kz3_run_ctx_hook)(spu_context*, int on, int kind, uint32_t a, uint32_t b);
void (*g_kz3_stall_scan)(unsigned long long now_ms);
SPU_THREAD_LOCAL uint32_t g_kz3_cur_taskset, g_kz3_cur_taskid;
void kz3_stall_diag_install(void);
void kz3_stall_diag_ppu_wait(int on, uint32_t obj, uint32_t flag, uint32_t lr);
int kz3_arena_free(uint32_t L, uint32_t W);

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static unsigned long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
}
static void be32w(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static char s_out[1 << 16];
static const char* run_scan(unsigned long long t)
{
    fflush(stderr);
    FILE* f = tmpfile();
    int saved = dup(2);
    dup2(fileno(f), 2);
    g_kz3_stall_scan(t);
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    rewind(f);
    size_t n = fread(s_out, 1, sizeof s_out - 1, f);
    s_out[n] = 0;
    fclose(f);
    return s_out;
}

static spu_context s_c;
/* The scan holds the slot busy through its 100 x 1 ms pc sampling (each sleep at least 1 ms), so a run end that waits
 * returns at least 100 ms after the scan started. Its own end time is no reference: the scan clears busy before it
 * returns, so the run end can legitimately finish first (CI 2026-10-22: pop 236674 < scan end 236675). */
static volatile unsigned long long s_scan_start, s_pop_ret;
static void* scan_thread(void* p)
{
    (void)p;
    s_scan_start = now_ms();
    run_scan(s_scan_start + 4000);
    return NULL;
}

int main(void)
{
    /* 1. the free test of 0x5B9F0 */
    CHECK(kz3_arena_free(0x30000100u, 0x30000080u) == 1, "same epoch, older code");
    CHECK(kz3_arena_free(0x30000100u, 0x30000100u) == 1, "same epoch, equal code");
    CHECK(kz3_arena_free(0x30000100u, 0x30000200u) == 0, "same epoch, newer code");
    CHECK(kz3_arena_free(0x30000100u, 0x20000F00u) == 1, "one epoch older");
    CHECK(kz3_arena_free(0xC0000000u, 0x10000000u) == 0, "eleven epochs older");
    CHECK(kz3_arena_free(0x30000100u, 0x50000000u) == 0, "two epochs newer");
    CHECK(kz3_arena_free(0x30000100u, 0xE0000000u) == 1, "five epochs older across the wrap");

    /* 2. off by default */
    unsetenv("KZ3_STALL_DIAG");
    kz3_stall_diag_install();
    CHECK(!g_kz3_run_ctx_hook && !g_kz3_stall_scan, "hooks installed without KZ3_STALL_DIAG");
    setenv("KZ3_STALL_DIAG", "1", 1);
    kz3_stall_diag_install();
    CHECK(g_kz3_run_ctx_hook && g_kz3_stall_scan, "hooks not installed with KZ3_STALL_DIAG=1");
    if (!g_kz3_run_ctx_hook || !g_kz3_stall_scan) return 1;

    /* 3. a stuck policy-module run with a frame-arena record at LS r85 */
    vm_base = calloc(1, 0x100000);
    g_kz3_sd_guest_size = 0x100000;
    s_c.ls = calloc(1, 0x40000);
    s_c.pc = 0xE6C0;
    s_c.image_id = 2;
    s_c.resident_ovl = 214;
    s_c.gpr[1]._u32[0] = 0x3F000;
    s_c.gpr[85]._u32[0] = 0x1000;
    const uint32_t rec[8] = {5, 0x100, 0x100, 0x10000, 0x200, 0, 0x300, 0x30000000};
    for (int i = 0; i < 8; i++) be32w(s_c.ls + 0x1000 + 4 * i, rec[i]);
    be32w(vm_base + 0x300, 0x30000100u);
    const uint32_t st[5] = {0x30000080u, 0x30000200u, 0x20000F00u, 0x50000000u, 0xE0000000u};
    for (int i = 0; i < 5; i++) be32w(vm_base + 0x100 + 4 * i, st[i]);
    g_kz3_run_ctx_hook(&s_c, 1, 1, 1, 2);
    const unsigned long long t0 = now_ms();
    CHECK(run_scan(t0 + 1000)[0] == 0, "reported before 3 s");
    const char* o = run_scan(t0 + 3500);
    CHECK(strstr(o, "[stall-diag] spu pm wid=1 spu=2 img=2 ovl=214") != NULL, "no pm report: %.200s", o);
    CHECK(strstr(o, "pc=0x0E6C0") != NULL, "pc missing");
    CHECK(strstr(o, "label@00000300=30000100") != NULL, "label missing");
    CHECK(strstr(o, "free by the test: 3") != NULL, "free count wrong: %.600s", o);
    CHECK(run_scan(t0 + 4000)[0] == 0, "repeated within 5 s");
    g_kz3_run_ctx_hook(&s_c, 0, 1, 1, 2);
    CHECK(run_scan(t0 + 20000)[0] == 0, "reported after the run ended");

    /* 4. the run's end waits while the scan reads its context (a task's context lives on its thread's stack) */
    g_kz3_run_ctx_hook(&s_c, 1, 0, 40, 0x1234);
    pthread_t th;
    pthread_create(&th, NULL, scan_thread, NULL);
    struct timespec d = {0, 20 * 1000000L};
    nanosleep(&d, NULL); /* the scan is in its 100 x 1 ms pc sampling */
    g_kz3_run_ctx_hook(&s_c, 0, 0, 0, 0);
    s_pop_ret = now_ms();
    pthread_join(th, NULL);
    CHECK(strstr(s_out, "[stall-diag] spu task/job img=40") != NULL, "no task report: %.200s", s_out);
    CHECK(s_pop_ret >= s_scan_start + 100, "run ended while the scan was reading (pop %llu < scan start %llu + 100)",
          s_pop_ret, s_scan_start);

    /* 4b. a task parked at the taskset syscall (LS 0xA70) is not a stall; its age counts from the last scan that
     * saw it parked (SPU pass 66: images 44 and 30 filled the K2/K4/T7 logs) */
    s_c.pc = 0xA70;
    g_kz3_run_ctx_hook(&s_c, 1, 0, 44, 0x5678);
    const unsigned long long t2 = now_ms();
    CHECK(run_scan(t2 + 3500)[0] == 0, "parked task reported: %.200s", s_out);
    CHECK(run_scan(t2 + 60000)[0] == 0, "parked task reported after 60 s: %.200s", s_out);
    s_c.pc = 0x1234; /* woke and runs its own code */
    CHECK(run_scan(t2 + 61000)[0] == 0, "reported 1 s after its last park: %.200s", s_out);
    o = run_scan(t2 + 63500);
    CHECK(strstr(o, "[stall-diag] spu task/job img=44") != NULL, "unparked task not reported after 3.5 s: %.200s", o);
    g_kz3_run_ctx_hook(&s_c, 0, 0, 0, 0);

    /* 5. the PPU flag wait */
    be32w(vm_base + 0x400, 0);
    kz3_stall_diag_ppu_wait(1, 0x800, 0x400, 0x2C2D6C);
    const unsigned long long t1 = now_ms();
    o = run_scan(t1 + 3500);
    CHECK(strstr(o, "[stall-diag] ppu wait 0x220A78") && strstr(o, "flag@00000400=00000000") &&
              strstr(o, "lr=0x002C2D6C"),
          "no ppu report: %.300s", o);
    kz3_stall_diag_ppu_wait(0, 0, 0, 0);
    CHECK(run_scan(t1 + 20000)[0] == 0, "ppu wait reported after it ended");

    printf("%s: test_stall_diag (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

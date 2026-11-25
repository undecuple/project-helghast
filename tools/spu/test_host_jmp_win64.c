/* Host test of patch 1166: runtime/platform/host_jmp_win64.h, the CRT-free frameless setjmp/longjmp.
 * Two builds from this one file (see test_host_jmp_win64.sh):
 *  - MinGW (WINTEST): the header as shipped, windows.h threads, jmp_buf; only compiled and import-checked (no Wine here).
 *  - native Linux x86-64 (default): the header with -D_WIN64 -D__MINGW64__ (its asm and ms_abi do not depend on the OS),
 *    run for real; a 256-byte buffer replaces jmp_buf (glibc's is smaller), pthreads replace CreateThread. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifdef WINTEST
#include <windows.h>
#else
#include <pthread.h>
#endif
#include "host_jmp_win64.h"

#ifdef WINTEST
static jmp_buf jb;
#else
static char jb[256] __attribute__((aligned(16)));
#endif
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)

__attribute__((noinline)) static void deep(int n, int v) { volatile char pad[256]; pad[0] = (char)n; if (n) deep(n - 1, v); else rc_host_longjmp(jb, v); }
__attribute__((noinline, sysv_abi)) static void deep_sysv(int v) { deep(8, v); }

__attribute__((noinline)) static int run(int v, int via_sysv)
{
    volatile int keep = 0x1234;
    volatile double d = 2.5;
    int r = rc_host_setjmp(jb);
    if (r == 0) { if (via_sysv) deep_sysv(v); else deep(30, v); return -1; }
    CHECK(keep == 0x1234 && d == 2.5, "locals lost (keep=%#x d=%g)", keep, d);
    return r;
}

__attribute__((sysv_abi, noinline)) static int run_sysv(int v)
{
    int r = rc_host_setjmp(jb);
    if (r == 0) deep(5, v);
    return r;
}

#ifndef WINTEST
/* The Win64 callee-saved registers survive: set them, setjmp, clobber, longjmp, read them back (-O0 keeps the compiler out). */
__attribute__((noinline, optimize("O0"))) static void regs_check(void)
{
    uint64_t g[6] = {0}, h[6] = {0}; double x[2] = {0}, y[2] = {0};
    asm volatile("movq $0x1111, %%rbx\n movq $0x1212, %%r12\n movq $0x1313, %%r13\n movq $0x1414, %%r14\n movq $0x1515, %%r15\n"
                 "movq $0x1616, %%rsi\n movq $0x1717, %%rdi\n movq $0x3ff0000000000000, %%rax\n movq %%rax, %%xmm6\n"
                 "movq $0x4000000000000000, %%rax\n movq %%rax, %%xmm15\n"
                 ::: "rbx", "r12", "r13", "r14", "r15", "rsi", "rdi", "rax", "xmm6", "xmm15");
    int r = rc_host_setjmp(jb);
    if (r == 0) {
        asm volatile("xorl %%ebx,%%ebx\n xorl %%r12d,%%r12d\n xorl %%r13d,%%r13d\n xorl %%r14d,%%r14d\n xorl %%r15d,%%r15d\n"
                     "xorl %%esi,%%esi\n xorl %%edi,%%edi\n xorps %%xmm6,%%xmm6\n xorps %%xmm15,%%xmm15\n"
                     ::: "rbx", "r12", "r13", "r14", "r15", "rsi", "rdi", "xmm6", "xmm15");
        rc_host_longjmp(jb, 7);
    }
    asm volatile("movq %%rbx,0(%0)\n movq %%r12,8(%0)\n movq %%r13,16(%0)\n movq %%r14,24(%0)\n movq %%r15,32(%0)\n movq %%rsi,40(%0)\n"
                 "movq %%xmm6,0(%1)\n movq %%xmm15,8(%1)\n" :: "r"(g), "r"(x) : "memory");
    asm volatile("movq %%rdi,0(%0)\n" :: "r"(h) : "memory");
    (void)y;
    CHECK(r == 7, "regs value %d", r);
    CHECK(g[0] == 0x1111 && g[1] == 0x1212 && g[2] == 0x1313 && g[3] == 0x1414 && g[4] == 0x1515 && g[5] == 0x1616 && h[0] == 0x1717,
          "GPRs not restored: %llx %llx %llx %llx %llx %llx %llx", (unsigned long long)g[0], (unsigned long long)g[1], (unsigned long long)g[2],
          (unsigned long long)g[3], (unsigned long long)g[4], (unsigned long long)g[5], (unsigned long long)h[0]);
    CHECK(x[0] == 1.0 && x[1] == 2.0, "xmm6/xmm15 not restored (%g %g)", x[0], x[1]);
}
#endif

#ifdef WINTEST
static DWORD WINAPI thr(void* p) { (void)p; CHECK(run(2, 0) == 2, "thread value"); return 0; }
#else
static void* thr(void* p) { (void)p; CHECK(run(2, 0) == 2, "thread value"); return 0; }
#endif

int main(void)
{
    CHECK(run(0, 0) == 1, "value 0 must land as 1");
    CHECK(run(2, 0) == 2, "value 2");
    CHECK(run(5, 1) == 5, "jump from a sysv_abi frame");
    CHECK(run_sysv(3) == 3, "setjmp in a sysv_abi function");
#ifndef WINTEST
    regs_check();
#endif
    for (int i = 0; i < 100000; i++) if (run(1 + (i & 3), i & 1) != 1 + (i & 3)) { CHECK(0, "loop %d", i); break; }
#ifdef WINTEST
    HANDLE h = CreateThread(NULL, 1 << 20, thr, NULL, 0, NULL); WaitForSingleObject(h, INFINITE);
#else
    pthread_t t; pthread_create(&t, NULL, thr, NULL); pthread_join(t, NULL);
#endif
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}

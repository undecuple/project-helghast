/* KZ3 SPU pass 72: the lifted SPU code is built with -mfma by default (KZ3_SPU_FMA, port/spu/CMakeLists.txt; patch
 * 1097's hardware fused multiply-add in spu__fma4, which also makes the compiler use AVX encodings in the whole
 * kz3_spu target). A CPU without FMA3/AVX (or an OS without AVX state) would die on the first such instruction, with
 * no word why. This file is built WITHOUT those flags (per-source options) and checks before anything else runs:
 * its constructor has priority 101, ahead of the default-priority SPU registration constructor
 * (kz3_spu_register.c). FMA3 + AVX: Intel Haswell (2013) and later, AMD Piledriver (2012) and later. */
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(KZ3_SPU_NEEDS_FMA) && (defined(__GNUC__) || defined(__clang__)) &&                                         \
    (defined(__x86_64__) || defined(__i386__))
__attribute__((constructor(101))) static void kz3_cpu_check(void)
{
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx") && __builtin_cpu_supports("fma")) return;
    static const char msg[] =
        "This build of Killzone 3 needs a CPU with AVX and FMA3 (Intel Haswell / AMD Piledriver or newer).\n"
        "This CPU (or the operating system) does not provide them. Rebuild with -DKZ3_SPU_FMA=OFF to run it here.";
    fprintf(stderr, "[kz3] %s\n", msg);
#if defined(_WIN32)
    MessageBoxA(NULL, msg, "Killzone 3", MB_OK | MB_ICONERROR | MB_TOPMOST);
#endif
    exit(1);
}
#endif

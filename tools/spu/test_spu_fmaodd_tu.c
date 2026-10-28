/* One translation unit of the patch 1106 host test: spu_fma/spu_fms/spu_fnms built with SPU_FMA_ODD4 as given on
 * the command line (TU_NAME = the wrapper prefix). Linked twice into test_spu_fmaodd (old and new forms). */
#include <stdint.h>
#include "spu_context.h"
#include "spu_helpers.h"
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
__attribute__((noinline)) u128 CAT(TU_NAME, _fma)(u128 a, u128 b, u128 c) { return spu_fma(a, b, c); }
__attribute__((noinline)) u128 CAT(TU_NAME, _fms)(u128 a, u128 b, u128 c) { return spu_fms(a, b, c); }
__attribute__((noinline)) u128 CAT(TU_NAME, _fnms)(u128 a, u128 b, u128 c) { return spu_fnms(a, b, c); }
#ifdef TU_MAIN_STUBS
int g_kz3_spu_slowcg = 0, g_kz3_fma_stats, g_kz3_xr_stats, g_kz3_spu_xrq = 1, g_kz3_spu_xr = 1;
void kz3_spu_xr_note(int op, unsigned m, const uint32_t* a, const uint32_t* b, const uint32_t* c, const uint32_t* r,
                     const void* ra) { (void)op; (void)m; (void)a; (void)b; (void)c; (void)r; (void)ra; }
void kz3_spu_fma_note(const double s[4], int ftz) { (void)s; (void)ftz; }
#endif

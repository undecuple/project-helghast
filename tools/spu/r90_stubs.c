/* Runtime globals the patched spu_helpers.h reads (defined by the port's runtime in the exe); host tests link these. */
#include <stdint.h>
int g_kz3_spu_xr = 1;
int g_kz3_spu_slowcg;
int g_kz3_fma_stats;
int g_kz3_xr_stats;
int g_kz3_spu_xrq = 1;
void (*g_kz3_wtrace)(const char*, int, uint32_t, uint64_t, uint64_t);
void kz3_spu_xr_note(int op, unsigned m, const uint32_t* a, const uint32_t* b, const uint32_t* c, const uint32_t* r,
                     const void* ra) { (void)op, (void)m, (void)a, (void)b, (void)c, (void)r, (void)ra; }
void kz3_spu_fma_note(const double s[4], int ftz) { (void)s, (void)ftz; }

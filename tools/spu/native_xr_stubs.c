/* Runtime globals the SPU float helpers (spu_helpers.h) reach, for host tests that link a native kernel without
 * spu_drain.c: extended-range recompute on (as the game), statistics off. */
#include <stdint.h>
int g_kz3_spu_xr = 1;
int g_kz3_spu_slowcg = 0;
int g_kz3_spu_xrq = 1;
int g_kz3_xr_stats = 0;
int g_kz3_fma_stats = 0;
void kz3_spu_xr_note(int op, unsigned m, const uint32_t* a, const uint32_t* b, const uint32_t* c, const uint32_t* r, const void* ra)
{ (void)op; (void)m; (void)a; (void)b; (void)c; (void)r; (void)ra; }
void kz3_spu_fma_note(const double s[4], int ftz) { (void)s; (void)ftz; }

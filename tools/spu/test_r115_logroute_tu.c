/* Spu pass 115 host test, helper TU: compiled twice, once like a port/spu source before pass 115 (no redirect)
 * and once like after it (-DKZ3_LOG_REDIRECT_STDIO -include port/kz3/kz3_log.h). See test_r115_logroute.c. */
#include <stdio.h>
#ifdef KZ3_LOG_REDIRECT_STDIO
void r115_routed_print(void)
{
    fprintf(stderr, "[fiber] S=0x114B7900 run_all #1 R115ROUTED\n"); /* pass 115 format: "S=" routes at info */
    fprintf(stderr, "[heap] R115ROUTED2\n");
    fprintf(stderr, "[fiber] run_all #1 R115OLDFMT\n"); /* pre-115 format: the plain fiber tag is debug */
}
void r115_routed_flush(void)
{
    fflush(stderr);
} /* what ps3_stdio's flush thread calls: kz3_log_fflush here */
#else
void r115_raw_print(void)
{
    fprintf(stderr, "[fiber] run_all #1 R115RAW\n");
}
#endif

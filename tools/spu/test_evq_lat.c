/* Patch 1155 host test: runtime/platform/kz3_evq_lat.h (the real-wait / wake-latency split of sys_event_queue_receive):
 * buckets, percentiles, means, immediate receives, the row table (shared key, full table) and concurrent notes. */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "kz3_evq_lat.h"
static int fail;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fail++;                                                                                                    \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                                                       \
        }                                                                                                              \
    } while (0)
static void* hammer(void* p)
{
    (void)p;
    for (int i = 0; i < 20000; i++) kz3_evq_lat_note(7, 0x1234, 1, 50000, 3000);
    return NULL;
}
int main(void)
{
    CHECK(kz3_evq_lat_bucket(0) == 0 && kz3_evq_lat_bucket(999) == 0 && kz3_evq_lat_bucket(1000) == 1);
    CHECK(kz3_evq_lat_bucket(3999) == 2 && kz3_evq_lat_bucket(4000) == 3 &&
          kz3_evq_lat_bucket(1ll << 60) == KZ3_EVQ_LAT_B - 1);
    /* 100 sleeps of 100 us wait / 10 us latency, plus 10 of 2000 us wait / 400 us latency, plus 5 immediate */
    for (int i = 0; i < 100; i++) kz3_evq_lat_note(2, 0xAA, 1, 100000, 10000);
    for (int i = 0; i < 10; i++) kz3_evq_lat_note(2, 0xAA, 1, 2000000, 400000);
    for (int i = 0; i < 5; i++) kz3_evq_lat_note(2, 0xAA, 0, 0, 0);
    kz3_evq_lat_row* r = kz3_evq_lat_row_for(2, 0xAA);
    CHECK(r && r->calls == 115 && r->slept == 110);
    CHECK(r && r->wait_ns / 1000 / r->slept == (100 * 100 + 10 * 2000) / 110); /* mean wait us */
    CHECK(r && kz3_evq_lat_pct_us(r->wait_b, r->slept, 50) == 128);  /* 100 us -> bucket 7 -> upper edge 128 */
    CHECK(r && kz3_evq_lat_pct_us(r->wait_b, r->slept, 99) == 2048); /* 2000 us -> bucket 11 */
    CHECK(r && kz3_evq_lat_pct_us(r->lat_b, r->slept, 50) == 16 && kz3_evq_lat_pct_us(r->lat_b, r->slept, 99) == 512);
    CHECK(kz3_evq_lat_row_for(2, 0xAB) != r &&
          kz3_evq_lat_row_for(2, 0xAA) == r); /* a second row, the first is found again */
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], 0, hammer, 0);
    for (int i = 0; i < 4; i++) pthread_join(t[i], 0);
    kz3_evq_lat_row* h = kz3_evq_lat_row_for(7, 0x1234);
    CHECK(h && h->calls == 80000 && h->slept == 80000 && h->wait_ns == 80000ull * 50000);
    int n = 0;
    for (unsigned q = 100; q < 140; q++)
        if (kz3_evq_lat_row_for(q, 1)) n++;
    CHECK(n == KZ3_EVQ_LAT_ROWS - 3); /* the table holds 32 rows: 3 used above, the rest fill, the surplus is dropped */
    CHECK(kz3_evq_lat_row_for(999, 9) == 0);
    kz3_evq_lat_tick(1000, stderr);
    kz3_evq_lat_tick(20000000000ll, stderr); /* first call arms, second prints */
    printf(fail ? "FAIL\n" : "ALL OK\n");
    return fail != 0;
}

/* Patch 1055 host test: the hashed NID index (runtime/ppu/kz3_nid_index.h) against the linear scan
 * (ps3_nid_table_find from include/ps3emu/nid.h). Self-contained apart from those two headers.
 *  1. every registered NID resolves to the same entry as the linear scan (first entry wins on duplicates);
 *  2. unknown NIDs stay unresolved (1e6 random probes, checked against the linear scan);
 *  3. growing the table after a lookup rebuilds the index (late registrations are found);
 *  4. a small context-handler-like table (stride != sizeof(ps3_nid_entry));
 *  5. 4 reader threads during growth agree with the linear scan;
 *  6. timing: linear vs hashed per lookup at the KZ3 table size. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ps3emu/nid.h"
#include "kz3_nid_index.h"

#define CAP 4096
static ps3_nid_entry st[CAP];
static ps3_nid_table tb;
static kz3_nid_cache cache;
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)rng;
}
static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            if (fails++ < 10) {                                                                                        \
                printf("FAIL: " __VA_ARGS__);                                                                          \
                printf("\n");                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

static int32_t hfind(uint32_t nid)
{
    return kz3_nid_cache_find(&cache, tb.entries, sizeof(ps3_nid_entry), tb.count, nid);
}
static int32_t lfind(uint32_t nid)
{
    ps3_nid_entry* e = ps3_nid_table_find(&tb, nid);
    return e ? (int32_t)(e - tb.entries) : -1;
}
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static volatile int stop;
static void* reader(void* a)
{
    (void)a;
    long n = 0;
    while (!stop) {
        uint32_t cnt = __atomic_load_n(&tb.count, __ATOMIC_ACQUIRE);
        if (!cnt) continue;
        uint32_t i = rnd() % cnt;
        uint32_t nid = st[i].nid;
        int32_t k = kz3_nid_cache_find(&cache, tb.entries, sizeof(ps3_nid_entry), cnt, nid);
        int32_t l = kz3_nid_linear(tb.entries, sizeof(ps3_nid_entry), cnt, nid);
        CHECK(k == l, "threaded lookup 0x%08X: hashed %d linear %d", nid, k, l);
        n++;
    }
    return (void*)n;
}

int main(void)
{
    ps3_nid_table_init(&tb, st, CAP);
    /* 1. ~3000 NIDs (KZ3 registers a few thousand), with 50 duplicates carrying other handlers. */
    for (int i = 0; i < 3000; i++) ps3_nid_table_add(&tb, rnd(), "f", (void*)(uintptr_t)(i + 1));
    for (int i = 0; i < 50; i++) ps3_nid_table_add(&tb, st[rnd() % 3000].nid, "dup", (void*)(uintptr_t)0xD00D);
    for (uint32_t i = 0; i < tb.count; i++) CHECK(hfind(st[i].nid) == lfind(st[i].nid), "nid 0x%08X", st[i].nid);
    for (uint32_t i = 0; i < tb.count; i++)
        CHECK(st[hfind(st[i].nid)].handler != (void*)(uintptr_t)0xD00D || i >= 3000,
              "duplicate won over the first entry");
    /* 2. unknown NIDs. */
    int unk = 0;
    for (int i = 0; i < 1000000; i++) {
        uint32_t n = rnd();
        int32_t l = lfind(n);
        CHECK(hfind(n) == l, "probe 0x%08X", n);
        unk += l < 0;
    }
    CHECK(hfind(0) == lfind(0) && hfind(0xFFFFFFFFu) == lfind(0xFFFFFFFFu), "edge NIDs");
    /* 3. growth after lookups. */
    uint32_t late = 0xC0FFEE01u;
    while (lfind(late) >= 0) late++;
    CHECK(hfind(late) == -1, "late NID before registration");
    ps3_nid_table_add(&tb, late, "late", (void*)1);
    CHECK(hfind(late) == (int32_t)tb.count - 1, "late NID after registration");
    /* 4. context-handler-like table: {nid, fn, name}. */
    struct {
        uint32_t nid;
        void* fn;
        const char* name;
    } ctxt[256];
    kz3_nid_cache cc = {0, 0};
    for (int i = 0; i < 200; i++) ctxt[i].nid = (i == 7) ? ctxt[3].nid : rnd();
    for (int i = 0; i < 200; i++)
        CHECK(kz3_nid_cache_find(&cc, ctxt, sizeof ctxt[0], 200, ctxt[i].nid) ==
                  kz3_nid_linear(ctxt, sizeof ctxt[0], 200, ctxt[i].nid),
              "ctx table entry %d", i);
    CHECK(kz3_nid_cache_find(&cc, ctxt, sizeof ctxt[0], 200, ctxt[7].nid) == 3, "ctx duplicate: first wins");
    CHECK(kz3_nid_cache_find(&cc, ctxt, sizeof ctxt[0], 0, 123) == -1, "empty table");
    /* 5. readers while the table grows (registration is at init in the runtime; this checks the rebuild path). */
    pthread_t th[4];
    long looks = 0;
    for (int i = 0; i < 4; i++) pthread_create(&th[i], 0, reader, 0);
    for (int i = 0; i < 900; i++) {
        uint32_t c = tb.count;
        st[c].nid = rnd();
        st[c].name = "g";
        st[c].handler = (void*)1;
        __atomic_store_n(&tb.count, c + 1, __ATOMIC_RELEASE);
        struct timespec ts = {0, 20000};
        nanosleep(&ts, 0);
    }
    stop = 1;
    for (int i = 0; i < 4; i++) {
        void* r;
        pthread_join(th[i], &r);
        looks += (long)r;
    }
    /* 6. timing. */
    const int N = 2000000;
    volatile int32_t sink = 0;
    double t0 = now();
    for (int i = 0; i < N; i++) sink += lfind(st[i % tb.count].nid);
    double t1 = now();
    for (int i = 0; i < N; i++) sink += hfind(st[i % tb.count].nid);
    double t2 = now();
    (void)sink;
    printf("nid index: %u entries, 1e6 unknown probes (%d unresolved), %ld threaded lookups during growth; "
           "linear %.1f ns, hashed %.1f ns per hit lookup\n",
           tb.count, unk, looks, (t1 - t0) / N * 1e9, (t2 - t1) / N * 1e9);
    printf("%s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

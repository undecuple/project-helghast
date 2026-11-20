/* Host test for KZ3 patch 2185 (LD_WB_DEFER): the pending-store queue of rsx_wb_defer.h with a fake GPU (no D3D, no game data).
 * Fake "readback slots" are byte arrays filled at record time; fake "guest memory" is one array per location; the store
 * callback copies a slot into guest memory like the real store does. Tests: oldest-first order, one wait on the newest
 * fence, slot lifetime (no reuse before the store ran), drain idempotence, overlap queries (edges, MOVE source), queue
 * full, wait failure, verify compare (every byte), summary line, and a two-thread run of the published range snapshot.
 *   cc ... test_wb_defer.c ; ./t [race] */
#include <pthread.h>
#include <stdio.h>
#include "rsx_wb_defer.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t slotbuf[RSX_WBD_SLOTS][64];
static uint8_t guest[2][0x10000];
static uint64_t last_waited_fence, wait_calls, completed;
static int fail_wait;
static uint32_t store_log[RSX_WBD_CAP], nstore;
static uint8_t slot_at_store[RSX_WBD_CAP];

static int fake_wait(void* ctx, uint64_t fence, uint64_t* blocked)
{
    (void)ctx; *blocked = 0;
    wait_calls++; last_waited_fence = fence;
    if (fail_wait) return 0;
    if (fence <= completed) return 1;
    completed = fence; *blocked = 1000; return 2;
}
/* The store: copy the slot's first byte over the entry's whole destination range (like a conversion into guest memory). */
static void fake_store(void* ctx, rsx_wbd_ent* e)
{
    (void)ctx;
    store_log[nstore] = (uint32_t)e->seq;
    slot_at_store[nstore] = e->slot >= 0 ? slotbuf[e->slot][0] : 0;
    nstore++;
    if (e->kind == RSX_WBD_SURF) memset(&guest[e->wloc][e->woff], slotbuf[e->slot][0], e->wsize);
    else memset(&guest[e->wloc][e->woff], 0xEE, e->wsize);   /* MOVE: a marker */
}

static void reset(void)
{
    memset(slotbuf, 0, sizeof slotbuf); memset(guest, 0, sizeof guest);
    wait_calls = last_waited_fence = completed = 0; fail_wait = 0; nstore = 0;
}
/* the glue's record: drain first when no slot is free, take, "GPU copy" fills the slot, push */
static int record(rsx_wbd* d, uint8_t val, uint64_t fence, uint32_t loc, uint32_t off, uint32_t size)
{
    if (rsx_wbd_need_drain_for_slots(d, 1)) rsx_wbd_drain(d, RSX_WBD_R_SLOT, fake_wait, fake_store, NULL);
    const int s = rsx_wbd_slot_take(d);
    if (s < 0) return -1;
    memset(slotbuf[s], val, sizeof slotbuf[s]);
    rsx_wbd_ent e; memset(&e, 0, sizeof e);
    e.kind = RSX_WBD_SURF; e.slot = s; e.fence = fence; e.wloc = loc; e.woff = off; e.wsize = size;
    if (!rsx_wbd_push(d, &e)) return -2;
    return s;
}

static rsx_wbd D;
/* Patch 2196: LD_WB_DEFER is default ON: unset (NULL) = on, "0" = off, "1"/other = on. */
static void test_env_default(void)
{
    CHECK(rsx_wbd_env_on(NULL) == 1);     /* unset = on */
    CHECK(rsx_wbd_env_on("0") == 0);      /* LD_WB_DEFER=0 = old synchronous behaviour */
    CHECK(rsx_wbd_env_on("1") == 1);
    CHECK(rsx_wbd_env_on("2") == 1);
}
static void test_order_wait(void)
{
    reset(); rsx_wbd_init(&D);
    /* three stores into overlapping bytes: the newest must land last */
    CHECK(record(&D, 1, 5, 1, 100, 32) >= 0); CHECK(record(&D, 2, 6, 1, 110, 32) >= 0); CHECK(record(&D, 3, 7, 1, 120, 32) >= 0);
    CHECK(D.n == 3 && D.pending_max == 3);
    CHECK(rsx_wbd_drain(&D, RSX_WBD_R_USERCMD, fake_wait, fake_store, NULL) == 3);
    CHECK(wait_calls == 1 && last_waited_fence == 7);                    /* one wait, the newest fence */
    CHECK(nstore == 3 && store_log[0] == 1 && store_log[1] == 2 && store_log[2] == 3);   /* oldest first */
    CHECK(guest[1][100] == 1 && guest[1][109] == 1 && guest[1][110] == 2 && guest[1][119] == 2 && guest[1][120] == 3 && guest[1][151] == 3);
    CHECK(guest[1][152] == 0 && guest[1][99] == 0);
    CHECK(D.waits == 1 && D.wait_ns == 1000 && D.stores == 3 && D.forced[RSX_WBD_R_USERCMD] == 1);
    CHECK(rsx_wbd_free_slots(&D) == RSX_WBD_SLOTS);
    /* idempotent: a second drain stores nothing, waits nothing, counts nothing */
    CHECK(rsx_wbd_drain(&D, RSX_WBD_R_USERCMD, fake_wait, fake_store, NULL) == 0);
    CHECK(wait_calls == 1 && nstore == 3 && D.drains == 1 && D.stores == 3);
    /* an already complete fence is not a wait */
    CHECK(record(&D, 9, 7, 1, 0, 8) >= 0);
    CHECK(rsx_wbd_drain(&D, RSX_WBD_R_FLIP, fake_wait, fake_store, NULL) == 1 && D.waits == 1 && wait_calls == 2 && D.forced[RSX_WBD_R_FLIP] == 1);
}

static void test_slots(void)
{
    reset(); rsx_wbd_init(&D);
    for (uint32_t i = 0; i < RSX_WBD_SLOTS; i++) CHECK(record(&D, (uint8_t)(10 + i), 100 + i, 0, i * 64, 64) == (int)i);
    CHECK(D.n == RSX_WBD_SLOTS && rsx_wbd_free_slots(&D) == 0 && rsx_wbd_need_drain_for_slots(&D, 1));
    /* the 17th record must drain first (reason slot): every store saw its own slot value, none the 17th's */
    CHECK(record(&D, 99, 200, 0, 2000, 8) >= 0);
    CHECK(D.forced[RSX_WBD_R_SLOT] == 1 && nstore == RSX_WBD_SLOTS);
    for (uint32_t i = 0; i < RSX_WBD_SLOTS; i++) CHECK(slot_at_store[i] == (uint8_t)(10 + i));
    CHECK(D.n == 1 && rsx_wbd_free_slots(&D) == RSX_WBD_SLOTS - 1);
    CHECK(!rsx_wbd_need_drain_for_slots(&D, 15) && rsx_wbd_need_drain_for_slots(&D, 16));
    /* queue full: push fails, the caller drains with reason full and pushes again */
    reset(); rsx_wbd_init(&D);
    rsx_wbd_ent e; memset(&e, 0, sizeof e); e.kind = RSX_WBD_MOVE; e.slot = -1; e.wloc = 0; e.woff = 0; e.wsize = 4;
    for (uint32_t i = 0; i < RSX_WBD_CAP; i++) CHECK(rsx_wbd_push(&D, &e));
    CHECK(!rsx_wbd_push(&D, &e));
    CHECK(rsx_wbd_drain(&D, RSX_WBD_R_FULL, fake_wait, fake_store, NULL) == RSX_WBD_CAP && rsx_wbd_push(&D, &e) && D.forced[RSX_WBD_R_FULL] == 1);
}

static void test_overlap(void)
{
    reset(); rsx_wbd_init(&D);
    CHECK(record(&D, 4, 1, 1, 0x1000, 0x100) >= 0);
    CHECK(rsx_wbd_overlap(&D, 1, 0x1000, 1) && rsx_wbd_overlap(&D, 1, 0x10FF, 1) && rsx_wbd_overlap(&D, 1, 0xFFF, 2));
    CHECK(!rsx_wbd_overlap(&D, 1, 0x1100, 16) && !rsx_wbd_overlap(&D, 1, 0xFF0, 0x10) && !rsx_wbd_overlap(&D, 0, 0x1000, 0x100));
    CHECK(rsx_wbd_overlap(&D, 1, 0x1050, 0));                           /* an unknown size counts as one byte */
    /* a MOVE reads its source range at the drain: a reader there needs the drain too */
    rsx_wbd_ent m; memset(&m, 0, sizeof m); m.kind = RSX_WBD_MOVE; m.slot = -1; m.wloc = 1; m.woff = 0x3000; m.wsize = 0x80;
    m.rloc = 0; m.roff = 0x500; m.rsize = 0x40;
    CHECK(rsx_wbd_push(&D, &m));
    CHECK(rsx_wbd_overlap(&D, 0, 0x53F, 1) && !rsx_wbd_overlap(&D, 0, 0x540, 1) && rsx_wbd_overlap(&D, 1, 0x307F, 1));
    CHECK(rsx_wbd_touch(&D, 1, 0x2000, 16, RSX_WBD_R_TOUCH, fake_wait, fake_store, NULL) == 0 && D.n == 2);   /* no overlap: nothing */
    CHECK(rsx_wbd_touch(&D, 1, 0x10F0, 16, RSX_WBD_R_TOUCH, fake_wait, fake_store, NULL) == 2 && D.n == 0 && D.forced[RSX_WBD_R_TOUCH] == 1);
    CHECK(nstore == 2 && store_log[0] == 1 && store_log[1] == 2);       /* a touch drains everything, oldest first */
}

static void test_abort(void)
{
    reset(); rsx_wbd_init(&D);
    CHECK(record(&D, 1, 3, 0, 0, 8) >= 0 && record(&D, 2, 4, 0, 8, 8) >= 0);
    fail_wait = 1;
    CHECK(rsx_wbd_drain(&D, RSX_WBD_R_USERCMD, fake_wait, fake_store, NULL) == 0);
    CHECK(nstore == 0 && D.aborted == 2 && D.n == 0 && rsx_wbd_free_slots(&D) == RSX_WBD_SLOTS);
}

static void test_verify(void)
{
    rsx_wbd_ver v; memset(&v, 0, sizeof v);
    uint8_t a[100], b[100];
    memset(a, 7, sizeof a); memset(b, 7, sizeof b);
    CHECK(rsx_wbd_cmp(&v, 0x1000, a, b, 100) == 0 && v.compared == 1 && v.mismatches == 0);
    b[99] ^= 1;                                                          /* the last byte */
    CHECK(rsx_wbd_cmp(&v, 0x2000, a, b, 100) == 1 && v.mismatches == 1 && v.first[0].tag == 0x2000 && v.first[0].off == 99);
    b[99] ^= 1; b[0] ^= 0x80; b[50] ^= 1;
    CHECK(rsx_wbd_cmp(&v, 0x3000, a, b, 100) == 2 && v.mismatches == 2 && v.first[1].off == 0 && v.bytes_bad == 3);
    for (int i = 0; i < 20; i++) { b[1] ^= 1; rsx_wbd_cmp(&v, 1, a, b, 100); b[1] ^= 1; }
    CHECK(v.nfirst == RSX_WBD_FIRST);                                    /* only the first few are remembered */
    reset(); rsx_wbd_init(&D);
    char line[400];
    CHECK(rsx_wbd_fmt(&D, 7, line, sizeof line) > 0 && strstr(line, "mismatches=0") && strstr(line, "forced: usercmd=0") && strstr(line, "pending_max=0"));
    D.ver.mismatches = 3;
    rsx_wbd_fmt(&D, 7, line, sizeof line);
    CHECK(strstr(line, "mismatches=3"));
}

static rsx_wbd_snap SN;
static rsx_wbd TD;
static void* snap_writer(void* arg)
{
    (void)arg;
    for (uint32_t i = 0; i < 20000; i++) {
        TD.n = 0;
        for (uint32_t k = 0; k < (i % 5); k++) { TD.q[k].wloc = 1; TD.q[k].woff = k * 0x1000u + i; TD.q[k].wsize = 0x100; TD.n++; }
        rsx_wbd_snap_publish(&SN, &TD);
    }
    return NULL;
}
static void* snap_reader(void* arg)
{
    uint64_t* hits = (uint64_t*)arg;
    for (uint32_t i = 0; i < 20000; i++) if (rsx_wbd_snap_hit(&SN, 1, (i % 5) * 0x1000u, 0x2000)) (*hits)++;
    return NULL;
}
static void test_race(void)
{
    memset(&SN, 0, sizeof SN); memset(&TD, 0, sizeof TD);
    uint64_t hits = 0; pthread_t a, b;
    pthread_create(&a, NULL, snap_writer, NULL); pthread_create(&b, NULL, snap_reader, &hits);
    pthread_join(a, NULL); pthread_join(b, NULL);
    CHECK(hits <= 20000);
    /* single-threaded semantics of the snapshot */
    reset(); rsx_wbd_init(&D);
    CHECK(record(&D, 1, 1, 1, 0x4000, 0x100) >= 0);
    rsx_wbd_snap_publish(&SN, &D);
    CHECK(rsx_wbd_snap_hit(&SN, 1, 0x40FF, 1) && !rsx_wbd_snap_hit(&SN, 1, 0x4100, 1) && !rsx_wbd_snap_hit(&SN, 0, 0x4000, 1));
    rsx_wbd_drain(&D, 0, fake_wait, fake_store, NULL);
    rsx_wbd_snap_publish(&SN, &D);
    CHECK(!rsx_wbd_snap_hit(&SN, 1, 0x4000, 0x100));
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "race")) { test_race(); printf("[test_wb_defer] race stress %s\n", fails ? "FAIL" : "PASS"); return fails != 0; }
    test_env_default(); test_order_wait(); test_slots(); test_overlap(); test_abort(); test_verify(); test_race();
    printf("[test_wb_defer] %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}

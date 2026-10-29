/* Host test for KZ3 patch 2137: the same-frame texture re-check's page write sequence (libs/video/rsx_tex_samef.h).
 * Model of the decision at a bind: "entry hashed in this frame" (old rule: stop there) vs "a tracked write stamped its
 * pages after the hash began" (patch 2137: re-hash). Build with -DOLD_RULE to see the old rule fail the late-write cases. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "rsx_tex_samef.h"

static uint32_t pg[LD_SF_PAGES];
static uint32_t counter;
typedef struct {
    uint32_t off, span, sf_seq, hashed_frame;
} entry_t;
static void mark(uint32_t off, uint32_t size)
{
    ld_sf_stamp(pg, off, size, ++counter);
}
static void hash(entry_t* e, uint32_t frame)
{
    e->sf_seq = counter;
    e->hashed_frame = frame;
}
/* returns 1 when the bind must re-hash the entry in frame `frame` */
static int bind_same_frame(const entry_t* e, uint32_t frame)
{
#ifdef OLD_RULE
    (void)frame;
    (void)e;
    return 0; /* hashed this frame: trusted until the flip */
#else
    return e->hashed_frame == frame && counter != e->sf_seq && ld_sf_after(pg, e->off, e->span, e->sf_seq);
#endif
}
static int fails;
#define CHECK(c, what)                                                                                                 \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL: %s\n", what);                                                                                \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main(void)
{
    entry_t t = {0x4C1800, 0x100, 0, 0}; /* the 8x8 ring slot of pass 127 (page 0x4C) */
    mark(0x4C1800, 0x100);               /* SPU put before the hash */
    hash(&t, 10);
    CHECK(!bind_same_frame(&t, 10), "write before the hash must not re-hash");
    mark(0x4C1800, 0x100); /* SPU rewrites the slot after the hash, same frame */
    CHECK(bind_same_frame(&t, 10), "write after the hash in the same frame must re-hash");
    hash(&t, 10);
    CHECK(!bind_same_frame(&t, 10), "after the re-hash the entry is clean again");
    mark(0x10000000, 0x1000); /* unrelated page */
    CHECK(!bind_same_frame(&t, 10), "a write to another page must not re-hash");
    mark(0x4C0000, 4); /* same 64 KB page, different bytes: page granularity re-hashes */
    CHECK(bind_same_frame(&t, 10), "same-page write re-hashes (64 KB granularity, documented)");
    entry_t u = {0xFFFF0, 0x40, 0, 11}; /* span straddles the 64 KB boundary 0x100000 */
    hash(&u, 11);
    mark(0x100010, 4);
    CHECK(bind_same_frame(&u, 11), "write in the second page of a straddling span");
    entry_t z = {0x1000, 0, 0, 12};
    hash(&z, 12);
    mark(0x1000, 16);
    CHECK(!bind_same_frame(&z, 12), "unknown span (0) is never claimed here (the flip flags it)");
    entry_t w = {0x20000, 0x10, 0xFFFFFFF0u, 13}; /* counter wrap */
    memset(pg, 0, sizeof pg);
    counter = 0xFFFFFFFEu;
    w.sf_seq = counter;
    mark(0x20000, 4);
    mark(0x20000, 4);
    CHECK(counter == 0 && ld_sf_after(pg, 0x20000, 0x10, 0xFFFFFFFEu) == 0,
          "stamp 0 reads as never (wrap edge, documented)");
    counter = 0xFFFFFFF0u;
    w.sf_seq = counter;
    hash(&w, 13);
    mark(0x20000, 4);
    CHECK(bind_same_frame(&w, 13), "wrap-safe compare");
    printf(fails ? "test_tex_samef: %d FAILED\n" : "test_tex_samef: PASS\n", fails);
    return fails != 0;
}

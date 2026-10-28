/* Patch 1113 host test (SPU pass 97): a registered runtime window is never handed out by the mapper
 * (sys_mmapper_allocate_address) or the user allocator (sys_memory_allocate). Synthetic VM, no title code or data.
 * Built by tools/spu/test_r97_mapper_window.sh against work/ps3recomp-patched. */
#include "runtime/syscalls/sys_memory.h"
#include <stdio.h>
#include <string.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
uint32_t ppu_hle_inject_base = 0x7F000000u;   /* KZ3's GCM window (port/kz3/main.cpp) */
static lv2_syscall_table table;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static uint32_t rd32(uint32_t a) { uint8_t* p = vm_base + a; return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static int64_t reserve(uint64_t size, uint64_t align, uint32_t* out)
{
    ppu_context c; memset(&c, 0, sizeof c); c.gpr[3] = size; c.gpr[5] = align; c.gpr[6] = 0x10000;
    int64_t rc = sys_mmapper_allocate_address(&c); *out = rd32(0x10000); return rc;
}
static int64_t user_alloc(uint64_t size, uint32_t* out)
{
    ppu_context c; memset(&c, 0, sizeof c); c.gpr[3] = size; c.gpr[4] = 0x200 /* 64K pages */; c.gpr[5] = 0x10000;
    int64_t rc = sys_memory_allocate(&c); *out = rd32(0x10000); return rc;
}
static int disjoint(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a + n <= b || b + m <= a; }
#define POOL 0xB8000000u   /* patch 5005's KZ3_FONT_POOL_BASE */
#define POOLSZ 0x00400000u
int main(void)
{
    CHECK(vm_init() == CELL_OK); CHECK(vm_commit(0x10000, 0x10000) == CELL_OK);
    sys_memory_init(&table);
    CHECK(sys_memory_reserve_window(POOL, POOLSZ) == CELL_OK);
    CHECK(sys_memory_reserve_window(POOL, POOLSZ) == CELL_OK);                       /* same window again */
    CHECK(sys_memory_reserve_window(POOL + 0x100000u, 0x1000) == (int)CELL_EBUSY);   /* partial overlap */
    CHECK(sys_memory_reserve_window(0, 0x1000) == (int)CELL_EINVAL);
    uint32_t any = sys_memory_alloc_window(0x00400000u, 0);                          /* library pool, no fixed EA */
    CHECK(any >= 0x40000000u && any + 0x00400000u <= 0xC0000000u && disjoint(any, 0x400000, POOL, POOLSZ));
    /* Fill the mapper range with mixed sizes and alignments: no reservation may touch either window, and at
     * least one must lie above the fixed pool (the mapper skipped it rather than stopping in front of it). */
    static const uint32_t sizes[][2] = { {0x10000000u, 0x10000000u}, {0x02300000u, 0x100000u}, {0x00010000u, 0x10000u},
                                         {0x07F00000u, 0x100000u}, {0x00500000u, 0x100000u} };
    int n = 0, above = 0;
    for (int round = 0; round < 200; ++round) {
        const uint32_t* s = sizes[round % 5]; uint32_t ea = 0;
        if (reserve(s[0], s[1], &ea) != CELL_OK) continue;
        ++n;
        CHECK(disjoint(ea, s[0], POOL, POOLSZ)); CHECK(disjoint(ea, s[0], any, 0x400000));
        if (ea >= POOL + POOLSZ) above = 1;
    }
    CHECK(n > 4); CHECK(above);
    /* A late registration over a live reservation is refused, never aliased. */
    uint32_t live = 0;
    sys_memory_init(&table);                    /* windows survive the reset; reservations do not */
    CHECK(reserve(0x00100000u, 0x100000u, &live) == CELL_OK && disjoint(live, 0x100000, any, 0x400000));
    CHECK(sys_memory_reserve_window(live, 0x1000) == (int)CELL_EBUSY);
    /* The straddling case exactly: 128 MiB steps up to 0xB0000000, 127 MiB more leaves the cursor 1 MiB below the
     * pool, then 2 MiB would cover its first MiB: it must land right after the pool instead. */
    sys_memory_init(&table);
    uint32_t step = 0;
    for (int i = 0; i < 16 && step != 0xA8000000u; ++i) CHECK(reserve(0x08000000u, 0x08000000u, &step) == CELL_OK);
    CHECK(step == 0xA8000000u);
    uint32_t ea = 0;
    CHECK(reserve(0x07F00000u, 0x00100000u, &ea) == CELL_OK && ea == 0xB0000000u);
    CHECK(reserve(0x00200000u, 0x00010000u, &ea) == CELL_OK && ea == POOL + POOLSZ);
    /* User allocator: a window at the start of the user domain is skipped too. */
    CHECK(sys_memory_reserve_window(0x10000000u, 0x00100000u) == CELL_OK);
    uint32_t u = 0;
    CHECK(user_alloc(0x00100000u, &u) == CELL_OK && u >= 0x10100000u);
    printf("PASS %u checks (%d mapper reservations, window 0x%08X, alloc_window 0x%08X)\n", checks, n, POOL, any);
    return 0;
}

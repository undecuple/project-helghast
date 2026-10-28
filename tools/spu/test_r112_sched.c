/* Patch 1136 host test: kz3_pcore_mask (runtime/spu/kz3_spu_sched.h) on synthetic topologies. No game data. */
#include <stdio.h>
#include "kz3_spu_sched.h"
static int bad;
static void chk(const char* n, uint64_t got, uint64_t want) { if (got != want) { printf("FAIL %s: 0x%llX want 0x%llX\n", n, (unsigned long long)got, (unsigned long long)want); bad++; } else printf("ok   %s\n", n); }
int main(void)
{
    /* i7-12700KF as Proton reports it: 8 P-cores with SMT (class 1, cpus 0-15), 4 E-cores (class 0, cpus 16-19) */
    uint64_t c[12]; int k[12];
    for (int i = 0; i < 8; i++) { c[i] = 3ull << (2 * i); k[i] = 1; }
    for (int i = 0; i < 4; i++) { c[8 + i] = 1ull << (16 + i); k[8 + i] = 0; }
    chk("12700KF", kz3_pcore_mask(c, k, 12), 0xFFFFull);
    /* order of the entries does not matter */
    uint64_t c2[12]; int k2[12];
    for (int i = 0; i < 12; i++) { c2[i] = c[11 - i]; k2[i] = k[11 - i]; }
    chk("reversed", kz3_pcore_mask(c2, k2, 12), 0xFFFFull);
    /* uniform CPU: no restriction */
    int u[12] = {0}; chk("uniform class 0", kz3_pcore_mask(c, u, 12), 0);
    int u1[12]; for (int i = 0; i < 12; i++) u1[i] = 1; chk("uniform class 1", kz3_pcore_mask(c, u1, 12), 0);
    /* three classes (e.g. Core Ultra: P, E, LP-E): the highest only */
    uint64_t t[3] = {0x3, 0xC, 0x30}; int tk[3] = {2, 1, 0};
    chk("three classes", kz3_pcore_mask(t, tk, 3), 0x3ull);
    chk("empty", kz3_pcore_mask(c, k, 0), 0);
    chk("single core", kz3_pcore_mask(c, k, 1), 0);
    /* the top class covers everything that exists: nothing to exclude */
    uint64_t a[2] = {0x1, 0x2}; int ak[2] = {1, 1}; chk("same class two cores", kz3_pcore_mask(a, ak, 2), 0);
    /* scope: default = async threads only, CPU sets; all = every thread; 0 = off; mask = async, affinity mask */
    chk("scope default async", (uint64_t)kz3_sched_scope(NULL, 1), 2);
    chk("scope default kernel", (uint64_t)kz3_sched_scope(NULL, 0), 0);
    chk("scope all kernel", (uint64_t)kz3_sched_scope("all", 0), 2);
    chk("scope off async", (uint64_t)kz3_sched_scope("0", 1), 0);
    chk("scope off all", (uint64_t)kz3_sched_scope("0", 0), 0);
    chk("scope sets async", (uint64_t)kz3_sched_scope("sets", 1), 1);
    chk("scope sets kernel", (uint64_t)kz3_sched_scope("sets", 0), 0);
    printf(bad ? "test_r112_sched: FAIL\n" : "test_r112_sched: PASS\n");
    return bad != 0;
}

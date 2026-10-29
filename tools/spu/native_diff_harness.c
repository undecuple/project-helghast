/* native_diff_harness.c -- native gate vs lifted function (SPU pass 122; KZ2's native_diff_harness design).
 * Built and driven by tools/spu/native_diff.py. Reuses job_diff_harness.c's runtime contract and trial setup
 * (main renamed), links the image's lifted spu_recomp.c and port/spu/kz3_native_post.c + kz3_native_dof.c.
 *
 * Usage: harness <image.bin> <base> <slot> <seed> <trials> [record.kz3nj ...]
 *   For the slot's gate entry (image, LS address, kernel, kind, constant pool): the pool hash must match the image;
 *   then per state - synthetic (random LS and registers, kind-specific argument registers; <trials> of them) or a
 *   record written by KZ3_NATIVE_REC - run the lifted function (gate array empty) on one copy and the native kernel
 *   on another, and compare the whole local store, r0, r1 and r80..r127 (the registers a caller may keep; the lifted
 *   body also clobbers r2..r79, which the native kernel leaves untouched: call sites read none, kz3_native_post.c).
 *   Prints `R <what> equal=<n> differ=<n> declined=<n> lifted_nr=<n>` and the first difference.
 */
#define main jdh_main
#include "job_diff_harness.c"
#undef main
#include "kz3_native_jobs.h"

int (*g_kz3_native_gate[64])(spu_context*);

static const kz3_native_entry* find_entry(int slot)
{
    for (unsigned i = 0; i < kz3_native_post_n; i++)
        if (kz3_native_post[i].slot == slot) return &kz3_native_post[i];
    for (unsigned i = 0; i < kz3_native_dof_n; i++)
        if (kz3_native_dof[i].slot == slot) return &kz3_native_dof[i];
    for (unsigned i = 0; i < kz3_native_lp_n; i++)
        if (kz3_native_lp[i].slot == slot) return &kz3_native_lp[i];
    return NULL;
}

static uint64_t fnv_pool(const uint8_t* ls, uint32_t lo_, uint32_t hi_)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t a = lo_; a < hi_; a += 8) {
        uint64_t v;
        memcpy(&v, ls + a, 8);
        h = (h ^ v) * 0x100000001b3ull;
    }
    return h;
}

/* Kind-specific synthetic arguments and data on top of setup()'s random LS and registers. */
static void synth_lp(spu_context* c, const char* kind, int variant);
static void synth(spu_context* c, const char* kind, int variant)
{
    uint8_t* ls = c->ls;
    const uint32_t area = lo, span = 0x3E000 - lo;
    if (!strncmp(kind, "lp_", 3)) { synth_lp(c, kind, variant); return; }
    if (!strcmp(kind, "blend")) {
        const uint32_t n = 4 * (1 + rnd() % (variant % 7 == 0 ? 3 : 200));
        const uint32_t a = area + ((rnd() % (span / 2 - 4 * 800)) & ~15u);
        const uint32_t b = area + span / 2 + ((rnd() % (span / 2 - 4 * 800)) & ~15u);
        c->gpr[3] = spu_splat_u32(a);
        c->gpr[4] = spu_splat_u32(b);
        c->gpr[5] = spu_splat_u32(n);
        /* extreme pixel values now and then: coverage 0 / 255, channels 0 / 255 */
        if (variant % 3 == 1)
            for (uint32_t i = 0; i < 4 * n + 16; i++) {
                ls[a + i] = (rnd() & 1) ? 0 : 0xFF;
                ls[b + i] = (rnd() & 1) ? 0 : 0xFF;
            }
        if (variant % 3 == 2)
            for (uint32_t i = 0; i < n; i++) ls[a + 4 * i] = (rnd() & 1) ? 0 : (rnd() & 1) ? 0xFF : rnd() & 0xFF;
    }
    if (!strcmp(kind, "dof_unpack")) {
        const uint32_t n = 4 * (1 + rnd() % (variant % 7 == 0 ? 3 : 150));
        const uint32_t d = area + ((rnd() % (span / 2 - 16 * 700)) & ~15u);
        const uint32_t s = area + span / 2 + ((rnd() % (span / 4 - 4 * 700)) & ~15u);
        const uint32_t w = area + span / 2 + span / 4 + ((rnd() % (span / 4 - 4 * 700)) & ~15u);
        c->gpr[3] = spu_splat_u32(d);
        c->gpr[4] = spu_splat_u32(s);
        c->gpr[5] = spu_splat_u32(w);
        c->gpr[6] = spu_splat_u32(n);
        if (variant % 3 == 1)
            for (uint32_t i = 0; i < 4 * n + 16; i++) {
                ls[s + i] = (rnd() & 1) ? 0 : 0xFF;
                ls[w + i] = (rnd() & 1) ? 0 : 0xFF;
            }
    }
}


static uint32_t g_dead_lo, g_dead_hi;   /* LS bytes of the kernel's own dead scratch frame (compare skips them) */

static void wbe32(uint8_t* ls, uint32_t a, uint32_t v)
{
    ls[a] = v >> 24; ls[a + 1] = v >> 16; ls[a + 2] = v >> 8; ls[a + 3] = (uint8_t)v;
}

/* a float with a random exponent in a useful range (some zeros, exponent-255 values and signs) */
static uint32_t rnd_float(int grid)
{
    const uint32_t r = rnd_word();
    if (grid) return ((r >> 8) % 17 > 8 ? 0x3F800000u : 0xBF800000u) + ((r & 7) << 22) * 0 + (((r >> 4) & 7) << 20);
    switch (r % 29) {
    case 0: return 0;
    case 1: return 0x80000000u;
    case 2: return 0x7F800000u | (r & 0x80000000u) | (rnd_word() & 0x7FFFFF);
    case 3: return (rnd_word() & 0x807FFFFFu) | ((10 + rnd_word() % 10) << 23);
    default: return (rnd_word() & 0x807FFFFFu) | ((100 + rnd_word() % 50) << 23);
    }
}

static void synth_lp(spu_context* c, const char* kind, int variant)
{
    uint8_t* ls = c->ls;
    const uint32_t area = (lo + 15) & ~15u;
    if (!strcmp(kind, "lp_kd")) {
        const uint32_t n = variant % 9 == 0 ? rnd() % 7 : 5 + rnd() % (variant % 5 == 0 ? 600 : 90);
        const uint32_t S = area, out = area + 64, base = area + 0x100 + 16 * (rnd() % 64);
        const int grid = variant % 3 == 0;   /* coarse positions: many equal distances */
        wbe32(ls, S, n);
        wbe32(ls, S + 20, base);
        for (uint32_t i = 0; i < n; i++) {
            for (int k = 0; k < 3; k++) {
                const uint32_t v = grid ? (0x3F800000u + ((rnd() % 5) << 21)) | ((rnd() & 1) << 31) : rnd_float(0);
                wbe32(ls, base + 96 * i + 4 * k, v);
            }
            wbe32(ls, base + 96 * i + 12, rnd() % (variant % 11 == 0 ? 4 : 3));
        }
        c->gpr[3] = spu_splat_u32(S);
        c->gpr[4] = spu_splat_u32(out);
        for (int k = 0; k < 4; k++) c->gpr[5]._u32[k] = grid ? (0x3F800000u + ((rnd() % 5) << 21)) : rnd_float(0);
        c->gpr[1]._u32[0] = 0x3F000;
    }
    if (!strcmp(kind, "lp_blend")) {
        const uint32_t n = variant % 8 == 0 ? 1 + rnd() % 4 : 5 + rnd() % (variant % 5 == 0 ? 400 : 60);
        const uint32_t S = area, out9 = area + 0x100, base = area + 0x200 + 16 * (rnd() % 16);
        const int grid = variant % 3 == 0;
        wbe32(ls, S, n);
        wbe32(ls, S + 20, base);
        for (uint32_t i = 0; i < n; i++) {
            for (int k = 0; k < 3; k++) {
                const uint32_t v = grid ? (0x3F800000u + ((rnd() % 5) << 21)) | ((rnd() & 1) << 31) : rnd_float(0);
                wbe32(ls, base + 96 * i + 4 * k, v);
            }
            wbe32(ls, base + 96 * i + 12, rnd() % 3);
            for (int k = 0; k < 72; k++) ls[base + 96 * i + 16 + k] = rnd() & 0xFF;
            if (variant % 4 == 1)   /* small coefficients and extremes */
                for (int k = 0; k < 36; k++) { ls[base + 96 * i + 16 + 2 * k] = (rnd() & 1) ? 0 : 0x7F + (rnd() & 1); }
        }
        c->gpr[3] = spu_splat_u32(S);
        c->gpr[5] = spu_splat_u32(out9);
        for (int k = 0; k < 4; k++) c->gpr[4]._u32[k] = grid ? (0x3F800000u + ((rnd() % 5) << 21)) : rnd_float(0);
        /* the query on (or a hair from) a probe: exact-hit path and the 1/128 threshold */
        if (variant % 6 < 3) {
            const uint32_t j = rnd() % n;
            static const uint32_t dl[] = {0, 0, 0x3C000000u, 0x3C000001u, 0x3BFFFFFFu, 0x3C010000u, 0x3B800000u};
            for (int k = 0; k < 3; k++) {
                uint32_t pv = ((uint32_t)ls[base + 96 * j + 4 * k] << 24) | (ls[base + 96 * j + 4 * k + 1] << 16) |
                              (ls[base + 96 * j + 4 * k + 2] << 8) | ls[base + 96 * j + 4 * k + 3];
                c->gpr[4]._u32[k] = pv;
            }
            /* offset one axis by +delta using the float helpers' own add (host float is fine for a test input) */
            const uint32_t dd = dl[rnd() % 7];
            float f, g;
            memcpy(&f, &c->gpr[4]._u32[0], 4);
            memcpy(&g, &dd, 4);
            f += g;
            if (f == f && f - f == 0) memcpy(&c->gpr[4]._u32[0], &f, 4);
        }
        c->gpr[1]._u32[0] = 0x3F000;
        g_dead_lo = 0x3F000 - 1968 + 0x30;
        g_dead_hi = 0x3F000 - 1968 + 0x790;
    }
    if (!strcmp(kind, "lp_light")) {
        /* r3 = L (64 B), r4 = query quad, r5 = out9; the kind is the halfword at L+0x3C (0 point, 1 spot, else directional) */
        const uint32_t L = area + 0x100 + 16 * (rnd() % 8), out9 = area + 0x300 + 16 * (rnd() % 8);
        const int grid = variant % 3 == 0;
        const uint32_t kinds[] = {0, 0, 1, 1, 2, 7, 0xFFFF};
        const uint32_t kd = kinds[(variant / 2) % 7], sel = variant % 5 == 0 ? rnd() % 3 : (kd == 1 ? (rnd() % 8 == 0 ? 3 : rnd() % 3) : rnd() & 0xFFFF);
        for (int k = 0; k < 16; k++) {
            uint32_t v = rnd_float(grid);
            if (variant % 4 == 1 && (k >= 8 && k < 12)) v = 0x3F000000u + ((rnd() & 0xFF) << 12) + ((rnd() & 1) << 31);   /* W0..W3 of moderate size */
            if (variant % 4 == 2 && k < 3) v = 0x3F800000u + ((rnd() % 5) << 21);                                       /* positions near 1 */
            if (variant % 7 == 3 && k >= 4 && k < 8) v = 0x3F800000u;                                                   /* colour 1 */
            wbe32(ls, L + 4 * k, v);
        }
        if (variant % 4 == 3 || variant % 4 == 0) {
            /* tame values: lights near the query, smoothsteps inside (0, 1) most of the time (both paths pass often) */
            const float lo_[16] = {-4, -4, -4, 0, 0, 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, 0}, hi_[16] = {4, 4, 4, 0, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1, 1, 0};
            for (int k = 0; k < 16; k++) {
                float f = lo_[k] + (hi_[k] - lo_[k]) * (float)(rnd() & 0xFFFF) / 65535.0f;
                if (k == 8) f = 0.5f + 3.0f * (float)(rnd() & 0xFFFF) / 65535.0f;            /* W0 */
                if (k == 9) f = -2.0f + 2.5f * (float)(rnd() & 0xFFFF) / 65535.0f;           /* W1 */
                if (k == 10) f = -0.6f * (float)(rnd() & 0xFFFF) / 65535.0f;                 /* W2 */
                if (k == 11) f = 0.3f + 1.5f * (float)(rnd() & 0xFFFF) / 65535.0f;           /* W3 */
                uint32_t v;
                memcpy(&v, &f, 4);
                wbe32(ls, L + 4 * k, v);
            }
        }
        wbe32(ls, L + 0x3C, (kd << 16) | sel);
        c->gpr[3] = spu_splat_u32(L);
        c->gpr[5] = spu_splat_u32(out9);
        for (int k = 0; k < 4; k++) c->gpr[4]._u32[k] = grid ? (0x3F800000u + ((rnd() % 5) << 21)) : rnd_float(0);
        if (variant % 4 == 3 || variant % 4 == 0)
            for (int k = 0; k < 3; k++) {
                const float f = -3.0f + 6.0f * (float)(rnd() & 0xFFFF) / 65535.0f;
                memcpy(&c->gpr[4]._u32[k], &f, 4);
            }
        if (variant % 6 == 0)   /* query on the light: exact zero distance */
            for (int k = 0; k < 3; k++) c->gpr[4]._u32[k] = ((uint32_t)ls[L + 4 * k] << 24) | (ls[L + 4 * k + 1] << 16) | (ls[L + 4 * k + 2] << 8) | ls[L + 4 * k + 3];
        c->gpr[1]._u32[0] = 0x3F000;
        g_dead_lo = 0x3F000 - 1760 + 0x10;
        g_dead_hi = 0x3F000;
    }
    if (!strcmp(kind, "lp_atten")) {
        const uint32_t buf = area + 0x100 + 16 * (rnd() % 8);
        for (int k = 0; k < 36; k++) wbe32(ls, buf + 4 * k, rnd_float(0));
        c->gpr[5] = spu_splat_u32(buf);
        c->gpr[4]._u32[0] = (variant % 4 == 0) ? (rnd() & 0x3C) : rnd();
    }
    if (!strcmp(kind, "lp_bake")) {
        const uint32_t cnt = variant % 7 == 0 ? 4 * (rnd() % 3) : (rnd() & 1) ? 64 : 128;
        const uint32_t S = area, shp = area + 0x100, scr = area + 0x200, tbl = area + 0x1200 + 4 * (rnd() % 8),
                       oh = area + 0x9000;
        wbe32(ls, S + 28, scr);
        for (uint32_t k = 0; k < 36; k++) wbe32(ls, shp + 4 * k, rnd_float(0));
        for (uint32_t i = 0; i < 128 * 13; i++) wbe32(ls, tbl + 4 * i, rnd_float(0));
        c->gpr[3] = spu_splat_u32(S);
        c->gpr[4] = spu_splat_u32(shp);
        c->gpr[5] = spu_splat_u32(tbl);
        c->gpr[6] = spu_splat_u32(cnt);
        c->gpr[7] = spu_splat_u32(oh);
    }
}

static int load_record(spu_context* c, const char* path, int slot)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char m[8];
    uint32_t h[4];
    int ok = fread(m, 1, 8, f) == 8 && !memcmp(m, KZ3_NJ_MAGIC, 8) && fread(h, 4, 4, f) == 4 && (int)h[0] == slot;
    spu_context_init(c, 0);
    ok = ok && fread(c->gpr, 16, 128, f) == 128 && fread(c->ls, 1, SPU_LS_SIZE, f) == SPU_LS_SIZE;
    fclose(f);
    if (ok) {
        c->image_id = 1;
        c->ls[SENTINEL + 2] = 0x3F;
        c->ls[SENTINEL + 3] = 0xFF;
        c->gpr[0] = spu_splat_u32(SENTINEL);
        c->gpr[0]._u32[1] = c->gpr[0]._u32[2] = c->gpr[0]._u32[3] = 0;
        c->pc = h[1];
    }
    return ok;
}

static int g_equal, g_differ, g_declined, g_nr, g_shown;
static int g_cov[3][2];   /* lp_light: [point, spot, directional][returned 0, 1] among the compared states */

static void compare(const char* what, spu_context* a, spu_context* b)
{
    int bad = 0;
    long off = -1;
    if (g_dead_hi > g_dead_lo) memcpy(b->ls + g_dead_lo, a->ls + g_dead_lo, g_dead_hi - g_dead_lo);
    if (memcmp(a->ls, b->ls, SPU_LS_SIZE)) {
        bad = 1;
        for (long i = 0; i < SPU_LS_SIZE; i++)
            if (a->ls[i] != b->ls[i]) { off = i; break; }
    }
    int reg = -1;
    for (int r = 0; r < 128 && !bad; r++)
        if ((r < 2 || r >= 80) && memcmp(&a->gpr[r], &b->gpr[r], 16)) { bad = 1; reg = r; }
    if ((b->pc & SPU_LS_MASK) != (a->pc & SPU_LS_MASK) && !bad) { bad = 1; reg = -2; }
    if (!bad) { g_equal++; return; }
    g_differ++;
    if (g_shown++ < 5) printf("DIFF %s ls_off=0x%lX reg=%d pc a=%X b=%X\n", what, off, reg, a->pc, b->pc);
}

int main(int argc, char** argv)
{
    if (argc < 6) { fprintf(stderr, "usage\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); image_len = ftell(f); fseek(f, 0, SEEK_SET);
    image = malloc(image_len);
    if (fread(image, 1, image_len, f) != (size_t)image_len) return 2;
    fclose(f);
    base = (uint32_t)strtoul(argv[2], 0, 0);
    if (base + (uint32_t)image_len > lo) lo = (base + (uint32_t)image_len + 0xFFF) & ~0xFFFu;
    const int slot = atoi(argv[3]);
    const uint64_t seed = strtoull(argv[4], 0, 0);
    const int trials = atoi(argv[5]);
    const kz3_native_entry* e = find_entry(slot);
    if (!e) { printf("R slot %d: no entry\n", slot); return 2; }
    extern void HARNESS_REGISTER(void);
    HARNESS_REGISTER();
#ifdef SPU_WWS_READ_CAP
    g_wws_read_probe = SPU_WWS_READ_CAP;
    g_wws_code_probe = SPU_WWS_CODE_CAP;
#endif
    spu_hop_init();
    signal(SIGALRM, on_alarm);
    fn_t fn = NULL;
    for (unsigned i = 0; i < nreg; i++) if (reg[i].addr == e->addr) fn = reg[i].fn;
    if (!fn) { printf("R slot %d: no lifted function 0x%X\n", slot, e->addr); return 2; }
    /* the image's pool must be what the native kernel expects (the gate's own check) */
    static spu_context P;
    setup(&P, e->addr, 1);
    if (fnv_pool(P.ls, e->pool_lo, e->pool_hi) != e->pool_hash) {
        printf("R slot %d %s: POOL HASH MISMATCH (image %s) have 0x%016llx\n", slot, e->kind, e->image, (unsigned long long)fnv_pool(P.ls, e->pool_lo, e->pool_hi));
        return 1;
    }
    const int nrec = argc - 6;
    for (int t = 0; t < trials + nrec; t++) {
        char what[96];
        if (t < trials) {
            setup(&A, e->addr, seed + (uint64_t)t * 7919);
            synth(&A, e->kind, t);
            snprintf(what, sizeof what, "synth#%d", t);
        } else {
            snprintf(what, sizeof what, "%s", argv[6 + t - trials]);
            if (!load_record(&A, argv[6 + t - trials], slot)) { printf("R bad record %s\n", what); g_nr++; continue; }
        }
        if (!strcmp(e->kind, "lp_light") && t >= trials) {
            g_dead_lo = (A.gpr[1]._u32[0] - 1760 + 0x10) & SPU_LS_MASK;
            g_dead_hi = A.gpr[1]._u32[0] & SPU_LS_MASK;
        }
        if (!strcmp(e->kind, "lp_blend") && t >= trials) {
            g_dead_lo = (A.gpr[1]._u32[0] - 1968 + 0x30) & SPU_LS_MASK;
            g_dead_hi = (A.gpr[1]._u32[0] - 1968 + 0x790) & SPU_LS_MASK;
        }
        memcpy(&B, &A, sizeof A);
        B.ls = B.ls_store;   /* the copy owns its own local store */
        A.ls = A.ls_store;
        memcpy(B.ls_store, A.ls_store, SPU_LS_SIZE);
        uint32_t lkind = 0;
        if (!strcmp(e->kind, "lp_light")) {
            const uint32_t Lp = A.gpr[3]._u32[0] & SPU_LS_MASK;
            lkind = ((uint32_t)A.ls[Lp + 0x3C] << 8) | A.ls[Lp + 0x3D];
        }
        const int r = run_lifted(&A, fn);
        if (r != R_RET) { g_nr++; continue; }
        const int took = e->fn(&B);
        if (!took) { g_declined++; continue; }
        B.pc = B.gpr[0]._u32[0];
        compare(what, &A, &B);
        if (!strcmp(e->kind, "lp_light") && 1) {
            const int ret = B.gpr[3]._u32[0] == 1;
            const int ck = lkind == 0 ? 0 : lkind == 1 ? 1 : 2;
            g_cov[ck][ret]++;
        }
    }
    printf("R slot %d %s %s 0x%X: equal=%d differ=%d declined=%d lifted_nr=%d\n", slot, e->kind, e->image, e->addr,
           g_equal, g_differ, g_declined, g_nr);
    if (!strcmp(e->kind, "lp_light"))
        printf("R   coverage (returned 0 / 1): point %d / %d, spot %d / %d, directional %d / %d\n", g_cov[0][0], g_cov[0][1], g_cov[1][0], g_cov[1][1], g_cov[2][0], g_cov[2][1]);
    return g_differ ? 1 : 0;
}

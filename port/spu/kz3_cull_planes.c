/* KZ3 SPU pass 41: KZ3_CULL_PLANES=1, the dilated occlusion planes check (the RE notes s.13.3 candidate 2).
 *
 * The occlusion block (r3 of 0x1D7E58, 1.14 0x1E35C8, which sizes it every frame) holds tilesX at +0x870, tilesY at
 * +0x874, the plane pitch in bytes at +0x878 and the EA of a buffer of pitch * tilesY * 5 bytes at +0x898 (u16 tiles,
 * 0xFFFF = far; read from 0x1D7E58 with rizin this round). Planes 1-4 are the dilated copies of plane 0 with radii
 * +0x87C..+0x888 = 1, 3, 5, 7; small objects are tested only against them (job 48 PSGOcclusionSPU).
 * ASSUMPTION: plane k lives at buffer + k * pitch * tilesY (plane-major) and the dilation is a max over a clamped
 * (2r+1)^2 tile window (max = farther = conservative). Both operators (max and min) are checked so that a wrong guess
 * shows as "min matches" instead of passing silently.
 *
 * At the entry of job 45 (OcclusionFilterSPU, module 0x113DD00 / 1.14 0x11BA180) the probe snapshots plane 0 (a host
 * copy per block, plus a checksum); at the entry of job 49 (module 0x11C7200 / 1.14 0x1244080; job 48 never runs in
 * mission_0), at most every 250 ms, it reads the 5 planes of every block 0x1D7E58 has seen and logs `[cull-planes]`:
 * plane 0 against the forward 2x2 max of the snapshot (job 45 rewrites plane 0 in place after the planes: pass 42), planes 1-4 against the clamped (2r+1)^2 max of the snapshot, mismatching tile counts
 * per plane (edge tiles and low-byte-only shortfalls, the SIMD shufb quirk, counted apart). KZ3_CULL_TILE=x,y adds
 * a per-frame `[cull-tile]` trace of one tile (snapshot + 5 planes, logged on change).
 * Entry hooks are patch 1044's; a block whose dims look wrong is skipped. Nothing is written. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kz3_spu_skip.h"
#ifdef _WIN32
#include <windows.h>
#endif

extern uint8_t* vm_base;
extern void spu_overlay_set_entry_hook(int image_id, int (*fn)(struct spu_context*, int, void*), void* user);

static int g_on = -1;
#define NBLK 8
static volatile uint32_t g_blk[NBLK];
static volatile unsigned long long g_blk_ms[NBLK];
static volatile uint32_t g_sum45[NBLK];
static volatile unsigned long long g_last48;
static volatile unsigned g_n45, g_n48, g_lines;
static volatile long g_busy;

static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    return 0;
#endif
}
static uint32_t rd32(uint32_t ea)
{
    uint32_t v;
    memcpy(&v, vm_base + ea, 4);
    return __builtin_bswap32(v);
}
static uint16_t rd16(uint32_t ea)
{
    return (uint16_t)(vm_base[ea] << 8 | vm_base[ea + 1]);
}

int kz3_cull_planes_on(void)
{
    if (g_on < 0) {
        const char* e = getenv("KZ3_CULL_PLANES");
        g_on = e && *e && *e != '0';
    }
    return g_on;
}

static void kz3_cull_planes_init(void);
static void cj_count(int job);
/* PPU side: 0x1D7E58 exit (r3 = the block). */
void kz3_cull_planes_block(uint32_t blk)
{
    static volatile long inited;
    if (!kz3_cull_planes_on() || blk < 0x10000u || blk >= 0x50000000u) return;
    if (!__atomic_exchange_n(&inited, 1, __ATOMIC_ACQ_REL)) kz3_cull_planes_init();
    const unsigned long long t = now_ms();
    int free_i = -1, old_i = 0;
    for (int i = 0; i < NBLK; i++) {
        if (g_blk[i] == blk) {
            g_blk_ms[i] = t;
            return;
        }
        if (!g_blk[i] && free_i < 0) free_i = i;
        if (g_blk_ms[i] < g_blk_ms[old_i]) old_i = i;
    }
    const int i = free_i >= 0 ? free_i : old_i;
    g_blk[i] = blk;
    g_blk_ms[i] = t;
    g_sum45[i] = 0;
    fprintf(stderr, "[cull-planes] block 0x%08X slot %d tiles %ux%u pitch %u buf 0x%08X radii %u/%u/%u/%u\n", blk, i,
            rd32(blk + 0x870), rd32(blk + 0x874), rd32(blk + 0x878), rd32(blk + 0x898), rd32(blk + 0x87C),
            rd32(blk + 0x880), rd32(blk + 0x884), rd32(blk + 0x888));
}

static int dims(uint32_t blk, uint32_t* tx, uint32_t* ty, uint32_t* pitch, uint32_t* buf)
{
    *tx = rd32(blk + 0x870);
    *ty = rd32(blk + 0x874);
    *pitch = rd32(blk + 0x878);
    *buf = rd32(blk + 0x898);
    return *tx && *ty && *tx <= 512 && *ty <= 512 && *pitch >= *tx * 2 && *pitch <= 2048 && *buf >= 0x10000u &&
           *buf < 0x50000000u - *pitch * *ty * 5;
}
static uint32_t plane_sum(uint32_t buf, uint32_t pitch, uint32_t tx, uint32_t ty, int k)
{
    uint32_t h = 2166136261u;
    for (uint32_t y = 0; y < ty; y++)
        for (uint32_t x = 0; x < tx; x++) h = (h ^ rd16(buf + (k * ty + y) * pitch + x * 2)) * 16777619u;
    return h;
}

/* Pass 42 (analysis of job 45): job 45 builds planes 1-4 from the plane 0 it
 * receives and then rewrites plane 0 in place as the forward 2x2 max `max(p0[y..y+1][x..x+1])` (clamped), so the
 * reference must be a snapshot of plane 0 taken at 45's entry, not the plane 0 that 49 sees. Plane 0 itself is
 * checked against the 2x2 max of the snapshot; on the SIMD path (tilesX % 8 == 0) element 7 of every qword but the
 * last gets a wrong low byte (shufb selector 0x20 quirk), so the low byte of tiles x = 7, 15, 23, ... is ignored. */
static uint16_t* g_snap[NBLK];
static volatile uint32_t g_snap_n[NBLK], g_snap_tx[NBLK];
static volatile unsigned g_snap_seq[NBLK];
static int g_tile_x = -1, g_tile_y = -1;   /* KZ3_CULL_TILE=x,y: per-frame values of one tile at 49's entry */
static unsigned g_tile_lines;
static uint16_t g_tile_last[6];

static int hook45(struct spu_context* ctx, int image_id, void* user)
{
    (void)ctx, (void)image_id, (void)user;
    cj_count(45);
    __atomic_add_fetch(&g_n45, 1, __ATOMIC_RELAXED);
    for (int i = 0; i < NBLK; i++) {
        uint32_t tx, ty, pitch, buf;
        if (!g_blk[i] || !dims(g_blk[i], &tx, &ty, &pitch, &buf)) continue;
        g_sum45[i] = plane_sum(buf, pitch, tx, ty, 0);
        const uint32_t n = tx * ty;
        if (g_snap_n[i] != n) {
            free(g_snap[i]);
            g_snap[i] = (uint16_t*)malloc(n * 2);
            g_snap_n[i] = g_snap[i] ? n : 0;
        }
        if (!g_snap[i]) continue;
        for (uint32_t y = 0; y < ty; y++)
            for (uint32_t x = 0; x < tx; x++) g_snap[i][y * tx + x] = rd16(buf + y * pitch + x * 2);
        g_snap_tx[i] = tx;
        g_snap_seq[i]++;
    }
    return 0;
}

static void check_block(int bi, uint32_t blk, unsigned long long t)
{
    uint32_t tx, ty, pitch, buf;
    if (!dims(blk, &tx, &ty, &pitch, &buf)) {
        static unsigned long long last_bad[NBLK];
        if (t - last_bad[bi] >= 10000) {
            last_bad[bi] = t;
            fprintf(stderr, "[cull-planes] t=%llu blk=0x%08X dims not usable: tiles %ux%u pitch %u buf 0x%08X (45=%u 49=%u)\n",
                    t, blk, tx, ty, pitch, buf, g_n45, g_n48);
        }
        return;
    }
    const uint32_t n = tx * ty;
    const uint16_t* snap = (g_snap_n[bi] == n && g_snap_tx[bi] == tx) ? g_snap[bi] : NULL;
    uint16_t* p = (uint16_t*)malloc(n * 5 * 2);
    if (!p) return;
    for (uint32_t k = 0; k < 5; k++)
        for (uint32_t y = 0; y < ty; y++)
            for (uint32_t x = 0; x < tx; x++) p[k * n + y * tx + x] = rd16(buf + (k * ty + y) * pitch + x * 2);
    const uint32_t s0 = plane_sum(buf, pitch, tx, ty, 0);
    unsigned far0 = 0;
    for (uint32_t i = 0; i < n; i++) far0 += p[i] == 0xFFFF;
    char line[640];
    int l = snprintf(line, sizeof line, "[cull-planes] t=%llu blk=0x%08X %ux%u 45=%u 49=%u far0=%u/%u p0-vs-45entry=%s snap=%s", t, blk,
                     tx, ty, g_n45, g_n48, far0, n, !g_sum45[bi] ? "none" : g_sum45[bi] == s0 ? "same" : "changed",
                     snap ? "yes" : "no");
    if (!snap) {
        fprintf(stderr, "%s\n", line);
        free(p);
        return;
    }
    /* plane 0 against the forward 2x2 max of the snapshot (low byte of x = 7 mod 8 ignored on the SIMD path) */
    {
        unsigned bad0 = 0, bad0_lo = 0;
        const int simd = tx % 8 == 0;
        for (uint32_t y = 0; y < ty; y++)
            for (uint32_t x = 0; x < tx; x++) {
                const uint32_t y1 = y + 1 < ty ? y + 1 : y, x1 = x + 1 < tx ? x + 1 : x;
                uint16_t mx = snap[y * tx + x];
                if (snap[y * tx + x1] > mx) mx = snap[y * tx + x1];
                if (snap[y1 * tx + x] > mx) mx = snap[y1 * tx + x];
                if (snap[y1 * tx + x1] > mx) mx = snap[y1 * tx + x1];
                const uint16_t got = p[y * tx + x];
                if (got == mx) continue;
                if (simd && (x & 7) == 7 && x + 1 < tx && (got & 0xFF00) == (mx & 0xFF00)) {
                    bad0_lo++;
                    continue;
                }
                bad0++;
            }
        l += snprintf(line + l, sizeof line - l, " p0:2x2-bad=%u(lo-byte %u)", bad0, bad0_lo);
    }
    static const int radius[4] = {1, 3, 5, 7};
    unsigned bad_first = 0;
    for (int k = 1; k <= 4; k++) {
        const int r = (int)rd32(blk + 0x87C + 4 * (k - 1));
        if (r != radius[k - 1]) l += snprintf(line + l, sizeof line - l, " r%d=%d!", k, r);
        unsigned bad_max = 0, bad_edge = 0, bad_lo = 0;
        for (int y = 0; y < (int)ty; y++)
            for (int x = 0; x < (int)tx; x++) {
                uint16_t mx = 0;
                for (int yy = y - r; yy <= y + r; yy++) {
                    if (yy < 0 || yy >= (int)ty) continue;
                    for (int xx = x - r; xx <= x + r; xx++) {
                        if (xx < 0 || xx >= (int)tx) continue;
                        const uint16_t v = snap[yy * tx + xx];
                        if (v > mx) mx = v;
                    }
                }
                const uint16_t got = p[k * n + y * tx + x];
                if (got == mx) continue;
                if ((got & 0xFF00) == (mx & 0xFF00) && got < mx) {   /* a low-byte-only shortfall */
                    bad_lo++;
                    continue;
                }
                bad_max++;
                bad_edge += x < r || y < r || x >= (int)tx - r || y >= (int)ty - r;
                if (k == 1 && !bad_first && g_lines < 40) {
                    bad_first = 1;
                    fprintf(stderr, "[cull-planes]   first p1 mismatch at (%d,%d): got 0x%04X max(snap) 0x%04X snap 0x%04X p0-now 0x%04X\n",
                            x, y, got, mx, snap[y * tx + x], p[y * tx + x]);
                }
            }
        l += snprintf(line + l, sizeof line - l, " p%d:bad=%u(edge %u, lo-byte %u)", k, bad_max, bad_edge, bad_lo);
        if (l >= (int)sizeof line - 80) break;
    }
    if (g_tile_x >= 0 && (uint32_t)g_tile_x < tx && (uint32_t)g_tile_y < ty) {
        const uint32_t i = (uint32_t)g_tile_y * tx + (uint32_t)g_tile_x;
        l += snprintf(line + l, sizeof line - l, " tile(%d,%d)=snap %04X p 0:%04X 1:%04X 2:%04X 3:%04X 4:%04X", g_tile_x, g_tile_y,
                      snap[i], p[i], p[n + i], p[2 * n + i], p[3 * n + i], p[4 * n + i]);
    }
    fprintf(stderr, "%s\n", line);
    free(p);
}

/* KZ3_CULL_TILE=x,y: at every 49 entry (once per frame) the snapshot value and the five plane values of that tile,
 * logged when any of them changes (first 3000 lines). A tile that flips near/0xFFFF between frames points at the
 * raster side (43/44/51), a steady tile at the test side (49/50). */
static void tile_trace(unsigned long long t)
{
    if (g_tile_x < 0 || g_tile_lines >= 3000) return;
    for (int i = 0; i < NBLK; i++) {
        uint32_t tx, ty, pitch, buf;
        if (!g_blk[i] || !dims(g_blk[i], &tx, &ty, &pitch, &buf)) continue;
        if ((uint32_t)g_tile_x >= tx || (uint32_t)g_tile_y >= ty) continue;
        uint16_t v[6];
        v[0] = (g_snap_n[i] == tx * ty && g_snap_tx[i] == tx) ? g_snap[i][g_tile_y * tx + g_tile_x] : 0;
        for (int k = 0; k < 5; k++) v[k + 1] = rd16(buf + (k * ty + g_tile_y) * pitch + g_tile_x * 2);
        if (memcmp(v, g_tile_last, sizeof v) == 0) return;
        memcpy(g_tile_last, v, sizeof v);
        g_tile_lines++;
        fprintf(stderr, "[cull-tile] t=%llu 45=%u 49=%u (%d,%d) snap %04X p0 %04X p1 %04X p2 %04X p3 %04X p4 %04X\n", t, g_n45, g_n48,
                g_tile_x, g_tile_y, v[0], v[1], v[2], v[3], v[4], v[5]);
        return;
    }
}

static int hook48(struct spu_context* ctx, int image_id, void* user)
{
    (void)ctx, (void)image_id, (void)user;
    cj_count((int)(intptr_t)user);
    __atomic_add_fetch(&g_n48, 1, __ATOMIC_RELAXED);
    const unsigned long long t = now_ms();
    if (__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQUIRE)) return 0;
    tile_trace(t);
    if (t - g_last48 >= 250 && g_lines < 4000) {
        g_last48 = t;
        for (int i = 0; i < NBLK; i++)
            if (g_blk[i]) {   /* 0x1D7E58 may run only at view setup: no freshness test */
                g_lines++;
                check_block(i, g_blk[i], t);
            }
    }
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
    return 0;
}

/* Entry counts of the visibility jobs 43-51 (which of them run at all), one line per 10 s from whichever hook runs. */
static const uint32_t CJ_EA[9][2] = {{0x01130800u, 0x011ACC80u}, {0x0113AF00u, 0x011B7380u}, {0x0113DD00u, 0x011BA180u},
                                     {0x011A6D80u, 0x01223180u}, {0x011AD200u, 0x01229900u}, {0x011BE480u, 0x0123AF80u},
                                     {0x011C7200u, 0x01244080u}, {0x011CC280u, 0x01249100u}, {0x011B8280u, 0x01234C80u}};
static volatile unsigned g_cj[9];
static volatile unsigned long long g_cj_last;
static void cj_count(int job)
{
    if (job < 43 || job > 51) return;
    __atomic_add_fetch(&g_cj[job - 43], 1, __ATOMIC_RELAXED);
    const unsigned long long t = now_ms();
    if (t - g_cj_last >= 10000) {
        g_cj_last = t;
        fprintf(stderr, "[cull-planes] t=%llu entries 43=%u 44=%u 45=%u 46=%u 47=%u 48=%u 49=%u 50=%u 51=%u\n", t, g_cj[0],
                g_cj[1], g_cj[2], g_cj[3], g_cj[4], g_cj[5], g_cj[6], g_cj[7], g_cj[8]);
    }
}
static int hook_count(struct spu_context* ctx, int image_id, void* user)
{
    (void)ctx, (void)image_id;
    cj_count((int)(intptr_t)user);
    return 0;
}

static int id_by_ea2(uint32_t a, uint32_t b)
{
    for (unsigned i = 0; i < kz3_spu_image_names_n; i++)
        if (kz3_spu_image_names[i].ea == a || kz3_spu_image_names[i].ea == b) return kz3_spu_image_names[i].image_id;
    return 0;
}

/* Called once from the first 0x1D7E58 call (the SPU images are registered by then; a KZ3_CULL_AB hook on 45/48 is
 * replaced, so do not combine the two). */
static void kz3_cull_planes_init(void)
{
    if (!kz3_cull_planes_on()) return;
    const char* tile = getenv("KZ3_CULL_TILE");
    if (tile && *tile) {
        if (sscanf(tile, "%d,%d", &g_tile_x, &g_tile_y) != 2) g_tile_x = 20, g_tile_y = 11;   /* "1" = screen centre of 40x23 */
    }
    for (int j = 43; j <= 51; j++) {   /* counters first; 45 and 48 get their own hooks below */
        const int id = id_by_ea2(CJ_EA[j - 43][0], CJ_EA[j - 43][1]);
        if (id > 0 && j != 45 && j != 49) spu_overlay_set_entry_hook(id, hook_count, (void*)(intptr_t)j);
    }
    /* pass 41 run spu-41p3: job 48 never runs (0 entries in 300 s), 49 (Scene.KdTree) runs once per frame right
     * after 45, so the check sits at 49's entry */
    const int id45 = id_by_ea2(0x0113DD00u, 0x011BA180u), id48 = id_by_ea2(0x011C7200u, 0x01244080u);
    if (id45 > 0) spu_overlay_set_entry_hook(id45, hook45, NULL);
    if (id48 > 0) spu_overlay_set_entry_hook(id48, hook48, (void*)(intptr_t)49);
    fprintf(stderr, "[cull-planes] on: job 45 image %d, check at job 49 image %d, tile %d,%d\n", id45, id48, g_tile_x, g_tile_y);
}

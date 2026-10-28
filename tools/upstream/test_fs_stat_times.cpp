/*
 * KZ3 patch 5006 (RE pass 163): cellFsStat / cellFsFstat report the host file's mtime and ctime.
 *
 * Model of the failing Botzone follow run: one guest path (the lump) is served from the disc entry first and from the
 * .pspatch output after the game flips its patch set; the patched entry is larger (+60 bytes, defaultshared) or
 * smaller than the base. Sony FIOS caches 512 KB blocks of the first file and keeps its mtime and ctime; at open it
 * stats the path again and drops the blocks only if one of them differs ("mod date changed" / "creation date changed",
 * KZ3 1.14 0xDF2B90..0xDF2C68, 0xDF7D9C..0xDF8160). The object pass then skips resident bodies and refills the tail
 * of the file at an unaligned offset (0x2B5769 of 0x2F3A2A).
 *
 * The model below keeps FIOS's rule: a block cache keyed by (mtime, ctime) from cellFsStat. Without the patch both
 * are 0, the cache keeps the blocks of the old file and the tail read returns the old bytes (short for the larger
 * patched entry, stale for the smaller one); with it the cache refills and every read matches the file.
 *
 *   tools/upstream/test_fs_stat_times.sh          (builds this against the patched toolkit; Linux only, no game data)
 */
#include "ppu_fs.cpp"
#include <sys/stat.h>
#include <utime.h>
#include <vector>
#include <algorithm>

extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ydkj_host_bt(const char* tag) { (void)tag; }
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
}
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

/* The port's miss hook (psarc_vfs.cpp miss_hook): the loose lump path does not exist, the hook answers with the host
 * file of the active patch set. */
static char g_served[1100];
extern "C" void ps3_vfs_ps3game_fallback(char* path, size_t cap)
{
    if (strstr(path, "lump.core")) snprintf(path, cap, "%s", g_served);
}

static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static int fails = 0;
#define CHECK(c, ...) do { const bool ok_ = (c); printf(ok_ ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); \
                           printf("\n"); if (!ok_) fails++; } while (0)
enum : uint32_t { PATH = 0x100, FDP = 0x500, NP = 0x508, STATB = 0x600, RBUF = 0x1000 };

static int32_t call(void (*fn)(ppu_context*), uint64_t a, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0)
{
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = a; ctx.gpr[4] = b; ctx.gpr[5] = c; ctx.gpr[6] = d;
    fn(&ctx);
    return (int32_t)(uint32_t)ctx.gpr[3];
}
static const char* G = "/dev_bdvd/PS3_GAME/USRDIR/LocalCachePS3/lumps/lump.core";
struct Times { uint64_t size, mtime, ctime; };
static Times guest_stat()
{
    strcpy((char*)vm_base + PATH, G);
    call(cellFsStat, PATH, STATB);
    return { rd64(STATB + 0x24), rd64(STATB + 0x14), rd64(STATB + 0x1C) };
}

/* FIOS's HDD cache for this one file: 64 KB blocks, the file's (mtime, ctime) at fill, valid byte count per block. */
struct Cache {
    bool filled = false; uint64_t mtime = 0, ctime = 0;
    std::vector<std::vector<uint8_t>> blk;
};
static const uint64_t BS = 65536;
static bool host_read(uint64_t off, uint64_t n, std::vector<uint8_t>& out)   /* the game's cellFsLseek + cellFsRead */
{
    strcpy((char*)vm_base + PATH, G);
    if (call(cellFsOpen, PATH, 0, FDP) != 0) return false;
    const uint32_t fd = rd32(FDP);
    call(cellFsLseek, fd, off, 0, NP);
    call(cellFsRead, fd, RBUF, n, NP);
    const uint64_t got = rd64(NP);
    out.assign(vm_base + RBUF, vm_base + RBUF + got);
    call(cellFsClose, fd);
    return true;
}
static void cache_open(Cache& c)   /* FIOS at open: stat, compare, flush on a changed date, fill on a miss */
{
    const Times t = guest_stat();
    if (c.filled && (c.mtime != t.mtime || c.ctime != t.ctime)) { c.filled = false; c.blk.clear(); }
    if (!c.filled) {
        c.mtime = t.mtime; c.ctime = t.ctime; c.filled = true;
        for (uint64_t o = 0; o < t.size; o += BS) { std::vector<uint8_t> b; host_read(o, BS, b); c.blk.push_back(b); }
    }
}
static std::vector<uint8_t> cache_read(Cache& c, uint64_t off, uint64_t n)   /* what the cache hands to the caller */
{
    std::vector<uint8_t> r;
    for (uint64_t p = off; p < off + n; ) {
        const uint64_t bi = p / BS;
        if (bi >= c.blk.size()) break;
        const std::vector<uint8_t>& b = c.blk[bi];
        if (p % BS >= b.size()) break;
        const uint64_t take = std::min<uint64_t>(off + n - p, b.size() - p % BS);
        r.insert(r.end(), b.begin() + p % BS, b.begin() + p % BS + take);
        p += take;
    }
    return r;
}

static void write_file(const char* p, size_t n, uint8_t salt, time_t mtime)
{
    FILE* f = fopen(p, "wb");
    for (size_t i = 0; i < n; i++) fputc((uint8_t)((i * 131 + (i >> 8) * 7 + salt * 29) & 0xFF), f);
    fclose(f);
    struct utimbuf u = { mtime, mtime }; utime(p, &u);
}

static void run_case(const char* dir, long delta)
{
    const size_t N = 3095022 / 8;   /* 386,877 bytes: the lump scaled down, the tail block is partial */
    char a[1200], b[1200];
    snprintf(a, sizeof a, "%s/disc.core", dir); snprintf(b, sizeof b, "%s/patched.core", dir);
    write_file(a, N, 1, 1700000000);
    write_file(b, N + delta, 2, 1700000900);   /* the .pspatch output: other bytes, other size, later date */
    Cache cache;
    snprintf(g_served, sizeof g_served, "%s", a);
    cache_open(cache);                                   /* boot: the SP preload reads the disc entry */
    const Times t0 = guest_stat();
    snprintf(g_served, sizeof g_served, "%s", b);        /* the game flips SelectPatchSetMP */
    const Times t1 = guest_stat();
    CHECK(t0.mtime == 1700000000 && t1.mtime == 1700000900, "case %+ld: cellFsStat reports the host mtime (%llu, %llu)", delta,
          (unsigned long long)t0.mtime, (unsigned long long)t1.mtime);
    CHECK(t1.size == N + delta, "case %+ld: cellFsStat size is the patched file's (%llu)", delta, (unsigned long long)t1.size);
    /* fstat reports the same times as stat */
    strcpy((char*)vm_base + PATH, G); call(cellFsOpen, PATH, 0, FDP);
    const uint32_t fd = rd32(FDP); call(cellFsFstat, fd, STATB);
    CHECK(rd64(STATB + 0x14) == t1.mtime && rd64(STATB + 0x1C) == t1.ctime && rd64(STATB + 0x24) == t1.size,
          "case %+ld: cellFsFstat agrees with cellFsStat (mtime %llu)", delta, (unsigned long long)rd64(STATB + 0x14));
    call(cellFsClose, fd);
    /* the object pass: open (stat + validate), then the skip's refill of the tail at an unaligned offset */
    cache_open(cache);
    const uint64_t total = t1.size, off = total - (total % BS) + 0 - BS / 2 + 5, len = total - off;
    std::vector<uint8_t> want, got = cache_read(cache, off, len);
    host_read(off, len, want);
    CHECK(want.size() == len && got.size() == len && got == want,
          "case %+ld: tail refill [%llu, %llu) of the patched entry matches the file (cache returned %zu of %llu bytes%s)", delta,
          (unsigned long long)off, (unsigned long long)total, got.size(), (unsigned long long)len,
          got == want ? "" : (got.size() < len ? ", short" : ", stale bytes"));
    std::vector<uint8_t> whole = cache_read(cache, 0, total), wf; host_read(0, total, wf);
    CHECK(whole == wf, "case %+ld: the whole patched entry reads back through the cache unchanged", delta);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t mem[0x80000];
    vm_base = mem; ppu_vm_size = sizeof mem;
    const char* dir = argc > 1 ? argv[1] : (getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    char root[900]; snprintf(root, sizeof root, "%s/kz3_fs_stat_times", dir);
    mkdir(root, 0777);
    ppu_vfs_root = root;
    run_case(root, +60);   /* defaultshared: 3,095,022 -> 3,095,082 */
    run_case(root, -40);   /* a patched entry smaller than its base */
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

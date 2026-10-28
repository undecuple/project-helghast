/*
 * KZ3 port, network pass 41: the FIOS disk cache belongs to the disc contents that built it (the RE notes s.35).
 *
 * FIOS (the game's file scheduler) keeps a disk cache of disc reads in /dev_hdd1/cache/BCUS98234/fios/cache.idx +
 * cache.dat. Each block descriptor in cache.idx records the file's mtime (ns) and size (seen in a run's cache.idx:
 * 512-byte entries, u64 at +8/+0x10 = mtime, u32 at +0x20 = size, 16-byte key at +0x28). On a PS3 the disc never
 * changes under a cache. The port does change disc files: it rewrites menu pages (kz3_host_composed.h) and generates
 * others, and a new port build (or other KZ3_ switches) serves different bytes under the same /dev_bdvd path. FIOS then
 * fails the read: the GUM request for that page fails (listener 0x00109054 on 1.14), the game stores GUM error 20000
 * and shows connect/networkerror.gum (players' "error 20000" on Options > Wireless Controller / Display, 0.1.3).
 * Reproduced under Proton (network pass 41, runs net41-fios: one run with KZ3_KB_PAGE=0 built the cache with the disc's
 * own pages, the next with the port's pages failed mainmenu.gum with 20000 at once).
 *
 * The fix keeps FIOS's contract: before the first open of cache.idx / cache.dat in a process (patch 4024's pre-open
 * hook), the stamp of what serves the disc files (a hash of this exe's bytes, the KZ3_ switches that change served
 * files) is compared with the one stored next to the cache (kz3_cache_stamp.txt). When it differs, or the cache has
 * none (built before this fix), cache.idx and cache.dat are removed, as FIOS itself removes a cache it rejects (patch
 * 4018, v1.00 0x00DA18E0), so FIOS builds a new one; the new stamp is written. Same stamp = the cache stays.
 * Not in the stamp: mtimes. FIOS keeps a cache whose files only got a new mtime (the port rewrites its pages at every
 * start; run net41-fios with the same exe and KZ3_FIOS_STAMP=0 read them from the kept cache with no error).
 * ASSUMPTION: a player who changes the 1.14 update data under the same exe also changes the rewritten pages (their
 * source is update + rows); that case is not in the stamp (rare: the update is installed once).
 * KZ3_FIOS_STAMP=0 turns the check off (tests and A/B of the old behaviour only).
 */
#ifndef KZ3_FIOS_STAMP_H
#define KZ3_FIOS_STAMP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifndef KZ3_NET_PRINTF
#define KZ3_NET_PRINTF printf
#endif

/* Switches that change the bytes the port serves under a disc path (pages added/rewritten, patch composition). */
static const char* const kz3_fs_stamp_env[] = {
    "KZ3_KB_PAGE", "KZ3_KB_PAGE_FILE", "KZ3_KB_PAGE_TEST", "KZ3_TEST_ONLINE_COOP", "KZ3_NATIVE_PATCH",
    "KZ3_EBOOT_VER", "KZ3_PADPAGE_LINK_ROW", "KZ3_PADSET_DESC_Y", "KZ3_OVERLAY_SELFKEY",
};
#define KZ3_FS_STAMP_FILE "kz3_cache_stamp.txt"
#define KZ3_FS_STAMP_MAX 2048

static const char* s_fs_stamp_exe;   /* test override of the exe path (NULL = this process's exe) */
static int s_fs_stamp_done;          /* the first cache open of the process has been handled */

/* 64-bit hash of a whole file (8-byte words, multiply-xor; change detection only, not security), 0 = not readable. */
static unsigned long long kz3_fs_stamp_hash_file(const char* path, long long* size_out)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    static unsigned char buf[1 << 20];
    unsigned long long h = 0x6A09E667F3BCC908ull, total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            unsigned long long w;
            memcpy(&w, buf + i, 8);
            h = (h ^ w) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
        }
        for (; i < n; i++) { h = (h ^ buf[i]) * 0x100000001B3ull; }
        total += n;
    }
    fclose(f);
    h ^= total;
    *size_out = (long long)total;
    return h ? h : 1;
}

/* The stamp text of this process: a hash of the exe's bytes (the code that composes the served pages; a copy of the
 * same exe, as each test run makes, gives the same stamp), then each switch above as NAME=value (unset ones left
 * out). memo = "<dir>/kz3_exe_hash.txt" (or NULL): "size mtime hash" of the last hashed exe, so an unchanged exe is not
 * read again (about 0.1 s for the 340 MB installer build, 0.5 s for a 1.7 GB debug-info build, once). */
static int kz3_fs_stamp_text_memo(char* out, size_t cap, const char* memo)
{
    char exe[1024] = "";
    if (s_fs_stamp_exe) snprintf(exe, sizeof exe, "%s", s_fs_stamp_exe);
    else {
#ifdef _WIN32
        DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
        if (n == 0 || n >= sizeof exe) exe[0] = 0;
#else
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        exe[n > 0 ? (size_t)n : 0] = 0;
#endif
    }
    struct stat st;
    if (!exe[0] || stat(exe, &st) != 0) return 0;
    unsigned long long h = 0;
    long long sz = 0;
    if (memo) {
        FILE* m = fopen(memo, "rb");
        if (m) {
            long long ms = -1, mt = -1; unsigned long long mh = 0;
            if (fscanf(m, "%lld %lld %llx", &ms, &mt, &mh) == 3 && ms == (long long)st.st_size &&
                mt == (long long)st.st_mtime && mh)
                { h = mh; sz = ms; }
            fclose(m);
        }
    }
    if (!h) {
        h = kz3_fs_stamp_hash_file(exe, &sz);
        if (!h) return 0;
        FILE* m = memo ? fopen(memo, "wb") : NULL;
        if (m) { fprintf(m, "%lld %lld %llx\n", (long long)st.st_size, (long long)st.st_mtime, h); fclose(m); }
    }
    int k = snprintf(out, cap, "kz3 fios cache stamp 2\nexe %lld %016llx\n", sz, h);
    for (size_t i = 0; i < sizeof kz3_fs_stamp_env / sizeof kz3_fs_stamp_env[0] && k > 0 && (size_t)k < cap; i++) {
        const char* v = getenv(kz3_fs_stamp_env[i]);
        if (v) k += snprintf(out + k, cap - (size_t)k, "%s=%.200s\n", kz3_fs_stamp_env[i], v);
    }
    return k > 0 && (size_t)k < cap;
}
static int kz3_fs_stamp_text(char* out, size_t cap) { return kz3_fs_stamp_text_memo(out, cap, NULL); }

/* 1 when guest path p is FIOS's cache index or data on /dev_hdd1 (".../fios/cache.idx" or ".../fios/cache.dat"). */
static int kz3_fs_stamp_is_cache(const char* p)
{
    size_t n = p ? strlen(p) : 0;
    if (n < 25 || strncmp(p, "/dev_hdd1/", 10)) return 0;
    return !strcmp(p + n - 15, "/fios/cache.idx") || !strcmp(p + n - 15, "/fios/cache.dat");
}

/* Patch 4024's pre-open hook. On the process's first open of the cache: compare stamps, remove a stale cache. */
static void kz3_fs_stamp_preopen(const char* gpath, const char* hpath)
{
    if (s_fs_stamp_done || !kz3_fs_stamp_is_cache(gpath) || !hpath) return;
    s_fs_stamp_done = 1;
    const char* e = getenv("KZ3_FIOS_STAMP");
    if (e && !strcmp(e, "0")) { KZ3_NET_PRINTF("[fios] KZ3_FIOS_STAMP=0: cache kept unchecked\n"); fflush(stdout); return; }
    size_t hn = strlen(hpath);
    if (hn < 9 || hn >= 1000) return;
    char dir[1000], idx[1100], dat[1100], stp[1100], memo[1100];
    memcpy(dir, hpath, hn - 9);                       /* strip "cache.idx" / "cache.dat" (same length) */
    dir[hn - 9] = 0;
    snprintf(idx, sizeof idx, "%scache.idx", dir);
    snprintf(dat, sizeof dat, "%scache.dat", dir);
    snprintf(stp, sizeof stp, "%s" KZ3_FS_STAMP_FILE, dir);
    snprintf(memo, sizeof memo, "%skz3_exe_hash.txt", dir);
    char want[KZ3_FS_STAMP_MAX], have[KZ3_FS_STAMP_MAX];
    if (!kz3_fs_stamp_text_memo(want, sizeof want, memo)) {
        KZ3_NET_PRINTF("[fios] no stamp for this exe (not readable): cache kept unchecked\n");
        fflush(stdout);
        return;
    }
    size_t hl = 0;
    FILE* f = fopen(stp, "rb");
    if (f) { hl = fread(have, 1, sizeof have - 1, f); fclose(f); }
    have[hl] = 0;
    struct stat st;
    const int cache = stat(idx, &st) == 0 || stat(dat, &st) == 0;
    if (!strcmp(have, want)) return;                  /* the cache was built by what serves the disc now */
    if (cache) {
        const int ri = remove(idx), rd = remove(dat);
        KZ3_NET_PRINTF("[fios] disk cache built by %s: removed (cache.idx %s, cache.dat %s), FIOS builds a new one\n",
                       f ? "another port build or other KZ3_ switches" : "a build without a cache stamp",
                       ri == 0 ? "removed" : "absent", rd == 0 ? "removed" : "absent");
    } else {
        KZ3_NET_PRINTF("[fios] new disk cache: stamp written\n");
    }
    fflush(stdout);
    FILE* o = fopen(stp, "wb");
    if (o) { fwrite(want, 1, strlen(want), o); fclose(o); }
    else { KZ3_NET_PRINTF("[fios] stamp NOT written (%s)\n", stp); fflush(stdout); }
}

/* Install at load time: FIOS opens its cache about 170 ms after start, long before the first Localizer lookup that
 * installs the other net hooks. */
#ifdef __cplusplus
extern "C" void (*g_ps3_fs_preopen_hook)(const char*, const char*);   /* patch 4024 */
#else
extern void (*g_ps3_fs_preopen_hook)(const char*, const char*);
#endif
__attribute__((constructor)) static void kz3_fs_stamp_install(void) { g_ps3_fs_preopen_hook = kz3_fs_stamp_preopen; }

#endif /* KZ3_FIOS_STAMP_H */

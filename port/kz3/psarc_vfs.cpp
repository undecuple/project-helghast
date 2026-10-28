/* Killzone 3 port: serve data_ps3.psarc through the host file layer.
 *
 * KZ3 opens the archive and reads its TOC itself, but FIOS's archive lookup does
 * not resolve names in the port, so every asset open falls through to a loose
 * path under USRDIR. ps3recomp calls g_ps3_vfs_miss_hook (patch 0004) for every
 * host path that misses; this answers it from the archive:
 *
 *   - the part after the last "USRDIR/" is looked up case-insensitively in the
 *     archive's manifest (entry 0; KZ3 asks for the same file in several cases);
 *   - a hit is inflated once into <vfs root>/psarc-cache/<archive name> and the
 *     path is rewritten to it, so open/read/seek/stat/fstat all work unchanged;
 *   - a directory that exists only inside the archive is created empty in the
 *     cache so stat/opendir succeed (its listing stays empty: ASSUMPTION that KZ3
 *     does not enumerate archive directories; logged so it shows if it does).
 *
 * ENHANCEMENT: replaces FIOS's dearchiver for reads. KZ3_PSARC_VFS=0 disables it.
 *
 * PSARC v1.4 (same layout as tools/psarc.py): 32-byte header 'PSAR', u16 major,
 * u16 minor, char[4] compression, u32 toc_len, u32 entry_size, u32 count,
 * u32 block_size, u32 flags; entries md5[16] u32 first_block u40 size u40 offset;
 * then the compressed block size table, each ceil(log256(block_size)) bytes;
 * a stored size of 0 means a full block. Blocks are zlib (with header) unless
 * their stored size equals the bytes wanted (stored raw). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#define kz3_mkdir(p) _mkdir(p)
#define kz3_fseek _fseeki64
#else
#include <sys/types.h>
#define kz3_mkdir(p) mkdir(p, 0755)
#define kz3_fseek fseeko
#endif

extern "C" void (*g_ps3_vfs_miss_hook)(char* path, size_t cap);   /* runtime/syscalls/sys_fs.c (patch 0004) */
extern "C" int (*g_ps3_fs_stat_hook)(const char* guest_path);     /* runtime/ppu/ppu_fs.cpp (patch 3008) */
extern "C" int stbi_zlib_decode_buffer(char* obuffer, int olen, const char* ibuffer, int ilen); /* stb_image, in the runtime */
#include "kz3_splitfile.h"                               /* port/vfs: data_ps3.psarc.666NN parts read as one file (issue 4) */
#include "kz3_overlay.h"                                   /* port/vfs: 1.14 update layers */
extern "C" const char* edat_resolve(const char* host_path, char* buf, size_t cap);   /* libs/filesystem/edat.c */
extern "C" void edat_set_cache_dir(const char* dir);                                 /* patch 5002 */
extern "C" void edat_add_klicensee(const uint8_t klic[16]);                          /* patch 5001 */
extern "C" uint8_t* vm_base;                                                          /* guest memory (main.cpp) */
static bool g_overlay_on = false;
/* KZ3_NATIVE_PATCH (default on since 2026-10-27; =0 restores host patching; pass 165, the RE notes s.48): the game's
 * own FIOS patch layer composes the .pspatch files (as on the PS3), so the guest's patch directories are served from the
 * update and the host overlay applies no patches, except to the menu files the port rewrites (net 38). */
static bool g_native_patch = false;
static std::string g_native_root;   /* <cache>/overlay/native: the update's patch dirs under the loose-path keys (s.48.6) */
static bool g_set_pinned = false;   /* KZ3_PATCH_SET given: the game's patch set switch is logged but not followed */

/* The game's patch set selectors (1.14 SelectPatchSetSP 0x284A0 / SelectPatchSetMP 0x284D8, overrides in
 * port/spu/kz3_ppu_overrides.cpp): flip the overlay's directory at the same moment the game flips its FIOS patch
 * directory, the RE notes s.33/s.37. set: 0 = sp, 1 = mp. lr = the game's caller, for the log. */
extern "C" void kz3_vfs_follow_patch_set(int set, unsigned lr)
{
    if (!g_overlay_on) return;
    if (g_set_pinned) {
        static int s_logged = 0;
        if (s_logged++ < 4) fprintf(stderr, "[overlay] game selects patch set %s (lr 0x%08X): KZ3_PATCH_SET pins the run, kept\n",
                                    set ? "mp" : "sp", lr);
        return;
    }
    kz3_overlay_set_patch_set(set, lr);
}
#ifndef KZ3_EBOOT_VER_DEFAULT
#define KZ3_EBOOT_VER_DEFAULT 114                          /* CMake always defines it: the lift's version (stamp) */
#endif

namespace {

struct Entry { uint32_t block; uint64_t size, offset; };

struct Psarc {
    kz3_sf* f = nullptr;
    uint32_t block_size = 0;
    std::vector<Entry> entries;
    std::vector<uint32_t> blocks;
    std::vector<std::string> names;                        /* names[i] for entries[i]; names[0] = "" (manifest) */
    std::unordered_map<std::string, size_t> by_key;        /* lowercased name -> index */
    std::unordered_set<std::string> dirs;                  /* lowercased directory prefixes */
    std::string cache;                                     /* host dir holding inflated files */
    std::mutex lock;
};

Psarc g_arc;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t be40(const uint8_t* p) { return (uint64_t)p[0] << 32 | be32(p + 1); }

std::string lower(std::string s)
{
    for (char& c : s) { if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); }
    return s;
}

/* Inflate entry `idx` into memory. Caller holds g_arc.lock (the FILE* is shared). */
bool read_entry(size_t idx, std::vector<uint8_t>& out)
{
    const Entry& e = g_arc.entries[idx];
    out.clear();
    out.reserve((size_t)e.size);
    if (kz3_sf_seek(g_arc.f, (int64_t)e.offset) != 0) return false;
    std::vector<uint8_t> in(g_arc.block_size);
    std::vector<uint8_t> blk(g_arc.block_size);
    uint32_t b = e.block;
    while (out.size() < e.size) {
        if (b >= g_arc.blocks.size()) return false;
        uint32_t csize = g_arc.blocks[b] ? g_arc.blocks[b] : g_arc.block_size;
        uint64_t want = e.size - out.size();
        if (want > g_arc.block_size) want = g_arc.block_size;
        if (csize > in.size()) in.resize(csize);
        if (kz3_sf_read(g_arc.f, in.data(), csize) != csize) return false;
        if (csize == want) {
            out.insert(out.end(), in.begin(), in.begin() + csize);              /* stored */
        } else {
            int n = stbi_zlib_decode_buffer((char*)blk.data(), (int)want, (const char*)in.data(), (int)csize);
            if (n < 0) out.insert(out.end(), in.begin(), in.begin() + csize);   /* raw short block (as psarc.py) */
            else out.insert(out.end(), blk.begin(), blk.begin() + n);
        }
        b++;
    }
    out.resize((size_t)e.size);
    return true;
}

void mkdirs(const std::string& path)   /* every parent of `path` (a file path) */
{
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') kz3_mkdir(path.substr(0, i).c_str());
}

/* "./PS3_GAME/USRDIR/LOCALCACHEPS3/x" or ".\USRDIR\x" -> "localcacheps3/x" (lowercased, after the last USRDIR/). */
bool archive_key(const char* path, std::string& key)
{
    std::string p = lower(path);
    size_t u = p.rfind("usrdir/");
    if (u == std::string::npos) return false;
    key = p.substr(u + 7);
    while (!key.empty() && key[0] == '/') key.erase(0, 1);
    while (!key.empty() && key.back() == '/') key.pop_back();
    return !key.empty();
}

/* "<root>/PS3_GAME/USRDIR/patch_data_ps3_mp/ABC.pspatch" -> "<cache>/overlay/native/patch_data_ps3_mp/ABC.pspatch" (case
 * kept: the game builds the hex name upper-case). On the PS3 the installed update merges into the disc's USRDIR, which is
 * how FIOS's patch layer reaches the directory (it opendir()s /dev_bdvd/PS3_GAME/USRDIR/patch_data_ps3_sp|mp). The
 * directory served is the update's, renamed by kz3_overlay_native_rekey: with the archive kept from FIOS the game
 * hashes "/<path>" instead of "/data_ps3/<path>" (1.14 path translator @0x0002c7d4), the RE notes s.48.6. */
bool native_patch_path(char* path, size_t cap)
{
    std::string p = lower(path);
    size_t u = p.rfind("usrdir/patch_data_ps3_");
    if (u == std::string::npos) return false;
    const size_t t = u + 7;
    const size_t n = strlen("patch_data_ps3_xx");
    if (p.compare(t + n - 2, 2, "sp") != 0 && p.compare(t + n - 2, 2, "mp") != 0) return false;
    if (p.size() > t + n && p[t + n] != '/') return false;
    const std::string tail(path + t);   /* copied first: snprintf into `path` while reading `path + t` overlaps (bz7 run 2026-10-27: mangled dir) */
    snprintf(path, cap, "%s/%s", g_native_root.c_str(), tail.c_str());
    return true;
}

void miss_hook(char* path, size_t cap)
{
    std::string key;
    if (!archive_key(path, key)) return;
    static int s_logged = 0;
    if (g_native_patch && native_patch_path(path, cap)) {
        if (s_logged++ < 64) fprintf(stderr, "[overlay] native patch dir %s\n", path);
        return;
    }
    /* 1.14 update first (the RE notes 8.5/10): an added_data_ps3.psarc entry, or the disc entry with its
     * .pspatch applied. The game's own lookup for both sits in FIOS's archive path, which the port bypasses,
     * so this is the only place the update is applied. KZ3_OVL_NONE / ERROR fall through to the disc archive. */
    if (g_overlay_on) {
        char out[1100];
        int r = kz3_overlay_resolve(key.c_str(), out, sizeof out);
        if (r == KZ3_OVL_ADDED || r == KZ3_OVL_PATCHED || r == KZ3_OVL_DIR) {
            if (s_logged++ < 64) fprintf(stderr, "[overlay] %s %s\n", r == KZ3_OVL_PATCHED ? "patched" : r == KZ3_OVL_ADDED ? "added" : "dir", key.c_str());
            snprintf(path, cap, "%s", out);
            return;
        }
        if (r == KZ3_OVL_ERROR) fprintf(stderr, "[overlay] %s failed, serving the disc entry\n", key.c_str());
    }
    auto it = g_arc.by_key.find(key);
    if (it == g_arc.by_key.end()) {
        if (g_arc.dirs.count(key)) {
            std::string dir = g_arc.cache + "/" + key;
            mkdirs(dir + "/");
            if (s_logged++ < 64) fprintf(stderr, "[psarc] dir %s (served empty)\n", key.c_str());
            snprintf(path, cap, "%s", dir.c_str());
        }
        return;
    }
    std::string host = g_arc.cache + "/" + g_arc.names[it->second];
    struct stat st;
    std::lock_guard<std::mutex> g(g_arc.lock);
    if (stat(host.c_str(), &st) != 0) {
        std::vector<uint8_t> data;
        if (!read_entry(it->second, data)) {
            fprintf(stderr, "[psarc] read FAILED for %s\n", g_arc.names[it->second].c_str());
            return;
        }
        mkdirs(host);
        std::string tmp = host + ".part";
        FILE* o = fopen(tmp.c_str(), "wb");
        if (!o) { fprintf(stderr, "[psarc] cannot write %s\n", tmp.c_str()); return; }
        bool ok = fwrite(data.data(), 1, data.size(), o) == data.size();
        ok = (fclose(o) == 0) && ok;
        if (!ok || rename(tmp.c_str(), host.c_str()) != 0) { remove(tmp.c_str()); return; }
        if (s_logged++ < 64) fprintf(stderr, "[psarc] %s (%zu bytes)\n", g_arc.names[it->second].c_str(), data.size());
    }
    snprintf(path, cap, "%s", host.c_str());
}

}  // namespace

/* cellFsStat on the archive answers ENOENT while this VFS serves it. FIOS mounts the archive only when that stat
 * succeeds; when it fails, FIOS opens every asset by its loose path, which miss_hook answers. Before patch 3007
 * the stat failed by accident (32-bit st_size: MinGW's stat wrapper returns EOVERFLOW for the 9.0 GiB archive),
 * which is the "why FIOS's lookup fails" of ; a UCRT build (llvm-mingw) truncated the size instead, FIOS took
 * the archive and hung at the Edge zlib push (_cellSpursLFQueuePushBody, not implemented). This keeps the
 * path on purpose, the same with any compiler or CRT. KZ3_PSARC_VFS=0 lets FIOS have the archive. */
static std::string g_arc_base;                                    /* lowercased archive file name, e.g. data_ps3.psarc */
static int stat_hook(const char* guest_path)
{
    const char* b = strrchr(guest_path, '/');
    if (lower(b ? b + 1 : guest_path) != g_arc_base) return 0;
    static std::atomic<int> once{0};
    if (!once.fetch_add(1))
        fprintf(stderr, "[psarc] cellFsStat('%s') -> ENOENT: FIOS stays off the archive, the VFS serves it\n", guest_path);
    return (int)0x80010006;                                /* CELL_FS_ENOENT */
}

/* The disc's TITLE_ID from PS3_GAME/PARAM.SFO next to the archive (<root>/PS3_GAME/USRDIR/<archive>): the update lives
 * in dev_hdd0/game/<title id> (US BCUS98234, EU BCES01007 as an untested region). The same field main.cpp hands to
 * cellGame_init_from_paramsfo a few lines later. ASSUMPTION: an unreadable SFO means the US disc, the only tested one. */
static std::string disc_title_id(const char* archive)
{
    std::string def = "BCUS98234", p = archive ? archive : "";
    for (char& c : p) if (c == '\\') c = '/';
    size_t u = p.rfind("/USRDIR/");
    if (u == std::string::npos) return def;
    FILE* f = fopen((p.substr(0, u) + "/PARAM.SFO").c_str(), "rb");
    if (!f) return def;
    std::vector<unsigned char> d(65536);
    d.resize(fread(d.data(), 1, d.size(), f));
    fclose(f);
    auto u16 = [&](size_t o) { return o + 2 <= d.size() ? (unsigned)(d[o] | d[o + 1] << 8) : 0u; };
    auto u32 = [&](size_t o) { return o + 4 <= d.size() ? (unsigned)(u16(o) | u16(o + 2) << 16) : 0u; };
    if (d.size() < 20 || memcmp(d.data(), "\0PSF", 4) != 0) return def;
    const size_t kt = u32(8), dt = u32(12), n = u32(16);
    for (size_t i = 0; i < n && 20 + 16 * (i + 1) <= d.size(); i++) {
        const size_t e = 20 + 16 * i, ko = kt + u16(e), len = u32(e + 4), dof = dt + u32(e + 12);
        if (ko + 9 > d.size() || memcmp(&d[ko], "TITLE_ID", 9) != 0 || dof + len > d.size()) continue;
        std::string id;
        for (size_t k = 0; k < len && d[dof + k]; k++) id += (char)d[dof + k];
        return id.empty() ? def : id;
    }
    return def;
}

/* Open the archive, load its manifest and install the hook. Returns the number of files, or -1. */
extern "C" int kz3_psarc_vfs_init(const char* archive, const char* cache_dir)
{
    const char* off = getenv("KZ3_PSARC_VFS");
    if (off && off[0] == '0') { fprintf(stderr, "[psarc] VFS disabled (KZ3_PSARC_VFS=0)\n"); return -1; }
    kz3_sf* f = kz3_sf_open(archive);                     /* the file, or its FAT32 parts .66600... (issue 4) */
    if (!f) { fprintf(stderr, "[psarc] cannot open %s (nor %s.66600)\n", archive, archive); return -1; }
    if (f->nparts > 1) fprintf(stderr, "[psarc] %s: %d split parts, %llu bytes\n", archive, f->nparts, (unsigned long long)kz3_sf_size(f));
    uint8_t h[32];
    if (kz3_sf_read(f, h, 32) != 32 || memcmp(h, "PSAR", 4) != 0 || memcmp(h + 8, "zlib", 4) != 0 || (be32(h + 28) & 4)) {
        fprintf(stderr, "[psarc] %s: not a zlib PSARC with a plain TOC\n", archive); kz3_sf_close(f); return -1;
    }
    uint32_t toc_len = be32(h + 12), esz = be32(h + 16), count = be32(h + 20);
    g_arc.block_size = be32(h + 24);
    std::vector<uint8_t> toc(toc_len - 32);
    if (esz < 30 || kz3_sf_read(f, toc.data(), toc.size()) != toc.size() || (uint64_t)count * esz > toc.size()) {
        fprintf(stderr, "[psarc] %s: bad TOC\n", archive); kz3_sf_close(f); return -1;
    }
    g_arc.f = f;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = toc.data() + (size_t)i * esz;
        g_arc.entries.push_back({be32(e + 16), be40(e + 20), be40(e + 25)});
    }
    unsigned w = 1;
    while ((1ull << (8 * w)) < g_arc.block_size) w++;
    for (size_t j = (size_t)count * esz; j + w <= toc.size(); j += w) {
        uint32_t v = 0;
        for (unsigned k = 0; k < w; k++) v = v << 8 | toc[j + k];
        g_arc.blocks.push_back(v);
    }
    std::vector<uint8_t> man;
    if (!read_entry(0, man)) { fprintf(stderr, "[psarc] cannot read the manifest\n"); return -1; }
    g_arc.names.assign(1, std::string());
    std::string cur;
    for (uint8_t c : man) {
        if (c == '\n') { g_arc.names.push_back(cur); cur.clear(); } else cur.push_back((char)c);
    }
    g_arc.names.push_back(cur);
    g_arc.names.resize(count);
    for (size_t i = 1; i < count; i++) {
        std::string& n = g_arc.names[i];
        while (!n.empty() && n[0] == '/') n.erase(0, 1);
        std::string k = lower(n);
        g_arc.by_key.emplace(k, i);                        /* first spelling wins for duplicates */
        for (size_t s = k.find('/'); s != std::string::npos; s = k.find('/', s + 1)) g_arc.dirs.insert(k.substr(0, s));
    }
    g_arc.cache = cache_dir;
    for (char& c : g_arc.cache) if (c == '\\') c = '/';
    kz3_mkdir(g_arc.cache.c_str());
    g_ps3_vfs_miss_hook = miss_hook;
    /* Patch set (KZ3_PATCH_SET, the RE notes s.36.12): none = disc archive only (the 1.00 EBOOT), sp / mp = the
     * 1.14 update layered over it (added_data_ps3.psarc.edat + patch_data_ps3_<set>). Unset = follow the EBOOT
     * version: KZ3_EBOOT_VER at run time, else the version this exe was built for (KZ3_EBOOT_VER_DEFAULT, CMake):
     * 114 -> sp, 100 -> none. 1.14 picks sp for the campaign (startup 0x2D5C0 -> 0x284A0) and mp for multiplayer
     * (0x284D8); serving mp in the campaign breaks defaultshared's links (s.30), so a run serves one set until the
     * two setters are hooked (s.37). KZ3_OVERLAY=0 disables the update whatever the set. */
    const char* ev = getenv("KZ3_EBOOT_VER");
    const int ever = ev && *ev ? atoi(ev) : KZ3_EBOOT_VER_DEFAULT;
    const char* pset = getenv("KZ3_PATCH_SET");
    std::string pset_s = pset && *pset ? lower(pset) : std::string(ever == 114 ? "sp" : "none");
    if (pset_s != "sp" && pset_s != "mp" && pset_s != "none") {
        fprintf(stderr, "[overlay] KZ3_PATCH_SET=%s is not sp|mp|none: using %s\n", pset, ever == 114 ? "sp" : "none");
        pset_s = ever == 114 ? "sp" : "none";
    }
    const bool mp = pset_s == "mp";
    /* KZ3_PATCH_SET set in the environment pins the set for tests: the game's own switch is then logged, not followed. */
    g_set_pinned = pset && *pset && pset_s != "none";
    const char* ov_off = getenv("KZ3_OVERLAY");
    const char* upd = getenv("KZ3_UPDATE_DIR");
    /* static: kz3_overlay_init keeps these pointers (g_cfg = *cfg), so they must outlive this function. */
    static std::string updir, added, pdir, pdir_sp, pdir_mp, ocache;
    updir = upd ? std::string(upd) : g_arc.cache + "/../dev_hdd0/game/" + disc_title_id(archive) + "/USRDIR";
    added = updir + "/added_data_ps3.psarc.edat";
    pdir_sp = updir + "/patch_data_ps3_sp";
    pdir_mp = updir + "/patch_data_ps3_mp";
    pdir = mp ? pdir_mp : pdir_sp;
    ocache = g_arc.cache + "/overlay";
    fprintf(stderr, "[overlay] patch set %s (EBOOT %d)\n", pset_s.c_str(), ever);
    struct stat ust;
    if (pset_s == "none") {
        /* disc data only */
    } else if (ov_off && ov_off[0] == '0') {
        fprintf(stderr, "[overlay] disabled (KZ3_OVERLAY=0)\n");
    } else if (stat(updir.c_str(), &ust) != 0) {
        fprintf(stderr, "[overlay] no update at %s: disc data only\n", updir.c_str());
    } else {
        /* Decrypted EDATs (the update archive) go to a writable cache, not beside the install (patch 5002). */
        std::string edc = g_arc.cache + "/edat";
        kz3_mkdir(edc.c_str());
        edat_set_cache_dir(edc.c_str());
        kz3_mkdir(ocache.c_str());
        kz3_overlay_config cfg = {};
        cfg.disc_psarc = archive;
        cfg.added_psarc = stat(added.c_str(), &ust) == 0 ? added.c_str() : nullptr;
        cfg.patch_dir_sp = stat(pdir_sp.c_str(), &ust) == 0 ? pdir_sp.c_str() : nullptr;
        cfg.patch_dir_mp = stat(pdir_mp.c_str(), &ust) == 0 ? pdir_mp.c_str() : nullptr;
        cfg.patch_dir = mp ? cfg.patch_dir_mp : cfg.patch_dir_sp;
        cfg.cache_dir = ocache.c_str();
        const char* nat = getenv("KZ3_NATIVE_PATCH");
        g_native_patch = !(nat && nat[0] == '0') && ever == 114;   /* default on: Botzone needs it (bz10/bz11, 2026-10-27) */
        if (g_native_patch) {
            /* The guest's patch layer keys each .pspatch by the path string it receives; under that is "/<path>",
             * so it needs the update's files under those names (s.48.6). Built once per update, then a size check. */
            g_native_root = ocache + "/native";
            std::vector<const char*> nv;
            nv.reserve(g_arc.names.size());
            for (const std::string& n : g_arc.names) nv.push_back(n.c_str());
            const struct { const char* src; const char* sub; } sets[] = {
                { stat(pdir_sp.c_str(), &ust) == 0 ? pdir_sp.c_str() : nullptr, "patch_data_ps3_sp" },
                { stat(pdir_mp.c_str(), &ust) == 0 ? pdir_mp.c_str() : nullptr, "patch_data_ps3_mp" } };
            for (const auto& st : sets) {
                if (!st.src) continue;
                const std::string dst = g_native_root + "/" + st.sub;
                int ren = 0;
                const int placed = kz3_overlay_native_rekey(st.src, dst.c_str(), nv.data(), nv.size(), &ren);
                fprintf(stderr, "[overlay] native %s: %d patches, %d under the loose-path key\n", st.sub, placed, ren);
                if (placed < 0) g_native_patch = false;
            }
            if (!g_native_patch) fprintf(stderr, "[overlay] KZ3_NATIVE_PATCH=1 but the patch dirs could not be prepared: host patching stays on\n");
        }
        cfg.native_patch = g_native_patch ? 1 : 0;
        if (g_native_patch) fprintf(stderr, "[overlay] KZ3_NATIVE_PATCH=1: FIOS's patch layer composes the .pspatch files, the host applies none\n");
        cfg.inflate = stbi_zlib_decode_buffer;
        cfg.edat_resolve = edat_resolve;
        g_overlay_on = kz3_overlay_init(&cfg) == 0;
        /* The game registers the update archive's key only from FIOS's media executor (1.14 0xE1AB2C), which the port
         * bypasses; take it from the loaded guest image instead, verified against the EDAT's dev_hash first
         * (the RE notes section 16). The ELF is already in guest memory (ppu_load_elf runs before this). */
        if (g_overlay_on && vm_base) kz3_overlay_selfkey(vm_base + KZ3_SELFKEY_VA, edat_add_klicensee);
        fprintf(stderr, "[overlay] %s: added %s, patches %s -> %s\n", updir.c_str(), cfg.added_psarc ? "yes" : "no",
                cfg.patch_dir ? "yes" : "no", g_overlay_on ? "on" : "init FAILED");
    }
    {
        const char* s = archive + strlen(archive);
        while (s > archive && s[-1] != '/' && s[-1] != '\\') s--;
        g_arc_base = lower(s);
        g_ps3_fs_stat_hook = stat_hook;
    }
    fprintf(stderr, "[psarc] %s: %u entries (%zu names, %zu dirs), cache %s\n", archive, count,
            g_arc.by_key.size(), g_arc.dirs.size(), g_arc.cache.c_str());
    return (int)count;
}

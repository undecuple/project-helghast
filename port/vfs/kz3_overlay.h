/* Killzone 3 port: 1.14 update overlay for the PSARC VFS (RE pass 36; design in
 * the RE notes sections 8.5 and 10).
 *
 * Lookup order for a game path below USRDIR (case-insensitive):
 *   (a) an entry of added_data_ps3.psarc (the update's new/overriding files; the archive
 *       ships as an NPD EDAT, decrypted through edat_resolve, patches 5001/5002);
 *   (b) a disc data_ps3.psarc entry with a .pspatch: the patch file name is
 *       MD5(upper("/data_ps3/" + path)) as 32 upper-case hex digits + ".pspatch"
 *       (traced in the 1.14 EBOOT: 0xe0e794 -> MD5 0xe21fe8 with upper-casing, the key is
 *       bsearched at 0xe0c490 and printed by 0xe0aae0 with "%s%016llX%016llX.pspatch").
 *       The patch is applied on first open; the output is cached as
 *       <cache>/pspatch/<base TOC md5>_<patch name> so later opens are a stat;
 *   (c) anything else: not ours, the caller serves the disc entry unchanged.
 * Plain C (MinGW and host gcc), no runtime dependencies: the zlib inflater and the EDAT
 * resolver are passed in, so the focused test (port/vfs/test_vfs_overlay.sh) links it alone. */
#ifndef KZ3_OVERLAY_H
#define KZ3_OVERLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same contract as stb_image's stbi_zlib_decode_buffer: inflate a zlib stream (with
 * header), return the bytes written or < 0. */
typedef int (*kz3_inflate_fn)(char* out, int out_len, const char* in, int in_len);
/* Same contract as the runtime's edat_resolve (libs/filesystem/edat.h). */
typedef const char* (*kz3_edat_resolve_fn)(const char* host_path, char* buf, size_t cap);

typedef struct {
    const char* disc_psarc;          /* .../PS3_GAME/USRDIR/data_ps3.psarc (required for layer b) */
    const char* added_psarc;         /* added_data_ps3.psarc[.edat], NULL = no layer a */
    const char* patch_dir;           /* .../patch_data_ps3_mp, NULL = no layer b */
    const char* cache_dir;           /* writable directory for inflated / patched files */
    kz3_inflate_fn inflate;          /* required */
    kz3_edat_resolve_fn edat_resolve;/* NULL = added_psarc must already be plaintext */
    /* Run-time patch set switch (pass 155): the update's two patch directories. The set in force at init is the one
     * whose directory equals patch_dir (else the first non-NULL of sp, mp). NULL = that set does not exist: switching to
     * it serves no patched files (the disc entry), it never turns the layers off. The pointers must stay valid. */
    const char* patch_dir_sp;        /* .../patch_data_ps3_sp or NULL */
    const char* patch_dir_mp;        /* .../patch_data_ps3_mp or NULL */
    /* Native patching (pass 165, the RE notes s.48): 1 = the host does NOT apply .pspatch files; the game's own
     * FIOS patch layer composes them (the caller maps the guest's /dev_bdvd/PS3_GAME/USRDIR/patch_data_ps3_{sp,mp}
     * onto the update directories). resolve() then returns only added entries and directories, so the file served under
     * a disc path is the same in every set, as on the PS3 (FIOS keeps one size/time per path for the whole run). */
    int native_patch;
} kz3_overlay_config;

enum { KZ3_SET_SP = 0, KZ3_SET_MP = 1 };

enum {
    KZ3_OVL_NONE = 0,     /* not in the update: serve the disc entry */
    KZ3_OVL_ADDED = 1,    /* file from added_data_ps3.psarc */
    KZ3_OVL_PATCHED = 2,  /* disc entry with its .pspatch applied */
    KZ3_OVL_DIR = 3,      /* directory that exists only in added_data_ps3.psarc (created empty) */
    KZ3_OVL_ERROR = -1    /* a layer claims the path but it could not be produced (logged) */
};

/* Open the archives. Returns 0, or -1 if nothing could be opened. The added archive is
 * opened lazily: until its EDAT can be decrypted (the 1.14 EBOOT registers the
 * klicensee with sceNpDrmIsAvailable2, or kz3_overlay_selfkey registers it), layer a is retried on
 * each resolve. */
int kz3_overlay_init(const kz3_overlay_config* cfg);

/* `key` is the path below USRDIR ("localcacheps3/lumps/x.core", any case, '/' or '\\').
 * On KZ3_OVL_ADDED / PATCHED / DIR, `out` holds a host path that exists. Thread-safe. */
int kz3_overlay_resolve(const char* key, char* out, size_t cap);

void kz3_overlay_shutdown(void);

/* Follow the game's own patch set switch (1.14 SelectPatchSetSP 0x284A0 / SelectPatchSetMP 0x284D8, the RE notes
 * s.33/s.37 and pass 155). Swaps the directory later resolves read, under the same lock as kz3_overlay_resolve, so an
 * open sees one set or the other; nothing is closed or invalidated (already-served files stay valid, the pspatch cache
 * is shared because shared patch names are byte-identical in the two sets). Returns the previous set, or -1 if the
 * overlay is not initialised or the set is not KZ3_SET_SP/KZ3_SET_MP. Logs "[overlay] patch set sp -> mp (lr 0x...)"
 * on a change. Thread-safe. */
int kz3_overlay_set_patch_set(int set, unsigned lr);
/* Current set (KZ3_SET_*), or -1 before init. */
int kz3_overlay_patch_set(void);

/* Self key (pass 43, the RE notes section 16). The 1.14 EBOOT passes its klicensee to
 * sceNpDrmIsAvailable2 only from the FIOS media executor, which the port's VFS bypasses, so the
 * added_data EDAT would never decrypt. KZ3_SELFKEY_VA is where the 1.14 EBOOT keeps the 16 bytes
 * (read-only segment). kz3_overlay_selfkey(cand, add): after kz3_overlay_init, checks `cand` against
 * the added EDAT's dev_hash and only then calls add(cand) (the runtime's edat_add_klicensee, patch 5001).
 * Returns 1 registered, 0 skipped (KZ3_OVERLAY_SELFKEY=0, no EDAT), -1 rejected (dev_hash mismatch).
 * Port wiring (psarc_vfs.cpp, after kz3_overlay_init):
 *   if (g_overlay_on && vm_base) kz3_overlay_selfkey(vm_base + KZ3_SELFKEY_VA, edat_add_klicensee); */
#define KZ3_SELFKEY_VA 0x00E7908Bu
typedef void (*kz3_add_klicensee_fn)(const uint8_t klic[16]);
int kz3_overlay_selfkey(const uint8_t* cand, kz3_add_klicensee_fn add);

/* Apply a .pspatch (format in the RE notes 8.1) to `base`. Returns a malloc'd
 * buffer of *out_len bytes, or NULL if the patch is malformed or reads past base/literals. */
uint8_t* kz3_pspatch_apply(const uint8_t* patch, size_t patch_len,
                           const uint8_t* base, size_t base_len, size_t* out_len);

/* Patch file name stem (32 hex digits + NUL) for a path below USRDIR. */
void kz3_pspatch_name(const char* key, char out[33]);

/* The same key for the loose path ("/" + path, upper-cased): what the guest's patch layer computes while
 * data_ps3.psarc is not mounted in FIOS (path translator 1.14 @0x0002c7d4, the RE notes s.48.6). */
void kz3_pspatch_name_loose(const char* key, char out[33]);

/* Native patching under (pass 168): copy every <src_dir>/<NAME>.pspatch into dst_dir (created), renamed to
 * kz3_pspatch_name_loose(path) when NAME == kz3_pspatch_name(path) for one of the `count` disc manifest `names`
 * (relative paths, any case), else kept under NAME (no disc base: inert, as on a PS3 without that base). Files already
 * there with the same size are kept; other *.pspatch in dst_dir are removed, so the directory lists exactly the update's
 * patch set. Returns the number of files placed, or -1 (src missing, or a copy failed); *renamed = files rekeyed. */
int kz3_overlay_native_rekey(const char* src_dir, const char* dst_dir, const char* const* names, size_t count,
                             int* renamed);

/* 1 for the keys the port rewrites after the update's patch (network pass 38): with native patching they stay
 * host-composed (resolve patches them, the rekey drops their .pspatch). */
int kz3_overlay_host_composed(const char* key);

void kz3_md5(const void* data, size_t len, uint8_t digest[16]);

/* Test hook: walk every disc entry, apply each patch whose name matches, in memory.
 * Counts distinct patch files used, failures, and patched .core outputs that start with
 * "RTTIBin<1.73>". Returns the number of patches applied, or -1. */
int kz3_overlay_sweep(int* failures, int* cores, int* cores_rtti);

#ifdef __cplusplus
}
#endif

#endif

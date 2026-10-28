/* Focused test for patch 5002 (writable EDAT cache) and the 1.14 VFS overlay (port/vfs/kz3_overlay.c).
 *
 *   test_vfs_overlay EDAT ELF DISC_PSARC PATCH_DIR CACHE KEY...
 *
 * Built by test_vfs_overlay.sh against a ps3recomp tree with 5001+5002 applied (edat.c is included so the
 * klicensee scan can use its static AES-CMAC, as in tools/upstream/test_edat_v3.c). $PS3_EDAT_CACHE must be set.
 *  step 1  edat_resolve without a guest key returns the source (no plaintext anywhere);
 *  step 1b the overlay's self key (kz3_overlay_selfkey, pass 43): gated off by KZ3_OVERLAY_SELFKEY=0, and a
 *          wrong key (one bit flipped) is rejected by the dev_hash check, nothing registered, EDAT still encrypted;
 *  step 2  the 16 bytes at guest VA KZ3_SELFKEY_VA (read via the ELF's PT_LOAD headers; never printed) verify
 *          and are registered through edat_add_klicensee; edat_resolve then decrypts into
 *          $PS3_EDAT_CACHE/<content id>/<name>.dec; a second call is a cache hit (mtime unchanged);
 *          a sidecar that does not match the header is rejected;
 *  step 3  overlay: for each KEY print "key<TAB>layer<TAB>size<TAB>md5<TAB>first 16 bytes" (the script
 *          compares against tools/psarc.py + tools/assets/pspatch.py);
 *  step 4  sweep: apply every patch whose name matches a disc entry, in memory (when $SWEEP != 0). */
#include "edat.c"
#include "kz3_overlay.h"

#include <zlib.h>
#include <pthread.h>

static int zinflate(char* out, int olen, const char* in, int ilen)
{
    uLongf n = (uLongf)olen;
    int rc = uncompress((Bytef*)out, &n, (const Bytef*)in, (uLong)ilen);
    return (rc == Z_OK || (rc == Z_BUF_ERROR && n == (uLongf)olen)) ? (int)n : -1;
}

static uint8_t* rd(const char* p, size_t* n)
{
    FILE* f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc((size_t)sz + 1);
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static int s_adds;
static void count_add(const uint8_t klic[16]) { s_adds++; edat_add_klicensee(klic); }

/* 16 bytes at a guest VA, read through the ELF's PT_LOAD headers the way ppu_load_elf places them. */
static int elf_va_bytes(const uint8_t* e, size_t n, uint32_t va, uint8_t out[16])
{
    if (n < 64 || memcmp(e, "\x7f" "ELF", 4) != 0) return -1;
    uint64_t phoff = be64(e + 0x20);
    unsigned phnum = (unsigned)(e[0x38] << 8 | e[0x39]);
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t* ph = e + phoff + 0x38ull * i;
        if (phoff + 0x38ull * (i + 1) > n || be32(ph) != 1) continue;
        uint64_t off = be64(ph + 8), vaddr = be64(ph + 16), fsz = be64(ph + 32);
        if (va >= vaddr && va + 16 <= vaddr + fsz && off + (va - vaddr) + 16 <= n) {
            memcpy(out, e + off + (va - vaddr), 16);
            return 0;
        }
    }
    return -1;
}

static const char* layer_name(int r)
{
    switch (r) {
    case KZ3_OVL_NONE: return "disc"; case KZ3_OVL_ADDED: return "added";
    case KZ3_OVL_PATCHED: return "patched"; case KZ3_OVL_DIR: return "dir"; default: return "error";
    }
}

/* Patch set switch (pass 155): after init the set is mp; for every key, PATCHED under a set iff that set's directory holds
 * the key's .pspatch (and no added entry wins); switching is reversible, logged once per change, and a switch racing with
 * resolves in another thread never yields anything but NONE/PATCHED/ADDED/DIR for the keys. */
static int expect_for(const char* dir, const char* key, int base)
{
    if (base != KZ3_OVL_NONE && base != KZ3_OVL_PATCHED) return base;      /* added / dir win over the patch layer */
    char name[33], pp[1300];
    kz3_pspatch_name(key, name);
    snprintf(pp, sizeof pp, "%s/%s.pspatch", dir, name);
    struct stat st;
    return stat(pp, &st) == 0 ? KZ3_OVL_PATCHED : KZ3_OVL_NONE;
}
static volatile int g_stop, g_bad;
static const char* const* g_keys; static int g_nkeys;
static void* flipper(void* a)
{
    (void)a;
    for (int i = 0; !g_stop; i++) kz3_overlay_set_patch_set(i & 1, 0xF11Fu);
    return NULL;
}
static void* resolver(void* a)
{
    (void)a;
    for (int n = 0; n < 300; n++)
        for (int i = 0; i < g_nkeys; i++) {
            char out[1200];
            int r = kz3_overlay_resolve(g_keys[i], out, sizeof out);
            if (r != KZ3_OVL_NONE && r != KZ3_OVL_PATCHED && r != KZ3_OVL_ADDED && r != KZ3_OVL_DIR) g_bad++;
        }
    return NULL;
}
static int patch_set_checks(int argc, char** argv, const char* spdir)
{
    int fails = 0;
    if (!spdir[0]) { printf("step 5: SKIP (no patch_data_ps3_sp directory next to %s)\n", argv[4]); return 0; }
    struct stat st;
    if (stat(spdir, &st) != 0) { printf("step 5: SKIP (%s missing)\n", spdir); return 0; }
    if (kz3_overlay_patch_set() != KZ3_SET_MP) { printf("FAIL: initial set is not mp\n"); return 1; }
    int mp_only = 0, shared = 0;
    for (int pass = 0; pass < 3; pass++) {                       /* mp, sp, mp again */
        int set = pass == 1 ? KZ3_SET_SP : KZ3_SET_MP;
        int prev = kz3_overlay_set_patch_set(set, 0x8B6524u);
        if (pass == 0 ? prev != KZ3_SET_MP : prev == set) { printf("FAIL: switch %d returned previous %d\n", pass, prev); fails++; }
        if (kz3_overlay_patch_set() != set) { printf("FAIL: set not %d\n", set); fails++; }
        for (int i = 6; i < argc; i++) {
            char out[1200] = "";
            int r = kz3_overlay_resolve(argv[i], out, sizeof out);
            int want = expect_for(set == KZ3_SET_MP ? argv[4] : spdir, argv[i], r);
            if (r == KZ3_OVL_ADDED || r == KZ3_OVL_DIR) continue;
            if (r != want) { printf("FAIL: %s under %s: %s, want %s\n", argv[i], set ? "mp" : "sp", layer_name(r), layer_name(want)); fails++; }
            if (pass == 1) { if (r == KZ3_OVL_PATCHED) shared++; }
            if (pass == 0 && r == KZ3_OVL_PATCHED) mp_only++;
        }
    }
    printf("step 5: switch mp -> sp -> mp: patched keys under mp %d, under sp %d (an MP-only patch must vanish under sp)\n", mp_only, shared);
    if (kz3_overlay_set_patch_set(7, 0) != -1) { printf("FAIL: bad set accepted\n"); fails++; }
    g_keys = (const char* const*)(argv + 6); g_nkeys = argc - 6;
    pthread_t a, b, c;
    pthread_create(&a, NULL, flipper, NULL);
    pthread_create(&b, NULL, resolver, NULL);
    pthread_create(&c, NULL, resolver, NULL);
    pthread_join(b, NULL); pthread_join(c, NULL);
    g_stop = 1; pthread_join(a, NULL);
    printf("step 5: switch racing with 2 resolver threads: %d bad results (expect 0)\n", g_bad);
    if (g_bad) fails++;
    return fails;
}

int main(int argc, char** argv)
{
    if (argc < 7) { fprintf(stderr, "usage: %s EDAT ELF DISC PATCHDIR CACHE KEY...\n", argv[0]); return 2; }
    const char* cache = getenv("PS3_EDAT_CACHE");
    if (!cache || !*cache) { printf("FAIL: PS3_EDAT_CACHE not set\n"); return 1; }
    char buf[1200];

    const char* r = edat_resolve(argv[1], buf, sizeof buf);
    printf("step 1: no guest key -> %s\n", r == argv[1] ? "source returned (ok)" : r);
    if (r != argv[1]) { printf("FAIL\n"); return 1; }

    uint8_t hdr[0x90];
    FILE* f = fopen(argv[1], "rb");
    if (!f || fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { printf("FAIL: header\n"); return 1; }
    fclose(f);
    size_t n = 0;
    uint8_t* elf = rd(argv[2], &n);
    uint8_t cand[16];
    if (!elf || elf_va_bytes(elf, n, KZ3_SELFKEY_VA, cand) != 0) { printf("FAIL: VA 0x%08X not in the ELF\n", KZ3_SELFKEY_VA); return 1; }
    free(elf);
    /* patch sets (pass 155): argv[4] is the MP directory; the SP one is the same path with _mp -> _sp (SPDIR overrides) */
    static char spdir[1200];
    {
        const char* e = getenv("SPDIR");
        if (e && *e) snprintf(spdir, sizeof spdir, "%s", e);
        else {
            snprintf(spdir, sizeof spdir, "%s", argv[4]);
            char* m = strstr(spdir, "patch_data_ps3_mp");
            if (m) m[15] = 's', m[16] = 'p';
            else spdir[0] = 0;
        }
    }
    kz3_overlay_config cfg = { argv[3], argv[1], argv[4], argv[5], zinflate, edat_resolve, spdir[0] ? spdir : NULL, argv[4] };
    if (kz3_overlay_init(&cfg) != 0) { printf("FAIL: overlay init\n"); return 1; }
    /* self key (pass 43): gate, wrong key, then the bytes the loaded 1.14 image holds at KZ3_SELFKEY_VA */
    setenv("KZ3_OVERLAY_SELFKEY", "0", 1);
    int g0 = kz3_overlay_selfkey(cand, count_add);
    unsetenv("KZ3_OVERLAY_SELFKEY");
    uint8_t wrong[16];
    memcpy(wrong, cand, 16); wrong[7] ^= 0x01;
    int w0 = kz3_overlay_selfkey(wrong, count_add);
    r = edat_resolve(argv[1], buf, sizeof buf);
    printf("step 1b: self key gated off -> %d, wrong key -> %d, registered %d, EDAT %s (expect 0 -1 0 still encrypted)\n",
           g0, w0, s_adds, r == argv[1] ? "still encrypted" : "DECRYPTED");
    if (g0 != 0 || w0 != -1 || s_adds != 0 || r != argv[1]) { printf("FAIL\n"); return 1; }
    int k1 = kz3_overlay_selfkey(cand, count_add);
    printf("step 2: self key from guest VA 0x%08X -> %d, registered %d (expect 1 1)\n", KZ3_SELFKEY_VA, k1, s_adds);
    if (k1 != 1 || s_adds != 1) { printf("FAIL\n"); return 1; }
    r = edat_resolve(argv[1], buf, sizeof buf);
    struct stat s1, s2;
    if (r == argv[1] || stat(r, &s1) != 0 || strncmp(r, cache, strlen(cache)) != 0) { printf("FAIL: not cached: %s\n", r); return 1; }
    printf("step 2: decrypted into the cache: %s (%lld bytes, header says %llu)\n", r + strlen(cache),
           (long long)s1.st_size, (unsigned long long)be64(hdr + 0x88));
    if ((uint64_t)s1.st_size != be64(hdr + 0x88)) { printf("FAIL: size\n"); return 1; }
    char dec[1200];
    snprintf(dec, sizeof dec, "%s", r);
    r = edat_resolve(argv[1], buf, sizeof buf);
    if (strcmp(r, dec) != 0 || stat(r, &s2) != 0 || s2.st_mtime != s1.st_mtime) { printf("FAIL: second call not a cache hit\n"); return 1; }
    uint8_t key[0x38], bad[0x38];
    edat_cache_key(hdr, key);
    memcpy(bad, key, sizeof bad); bad[0] ^= 1;
    char keyf[1220];
    snprintf(keyf, sizeof keyf, "%s.key", dec);
    int v_ok = edat_cache_valid(dec, keyf, key, be64(hdr + 0x88));
    int v_bad = edat_cache_valid(dec, keyf, bad, be64(hdr + 0x88));
    int v_size = edat_cache_valid(dec, keyf, key, be64(hdr + 0x88) + 1);
    printf("step 2: second call is a cache hit; sidecar check: match %d, other digest %d, other size %d (expect 1 0 0)\n",
           v_ok, v_bad, v_size);
    if (v_ok != 1 || v_bad != 0 || v_size != 0) { printf("FAIL\n"); return 1; }

    int fails = 0;
    for (int i = 6; i < argc; i++) {
        char out[1200] = "";
        int lr = kz3_overlay_resolve(argv[i], out, sizeof out);
        if (lr == KZ3_OVL_ADDED || lr == KZ3_OVL_PATCHED) {
            size_t dn = 0;
            uint8_t* d = rd(out, &dn);
            uint8_t md[16];
            char hx[33] = "", head[17] = "";
            if (d) {
                kz3_md5(d, dn, md);
                for (int j = 0; j < 16; j++) sprintf(hx + 2 * j, "%02x", md[j]);
                for (size_t j = 0; j < 16 && j < dn; j++) head[j] = (d[j] >= 32 && d[j] < 127) ? (char)d[j] : '.';
            } else fails++;
            printf("%s\t%s\t%zu\t%s\t%s\n", argv[i], layer_name(lr), dn, hx, head);
            free(d);
        } else {
            if (lr == KZ3_OVL_ERROR) fails++;
            printf("%s\t%s\t-\t-\t-\n", argv[i], layer_name(lr));
        }
    }
    const char* sw = getenv("SWEEP");
    if (!sw || sw[0] != '0') {
        int sf = 0, sc = 0, sr = 0;
        int applied = kz3_overlay_sweep(&sf, &sc, &sr);
        printf("step 4: sweep applied %d patches, %d failed; patched .core %d, RTTIBin<1.73> %d\n", applied, sf, sc, sr);
        if (applied <= 0 || sf) fails++;
    }
    fails += patch_set_checks(argc, argv, spdir);
    kz3_overlay_shutdown();
    printf("%s\n", fails ? "FAIL" : "C-PASS");
    return fails ? 1 : 0;
}

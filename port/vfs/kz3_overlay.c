/* Killzone 3 port: 1.14 update overlay (see kz3_overlay.h and the RE notes section 10). */
#include "kz3_overlay.h"
#include "../net/kz3_host_composed.h"   /* the host-composed set (network pass 40) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define ovl_mkdir(p) _mkdir(p)
#define ovl_fseek _fseeki64
typedef SRWLOCK ovl_lock_t;
#define OVL_LOCK_INIT SRWLOCK_INIT
#define ovl_lock(l) AcquireSRWLockExclusive(l)
#define ovl_unlock(l) ReleaseSRWLockExclusive(l)
#else
#include <pthread.h>
#include <sys/types.h>
#define ovl_mkdir(p) mkdir(p, 0755)
#define ovl_fseek fseeko
typedef pthread_mutex_t ovl_lock_t;
#define OVL_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define ovl_lock(l) pthread_mutex_lock(l)
#define ovl_unlock(l) pthread_mutex_unlock(l)
#endif

/* ------------------------------------------------------------------ MD5 (RFC 1321) */

typedef struct { uint32_t s[4]; uint64_t n; uint8_t buf[64]; } md5_ctx;

static uint32_t rol(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }

static void md5_block(md5_ctx* c, const uint8_t* p)
{
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const int R[16] = { 7,12,17,22, 5,9,14,20, 4,11,16,23, 6,10,15,21 };
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] | (uint32_t)p[4*i+1] << 8 | (uint32_t)p[4*i+2] << 16 | (uint32_t)p[4*i+3] << 24;
    uint32_t a = c->s[0], b = c->s[1], cc = c->s[2], d = c->s[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16)      { f = (b & cc) | (~b & d);  g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc);  g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d;           g = (3 * i + 5) & 15; }
        else             { f = cc ^ (b | ~d);        g = (7 * i) & 15; }
        uint32_t t = d; d = cc; cc = b;
        b = b + rol(a + f + K[i] + w[g], R[(i >> 4) * 4 + (i & 3)]);
        a = t;
    }
    c->s[0] += a; c->s[1] += b; c->s[2] += cc; c->s[3] += d;
}

void kz3_md5(const void* data, size_t len, uint8_t digest[16])
{
    md5_ctx c = { { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }, 0, { 0 } };
    const uint8_t* p = (const uint8_t*)data;
    c.n = len;
    for (; len >= 64; p += 64, len -= 64) md5_block(&c, p);
    uint8_t tail[128] = { 0 };
    memcpy(tail, p, len);
    tail[len] = 0x80;
    size_t tl = (len < 56) ? 64 : 128;
    uint64_t bits = c.n * 8;
    for (int i = 0; i < 8; i++) tail[tl - 8 + i] = (uint8_t)(bits >> (8 * i));
    md5_block(&c, tail);
    if (tl == 128) md5_block(&c, tail + 64);
    for (int i = 0; i < 16; i++) digest[i] = (uint8_t)(c.s[i >> 2] >> (8 * (i & 3)));
}

static void hex16(const uint8_t d[16], char out[33])
{
    static const char H[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) { out[2*i] = H[d[i] >> 4]; out[2*i+1] = H[d[i] & 15]; }
    out[32] = 0;
}

/* @0x00e0e794 (1.14): MD5 (@0x00e21fe8, flag 1 = upper-case each byte a-z first) of the
 * archive path "/data_ps3/<path>"; the 16 digest bytes are printed as two big-endian u64
 * with "%016llX%016llX", i.e. the digest in hex, upper case. */
static void pspatch_key(const char* prefix, const char* key, char out[33])
{
    char s[1200];
    size_t n = (size_t)snprintf(s, sizeof s, "%s%s", prefix, key);
    if (n >= sizeof s) n = sizeof s - 1;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\') s[i] = '/';
        if (s[i] >= 'a' && s[i] <= 'z') s[i] = (char)(s[i] - 0x20);
    }
    uint8_t d[16];
    kz3_md5(s, n, d);
    hex16(d, out);
}

void kz3_pspatch_name(const char* key, char out[33]) { pspatch_key("/data_ps3/", key, out); }

/* Pass 168 (the RE notes s.48.6): the game's file device translates "/dev_bdvd/PS3_GAME/USRDIR/<path>" in
 * @0x0002c7d4 to "<mount>/<path>" ("/data_ps3/<path>") only when data_ps3.psarc is mounted (flag +0x20, set at
 * 0x0002d480), else to "/<path>"; FIOS's patch layer (@0x00e0c450) hashes that string as it arrives. The port keeps the
 * archive away from FIOS, so the guest computes the "/<path>" key. */
void kz3_pspatch_name_loose(const char* key, char out[33]) { pspatch_key("/", key, out); }

/* ------------------------------------------------------------------ string -> index map */

typedef struct { char** keys; uint32_t* vals; size_t cap, n; } strmap;

static uint64_t fnv(const char* s) { uint64_t h = 1469598103934665603ull; while (*s) h = (h ^ (uint8_t)*s++) * 1099511628211ull; return h; }

static int strmap_get(const strmap* m, const char* k, uint32_t* v)
{
    if (!m->cap) return 0;
    for (size_t i = fnv(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->keys[i]) return 0;
        if (strcmp(m->keys[i], k) == 0) { if (v) *v = m->vals[i]; return 1; }
    }
}

static void strmap_put(strmap* m, const char* k, uint32_t v)   /* first insert wins */
{
    if ((m->n + 1) * 2 > m->cap) {
        strmap o = *m;
        m->cap = o.cap ? o.cap * 2 : 1024; m->n = 0;
        m->keys = (char**)calloc(m->cap, sizeof(char*));
        m->vals = (uint32_t*)calloc(m->cap, sizeof(uint32_t));
        for (size_t i = 0; i < o.cap; i++) if (o.keys[i]) {
            size_t j = fnv(o.keys[i]) & (m->cap - 1);
            while (m->keys[j]) j = (j + 1) & (m->cap - 1);
            m->keys[j] = o.keys[i]; m->vals[j] = o.vals[i]; m->n++;
        }
        free(o.keys); free(o.vals);
    }
    size_t i = fnv(k) & (m->cap - 1);
    for (; m->keys[i]; i = (i + 1) & (m->cap - 1)) if (strcmp(m->keys[i], k) == 0) return;
    m->keys[i] = strdup(k); m->vals[i] = v; m->n++;
}

static void strmap_free(strmap* m)
{
    for (size_t i = 0; i < m->cap; i++) free(m->keys[i]);
    free(m->keys); free(m->vals);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------------ PSARC v1.4 reader */

typedef struct { uint8_t md5[16]; uint32_t block; uint64_t size, offset; } ovl_entry;

typedef struct {
    FILE* f;
    uint32_t block_size, count;
    ovl_entry* e;
    uint32_t* blocks; size_t nblocks;
    char** names;           /* manifest spelling, names[0] = "" */
    strmap by_key;          /* normalised (lower-case) path -> index */
    strmap dirs;            /* normalised directory prefixes */
} ovl_psarc;

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be40(const uint8_t* p) { return (uint64_t)p[0] << 32 | be32(p + 1); }

static kz3_overlay_config g_cfg;

/* Lower-case, '\\' -> '/', no leading/trailing '/'. */
static void norm(const char* in, char* out, size_t cap)
{
    while (*in == '/' || *in == '\\') in++;
    size_t n = 0;
    for (; *in && n + 1 < cap; in++) {
        char c = *in == '\\' ? '/' : *in;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 0x20);
        out[n++] = c;
    }
    while (n && out[n - 1] == '/') n--;
    out[n] = 0;
}

static uint8_t* psarc_read(ovl_psarc* a, uint32_t idx, size_t* len)
{
    const ovl_entry* e = &a->e[idx];
    uint8_t* out = (uint8_t*)malloc(e->size ? (size_t)e->size : 1);
    uint8_t* in = (uint8_t*)malloc(a->block_size);
    if (!out || !in || ovl_fseek(a->f, (int64_t)e->offset, SEEK_SET) != 0) goto fail;
    size_t got = 0;
    for (uint32_t b = e->block; got < e->size; b++) {
        if (b >= a->nblocks) goto fail;
        uint32_t cs = a->blocks[b] ? a->blocks[b] : a->block_size;
        uint64_t want = e->size - got;
        if (want > a->block_size) want = a->block_size;
        if (cs > a->block_size || fread(in, 1, cs, a->f) != cs) goto fail;
        if (cs == want) { memcpy(out + got, in, cs); got += cs; continue; }       /* stored */
        int n = g_cfg.inflate((char*)out + got, (int)want, (const char*)in, (int)cs);
        if (n < 0) { if (cs > want) goto fail; memcpy(out + got, in, cs); n = (int)cs; }  /* raw short block */
        got += (size_t)n;
    }
    free(in);
    *len = (size_t)e->size;
    return out;
fail:
    free(in); free(out);
    return NULL;
}

static void psarc_close(ovl_psarc* a);

static int psarc_open(ovl_psarc* a, const char* path, int want_dirs)
{
    memset(a, 0, sizeof *a);
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[32];
    if (fread(h, 1, 32, f) != 32 || memcmp(h, "PSAR", 4) || memcmp(h + 8, "zlib", 4) || (be32(h + 28) & 4)) {
        fprintf(stderr, "[overlay] %s: not a zlib PSARC with a plain TOC\n", path); fclose(f); return -1;
    }
    uint32_t toc_len = be32(h + 12), esz = be32(h + 16);
    a->count = be32(h + 20); a->block_size = be32(h + 24);
    if (toc_len < 32 || esz < 30 || a->block_size == 0) { fclose(f); return -1; }
    size_t tn = toc_len - 32;
    uint8_t* toc = (uint8_t*)malloc(tn);
    if (!toc || fread(toc, 1, tn, f) != tn || (uint64_t)a->count * esz > tn || a->count == 0) {
        fprintf(stderr, "[overlay] %s: bad TOC\n", path); free(toc); fclose(f); return -1;
    }
    a->f = f;
    a->e = (ovl_entry*)calloc(a->count, sizeof(ovl_entry));
    for (uint32_t i = 0; i < a->count; i++) {
        const uint8_t* e = toc + (size_t)i * esz;
        memcpy(a->e[i].md5, e, 16);
        a->e[i].block = be32(e + 16); a->e[i].size = be40(e + 20); a->e[i].offset = be40(e + 25);
    }
    unsigned w = 1;
    while ((1ull << (8 * w)) < a->block_size) w++;
    size_t start = (size_t)a->count * esz;
    a->nblocks = (tn - start) / w;
    a->blocks = (uint32_t*)calloc(a->nblocks + 1, sizeof(uint32_t));
    for (size_t j = 0; j < a->nblocks; j++) {
        uint32_t v = 0;
        for (unsigned k = 0; k < w; k++) v = v << 8 | toc[start + j * w + k];
        a->blocks[j] = v;
    }
    free(toc);
    size_t ml = 0;
    uint8_t* man = psarc_read(a, 0, &ml);
    if (!man) { fprintf(stderr, "[overlay] %s: cannot read the manifest\n", path); psarc_close(a); return -1; }
    a->names = (char**)calloc(a->count, sizeof(char*));
    a->names[0] = strdup("");
    uint32_t i = 1;
    size_t s = 0;
    for (size_t p = 0; p <= ml && i < a->count; p++) {
        if (p == ml || man[p] == '\n') {
            char* n = (char*)malloc(p - s + 1);
            memcpy(n, man + s, p - s); n[p - s] = 0;
            a->names[i++] = n; s = p + 1;
        }
    }
    free(man);
    for (; i < a->count; i++) a->names[i] = strdup("");
    char k[1100];
    for (i = 1; i < a->count; i++) {
        norm(a->names[i], k, sizeof k);
        if (!*k) continue;
        strmap_put(&a->by_key, k, i);
        if (want_dirs)
            for (char* p = strchr(k, '/'); p; p = strchr(p + 1, '/')) { *p = 0; strmap_put(&a->dirs, k, 0); *p = '/'; }
    }
    return 0;
}

static void psarc_close(ovl_psarc* a)
{
    if (a->f) fclose(a->f);
    if (a->names) for (uint32_t i = 0; i < a->count; i++) free(a->names[i]);
    free(a->names); free(a->e); free(a->blocks);
    strmap_free(&a->by_key); strmap_free(&a->dirs);
    memset(a, 0, sizeof *a);
}

/* ------------------------------------------------------------------ .pspatch */

/* @ the RE notes 8.1 (port of tools/assets/pspatch.py apply). */
uint8_t* kz3_pspatch_apply(const uint8_t* p, size_t pn, const uint8_t* base, size_t bn, size_t* out_len)
{
    if (pn < 12) return NULL;
    uint64_t A = be32(p), B = be32(p + 4), C = be32(p + 8), lit0 = A + 8;
    if (lit0 > pn || lit0 + B > pn || lit0 < 12 || (lit0 - 12) % 8) return NULL;
    const uint8_t* lit = p + lit0;
    size_t nops = (size_t)(lit0 - 12) / 8;
    uint8_t* out = (uint8_t*)malloc(C ? (size_t)C : 1);
    if (!out) return NULL;
    uint64_t pos = 0;
    for (size_t i = 0; i < nops; i++) {
        uint32_t x = be32(p + 12 + 8 * i), y = be32(p + 16 + 8 * i);
        uint64_t at = x & 0x7fffffffu;
        uint64_t end = (i + 1 < nops) ? (be32(p + 12 + 8 * (i + 1)) & 0x7fffffffu) : C;
        if (at != pos || end < at || end > C) goto bad;
        uint64_t n = end - at;
        if (x & 0x80000000u) { if ((uint64_t)y + n > bn) goto bad; memcpy(out + pos, base + y, (size_t)n); }
        else                 { if ((uint64_t)y + n > B)  goto bad; memcpy(out + pos, lit + y, (size_t)n); }
        pos = end;
    }
    if (pos != C) goto bad;
    *out_len = (size_t)C;
    return out;
bad:
    free(out);
    return NULL;
}

/* ------------------------------------------------------------------ overlay */

static ovl_lock_t g_lock = OVL_LOCK_INIT;
static ovl_psarc g_disc, g_added;
static int g_have_disc, g_added_state;   /* 0 not tried yet / pending, 1 open, -1 failed for good */
static int g_logged;
static int g_set = -1;                   /* KZ3_SET_SP / KZ3_SET_MP, -1 = not initialised */

static uint8_t* slurp(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    ovl_fseek(f, 0, SEEK_END);
    long long sz = (long long)ftell(f);
    ovl_fseek(f, 0, SEEK_SET);
    uint8_t* b = sz >= 0 ? (uint8_t*)malloc((size_t)sz + 1) : NULL;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void mkdirs(const char* file_path)
{
    char t[1200];
    snprintf(t, sizeof t, "%s", file_path);
    for (char* p = t + 1; *p; p++) if (*p == '/') { *p = 0; ovl_mkdir(t); *p = '/'; }
}

static int write_atomic(const char* path, const uint8_t* d, size_t n)
{
    char part[1220];
    snprintf(part, sizeof part, "%s.part", path);
    mkdirs(path);
    FILE* o = fopen(part, "wb");
    if (!o) return -1;
    int ok = fwrite(d, 1, n, o) == n;
    ok = (fclose(o) == 0) && ok;
    remove(path);                        /* rename does not replace on Windows */
    if (!ok || rename(part, path) != 0) { remove(part); return -1; }
    return 0;
}

static int file_size_is(const char* path, uint64_t n)
{
    struct stat st;
    return stat(path, &st) == 0 && (uint64_t)st.st_size == n;
}

static int is_npd(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t m[4] = { 0 };
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    return n == 4 && memcmp(m, "NPD", 4) == 0;
}

/* Layer a: open added_data lazily (its EDAT needs the guest's klicensee first). */
static void try_open_added(void)
{
    if (g_added_state != 0 || !g_cfg.added_psarc) return;
    const char* p = g_cfg.added_psarc;
    char buf[1200];
    if (is_npd(p)) {
        if (!g_cfg.edat_resolve) { g_added_state = -1; return; }
        p = g_cfg.edat_resolve(p, buf, sizeof buf);
        if (!p || is_npd(p)) {
            if (!g_logged++) fprintf(stderr, "[overlay] %s not decrypted yet (no klicensee); retrying on later opens\n",
                                     g_cfg.added_psarc);
            return;
        }
    }
    if (psarc_open(&g_added, p, 1) == 0) {
        g_added_state = 1;
        fprintf(stderr, "[overlay] added archive %s: %u entries\n", p, g_added.count - 1);
    } else {
        g_added_state = -1;
    }
}

int kz3_overlay_init(const kz3_overlay_config* cfg)
{
    if (!cfg || !cfg->inflate || !cfg->cache_dir) return -1;
    ovl_lock(&g_lock);
    g_cfg = *cfg;
    /* Initial set: the one patch_dir names, else the first directory given. A switchable config keeps both. */
    g_set = KZ3_SET_SP;
    if (cfg->patch_dir && cfg->patch_dir_mp && strcmp(cfg->patch_dir, cfg->patch_dir_mp) == 0) g_set = KZ3_SET_MP;
    else if (!cfg->patch_dir && !cfg->patch_dir_sp && cfg->patch_dir_mp) g_set = KZ3_SET_MP;
    if (!g_cfg.patch_dir) g_cfg.patch_dir = g_set == KZ3_SET_MP ? cfg->patch_dir_mp : cfg->patch_dir_sp;
    /* The disc layer needs a patch directory in at least one set; a set without one just serves disc entries. */
    g_have_disc = cfg->disc_psarc && (g_cfg.patch_dir || cfg->patch_dir_sp || cfg->patch_dir_mp) &&
                  psarc_open(&g_disc, cfg->disc_psarc, 0) == 0;
    g_added_state = 0;
    g_logged = 0;
    ovl_mkdir(cfg->cache_dir);
    try_open_added();
    int ok = g_have_disc || g_added_state >= 0;
    fprintf(stderr, "[overlay] disc %s, patches %s, added %s, cache %s\n", g_have_disc ? "open" : "-",
            cfg->patch_dir ? cfg->patch_dir : "-", g_added_state == 1 ? "open" : g_added_state == 0 ? "pending" : "-",
            cfg->cache_dir);
    ovl_unlock(&g_lock);
    return ok ? 0 : -1;
}

void kz3_overlay_shutdown(void)
{
    ovl_lock(&g_lock);
    psarc_close(&g_disc); psarc_close(&g_added);
    g_have_disc = 0; g_added_state = 0; g_set = -1;
    ovl_unlock(&g_lock);
}

int kz3_overlay_patch_set(void)
{
    ovl_lock(&g_lock);
    int s = g_set;
    ovl_unlock(&g_lock);
    return s;
}

int kz3_overlay_set_patch_set(int set, unsigned lr)
{
    if (set != KZ3_SET_SP && set != KZ3_SET_MP) return -1;
    ovl_lock(&g_lock);
    int prev = g_set;
    if (prev >= 0) {
        g_cfg.patch_dir = set == KZ3_SET_MP ? g_cfg.patch_dir_mp : g_cfg.patch_dir_sp;
        g_set = set;
        if (prev != set)
            fprintf(stderr, "[overlay] patch set %s -> %s (lr 0x%08X)%s\n", prev == KZ3_SET_MP ? "mp" : "sp",
                    set == KZ3_SET_MP ? "mp" : "sp", lr, g_cfg.patch_dir ? "" : " (no such directory: disc files only)");
    }
    ovl_unlock(&g_lock);
    return prev;
}

/* ------------------------------------------------------------------ self key (pass 43)
 * The 1.14 EBOOT hands its klicensee to sceNpDrmIsAvailable2 only from the FIOS media executor
 * (0xe1ab2c, three call sites 0xe1ae90 / 0xe1bfcc / 0xe1c580, key = *(*(media+0xc)+0x28)) when FIOS
 * itself opens an EDAT. The port serves PSARC opens through its own VFS, so that call never
 * comes. The overlay takes the 16 bytes from the loaded guest image instead (KZ3_SELFKEY_VA), checks
 * them against the EDAT header's dev_hash and only then registers them. The key is never logged. */

static uint8_t aes_sbox[256];
static int aes_ready;
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t rotl8(uint8_t x, int s) { return (uint8_t)((x << s) | (x >> (8 - s))); }

static void aes_init(void)
{
    uint8_t p = 1, q = 1;
    do {   /* walk the multiplicative group with generator 3; q = p^-1 */
        p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
        if (q & 0x80) q ^= 0x09;
        aes_sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
    } while (p != 1);
    aes_sbox[0] = 0x63;
    aes_ready = 1;
}

static void aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[176], s[16], t[16], rcon = 1;
    if (!aes_ready) aes_init();
    memcpy(rk, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t w[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };
        if (i % 16 == 0) {
            uint8_t w0 = w[0];
            w[0] = (uint8_t)(aes_sbox[w[1]] ^ rcon); w[1] = aes_sbox[w[2]]; w[2] = aes_sbox[w[3]]; w[3] = aes_sbox[w0];
            rcon = xt(rcon);
        }
        for (int j = 0; j < 4; j++) rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ w[j]);
    }
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++) t[i] = aes_sbox[s[(i + 4 * (i & 3)) & 15]];   /* SubBytes + ShiftRows */
        if (r < 10)
            for (int c = 0; c < 16; c += 4) {                                       /* MixColumns */
                uint8_t a0 = t[c], a1 = t[c + 1], a2 = t[c + 2], a3 = t[c + 3], x = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                t[c] ^= (uint8_t)(x ^ xt((uint8_t)(a0 ^ a1))); t[c + 1] ^= (uint8_t)(x ^ xt((uint8_t)(a1 ^ a2)));
                t[c + 2] ^= (uint8_t)(x ^ xt((uint8_t)(a2 ^ a3))); t[c + 3] ^= (uint8_t)(x ^ xt((uint8_t)(a3 ^ a0)));
            }
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(t[i] ^ rk[16 * r + i]);
    }
    memcpy(out, s, 16);
}

static void cmac_dbl(uint8_t b[16])
{
    uint8_t carry = (uint8_t)(b[0] >> 7);
    for (int i = 0; i < 15; i++) b[i] = (uint8_t)((b[i] << 1) | (b[i + 1] >> 7));
    b[15] = (uint8_t)((b[15] << 1) ^ (carry * 0x87));
}

/* AES-CMAC (RFC 4493). */
static void aes128_cmac(const uint8_t key[16], const uint8_t* m, size_t n, uint8_t mac[16])
{
    uint8_t k1[16] = { 0 }, k2[16], x[16] = { 0 }, last[16] = { 0 };
    aes128_encrypt(key, k1, k1);
    cmac_dbl(k1); memcpy(k2, k1, 16); cmac_dbl(k2);
    size_t nb = n ? (n + 15) / 16 : 1;
    int complete = n && n % 16 == 0;
    for (size_t b = 0; b + 1 < nb; b++) {
        for (int i = 0; i < 16; i++) x[i] ^= m[16 * b + i];
        aes128_encrypt(key, x, x);
    }
    size_t rem = n - 16 * (nb - 1);
    memcpy(last, m + 16 * (nb - 1), rem);
    if (!complete) last[rem] = 0x80;
    for (int i = 0; i < 16; i++) x[i] ^= (uint8_t)(last[i] ^ (complete ? k1[i] : k2[i]));
    aes128_encrypt(key, x, mac);
}

/* Public platform constant (same as the runtime's libs/filesystem/edat.c): dev_hash =
 * AES-CMAC(klicensee ^ NP_OMAC_KEY_2, NPD header 0x00..0x5F). */
static const uint8_t NP_OMAC_KEY_2[16] = {
    0x6B, 0xA5, 0x29, 0x76, 0xEF, 0xDA, 0x16, 0xEF, 0x3C, 0x33, 0x9F, 0xB2, 0x97, 0x1E, 0x25, 0x6B };

int kz3_overlay_selfkey(const uint8_t* cand, kz3_add_klicensee_fn add)
{
    const char* gate = getenv("KZ3_OVERLAY_SELFKEY");
    if (gate && gate[0] == '0') { fprintf(stderr, "[overlay] self key disabled (KZ3_OVERLAY_SELFKEY=0)\n"); return 0; }
    if (!cand || !add) return 0;
    ovl_lock(&g_lock);
    const char* path = g_cfg.added_psarc;
    uint8_t hdr[0x70];
    int ok = 0;
    FILE* f = path ? fopen(path, "rb") : NULL;
    if (f) { ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && memcmp(hdr, "NPD", 4) == 0; fclose(f); }
    ovl_unlock(&g_lock);
    if (!ok) return 0;   /* no update archive, or already plaintext: nothing to unlock */
    uint8_t ck[16], mac[16];
    for (int i = 0; i < 16; i++) ck[i] = (uint8_t)(cand[i] ^ NP_OMAC_KEY_2[i]);
    aes128_cmac(ck, hdr, 0x60, mac);
    int match = memcmp(mac, hdr + 0x60, 16) == 0;
    memset(ck, 0, sizeof ck);
    if (!match) {
        fprintf(stderr, "[overlay] self key: the bytes at guest 0x%08X do not verify the EDAT dev_hash "
                        "(not the 1.14 EBOOT?); not registered\n", KZ3_SELFKEY_VA);
        return -1;
    }
    add(cand);
    fprintf(stderr, "[overlay] self key: guest 0x%08X verifies the EDAT dev_hash; registered\n", KZ3_SELFKEY_VA);
    return 1;
}

static int resolve_added(const char* k, char* out, size_t cap)
{
    uint32_t idx;
    if (strmap_get(&g_added.by_key, k, &idx)) {
        char host[1200];
        snprintf(host, sizeof host, "%s/added/%s", g_cfg.cache_dir, k);
        if (!file_size_is(host, g_added.e[idx].size)) {
            size_t n = 0;
            uint8_t* d = psarc_read(&g_added, idx, &n);
            int rc = d ? write_atomic(host, d, n) : -1;
            free(d);
            if (rc != 0) { fprintf(stderr, "[overlay] cannot produce added %s\n", k); return KZ3_OVL_ERROR; }
        }
        snprintf(out, cap, "%s", host);
        return KZ3_OVL_ADDED;
    }
    if (strmap_get(&g_added.dirs, k, NULL) && !(g_have_disc && strmap_get(&g_disc.by_key, k, NULL))) {
        char host[1200];
        snprintf(host, sizeof host, "%s/added/%s/", g_cfg.cache_dir, k);
        mkdirs(host);
        host[strlen(host) - 1] = 0;
        snprintf(out, cap, "%s", host);
        return KZ3_OVL_DIR;
    }
    return KZ3_OVL_NONE;
}

static int resolve_patched(const char* k, char* out, size_t cap)
{
    uint32_t idx;
    if (!strmap_get(&g_disc.by_key, k, &idx)) return KZ3_OVL_NONE;
    char name[33], ppath[1200], host[1200], md[33];
    if (!g_cfg.patch_dir) return KZ3_OVL_NONE;                /* this set has no directory */
    kz3_pspatch_name(k, name);
    snprintf(ppath, sizeof ppath, "%s/%s.pspatch", g_cfg.patch_dir, name);
    FILE* f = fopen(ppath, "rb");
    if (!f) return KZ3_OVL_NONE;
    uint8_t h[12];
    size_t hn = fread(h, 1, 12, f);
    fclose(f);
    if (hn != 12) return KZ3_OVL_ERROR;
    hex16(g_disc.e[idx].md5, md);
    snprintf(host, sizeof host, "%s/pspatch/%s_%s", g_cfg.cache_dir, md, name);
    if (!file_size_is(host, be32(h + 8))) {
        size_t pn = 0, bn = 0, on = 0;
        uint8_t* p = slurp(ppath, &pn);
        uint8_t* b = p ? psarc_read(&g_disc, idx, &bn) : NULL;
        uint8_t* o = b ? kz3_pspatch_apply(p, pn, b, bn, &on) : NULL;
        int rc = o ? write_atomic(host, o, on) : -1;
        free(p); free(b); free(o);
        if (rc != 0) { fprintf(stderr, "[overlay] cannot patch %s with %s.pspatch\n", k, name); return KZ3_OVL_ERROR; }
        fprintf(stderr, "[overlay] patched %s (%s.pspatch, %zu -> %zu bytes, set %s)\n", k, name, bn, on,
                g_set == KZ3_SET_MP ? "mp" : "sp");
    }
    snprintf(out, cap, "%s", host);
    return KZ3_OVL_PATCHED;
}

/* Net pass 38 (the RE notes s.32): the files the port rewrites after the update's patch (kz3_kb_miss_hook in
 * port/kz3/main.cpp adds the EXIT GAME row, the Display mode row, the Keyboard + Mouse link rows, the pause menu's
 * QUIT TO DESKTOP row; port/net/kz3_prompts_ui.h the Layout and Prompts row and the controller picture). With native
 * patching the guest's patch layer would apply the .pspatch to the REWRITTEN file (a binary delta against the disc
 * base: the offsets shift and the menu shows a wrong row, e.g. BUY NOW on the main menu, which the 1.14 patch removes).
 * These keys stay host-composed: resolve_patched serves update + rows as the file, and the rekey drops their .pspatch
 * so the patch layer has nothing to apply. Net pass 40: the set is port/net/kz3_host_composed.h, the header every
 * rewriter takes its key from (host tests tools/net/test_host_composed.c/.py), so a new rewrite cannot be missing here. */
int kz3_overlay_host_composed(const char* key)
{
    char k[1100];
    if (!key) return 0;
    norm(key, k, sizeof k);
    return kz3_host_composed_has(k);
}

int kz3_overlay_resolve(const char* key, char* out, size_t cap)
{
    char k[1100];
    if (!key) return KZ3_OVL_NONE;
    norm(key, k, sizeof k);
    if (!*k) return KZ3_OVL_NONE;
    int r = KZ3_OVL_NONE;
    ovl_lock(&g_lock);
    try_open_added();
    if (g_added_state == 1) r = resolve_added(k, out, cap);
    if (r == KZ3_OVL_NONE && g_have_disc && (!g_cfg.native_patch || kz3_overlay_host_composed(k))) r = resolve_patched(k, out, cap);
    ovl_unlock(&g_lock);
    return r;
}

int kz3_overlay_sweep(int* failures, int* cores, int* cores_rtti)
{
    *failures = *cores = *cores_rtti = 0;
    if (!g_have_disc || !g_cfg.patch_dir) return -1;
    ovl_lock(&g_lock);
    strmap used = { 0 };
    int applied = 0;
    char k[1100], name[33], ppath[1200];
    for (uint32_t i = 1; i < g_disc.count; i++) {
        norm(g_disc.names[i], k, sizeof k);
        kz3_pspatch_name(k, name);
        if (strmap_get(&used, name, NULL)) continue;          /* case variants share one patch */
        snprintf(ppath, sizeof ppath, "%s/%s.pspatch", g_cfg.patch_dir, name);
        size_t pn = 0, bn = 0, on = 0;
        uint8_t* p = slurp(ppath, &pn);
        if (!p) continue;
        strmap_put(&used, name, i);
        uint8_t* b = psarc_read(&g_disc, i, &bn);
        uint8_t* o = b ? kz3_pspatch_apply(p, pn, b, bn, &on) : NULL;
        if (!o) { (*failures)++; fprintf(stderr, "[overlay] sweep: %s.pspatch fails on %s\n", name, k); }
        else {
            applied++;
            size_t kl = strlen(k);
            if (kl > 5 && strcmp(k + kl - 5, ".core") == 0) {
                (*cores)++;
                if (on >= 13 && memcmp(o, "RTTIBin<1.73>", 13) == 0) (*cores_rtti)++;
            }
        }
        free(p); free(b); free(o);
    }
    strmap_free(&used);
    ovl_unlock(&g_lock);
    return applied;
}

/* ------------------------------------------------------------------ native patching namespace (pass 168) */

static int is_patch_file(const char* fn, char stem[33])
{
    size_t n = strlen(fn);
    if (n != 32 + 8) return 0;
    for (int i = 0; i < 8; i++) { char c = fn[32 + i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 0x20); if (c != ".pspatch"[i]) return 0; }
    for (int i = 0; i < 32; i++) {
        char c = fn[i];
        if (c >= 'a' && c <= 'f') c = (char)(c - 0x20);
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return 0;
        stem[i] = c;
    }
    stem[32] = 0;
    return 1;
}

int kz3_overlay_native_rekey(const char* src_dir, const char* dst_dir, const char* const* names, size_t count,
                             int* renamed)
{
    if (renamed) *renamed = 0;
    if (!src_dir || !dst_dir || (!names && count)) return -1;
    strmap orig = { 0 }, made = { 0 }, hostc = { 0 };
    char k[1100], name[33], loose[33], stem[33], sp[1200], dp[1200];
    for (unsigned i = 0; kz3_host_composed_key(i); i++) {
        kz3_pspatch_name(kz3_host_composed_key(i), name);
        strmap_put(&hostc, name, 1);
    }
    for (size_t i = 0; i < count; i++) {
        if (!names[i] || !*names[i]) continue;
        norm(names[i], k, sizeof k);
        if (!*k) continue;
        kz3_pspatch_name(k, name);
        strmap_put(&orig, name, (uint32_t)i);                  /* case variants share one key: first wins */
    }
    snprintf(dp, sizeof dp, "%s/x", dst_dir);
    mkdirs(dp);
    ovl_mkdir(dst_dir);
    DIR* d = opendir(src_dir);
    if (!d) { strmap_free(&orig); strmap_free(&hostc); return -1; }
    int placed = 0, failed = 0;
    for (struct dirent* e; (e = readdir(d)) != NULL;) {
        if (!is_patch_file(e->d_name, stem)) continue;
        if (strmap_get(&hostc, stem, NULL)) continue;           /* host-composed (pass 38): no patch for the guest */
        uint32_t idx;
        const char* out_stem = stem;
        if (strmap_get(&orig, stem, &idx)) {
            norm(names[idx], k, sizeof k);
            kz3_pspatch_name_loose(k, loose);
            out_stem = loose;
        }
        snprintf(sp, sizeof sp, "%s/%s", src_dir, e->d_name);
        snprintf(dp, sizeof dp, "%s/%s.pspatch", dst_dir, out_stem);
        struct stat st;
        if (stat(sp, &st) != 0) { failed++; continue; }
        if (!file_size_is(dp, (uint64_t)st.st_size)) {
            size_t n = 0;
            uint8_t* b = slurp(sp, &n);
            int rc = b ? write_atomic(dp, b, n) : -1;
            free(b);
            if (rc != 0) { failed++; continue; }
        }
        strmap_put(&made, out_stem, 1);
        placed++;
        if (out_stem == loose && renamed) (*renamed)++;
    }
    closedir(d);
    /* Leftovers of an older update: the patch layer would list them too, so the table would differ from the PS3's. */
    if ((d = opendir(dst_dir)) != NULL) {
        for (struct dirent* e; (e = readdir(d)) != NULL;) {
            if (!is_patch_file(e->d_name, stem) || strmap_get(&made, stem, NULL)) continue;
            snprintf(dp, sizeof dp, "%s/%s", dst_dir, e->d_name);
            remove(dp);
        }
        closedir(d);
    }
    strmap_free(&orig);
    strmap_free(&made);
    strmap_free(&hostc);
    return failed ? -1 : placed;
}

/* Killzone 3 port: read a file that may be stored as FAT32 split parts as one contiguous file.
 *
 * A disc dump made on a FAT32 drive holds files over 4 GiB cut into <name>.66600, <name>.66601, ... in order
 * (data_ps3.psarc.66600 ... .66609 in the field report, public issue 4). kz3_sf_open(path) opens <path> when it
 * exists, else the consecutive parts <path>.66600, .66601, ... (no gaps; the set ends at the first missing number).
 * Part sizes are taken from the files themselves, never assumed: offset -> part is a walk over the real sizes, and a
 * read that crosses a part boundary is split. Only one part is open at a time. Plain C, header only; the archive
 * readers in port/kz3/psarc_vfs.cpp and port/vfs/kz3_overlay.c use it in place of FILE* (SEEK_SET only).
 * Host test: port/vfs/test_splitfile.sh. Not thread safe: callers hold their own lock (as they did for FILE*). */
#ifndef KZ3_SPLITFILE_H
#define KZ3_SPLITFILE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define KZ3_SF_SEEK _fseeki64
#else
#include <sys/types.h>
#define KZ3_SF_SEEK fseeko
#endif

#define KZ3_SF_FIRST_PART 66600   /* the FAT32 splitter's first suffix */
#define KZ3_SF_MAX_PARTS 4096

typedef struct {
    int nparts;
    char** paths;           /* nparts file names */
    uint64_t* start;        /* nparts + 1 cumulative offsets; start[nparts] = total size */
    uint64_t pos;           /* logical read position */
    FILE* f;                /* the open part, or NULL */
    int cur;                /* index of the open part */
    uint64_t fpos;          /* the open part's own position (to skip needless seeks) */
} kz3_sf;

static inline int kz3_sf_file_size_(const char* p, uint64_t* out)
{
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    int ok = KZ3_SF_SEEK(f, 0, SEEK_END) == 0;
    int64_t n = 0;
#ifdef _WIN32
    if (ok) n = _ftelli64(f);
#else
    if (ok) n = (int64_t)ftello(f);
#endif
    fclose(f);
    if (!ok || n < 0) return 0;
    *out = (uint64_t)n;
    return 1;
}

static inline void kz3_sf_close(kz3_sf* s)
{
    if (!s) return;
    if (s->f) fclose(s->f);
    for (int i = 0; i < s->nparts; i++) free(s->paths[i]);
    free(s->paths); free(s->start); free(s);
}

/* NULL when neither <path> nor <path>.66600 can be opened. */
static inline kz3_sf* kz3_sf_open(const char* path)
{
    kz3_sf* s = (kz3_sf*)calloc(1, sizeof *s);
    if (!s) return NULL;
    size_t pl = strlen(path);
    uint64_t sz;
    if (kz3_sf_file_size_(path, &sz)) {
        s->paths = (char**)calloc(1, sizeof(char*));
        s->start = (uint64_t*)calloc(2, sizeof(uint64_t));
        if (!s->paths || !s->start) { kz3_sf_close(s); return NULL; }
        s->paths[0] = strdup(path);
        s->start[1] = sz;
        s->nparts = 1;
        return s;
    }
    s->paths = (char**)calloc(KZ3_SF_MAX_PARTS, sizeof(char*));
    s->start = (uint64_t*)calloc(KZ3_SF_MAX_PARTS + 1, sizeof(uint64_t));
    if (!s->paths || !s->start) { kz3_sf_close(s); return NULL; }
    for (int i = 0; i < KZ3_SF_MAX_PARTS; i++) {
        char* p = (char*)malloc(pl + 16);
        if (!p) break;
        snprintf(p, pl + 16, "%s.%d", path, KZ3_SF_FIRST_PART + i);
        if (!kz3_sf_file_size_(p, &sz)) { free(p); break; }
        s->paths[i] = p;
        s->start[i + 1] = s->start[i] + sz;
        s->nparts = i + 1;
    }
    if (s->nparts == 0) { kz3_sf_close(s); return NULL; }
    return s;
}

static inline uint64_t kz3_sf_size(const kz3_sf* s) { return s->start[s->nparts]; }

/* SEEK_SET only. Seeking past the end is allowed (the next read returns 0). */
static inline int kz3_sf_seek(kz3_sf* s, int64_t off)
{
    if (off < 0) return -1;
    s->pos = (uint64_t)off;
    return 0;
}

/* fread(buf, 1, n, f) semantics: returns the bytes read (short only at the end of the last part or on an I/O error). */
static inline size_t kz3_sf_read(kz3_sf* s, void* buf, size_t n)
{
    uint8_t* o = (uint8_t*)buf;
    size_t done = 0;
    while (done < n && s->pos < kz3_sf_size(s)) {
        int i = 0;
        while (s->pos >= s->start[i + 1]) i++;
        uint64_t in_part = s->pos - s->start[i], left = s->start[i + 1] - s->pos;
        if (!s->f || s->cur != i) {
            if (s->f) fclose(s->f);
            s->f = fopen(s->paths[i], "rb");
            s->cur = i; s->fpos = 0;
            if (!s->f) return done;
        }
        if (s->fpos != in_part) {
            if (KZ3_SF_SEEK(s->f, (int64_t)in_part, SEEK_SET) != 0) return done;
            s->fpos = in_part;
        }
        size_t want = n - done;
        if ((uint64_t)want > left) want = (size_t)left;
        size_t got = fread(o + done, 1, want, s->f);
        done += got; s->pos += got; s->fpos += got;
        if (got != want) break;
    }
    return done;
}
#endif

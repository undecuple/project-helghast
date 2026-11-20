/* Host test for patch 1164 (and the staged edat_cache_size.diff): runs the patched edat.c's own edat_seek / edat_file_size
 * (their text is cut out of the patched source and #included as SEEK_SRC) against a sparse 3 GiB file with a marker at 2.5 GiB.
 * With -DSIM32 the seek and size are truncated to 32 bits as on Windows' long / _off_t (the negative control: must FAIL). */
#define _FILE_OFFSET_BITS 64
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include SEEK_SRC
int main(int argc, char** argv)
{
    const char* path = argv[1];
    const uint64_t at = 5ULL << 29, total = 3ULL << 30; /* 2.5 GiB, 3 GiB */
    FILE* f = fopen(path, "wb");
    if (!f) {
        puts("FAIL: cannot create");
        return 1;
    }
    if (fseeko(f, (off_t)at, SEEK_SET) != 0 || fwrite("KZ3EDAT!", 1, 8, f) != 8 ||
        fseeko(f, (off_t)(total - 1), SEEK_SET) != 0 || fputc(0, f) == EOF) {
        puts("SKIP: no sparse 3 GiB file here");
        fclose(f);
        remove(path);
        return 0;
    }
    fclose(f);
    int rc = 0;
    f = fopen(path, "rb");
    uint64_t off = at;
#ifdef SIM32
    off = (uint64_t)(int64_t)(int32_t)off; /* Windows long: 2.5 GiB is negative */
#endif
    char buf[8] = {0};
    if (edat_seek(f, off) != 0 || fread(buf, 1, 8, f) != 8 || memcmp(buf, "KZ3EDAT!", 8)) {
        puts("FAIL: seek to 2.5 GiB did not land on the marker");
        rc = 1;
    } else
        puts("ok: seek to 2.5 GiB reads the marker");
    fclose(f);
#ifdef HAVE_FILE_SIZE
    int64_t sz = edat_file_size(path);
#ifdef SIM32
    sz = (int64_t)(int32_t)sz;
#endif
    if (sz != (int64_t)total) {
        printf("FAIL: size %lld != %llu\n", (long long)sz, (unsigned long long)total);
        rc = 1;
    } else
        puts("ok: 64-bit file size of the 3 GiB file");
    if (edat_file_size("/nonexistent/x") != -1) {
        puts("FAIL: missing file must give -1");
        rc = 1;
    }
#endif
    remove(path);
    printf("RESULT edat rc=%d\n", rc);
    return rc;
}

/*
 * Host test for KZ3 patch 1139 (lv2 sys_fs open flags are the console values; the Killzone 2 port's patch 6036 is the same change).
 *
 * Includes the toolkit's runtime/syscalls/sys_fs.c and calls sys_fs_open (lv2 syscall 801) on a scratch tree mapped as
 * /dev_hdd0 (PS3_HDD0_ROOT), with the flag words a guest sends:
 *   - the CELL_FS_O_* defines equal the console values (CREAT 0x40, EXCL 0x80, TRUNC 0x200, APPEND 0x400) and the
 *     cellFs HLE header (libs/filesystem/cellFs.h) agrees with sys_fs.h;
 *   - 0x41 (WRONLY|CREAT), 0x42 (RDWR|CREAT) and 0x241 (WRONLY|CREAT|TRUNC, fopen "w") create a missing file;
 *   - 0x41 on an existing file keeps its contents (no truncation), 0x241 truncates it;
 *   - 0x401 (WRONLY|APPEND) on an existing file appends; 0x441 (fopen "a") creates and appends;
 *   - 0xC1 (WRONLY|CREAT|EXCL) fails with CELL_EEXIST on an existing file and creates a missing one;
 *   - 0x401 and 0x201 without O_CREAT on a missing file fail with CELL_ENOENT and create nothing;
 *   - 0x0 (read-only) still opens an existing file.
 * On a toolkit without the patch (the old hex set: CREAT 0x100, EXCL 0x400, APPEND 0x2000) the define checks fail and
 * 0x41/0x42 create nothing, 0x401 overwrites at offset 0: the negative control.
 *
 *   test_sys_fs_flags DIR      (DIR: an empty scratch directory)
 * Built and run by tools/runtime/test_sys_fs_flags.sh; no game data.
 */
#include "sys_fs.c"
static const unsigned SYS_CREAT = CELL_FS_O_CREAT, SYS_EXCL = CELL_FS_O_EXCL, SYS_TRUNC = CELL_FS_O_TRUNC,
                      SYS_APPEND = CELL_FS_O_APPEND,
                      SYS_ACC =
                          CELL_FS_O_RDONLY | CELL_FS_O_WRONLY << 4 | CELL_FS_O_RDWR << 8 | CELL_FS_O_ACCMODE << 12;
/* cellFs.h's values come from a second translation unit (test_sys_fs_flags.sh writes it): cellFs.h's CellFsStat
 * members clash with glibc's st_atime macros once <sys/stat.h> is in. */
extern const unsigned CFS_CREAT, CFS_EXCL, CFS_TRUNC, CFS_APPEND;

/* ---- link stubs: nothing on the paths this test drives needs more ---- */
uint8_t* vm_base = NULL;
uint32_t ppu_vm_size = 4096;
const char* edat_resolve(const char* host_path, char* buf, size_t cap)
{
    (void)buf;
    (void)cap;
    return host_path;
}

static int g_fail, g_pass;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (c) {                                                                                                       \
            g_pass++;                                                                                                  \
            printf("[PASS] ");                                                                                         \
        } else {                                                                                                       \
            g_fail++;                                                                                                  \
            printf("[FAIL] ");                                                                                         \
        }                                                                                                              \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
    } while (0)

static uint8_t g_mem[4096]; /* guest memory: path at 0x100, fd out at 0x10 */
static char g_root[600];

static int64_t open_guest(const char* name, uint32_t flags, int* fd)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    snprintf((char*)g_mem + 0x100, 512, "/dev_hdd0/%s", name);
    ctx.gpr[3] = 0x100;
    ctx.gpr[4] = flags;
    ctx.gpr[5] = 0x10;
    ctx.gpr[6] = 0666;
    int64_t r = sys_fs_open(&ctx);
    *fd = (int)((g_mem[0x10] << 24) | (g_mem[0x11] << 16) | (g_mem[0x12] << 8) | g_mem[0x13]);
    return r;
}

static void host_name(char* out, size_t cap, const char* name)
{
    snprintf(out, cap, "%s/%s", g_root, name);
}

static int exists(const char* name)
{
    char p[700];
    struct stat s;
    host_name(p, sizeof p, name);
    return stat(p, &s) == 0;
}

static void put(const char* name, const char* text)
{
    char p[700];
    host_name(p, sizeof p, name);
    FILE* f = fopen(p, "wb");
    if (!f) {
        perror(p);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static void get(const char* name, char* out, size_t cap)
{
    char p[700];
    host_name(p, sizeof p, name);
    out[0] = 0;
    FILE* f = fopen(p, "rb");
    if (!f) return;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    fclose(f);
}

/* Write `text` through the lv2 fd (sys_fs_write r3 = fd, r4 = buffer, r5 = size, r6 = nwrite out), then close. */
static void write_close(int fd, const char* text)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    size_t n = strlen(text);
    memcpy(g_mem + 0x400, text, n);
    ctx.gpr[3] = (uint64_t)fd;
    ctx.gpr[4] = 0x400;
    ctx.gpr[5] = n;
    ctx.gpr[6] = 0x20;
    sys_fs_write(&ctx);
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = (uint64_t)fd;
    sys_fs_close(&ctx);
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s DIR\n", argv[0]);
        return 2;
    }
    snprintf(g_root, sizeof g_root, "%s", argv[1]);
    setenv("PS3_HDD0_ROOT", g_root, 1);
    vm_base = g_mem;
    char buf[256];
    int fd;
    int64_t r;

    /* The defines: the console values, and sys_fs.h equal to cellFs.h (both name one header with the patch). */
    CHECK(SYS_CREAT == 0x40, "sys_fs.h CELL_FS_O_CREAT == 0x40 (is 0x%x)", SYS_CREAT);
    CHECK(SYS_EXCL == 0x80, "sys_fs.h CELL_FS_O_EXCL == 0x80 (is 0x%x)", SYS_EXCL);
    CHECK(SYS_TRUNC == 0x200, "sys_fs.h CELL_FS_O_TRUNC == 0x200 (is 0x%x)", SYS_TRUNC);
    CHECK(SYS_APPEND == 0x400, "sys_fs.h CELL_FS_O_APPEND == 0x400 (is 0x%x)", SYS_APPEND);
    CHECK(SYS_ACC == 0x3210, "sys_fs.h access modes RDONLY 0, WRONLY 1, RDWR 2, ACCMODE 3");
    CHECK(CFS_CREAT == SYS_CREAT && CFS_EXCL == SYS_EXCL && CFS_TRUNC == SYS_TRUNC && CFS_APPEND == SYS_APPEND,
          "cellFs.h agrees with sys_fs.h (cellFs.h CREAT 0x%x, EXCL 0x%x, TRUNC 0x%x, APPEND 0x%x)", CFS_CREAT,
          CFS_EXCL, CFS_TRUNC, CFS_APPEND);

    /* Create. */
    r = open_guest("c41.dat", 0x41, &fd);
    CHECK(r == 0 && exists("c41.dat"), "0x41 (WRONLY|CREAT) creates a missing file (r=0x%llx)", (unsigned long long)r);
    if (r == 0) write_close(fd, "abc");
    get("c41.dat", buf, sizeof buf);
    CHECK(strcmp(buf, "abc") == 0, "0x41 writes reach the new file ('%s')", buf);
    r = open_guest("c42.dat", 0x42, &fd);
    CHECK(r == 0 && exists("c42.dat"), "0x42 (RDWR|CREAT) creates a missing file (r=0x%llx)", (unsigned long long)r);
    if (r == 0) write_close(fd, "x");
    r = open_guest("c241.dat", 0x241, &fd);
    CHECK(r == 0 && exists("c241.dat"), "0x241 (WRONLY|CREAT|TRUNC) creates a missing file (r=0x%llx)",
          (unsigned long long)r);
    if (r == 0) write_close(fd, "w");

    /* CREAT without TRUNC keeps an existing file; TRUNC empties it. */
    put("keep.dat", "0123456789");
    r = open_guest("keep.dat", 0x41, &fd);
    if (r == 0) write_close(fd, "AB");
    get("keep.dat", buf, sizeof buf);
    CHECK(r == 0 && strcmp(buf, "AB23456789") == 0, "0x41 on an existing file writes at 0 without truncating ('%s')",
          buf);
    put("trunc.dat", "0123456789");
    r = open_guest("trunc.dat", 0x241, &fd);
    if (r == 0) write_close(fd, "AB");
    get("trunc.dat", buf, sizeof buf);
    CHECK(r == 0 && strcmp(buf, "AB") == 0, "0x241 on an existing file truncates ('%s')", buf);

    /* Append. */
    put("app.dat", "head");
    r = open_guest("app.dat", 0x401, &fd);
    if (r == 0) write_close(fd, "+tail");
    get("app.dat", buf, sizeof buf);
    CHECK(r == 0 && strcmp(buf, "head+tail") == 0, "0x401 (WRONLY|APPEND) appends ('%s', r=0x%llx)", buf,
          (unsigned long long)r);
    r = open_guest("app441.dat", 0x441, &fd);
    if (r == 0) write_close(fd, "one");
    r = open_guest("app441.dat", 0x441, &fd);
    if (r == 0) write_close(fd, "two");
    get("app441.dat", buf, sizeof buf);
    CHECK(strcmp(buf, "onetwo") == 0, "0x441 (fopen \"a\") creates, then appends ('%s')", buf);

    /* Exclusive create. */
    put("excl.dat", "keep");
    r = open_guest("excl.dat", 0xC1, &fd);
    get("excl.dat", buf, sizeof buf);
    CHECK(r == (int64_t)(int32_t)CELL_EEXIST && strcmp(buf, "keep") == 0,
          "0xC1 (WRONLY|CREAT|EXCL) on an existing file fails with CELL_EEXIST (r=0x%llx, '%s')", (unsigned long long)r,
          buf);
    r = open_guest("excl_new.dat", 0xC1, &fd);
    CHECK(r == 0 && exists("excl_new.dat"), "0xC1 creates a missing file (r=0x%llx)", (unsigned long long)r);
    if (r == 0) write_close(fd, "n");

    /* No O_CREAT: a missing file stays missing. */
    r = open_guest("no401.dat", 0x401, &fd);
    CHECK(r == (int64_t)(int32_t)CELL_ENOENT && !exists("no401.dat"),
          "0x401 on a missing file fails with CELL_ENOENT and creates nothing (r=0x%llx)", (unsigned long long)r);
    r = open_guest("no201.dat", 0x201, &fd);
    CHECK(r == (int64_t)(int32_t)CELL_ENOENT && !exists("no201.dat"),
          "0x201 (WRONLY|TRUNC) on a missing file fails with CELL_ENOENT and creates nothing (r=0x%llx)",
          (unsigned long long)r);

    /* Read-only is unchanged. */
    r = open_guest("app.dat", 0x0, &fd);
    CHECK(r == 0, "0x0 (RDONLY) opens an existing file (r=0x%llx)", (unsigned long long)r);
    if (r == 0) {
        ppu_context ctx;
        memset(&ctx, 0, sizeof ctx);
        ctx.gpr[3] = (uint64_t)fd;
        sys_fs_close(&ctx);
    }
    r = open_guest("missing.dat", 0x0, &fd);
    CHECK(r == (int64_t)(int32_t)CELL_ENOENT, "0x0 on a missing file is CELL_ENOENT (r=0x%llx)", (unsigned long long)r);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/* Host test for KZ3 patch 2078 (host file reads report the guest bytes they write).
 *
 * cellFsRead and cellFsAioRead fread() straight into guest memory, which no recompiled store or SPU DMA put sees, so
 * the live renderer's page bitmap missed them (the RE notes "Pass 138"). With 2078 both call the hook
 * set by fs_set_guest_write_hook after the copy, with the guest address and the bytes actually read. This test
 * compiles runtime/ppu/ppu_fs.cpp in (stubs as tools/upstream/test_cellfs_imports.cpp), puts a host file under a
 * descriptor and checks:
 *   - no hook installed: a read still works (the hook pointer starts null);
 *   - cellFsRead of 64 bytes at 0x1000: one hook call (0x1000, 64), the bytes landed;
 *   - a short read (request 4096, 100 bytes left): one call with n = 100, not the request;
 *   - cellFsAioRead of 32 bytes at offset 16 into 0x3000: one call (0x3000, 32);
 *   - a read at end of file (0 bytes): no call.
 * Build/run (no game data): nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_fs_write_hook.sh
 */
#include "runtime/ppu/ppu_fs.cpp"

extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
void ydkj_host_bt(const char* tag) { (void)tag; }
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*)) { (void)nid; (void)name; (void)fn; }
}
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

static int g_calls; static uint32_t g_ea, g_size;
static void hook(uint32_t ea, uint32_t size) { g_calls++; g_ea = ea; g_size = size; }
static void wbe32(uint32_t a, uint32_t v) { vm_write32(a, v); }
static void wbe64(uint32_t a, uint64_t v) { vm_write64(a, v); }

int main()
{
    static uint8_t mem[1 << 20];
    vm_base = mem; ppu_vm_size = sizeof mem;
    FILE* f = tmpfile();
    if (!f) { printf("[test_fs_write_hook] SKIP: no tmpfile\n"); return 0; }
    for (int i = 0; i < 200; i++) fputc((i * 7 + 3) & 0xFF, f);
    rewind(f);
    const int fd = 5;
    g_files[fd] = f;
    int fail = 0;
    ppu_context ctx;
    auto rd = [&](uint32_t buf, uint64_t n) {
        memset(&ctx, 0, sizeof ctx);
        ctx.gpr[3] = (uint64_t)fd; ctx.gpr[4] = buf; ctx.gpr[5] = n; ctx.gpr[6] = 0x800;
        cellFsRead(&ctx);
    };
    auto expect = [&](const char* what, int calls, uint32_t ea, uint32_t size) {
        const bool ok = g_calls == calls && (calls == 0 || (g_ea == ea && g_size == size));
        printf("[test_fs_write_hook] %-44s calls=%d ea=0x%X size=%u %s\n", what, g_calls, g_ea, g_size, ok ? "ok" : "WRONG");
        if (!ok) fail = 1;
        g_calls = 0; g_ea = g_size = 0;
    };

    rd(0x0F00, 4);                                   /* no hook yet */
    if (mem[0x0F00] != 3 || mem[0x0F03] != 24) { printf("[test_fs_write_hook] read without hook WRONG\n"); fail = 1; }
    fs_set_guest_write_hook(hook);
    rd(0x1000, 64);                                  /* bytes 4..67 */
    expect("cellFsRead 64 B", 1, 0x1000, 64);
    if (mem[0x1000] != ((4 * 7 + 3) & 0xFF)) { printf("[test_fs_write_hook] read bytes WRONG\n"); fail = 1; }
    rd(0x2000, 4096);                                /* 132 left: short read */
    expect("cellFsRead short (4096 asked, 132 left)", 1, 0x2000, 132);
    rd(0x2800, 16);                                  /* at end of file */
    expect("cellFsRead at EOF (0 B)", 0, 0, 0);

    wbe32(0x4000, (uint32_t)fd); wbe64(0x4008, 16); wbe32(0x4010, 0x3000); wbe64(0x4018, 32);
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = 0x4000; ctx.gpr[4] = 0x4100; ctx.gpr[5] = 0;   /* no callback */
    cellFsAioRead(&ctx);
    expect("cellFsAioRead 32 B at offset 16", 1, 0x3000, 32);
    if (mem[0x3000] != ((16 * 7 + 3) & 0xFF)) { printf("[test_fs_write_hook] aio bytes WRONG\n"); fail = 1; }

    fs_set_guest_write_hook(nullptr);
    printf("[test_fs_write_hook] %s\n", fail ? "FAIL" : "PASS");
    return fail;
}

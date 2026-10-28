/* Focused upstream test for patch 3002 (cellFs imports v0.13.0 leaves unregistered).
 *
 * ps3recomp v0.13.0 registers no handler for cellFsGetFreeSize, cellFsChmod, cellFsUtime and cellFsSdataOpenByFd, so a
 * title's calls take the unresolved-NID path: CELL_OK with out-parameters untouched (KZ3's FIOS2 cache asks for free
 * space and would read garbage). This test compiles runtime/ppu/ppu_fs.cpp in, captures every ps3_hle_register_ctx
 * call made by ppu_fs_register(), and for each of the four NIDs checks:
 *   - the NID is the hash of the function name (ps3_compute_nid), i.e. the registration is under the right NID;
 *   - a handler is registered;
 *   - GetFreeSize writes block_size 4096 and a non-zero free block count (big-endian) and returns CELL_OK;
 *   - Chmod and Utime return CELL_OK; SdataOpenByFd returns CELL_ENOSYS (0x80010003), not a silent OK.
 * Expected: FAIL on v0.13.0 (no handlers), PASS with 3002. Build/run: tools/upstream/test_cellfs_imports.sh
 */
#include "runtime/ppu/ppu_fs.cpp"
#include <vector>

/* ---- link stubs (none is on the path this test drives) ---- */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
void ydkj_host_bt(const char* tag) { (void)tag; }
}
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

struct Reg { uint32_t nid; const char* name; void (*fn)(ppu_context*); };
static std::vector<Reg> g_regs;
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*)) { g_regs.push_back({nid, name, fn}); }

static void (*find(uint32_t nid))(ppu_context*) { for (auto& r : g_regs) if (r.nid == nid) return r.fn; return nullptr; }
static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

int main()
{
    static uint8_t mem[1 << 20];
    vm_base = mem; ppu_vm_size = sizeof mem;
    ppu_fs_register();

    struct { uint32_t nid; const char* name; } want[] = {
        {0xAA3B4BCDu, "cellFsGetFreeSize"}, {0x99406D0Bu, "cellFsChmod"},
        {0xBEF554A4u, "cellFsUtime"},       {0x6D3BB15Bu, "cellFsSdataOpenByFd"},
    };
    int fail = 0;
    for (auto& w : want) {
        uint32_t h = ps3_compute_nid(w.name);
        bool reg = find(w.nid) != nullptr;
        printf("%-22s NID 0x%08X hash 0x%08X %s, handler %s\n", w.name, w.nid, h, h == w.nid ? "match" : "MISMATCH",
               reg ? "registered" : "MISSING");
        if (h != w.nid || !reg) fail = 1;
    }

    ppu_context ctx;
    if (auto f = find(0xAA3B4BCDu)) {                 /* GetFreeSize(path, u32* block_size, u64* free_blocks) */
        memset(&ctx, 0, sizeof ctx);
        strcpy((char*)mem + 0x100, "/dev_hdd0");
        memset(mem + 0x200, 0xEE, 16);
        ctx.gpr[3] = 0x100; ctx.gpr[4] = 0x200; ctx.gpr[5] = 0x208;
        f(&ctx);
        uint32_t bs = rd32(0x200); uint64_t fb = rd64(0x208);
        bool ok = ctx.gpr[3] == 0 && bs == 4096 && fb != 0 && fb != 0xEEEEEEEEEEEEEEEEull;
        printf("GetFreeSize: r3=0x%llX block_size=%u free_blocks=%llu %s\n", (unsigned long long)ctx.gpr[3], bs,
               (unsigned long long)fb, ok ? "ok" : "WRONG");
        if (!ok) fail = 1;
    }
    struct { uint32_t nid; const char* name; uint32_t ret; } rets[] = {
        {0x99406D0Bu, "Chmod", 0}, {0xBEF554A4u, "Utime", 0}, {0x6D3BB15Bu, "SdataOpenByFd", 0x80010003u}};
    for (auto& r : rets) if (auto f = find(r.nid)) {
        memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100;
        f(&ctx);
        bool ok = (uint32_t)ctx.gpr[3] == r.ret;
        printf("%s: r3=0x%08X want 0x%08X %s\n", r.name, (uint32_t)ctx.gpr[3], r.ret, ok ? "ok" : "WRONG");
        if (!ok) fail = 1;
    }
    printf("%s\n", fail ? "FAIL" : "PASS");
    return fail;
}

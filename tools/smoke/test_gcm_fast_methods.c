/* Host test for KZ3 patch 2149 (GCM_FAST_METHODS): the walker's method fast path hands live draw, the draw engine and the
 * legacy state exactly the words, in exactly the order, of the per-word loop. A synthetic ring of random increment and
 * non-increment headers (subchannels 0 and 1, 1-40 words, some crossing the 1 MB IO page edges) is walked; the stream of
 * rsx_live_draw_method / rsx_draw_engine_method / rsx_process_method calls is hashed. The script runs this twice (switch
 * off, on) and compares the digests. Stubs follow the toolkit's test_gcm_legacy_state_filter.c; no game data, no GPU.
 *   argv: fast|slow  vtable|novtable  live|nolive */
#include "cellGcmSys.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
ps3_guest_caller_fn g_ps3_guest_caller;
PPU_THREAD_LOCAL ppu_context* g_active_ctx;
u32 g_rsx_last_reference;
void (*g_spu_put_hook)(uint32_t ea, uint32_t size);
void ppu_dump_bctrl_ring(uint32_t n, const char* why) { (void)n; (void)why; }
void ppu_dump_guest_stack(ppu_context* c, const char* why) { (void)c; (void)why; }
void rsx_state_init(rsx_state* state) { (void)state; }

static unsigned long long digest = 1469598103934665603ull;
static unsigned long n_live, n_eng, n_legacy;
static int live_on, vtable_on;
static void mix(u32 tag, u32 a, u32 b)
{
    const u32 w[3] = { tag, a, b };
    for (int i = 0; i < 3; i++) digest = (digest ^ w[i]) * 1099511628211ull;
}
int  rsx_process_method(rsx_state* state, u32 method, u32 data) { (void)state; n_legacy++; mix(1, method, data); return 0; }
int  rsx_draw_engine_enabled(void) { return live_on; }
rsx_backend* rsx_get_backend(void) { static rsx_backend dummy; return vtable_on ? &dummy : NULL; }
int  rsx_process_vp_run(rsx_state* state, u32 method, const u8* src, u32 count)
{ (void)state; (void)method; (void)src; (void)count; return 0; }
void rsx_draw_engine_method(u32 method, u32 arg) { n_eng++; mix(2, method, arg); }
void rsx_draw_engine_note_flip(u32 buffer_id) { (void)buffer_id; }
void rsx_draw_engine_guest_read(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
void rsx_draw_engine_guest_wrote(u32 l, u32 o, u32 p, u32 w, u32 h) { (void)l; (void)o; (void)p; (void)w; (void)h; }
int  rsx_live_draw_enabled(void) { return live_on; }
void rsx_live_draw_method(u32 method, u32 arg) { n_live++; mix(3, method, arg); }
int  rsx_live_draw_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 w, u32 h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)w; (void)h; return 0; }
void rsx_live_draw_note_resolve(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
int  rsx_live_draw_resolve_blit(u32 a, u32 b, u32 c, u32 d, u32 e, u32 f, u32 g, u32 h,
                                u32 i, u32 j, u32 k, u32 l)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; (void)j; (void)k; (void)l; return 0; }
void rsx_raise_user_cmd(uint32_t arg) { (void)arg; }

#define IO_ADDR   0x00100000u
#define IO_SIZE   0x00300000u
#define CMD_SIZE  0x00300000u
static uint32_t g_heap = 0x00500000u;
static u32 host_alloc(u32 size, u32 align)
{
    if (!align) align = 4;
    g_heap = (g_heap + align - 1u) & ~(align - 1u);
    u32 r = g_heap;
    g_heap += size;
    return r;
}
static void gw32(uint32_t ea, uint32_t v)
{
    uint8_t* p = vm_base + ea;
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void host_w32(u32 ea, u32 v) { gw32(ea, v); }
static uint32_t rng = 777u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

int main(int argc, char** argv)
{
    if (argc < 4) { puts("usage: fast|slow vtable|novtable live|nolive"); return 2; }
    if (!strcmp(argv[1], "fast")) setenv("GCM_FAST_METHODS", "1", 1); else unsetenv("GCM_FAST_METHODS");
    vtable_on = !strcmp(argv[2], "vtable"); live_on = !strcmp(argv[3], "live");
    unsetenv("RSX_TRACE");
    setvbuf(stdout, NULL, _IONBF, 0);
    const uint32_t vm_size = ppu_hle_inject_base + 0x10000u;
    vm_base = (uint8_t*)calloc(1, vm_size);
    if (!vm_base) { puts("FAIL: guest VM alloc"); return 1; }
    ppu_vm_size = vm_size;
    u32 ctx_out = host_alloc(4, 4);
    (void)cellGcmSetupContext(ctx_out, CMD_SIZE, IO_SIZE, IO_ADDR, host_alloc, host_w32);
    const uint32_t ctrl = (uint32_t)(uintptr_t)cellGcmGetControlRegister();

    /* Registers that are plain state for the walker: no reference/label/notify/flip/NV0039 effects. */
    static const u32 regs[] = { 0x0200u, 0x0208u, 0x0A00u, 0x0A74u, 0x1000u, 0x1680u, 0x1700u, 0x1EFCu, 0x1F00u, 0x1740u, 0x1E94u };
    u32 io = 0, cmds = 0, straddle = 0;
    while (io < 0x2F0000u) {
        const u32 count = 1 + rnd() % 40, subch = (rnd() % 5 == 0) ? 1u : 0u;
        const u32 method = regs[rnd() % (sizeof regs / sizeof regs[0])];
        const u32 type = (rnd() % 4 == 0) ? 0x40000000u : 0u;   /* non-increment: every word to the same method */
        if (((io + 4) >> 20) != ((io + count * 4) >> 20)) straddle++;
        gw32(IO_ADDR + io, type | (count << 18) | (subch << 13) | method);
        for (u32 i = 0; i < count; i++) gw32(IO_ADDR + io + 4 + i * 4, rnd());
        io += 4 + count * 4;
        cmds++;
    }
    s_fifo_getoff = 0;
    vm_write32(ctrl + 4, 0);
    vm_write32(ctrl + 0, io);
    for (int pass = 0; pass < 200 && s_fifo_getoff < io; pass++) cellGcm_rsx_process_fifo();
    printf("[fast_methods] %s %s %s: cmds %u straddling %u walked %s live %lu eng %lu legacy %lu digest %016llx\n", argv[1], argv[2],
           argv[3], cmds, straddle, s_fifo_getoff >= io ? "all" : "PARTIAL", n_live, n_eng, n_legacy, digest);
    free(vm_base);
    return s_fifo_getoff >= io && straddle >= 1 && (n_legacy || n_live) ? 0 : 1;
}

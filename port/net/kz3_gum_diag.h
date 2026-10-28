/*
 * KZ3 port, network pass 29: who raises GUM error 20000 (the "network error" page after a settings sub-page).
 * Included by kz3_net_hooks.h (compiled inside port/spu/kz3_ppu_overrides.cpp); installed once from the first
 * Localizer lookup, like the Button prompts page hook. v1.00 and 1.14 (network pass 41: a table per EBOOT, picked by
 * six instruction words; addresses below are v1.00, the 1.14 ones are in s_gd_tables).
 *
 * The game's error word 0x014DEF34 (1.14 0x015545B4; checked each frame by 0x004C8874, which then opens connect/networkerror.gum) gets
 * 20000 from the menu's request listener (vtable 0x00EAB5D0, a Menu base; notes s.25):
 *   0x00106C50 / 0x00106CF0  request done   (this = Menu / Menu+8): entry -> 0x0010661C, the GUM document build;
 *                            the build stores 20000 itself (@0x0010676C) when the DOM has no root (parse failure)
 *   0x00106D94 / 0x00106E14  request failed (this = Menu / Menu+8): stores 20000 (@0x00106DFC / @0x00106E80)
 * Both pairs look r4 (the request) up in the Menu's pending list (count this+0x28 / +0x20, array +0x2C / +0x24). The
 * request's +4/+8/+0xC/+0x14/+0x18 are Strings
 * (copied by 0x0001470C in 0x0010661C): +4 the URI "file:///common/displaysettings.gum", +8 its
 * path, +0x14 the scheme "file" (run net-r29-disp; +0xC/+0x18 empty there), +0x24 its data stream.
 * The other four 20000 stores (0x00102158(20000) at 0x007E0C4C, 0x007E0D4C, 0x007E22C0, 0x007E2814) are in
 * NetClanManager::vf4 0x007E0BA4 / vf5 0x007E0D38 and NetMailClanInvite::vf8 0x007E2210 / vf9 0x007E2800 (online clan
 * and mail requests); they are wrapped too and log only when the word became 20000 inside them.
 * All eight are virtual (called through the vtable), so ppu_register_function's indirect-call map reaches them.
 *
 * Logging: every failed request, and every done request after which the word became 20000, prints
 *   [gum-req] <which> req=0x.. lr=0x.. err 0x.. -> 0x..: "<string>" "<string>" ...
 * (at most 32 lines). KZ3_GUM_TRACE=1 also prints every done request (at most 400 lines): the page and data
 * requests of a menu session, for comparing a good run with a failing one. KZ3_GUM_DIAG=0 does not install it.
 */
#ifndef KZ3_GUM_DIAG_H
#define KZ3_GUM_DIAG_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Lift symbols: ppu_register_function (ppu_loader.cpp) and function_table/function_table_count (ppu_recomp.h, the
 * lifted image's table); the host test tools/net/test_kz3_net.c defines stubs of all three. */
#ifdef __cplusplus
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
#else
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
#endif

/* One table per EBOOT (picked by kz3_gum_diag_install from six instruction words: the three `li rX,20000` and the
 * three `stw` into the error word that follow them). Slots 0-3 are the menu request listener (required); the rest
 * are the online managers' 0x00102158(20000) / 1.14 0x0010418C(20000) callers, wrapped when lifted. Only calls that
 * go through the indirect-call map reach a wrapper (the listener slots and the vfN are virtual; the unnamed ones may be
 * called directly, then the wrapper never runs). Found by scanning every `bl setter` with `li r3,0x4E20` before it and
 * every `stw` into the error word (network pass 41, notes s.35). */
#define KZ3_GD_MAX 13
typedef struct {
    const char* ver;
    uint32_t err_word;
    uint32_t check_at[6], check_word[6];
    int n;
    uint32_t addr[KZ3_GD_MAX];
    const char* name[KZ3_GD_MAX];
} Kz3GdTable;
static const Kz3GdTable s_gd_tables[2] = {
    { "v1.00", 0x014DEF34u,
      { 0x0010676Cu, 0x00106778u, 0x00106DFCu, 0x00106E00u, 0x00106E80u, 0x00106E84u },
      { 0x38A04E20u, 0x90A4EF34u, 0x38804E20u, 0x9083EF34u, 0x38804E20u, 0x9083EF34u },
      10,
      { 0x00106C50u, 0x00106CF0u, 0x00106D94u, 0x00106E14u,
        0x007E0BA4u, 0x007E0D38u, 0x007E0D70u, 0x007E2210u, 0x007E2800u, 0x007E2838u },
      { "done", "done(+8)", "FAILED", "FAILED(+8)", "NetClanManager::vf4", "NetClanManager::vf5", "0x7E0D70",
        "NetMailClanInvite::vf8", "NetMailClanInvite::vf9", "0x7E2838" } },
    { "1.14", 0x015545B4u,
      { 0x001089ACu, 0x001089B8u, 0x0010903Cu, 0x00109040u, 0x001090C0u, 0x001090C4u },
      { 0x38A04E20u, 0x90A445B4u, 0x38804E20u, 0x908345B4u, 0x38804E20u, 0x908345B4u },
      13,
      { 0x00108E90u, 0x00108F30u, 0x00108FD4u, 0x00109054u,
        0x007FFE74u, 0x008002A8u, 0x008002E0u, 0x00801898u, 0x00801B94u, 0x00801BCCu,
        0x00802014u, 0x00802604u, 0x0080263Cu },
      { "done", "done(+8)", "FAILED", "FAILED(+8)", "NetClanManager::vf4", "NetClanManager::vf5", "0x8002E0",
        "NetMailPartyInvite::vf8", "NetMailPartyInvite::vf9", "0x801BCC",
        "NetMailClanInvite::vf8", "NetMailClanInvite::vf9", "0x80263C" } },
};
static const Kz3GdTable* s_gd;
static void (*s_gd_orig[KZ3_GD_MAX])(ppu_context*);
static int s_gd_trace, s_gd_lines, s_gd_tlines;

/* A guest String { u32 p } -> { refcount, ?, length @+8, ?, text @+0x10 } as printable text (or "" if not one). */
static void kz3_gd_string(uint32_t sp, char* out, size_t cap)
{
    out[0] = 0;
    if (!KZ3_NET_GUEST_PTR_OK(sp)) return;
    uint32_t p = vm_read32(sp);
    if (!KZ3_NET_GUEST_PTR_OK(p)) return;
    uint32_t n = vm_read32(p + 8);
    if (n == 0 || n > 4096) return;
    size_t k = 0;
    for (uint32_t i = 0; i < n && k + 1 < cap; i++) {
        unsigned char c = vm_base[p + 0x10 + i];
        if (c < 0x20 || c > 0x7E) { out[0] = 0; return; }   /* not a String */
        out[k++] = (char)c;
    }
    out[k] = 0;
}

static void kz3_gd_call(int w, ppu_context* ctx)
{
    const uint32_t self = (uint32_t)ctx->gpr[3], req = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;
    const uint32_t e0 = vm_read32(s_gd->err_word);
    s_gd_orig[w](ctx);
    const uint32_t e1 = vm_read32(s_gd->err_word);
    const int fail = w >= 2 && w < 4, became = e1 == 20000u && e0 != 20000u;
    if (w >= 4 && !became) return;   /* the online managers: only when they set it */
    if (!((fail || became) ? s_gd_lines++ < 32 : (s_gd_trace && s_gd_tlines++ < 400))) return;
    char s[5][160];
    static const uint32_t offs[5] = { 4, 8, 0xC, 0x14, 0x18 };
    for (int i = 0; i < 5; i++) {
        s[i][0] = 0;
        if (w < 4 && KZ3_NET_GUEST_PTR_OK(req)) kz3_gd_string(req + offs[i], s[i], sizeof s[i]);
    }
    KZ3_NET_PRINTF("[gum-req] %s req=0x%08X lr=0x%08X, err 0x%X -> 0x%X%s: \"%s\" \"%s\" \"%s\" \"%s\" \"%s\"\n",
                   s_gd->name[w], req, lr, e0, e1, became ? " (20000 SET HERE)" : "",
                   s[0], s[1], s[2], s[3], s[4]);
    fflush(stdout);
}
static void kz3_gd_0(ppu_context* c) { kz3_gd_call(0, c); }
static void kz3_gd_1(ppu_context* c) { kz3_gd_call(1, c); }
static void kz3_gd_2(ppu_context* c) { kz3_gd_call(2, c); }
static void kz3_gd_3(ppu_context* c) { kz3_gd_call(3, c); }
static void kz3_gd_4(ppu_context* c) { kz3_gd_call(4, c); }
static void kz3_gd_5(ppu_context* c) { kz3_gd_call(5, c); }
static void kz3_gd_6(ppu_context* c) { kz3_gd_call(6, c); }
static void kz3_gd_7(ppu_context* c) { kz3_gd_call(7, c); }
static void kz3_gd_8(ppu_context* c) { kz3_gd_call(8, c); }
static void kz3_gd_9(ppu_context* c) { kz3_gd_call(9, c); }
static void kz3_gd_10(ppu_context* c) { kz3_gd_call(10, c); }
static void kz3_gd_11(ppu_context* c) { kz3_gd_call(11, c); }
static void kz3_gd_12(ppu_context* c) { kz3_gd_call(12, c); }

static void kz3_gum_diag_install(void)
{
    const char* e = getenv("KZ3_GUM_DIAG");
    if (e && !strcmp(e, "0")) return;
    s_gd = NULL;
    for (int t = 0; t < 2 && !s_gd; t++) {
        int ok = 1;
        for (int k = 0; k < 6; k++)
            if (vm_read32(s_gd_tables[t].check_at[k]) != s_gd_tables[t].check_word[k]) { ok = 0; break; }
        if (ok) s_gd = &s_gd_tables[t];
    }
    if (!s_gd) {
        KZ3_NET_PRINTF("[gum-req] not installed (neither the v1.00 nor the 1.14 listener)\n");
        fflush(stdout);
        return;
    }
    void (*fns[KZ3_GD_MAX])(ppu_context*) = { kz3_gd_0, kz3_gd_1, kz3_gd_2, kz3_gd_3, kz3_gd_4, kz3_gd_5, kz3_gd_6,
                                              kz3_gd_7, kz3_gd_8, kz3_gd_9, kz3_gd_10, kz3_gd_11, kz3_gd_12 };
    int nw = 0;
    for (int i = 0; i < s_gd->n; i++) {
        s_gd_orig[i] = NULL;
        for (uint64_t k = 0; k < function_table_count; k++)
            if ((uint32_t)function_table[k].addr == s_gd->addr[i]) { s_gd_orig[i] = function_table[k].func; break; }
        if (!s_gd_orig[i] && i < 4) {   /* the listener is the point: without it, nothing */
            KZ3_NET_PRINTF("[gum-req] 0x%08X not lifted: not installed\n", s_gd->addr[i]);
            fflush(stdout);
            s_gd = NULL;
            return;
        }
    }
    e = getenv("KZ3_GUM_TRACE");
    s_gd_trace = e && *e && strcmp(e, "0");
    for (int i = 0; i < s_gd->n; i++)
        if (s_gd_orig[i]) { ppu_register_function(s_gd->addr[i], fns[i]); nw++; }
    KZ3_NET_PRINTF("[gum-req] %s: watching error word 0x%08X: the menu request listener (0x%X/0x%X done, 0x%X/0x%X failed) and %d of %d online 20000 stores%s\n",
                   s_gd->ver, s_gd->err_word, s_gd->addr[0], s_gd->addr[1], s_gd->addr[2], s_gd->addr[3], nw - 4,
                   s_gd->n - 4, s_gd_trace ? ", KZ3_GUM_TRACE on" : "");
    fflush(stdout);
}

#endif /* KZ3_GUM_DIAG_H */

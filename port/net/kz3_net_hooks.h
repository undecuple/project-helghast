/*
 * KZ3 port, net: PPU override glue for the online path (pass 9). Included by port/spu/kz3_ppu_overrides.cpp
 * inside `#ifdef PPU_OVERRIDE_<ADDR>` (the row goes in port/spu/ppu_overrides.txt), like kz3_gameplay.h.
 *
 * KZ3_HOOK_REGION_LABEL(ADDR, ASSIGN): the region label of online.ini (`region = ...`, kz3_net.h) in the game's
 * region list. The list row's label is the localisation key "menu_region_<id>" (regionselect.gum,
 * `@key{menu_region_@data{id/text()}}`), so the hook sits on the Localizer lookup:
 *   ADDR   Localizer lookup(this r3, const String* key r4, WString* out r5) -> r3 1 found / 0 not found
 *          (v1.00 0x0031B9F0; 1.14 0x00327620, ASSUMPTION: masked-byte match of the v1.00 body).
 *          String = { u32 p }, p -> { u32 refcount, u32, u32 length @+8, u32, char text[] @+0x10 }.
 *   ASSIGN WString::assign(WString* r3, const String* r4) (v1.00 0x00023FF8, 1.14 0x000244C4, ASSUMPTION as above):
 *          reads p+8 and p+0x10 and decodes the UTF-8 into the 16-bit string (@0x22684); never touches the refcount.
 * After the game's own lookup, a key "menu_region_<digits>" gets the configured label when the player is online and
 * a label is set (kz3_net_region_label_for_key); everything else is unchanged. The label is written below the
 * hook's r1 as a String header plus text (the callee's frame goes below that), r1/r2/lr are restored, r3 = 1.
  *
 * Pass 24 (stage 2), same hook: the text override table (kz3_text_overrides.h, from port/text/d83_overrides.tsv
 * embedded at build time with #embed; a tree without the file, e.g. the public export, builds with an empty table).
 * When the lookup found the key, a key of the table gets its text read from the result WString (WString = { u32 p },
 * p -> { u32 refcount, u32 hash (-1 = not computed), u32 length (UTF-16 units) @+8, u32 capacity @+0xC,
 * u16 text[] @+0x10 }, big-endian; from 0x22684 / 0x2251C), hashed and, on a match, replaced: ASSIGN is called with a
 * String of as many 'x' bytes as the replacement has UTF-16 units (it allocates a fresh buffer of refcount 1, the
 * table's string is shared and so never written: 0x2251C copies when refcount != 1), then the units are written over
 * it. (ASSIGN copies bytes as units and turns 0x01 lo hi into one unit, so it cannot carry UTF-8 itself.) The motion
 * tip without motion sensors is looked up again under KZ3_TX_MOTION_ALT_KEY. KZ3_TEXT_OVERRIDES=0 turns this off.
 * The same first lookup installs the Button prompts row's page hook (kz3_prompts_ui.h).
 */

#ifndef KZ3_NET_HOOKS_H
#define KZ3_NET_HOOKS_H

#include "kz3_net.h"
#include "kz3_text_overrides.h"
#define KZ3_PROMPTS_UI_GLUE
#include "kz3_prompts_ui.h"
#include <stdio.h>
#include <string.h>

#ifndef KZ3_NET_GUEST_PTR_OK
#define KZ3_NET_GUEST_PTR_OK(p) ((uint32_t)(p) >= 0x10000u && (uint32_t)(p) < 0xF0000000u)
#endif
#include "kz3_gum_diag.h"   /* pass 29: who raises GUM error 20000 */

#ifdef __cplusplus
extern "C" {
#endif
int pad_prompt_motion(void);   /* cellPad.c (patch 4011): the device in use has motion sensors */
#ifdef __cplusplus
}
#endif

#if defined(__has_embed)
#  if __has_embed("../text/d83_overrides.tsv")
static const unsigned char kz3_tx_tsv[] = {
#    embed "../text/d83_overrides.tsv"
};
#    define KZ3_TX_HAVE_TSV 1
#  endif
#endif

static Kz3TxTable s_kz3_tx;
static int s_kz3_tx_state;   /* 0 not parsed, 1 parsing, 2 ready, 3 off */

/* main.cpp's Keyboard-page Lua bridge exists on this EBOOT (the check kz3_kb_bridge_install makes). */
static int kz3_net_kb_bridge_known(void)
{
    static const uint32_t v[][2] = { { 0x004D1A08u, 0x00BA5744u }, { 0x004DF120u, 0x00BB0F14u } };
    for (unsigned i = 0; i < 2; i++)
        if ((vm_read32(v[i][0]) >> 16) == 0xF821u && (vm_read32(v[i][1]) >> 16) == 0x3063u) return 1;
    return 0;
}

static const Kz3TxTable* kz3_net_tx_table(void)
{
    int st = __atomic_load_n(&s_kz3_tx_state, __ATOMIC_ACQUIRE);
    if (st == 2) return &s_kz3_tx;
    if (st != 0) return NULL;
    int zero = 0;
    if (!__atomic_compare_exchange_n(&s_kz3_tx_state, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return NULL;
    kz3_prompts_ui_install(kz3_net_kb_bridge_known());
    kz3_gum_diag_install();
    const char* e = getenv("KZ3_TEXT_OVERRIDES");
    if (e && !strcmp(e, "0")) {
        KZ3_NET_PRINTF("[text] overrides off (KZ3_TEXT_OVERRIDES=0)\n");
        fflush(stdout);
        __atomic_store_n(&s_kz3_tx_state, 3, __ATOMIC_RELEASE);
        return NULL;
    }
#ifdef KZ3_TX_HAVE_TSV
    kz3_tx_parse(&s_kz3_tx, (const char*)kz3_tx_tsv, sizeof kz3_tx_tsv);
#endif
    KZ3_NET_PRINTF("[text] %u override row(s), %d rejected, %d conflicting key/hash pair(s) dropped%s\n", s_kz3_tx.nrows,
           s_kz3_tx.rejected, s_kz3_tx.conflicts,
#ifdef KZ3_TX_HAVE_TSV
           ""
#else
           " (built without port/text/d83_overrides.tsv)"
#endif
    );
    fflush(stdout);
    __atomic_store_n(&s_kz3_tx_state, 2, __ATOMIC_RELEASE);
    return &s_kz3_tx;
}

/* A String { u32 p } -> { refcount 1, 0, length, 0, bytes[] } in the guest frame at f (header at f+0x100). */
static uint32_t kz3_net_put_string(uint32_t f, const char* bytes, size_t n, char fill)
{
    uint32_t obj = f + 0x100, hdr = f + 0x110;
    vm_write32(obj, hdr);
    vm_write32(hdr + 0, 1);
    vm_write32(hdr + 4, 0);
    vm_write32(hdr + 8, (uint32_t)n);
    vm_write32(hdr + 12, 0);
    if (bytes) memcpy(vm_base + hdr + 0x10, bytes, n);
    else memset(vm_base + hdr + 0x10, fill, n);
    vm_base[hdr + 0x10 + n] = 0;
    return obj;
}

/* Call a guest function from the hook on a frame below the hook's r1; r1/r2/lr restored afterwards. */
static void kz3_net_call(ppu_context* ctx, void (*fn)(ppu_context*), uint32_t f, uint64_t sp, uint64_t toc, uint64_t lr)
{
    ctx->gpr[1] = f;
    ctx->lr = 0;
    fn(ctx);
    while (g_trampoline_fn) { void (*t_)(void*) = g_trampoline_fn; g_trampoline_fn = 0; t_(ctx); }
    ctx->gpr[1] = sp;
    ctx->gpr[2] = toc;
    ctx->lr = lr;
}

/* After the game's lookup (r3 = its result): the region label, else the text overrides. */
static void kz3_net_localizer_post(ppu_context* ctx, uint64_t this_, uint32_t key, uint32_t out, uint64_t sp, uint64_t toc,
                                   uint64_t lr, void (*lookup)(ppu_context*), void (*assign)(ppu_context*))
{
    if (!vm_base || !KZ3_NET_GUEST_PTR_OK(key) || !KZ3_NET_GUEST_PTR_OK(out)) return;
    uint32_t p = vm_read32(key);
    if (!KZ3_NET_GUEST_PTR_OK(p)) return;
    uint32_t n = vm_read32(p + 8);
    char k[KZ3_TX_KEY_MAX + 1];
    if (n == 0 || n > KZ3_TX_KEY_MAX) return;
    memcpy(k, vm_base + p + 0x10, n);
    k[n] = 0;
    const char* label = n < 32 ? kz3_net_region_label_for_key(k, n) : NULL;
    if (label) {
        size_t ll = strlen(label);
        if (ll > KZ3_NET_LABEL_MAX_BYTES) return;
        uint32_t f = (uint32_t)((sp - 0x200) & ~(uint64_t)0xF);   /* 0x200 below r1: hdr at f+0x100 */
        uint32_t obj = kz3_net_put_string(f, label, ll, 0);
        ctx->gpr[3] = out;
        ctx->gpr[4] = obj;
        kz3_net_call(ctx, assign, f, sp, toc, lr);
        ctx->gpr[3] = 1;
        return;
    }
    if ((uint32_t)ctx->gpr[3] != 1) return;              /* not found: the game's own fallback */
    const Kz3TxTable* t = kz3_net_tx_table();
    if (!t || !kz3_tx_has_key(t, k, n)) return;
    uint32_t q = vm_read32(out);
    if (!KZ3_NET_GUEST_PTR_OK(q)) return;
    uint32_t len = vm_read32(q + 8);
    if (len > KZ3_TX_TEXT_MAX) return;
    uint16_t orig[KZ3_TX_TEXT_MAX];
    uint32_t m = 0;
    for (; m < len; m++) {
        uint16_t c = vm_read16(q + 0x10 + 2 * m);
        if (!c) break;
        orig[m] = c;
    }
    const uint16_t* rep;
    size_t rn;
    int act = kz3_tx_decide(t, k, n, orig, m, pad_prompt_motion(), &rep, &rn);
    uint32_t f = (uint32_t)((sp - 0x900) & ~(uint64_t)0xF);   /* hdr at f+0x110, up to 0x400 bytes of text */
    if (act == KZ3_TX_OTHER_KEY) {
        uint32_t obj = kz3_net_put_string(f, KZ3_TX_MOTION_ALT_KEY, sizeof KZ3_TX_MOTION_ALT_KEY - 1, 0);
        ctx->gpr[3] = this_;
        ctx->gpr[4] = obj;
        ctx->gpr[5] = out;
        kz3_net_call(ctx, lookup, f, sp, toc, lr);         /* r3 = that lookup's result */
        static int said_alt;
        if (said_alt++ < 4) { KZ3_NET_PRINTF("[text] %s: no motion sensors, %s shown instead\n", k, KZ3_TX_MOTION_ALT_KEY); fflush(stdout); }
        return;
    }
    if (act != KZ3_TX_REPLACE || rn == 0 || rn > KZ3_TX_TEXT_MAX) return;
    uint32_t obj = kz3_net_put_string(f, NULL, rn, 'x');
    ctx->gpr[3] = out;
    ctx->gpr[4] = obj;
    kz3_net_call(ctx, assign, f, sp, toc, lr);
    ctx->gpr[3] = 1;
    uint32_t q2 = vm_read32(out);
    if (!KZ3_NET_GUEST_PTR_OK(q2) || vm_read32(q2) != 1 || vm_read32(q2 + 8) != rn || vm_read32(q2 + 12) < rn) {
        /* not a buffer of its own with room for the text: look the original up again, so nothing shows the 'x's */
        static int said;
        if (said++ < 4) { KZ3_NET_PRINTF("[text] %s: the game's string copy is not the expected layout, original kept\n", k); fflush(stdout); }
        ctx->gpr[3] = this_;
        ctx->gpr[4] = key;
        ctx->gpr[5] = out;
        kz3_net_call(ctx, lookup, f, sp, toc, lr);
        return;
    }
    for (size_t i = 0; i < rn; i++) vm_write16(q2 + 0x10 + 2 * (uint32_t)i, rep[i]);
    vm_write16(q2 + 0x10 + 2 * (uint32_t)rn, 0);
    static int said_rep;
    if (said_rep++ < 24) { KZ3_NET_PRINTF("[text] %s: override applied (%u units)\n", k, (unsigned)rn); fflush(stdout); }
}

#define KZ3_HOOK_REGION_LABEL(ADDR, ASSIGN)                                                                            \
    void func_##ADDR(ppu_context* ctx)                                                                                 \
    {                                                                                                                  \
        uint32_t key_ = (uint32_t)ctx->gpr[4], out_ = (uint32_t)ctx->gpr[5];                                           \
        uint64_t this_ = ctx->gpr[3], sp_ = ctx->gpr[1], toc_ = ctx->gpr[2], lr_ = ctx->lr;                            \
        func_##ADDR##__lifted(ctx);                                                                                    \
        kz3_net_localizer_post(ctx, this_, key_, out_, sp_, toc_, lr_, func_##ADDR##__lifted, func_##ASSIGN);          \
    }

#endif /* KZ3_NET_HOOKS_H */

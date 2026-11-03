/*
 * KZ3 port, network pass 53: one log line when the player's menu choice starts a level, so a support log shows
 * whether a level was ever requested (tester report 2026-11-02: a clean-install log had the boot-time preload opens
 * and no way to tell "never chose Campaign" from "chose it and nothing happened").
 *
 * Hook: GameState:AddRound (Lua method; v1.00 0x004CDDDC, 1.14 0x004DB394 by re/data/addr_map_114.tsv, 88 instructions
 * on both, argument 2 = the level id as an int, kept in r31 by the prologue). Campaign Select Mission / New Campaign
 * call it for ids k..208 (the RE notes s.3); only the first of a burst is logged:
 *   [menu] level requested: AddRound id=201 (campaign level_id, re/data/campaign_levels.tsv: 201 = SP_Mission_12)
 * The table is not in the log (kept out of the runtime); the id is enough to look it up. Installed from the same
 * place as kz3_gum_diag (first Localizer lookup, i.e. in the menu, before any level). Picked by two words of the
 * function (stdu r1,-0xa0(r1) at +0 and ori r31,r4,0 at +0x14); a mismatch installs nothing. KZ3_MENU_REQ_LOG=0 off.
 * ASSUMPTION: the Lua binding reaches the function through the indirect-call map (like the menu listener hook); not
 * run with game data this round (none mounted), so the next GPU run must show the line once per level start.
 */
#ifndef KZ3_MENU_REQ_H
#define KZ3_MENU_REQ_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
#else
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
#endif

static void (*s_mr_orig)(ppu_context*);
static int s_mr_next = -1, s_mr_lines;

/* Returns 1 when the line is printed: a new burst (id is not the previous id + 1), at most 64 lines. */
static int kz3_menu_req_note(int id)
{
    const int fresh = id != s_mr_next;
    s_mr_next = id + 1;
    if (!fresh || s_mr_lines >= 64) return 0;
    s_mr_lines++;
    KZ3_NET_PRINTF("[menu] level requested: AddRound id=%d (campaign level_id; re/data/campaign_levels.tsv)\n", id);
    fflush(stdout);
    return 1;
}
static void kz3_menu_req_wrap(ppu_context* ctx)
{
    kz3_menu_req_note((int)(int32_t)(uint32_t)ctx->gpr[4]);
    s_mr_orig(ctx);
}

static void kz3_menu_req_install(void)
{
    const char* e = getenv("KZ3_MENU_REQ_LOG");
    if (e && !strcmp(e, "0")) return;
    static const uint32_t addrs[2] = { 0x004CDDDCu, 0x004DB394u };
    for (int t = 0; t < 2; t++) {
        const uint32_t a = addrs[t];
        if (vm_read32(a) != 0xF821FF61u || vm_read32(a + 0x14) != 0x609F0000u) continue;
        s_mr_orig = NULL;
        for (uint64_t k = 0; k < function_table_count; k++)
            if ((uint32_t)function_table[k].addr == a) { s_mr_orig = function_table[k].func; break; }
        if (!s_mr_orig) return;
        ppu_register_function(a, kz3_menu_req_wrap);
        KZ3_NET_PRINTF("[menu] level-request log installed at 0x%08X\n", a);
        fflush(stdout);
        return;
    }
}

#endif /* KZ3_MENU_REQ_H */

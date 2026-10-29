/*
 * KZ3 port, network pass 26 (stage 3b, patch 4014): the in-game "Edit custom layout" page. ENHANCEMENT, opt-in
 * (nothing changes until the player opens it; the Controller layout row stays on its own choice).
 *
 * The page is generated here, the way port/kz3/main.cpp generates the Keyboard page (kz3_kb_page_gum): the same
 * widgets and classes (PEDS list 760 wide with 720-wide rows, `mystats` for a button used twice, the confirm dialog,
 * the footer line 532 px down), so it looks like the Keyboard page. Rows are the game's actions, one per game button;
 * each shows the game's own action name (read from settings:controllerconfigs/activeconfig/<EButton> at run time, so
 * it is the player's language and active scheme) and the pad button that presses it. Accept = press the pad button to
 * bind it, Options = unbind, the last entry copies the Default layout into Custom (network pass 35: the "reset to Modern
 * layout" entry is gone: Modern is now Shadow Fall's per-action layout, which one table per button cannot express,
 * and the game's pages offer one reset only). Everything goes to
 * the host through the Keyboard page's Lua bridge ("kb:pm ..." -> pad_kb_ui_cmd -> pmu_command, pad_remap_ui.h in
 * patch 4014), which also saves keybinds.ini [pad].
 *
 * kz3_padpage_gum is pure (host test: tools/net/test_pad_remap_ui.c). It is served on a miss of
 * interface/menu/gum/common/padlayoutsettings.gum by the hook of kz3_prompts_ui.h, and the controller page gets a
 * row that opens it.
 */
#ifndef KZ3_PADPAGE_H
#define KZ3_PADPAGE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kz3_host_composed.h"   /* every rewritten / generated .gum key (network pass 40) */
#include "kz3_tr.h"              /* network pass 45: the port UI text in the game's language */

#define KZ3_PADPAGE_KEY KZ3_GEN_PADLAYOUT
#define KZ3_PADPAGE_OPEN "open(file:///common/padlayoutsettings.gum)"

/* Net pass 40 (patch 4023): Custom per ACTION. The labels of pad_action_custom.h's groups, in its order (the host
 * test compares them): 15 on-foot groups on this page, the vehicles / exo / guns groups on the sub-page. English, like
 * the Keyboard page's rows (the game has no string per action: its legends name buttons, e.g. "Melee / Jetpack"). */
static const char* const kz3_pa_labels[] = {
    "Fire", "Aim / Zoom", "Grenade Quick Throw", "Reload / Pickup", "Use", "Crouch / Cover", "Melee", "Jump / Vault",
    "Sprint", "Weapon switch", "Flashlight", "Direction / Sniper zoom", "Jetpack climb", "Jetpack boost", "Objectives",
    "Vehicle fire", "Vehicle secondary fire", "Accelerate", "Vehicle boost", "Handbrake", "Exit vehicle", "Vehicle zoom",
    "Rear view", "Reset camera", "Exo fire", "Exo secondary fire", "Exo zoom", "Exo camera",
    "Mounted gun secondary fire", "Dismount", "AA gun fire", "AA gun secondary fire", "AA gun zoom",
};
#define KZ3_PA_N ((int)(sizeof kz3_pa_labels / sizeof kz3_pa_labels[0]))
/* Net pass 45: the label of each group in the page's language: the game's own word where it has the same one
 * (the RE notes 173.3: `@game:` = @key{...} in the GUM), else the port_ui.tsv key (pad.act.* ...). */
static const char* const kz3_pa_trkeys[] = {
    "@game:menu_controller_legend_primaryfire", "pad.aim", "@game:menu_controller_legend_quickthrow",
    "@game:menu_controller_legend_reload_pickup", "@game:menu_controller_legend_use", "pad.crouch",
    "@game:menu_controller_legend_closecombat", "pad.jump", "pad.sprint", "@game:menu_controller_legend_nextweapon",
    "pad.act.flashlight", "@game:menu_controller_legend_sniperzoom_direction", "pad.act.jetpack_climb",
    "pad.act.jetpack_boost", "@game:menu_controller_legend_objectives",
    "pad.act.vehicle_fire", "pad.act.vehicle_secondary_fire", "pad.act.accelerate", "pad.act.vehicle_boost",
    "pad.act.handbrake", "pad.act.exit_vehicle", "pad.act.vehicle_zoom", "pad.act.rear_view", "pad.act.reset_camera",
    "pad.act.exo_fire", "pad.act.exo_secondary_fire", "pad.act.exo_zoom", "pad.act.exo_camera",
    "pad.act.mounted_gun_secondary_fire", "pad.act.dismount", "pad.act.aa_gun_fire", "pad.act.aa_gun_secondary_fire",
    "pad.act.aa_gun_zoom",
};
typedef char kz3_pa_trkeys_match[(sizeof kz3_pa_trkeys / sizeof kz3_pa_trkeys[0]) == (unsigned)KZ3_PA_N ? 1 : -1];
#define KZ3_PA_N_FOOT 15
#define KZ3_PADPAGE2_KEY KZ3_GEN_PADLAYOUT2
#define KZ3_PADPAGE2_OPEN "open(file:///common/kz3padlayout2.gum)"
/* the main page's extra rows (list values) */
#define KZ3_PA_ROW_SUB 100
#define KZ3_PA_ROW_COPY_DEFAULT 101
#define KZ3_PA_ROW_COPY_MODERN 102

/* The row that opens the page, last on the Layout and Prompts page (kz3_prompts_ui.h, after "Controller layout"); as
 * wide as that page's selects. */
#define KZ3_PADPAGE_LINK_ROW \
    "\n\t\t\t<listitem label=\"Edit custom layout\" width=\"660\" labelaccept=\"@key{menu_legend_select}\"\n" \
    "\t\t\t\tonaccept=\"" KZ3_PADPAGE_OPEN "\"/>"

typedef struct { char* p; size_t n, cap; int err; } Kz3PpBuf;
static void kz3_pp_put(Kz3PpBuf* b, const char* s);
/* Net pass 45: put a template with kz3_tr.h markers, rendered in the game's language. */
static void kz3_pp_tr(Kz3PpBuf* b, const char* tmpl)
{
    char* r = NULL;
    kz3_tr_render(tmpl, strlen(tmpl), &r);
    if (!r) { b->err = 1; return; }
    kz3_pp_put(b, r);
    free(r);
}
static void kz3_pp_put(Kz3PpBuf* b, const char* s)
{
    size_t k = strlen(s);
    if (b->err) return;
    if (b->n + k + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 8192;
        char* q;
        while (nc < b->n + k + 1) nc *= 2;
        q = (char*)realloc(b->p, nc);
        if (!q) { b->err = 1; return; }
        b->p = q; b->cap = nc;
    }
    memcpy(b->p + b->n, s, k + 1);
    b->n += k;
}

/* One page (malloc'd, NUL-terminated, *out_n bytes; NULL when out of memory): groups first..last-1 of
 * pad_action_custom.h, `main` adds the sub-page link and the two copy rows. Same frame as before (network pass 26): the
 * game's PEDS list, the pad button in a column, a conflict in the game's "used twice" style (mystats), the game's
 * confirm dialog for a copy, the status line under the list. */
static char* kz3_pa_page(int first, int last, int main, size_t* out_n)
{
    Kz3PpBuf b = { 0, 0, 0, 0 };
    char line[1024];
    int r;
    kz3_pp_tr(&b,
        "<gum default-focus=\"lst_pad\">\n\n"
        "\t<texture class=\"background\"/>\n"
        "\t<movie src=\"source:movies/MenuBackground.bik\" persistid=\"movie_back\"/>\n"
        "\t<timer id=\"pm_timer\" reset-on-input=\"false\" countdown=\"150\" on-timer-end=\"script(pmscript, Tick)\"/>\n\n"
        "\t<panel id=\"pnl_main\" class=\"safearea\" scrollable=\"false\"\n"
        "\t\toncancel=\"back()\" labelcancel=\"@key{menu_legend_back}\" onstart=\"back()\"\n"
        "\t\tonoptions=\"script(pmscript, Unbind)\" labeloptions=\"@tr{pad.unbind|Unbind}\">\n\n"
        "\t\t@include{file:///hudelements/type3.gum}\n\n"
        "\t\t<text class=\"sectiontitle\">@key{menu_options}</text>\n");   /* the sibling Options pages' form (RSX pass 104) */
    kz3_pp_tr(&b, main ? "\t\t<text class=\"pagetitle\">@tr{pad.title_foot|Change the custom layout: on foot}</text>\n\n"
                       : "\t\t<text class=\"pagetitle\">@tr{pad.title_veh|Change the vehicle controls}</text>\n\n");
    kz3_pp_tr(&b,
        "\t\t<text id=\"hdr_0\" x=\"382\" y=\"106\" width=\"300\" text-align=\"center\">@tru{pad.hdr|Pad button}</text>\n"
        "\t\t<list id=\"lst_pad\" class=\"PEDS\" y=\"186\" width=\"760\" height=\"280\" labelaccept=\"@key{menu_startup_controls_change}\"\n"
        "\t\t\tonaccept=\"script(pmscript, Bind)\">\n");
    for (r = first; r < last && r < KZ3_PA_N; r++) {
        snprintf(line, sizeof line,
                 "\t\t\t<listitem id=\"row_%d\" value=\"%d\" label=\"@tr{%s|%s}\" width=\"720\">\n"
                 "\t\t\t\t<text id=\"k0_%d\" x=\"380\" width=\"150\" text-align=\"center\"> </text>\n"
                 "\t\t\t\t<text id=\"c0_%d\" class=\"mystats\" visible=\"false\" x=\"380\" y=\"2\" width=\"150\" text-align=\"center\"> </text>\n"
                 "\t\t\t</listitem>\n",
                 r, r, kz3_pa_trkeys[r], kz3_pa_labels[r], r, r);
        if (!strncmp(kz3_pa_trkeys[r], "@game:", 6))   /* the game's own word: its Localizer key */
            snprintf(line, sizeof line,
                     "\t\t\t<listitem id=\"row_%d\" value=\"%d\" label=\"@key{%s}\" width=\"720\">\n"
                     "\t\t\t\t<text id=\"k0_%d\" x=\"380\" width=\"150\" text-align=\"center\"> </text>\n"
                     "\t\t\t\t<text id=\"c0_%d\" class=\"mystats\" visible=\"false\" x=\"380\" y=\"2\" width=\"150\" text-align=\"center\"> </text>\n"
                     "\t\t\t</listitem>\n",
                     r, r, kz3_pa_trkeys[r] + 6, r, r);
        kz3_pp_tr(&b, line);
    }
    if (main) {
        snprintf(line, sizeof line,
                 "\t\t\t<listitem id=\"row_%d\" value=\"%d\" label=\"@tr{pad.vehicles|Vehicles, exo and guns}\" width=\"720\" labelaccept=\"@key{menu_legend_select}\"/>\n"
                 "\t\t\t<listitem id=\"row_%d\" value=\"%d\" label=\"@tr{pad.copy_default|Copy Default layout}\" width=\"720\" labelaccept=\"@key{menu_legend_select}\"/>\n"
                 "\t\t\t<listitem id=\"row_%d\" value=\"%d\" label=\"@tr{pad.copy_modern|Copy Modern layout}\" width=\"720\" labelaccept=\"@key{menu_legend_select}\"/>\n",
                 KZ3_PA_ROW_SUB, KZ3_PA_ROW_SUB, KZ3_PA_ROW_COPY_DEFAULT, KZ3_PA_ROW_COPY_DEFAULT, KZ3_PA_ROW_COPY_MODERN,
                 KZ3_PA_ROW_COPY_MODERN);
        kz3_pp_tr(&b, line);
    }
    kz3_pp_put(&b,
        "\t\t</list>\n\n"
        "\t\t<text id=\"txt_status\" x=\"60\" y=\"532\" width=\"900\"> </text>\n\n"
        "\t\t@include{file:///common/includes/dialogconfirm.gum}\n"
        "\t</panel>\n\n"
        "\t<data id=\"settings\" src=\"object://settings\"/>\n\n"
        "\t<script id=\"pmscript\">\n<![CDATA[\n");
    snprintf(line, sizeof line, "\t\tgPmSel = -1\n\t\tgPaFirst = %d\n\t\tgPaLast = %d\n\t\tgPmReset = \"\"\n\n", first, last);
    kz3_pp_put(&b, line);
    /* network pass 45: the action names of the status line in the page's language (the bridge's are English; the
     * game-word rows keep the bridge's name: their text comes from the Localizer, which C does not call here) */
    kz3_pp_put(&b, "\t\tgPaLab = {}\n");
    for (r = first; r < last && r < KZ3_PA_N; r++) {
        if (!strncmp(kz3_pa_trkeys[r], "@game:", 6)) continue;
        snprintf(line, sizeof line, "\t\tgPaLab[%d] = \"@trl{%s|%s}\"\n", r, kz3_pa_trkeys[r], kz3_pa_labels[r]);
        kz3_pp_tr(&b, line);
    }
    kz3_pp_tr(&b,
        "\t\tfunction Lab(r)\n"
        "\t\t\tlocal t = gPaLab[tonumber(r)]\n"
        "\t\t\tif (t == nil) then\n"
        "\t\t\t\tt = Cmd(\"label \"..r)\n"
        "\t\t\tend\n"
        "\t\t\treturn t\n"
        "\t\tend\n\n"
        "\t\tfunction Preset()\n"
        "\t\t\tlocal p = Cmd(\"preset\")\n"
        "\t\t\tif (p == \"modern\") then\n"
        "\t\t\t\tp = \"@trl{lp.v.modern|Modern shooter}\"\n"
        "\t\t\tend\n"
        "\t\t\treturn p\n"
        "\t\tend\n\n"
        "\t\tfunction Cmd(c)\n"
        "\t\t\tgPlayerProfile:SetTimeZone(\"kb:pa \"..c)\n"
        "\t\t\treturn gPlayerProfile:GetTimeZone()\n"
        "\t\tend\n\n"
        "\t\tfunction Cell(r)\n"
        "\t\t\tif (r < gPaFirst or r >= gPaLast) then\n"
        "\t\t\t\treturn\n"
        "\t\t\tend\n"
        "\t\t\tlocal t = Cmd(\"key \"..r)\n"
        "\t\t\tlocal k = gRoot:GetTag(\"k0_\"..r)\n"
        "\t\t\tlocal c = gRoot:GetTag(\"c0_\"..r)\n"
        "\t\t\tlocal d = Cmd(\"dup \"..r) == \"1\"\n"
        "\t\t\tif (Cmd(\"state\") == \"capture\" and tostring(r) == Cmd(\"cap\")) then\n"
        "\t\t\t\tt = \"...\"\n"
        "\t\t\tend\n"
        "\t\t\tif (r == gPmSel) then\n"
        "\t\t\t\tt = \"[ \"..t..\" ]\"\n"
        "\t\t\tend\n"
        "\t\t\tk:SetValue(t)\n"
        "\t\t\tc:SetValue(t)\n"
        "\t\t\tk:SetVisible(not d)\n"
        "\t\t\tc:SetVisible(d)\n"
        "\t\tend\n\n"
        "\t\tfunction Status()\n"
        "\t\t\tlocal st = Cmd(\"state\")\n"
        "\t\t\tlocal t = @trx{pad.layout_in_use_custom|Layout in use: custom.}\n"
        "\t\t\tif (Cmd(\"preset\") ~= \"custom\") then\n"
        "\t\t\t\tt = @trx{pad.layout_in_use|Layout in use: {0}. A change switches it to custom.|Preset()}\n"
        "\t\t\tend\n"
        "\t\t\tif (st == \"capture\") then\n"
        "\t\t\t\tt = @trx{pad.capture|Press a pad button for {0}. Delete clears, Esc cancels.|Lab(Cmd(\"cap\"))}\n"
        "\t\t\telseif (st == \"swapped\") then\n"
        "\t\t\t\tt = @trx{pad.swapped|Bound. {0} took the old button.|Lab(Cmd(\"other\"))}\n"
        "\t\t\telseif (st == \"bound\") then\n"
        "\t\t\t\tt = @trx{pad.bound|Bound.}\n"
        "\t\t\telseif (st == \"cleared\") then\n"
        "\t\t\t\tt = @trx{pad.cleared|Unbound.}\n"
        "\t\t\telseif (st == \"refused\") then\n"
        "\t\t\t\tt = @trx{pad.refused|The menu button stays on Start.}\n"
        "\t\t\telseif (st == \"cancel\" or st == \"timeout\") then\n"
        "\t\t\t\tt = @trx{pad.nochange|Nothing changed.}\n"
        "\t\t\tend\n"
        "\t\t\tgRoot:GetTag(\"txt_status\"):SetValue(t)\n"
        "\t\tend\n\n"
        "\t\tfunction Refresh()\n"
        "\t\t\tfor r = gPaFirst, gPaLast - 1 do\n"
        "\t\t\t\tCell(r)\n"
        "\t\t\tend\n"
        "\t\t\tStatus()\n"
        "\t\tend\n\n"
        "\t\tfunction Select()\n"
        "\t\t\tlocal item = gRoot:GetTag(\"lst_pad\"):GetSelectedItem()\n"
        "\t\t\tlocal v = -1\n"
        "\t\t\tif (item ~= nil) then\n"
        "\t\t\t\tv = 0+item:GetValue()\n"
        "\t\t\tend\n"
        "\t\t\tif (v ~= gPmSel) then\n"
        "\t\t\t\tlocal old = gPmSel\n"
        "\t\t\t\tgPmSel = v\n"
        "\t\t\t\tCell(old)\n"
        "\t\t\t\tCell(v)\n"
        "\t\t\tend\n"
        "\t\tend\n\n"
        "\t\tfunction OnPageOn()\n"
        "\t\t\tgPlayerProfile:SetTimeZone(\"kb:lang \"..gPlayerProfile:GetLanguage())\n"   /* net 45: for the next page */
        "\t\t\tSelect()\n"
        "\t\t\tRefresh()\n"
        "\t\tend\n\n"
        "\t\tfunction OnPageOff()\n"
        "\t\t\tCmd(\"cancel\")\n"
        "\t\tend\n\n"
        "\t\tfunction Tick()\n"
        "\t\t\tSelect()\n"
        "\t\t\tif (Cmd(\"poll\") == \"changed\") then\n"
        "\t\t\t\tRefresh()\n"
        "\t\t\tend\n"
        "\t\t\tgRoot:GetTag(\"pm_timer\"):Reset()\n"
        "\t\tend\n\n"
        "\t\tfunction Bind(e)\n"
        "\t\t\tif (e ~= nil) then\n"
        "\t\t\t\te:SetHandled(true)\n"
        "\t\t\tend\n"
        "\t\t\tlocal item = gRoot:GetTag(\"lst_pad\"):GetSelectedItem()\n"
        "\t\t\tif (item == nil) then\n"
        "\t\t\t\treturn\n"
        "\t\t\tend\n"
        "\t\t\tlocal v = 0+item:GetValue()\n"
        "\t\t\tif (v >= gPaFirst and v < gPaLast) then\n"
        "\t\t\t\tCmd(\"cap \"..v)\n"
        "\t\t\t\tRefresh()\n");
    snprintf(line, sizeof line,
        "\t\t\telseif (v == %d) then\n"
        "\t\t\t\tgMenu:DoAction(\"" KZ3_PADPAGE2_OPEN "\")\n"
        "\t\t\telseif (v == %d or v == %d) then\n"
        "\t\t\t\tgPmReset = \"default\"\n"
        "\t\t\t\tif (v == %d) then\n"
        "\t\t\t\t\tgPmReset = \"modern\"\n"
        "\t\t\t\tend\n"
        "\t\t\t\tgMenu:DoAction(\"dialog(dlg_confirmdefaults, pmscript, OnDialogClose)\")\n"
        "\t\t\tend\n"
        "\t\tend\n\n",
        KZ3_PA_ROW_SUB, KZ3_PA_ROW_COPY_DEFAULT, KZ3_PA_ROW_COPY_MODERN, KZ3_PA_ROW_COPY_MODERN);
    kz3_pp_put(&b, line);
    kz3_pp_put(&b,
        "\t\tfunction Unbind(e)\n"
        "\t\t\tif (e ~= nil) then\n"
        "\t\t\t\te:SetHandled(true)\n"
        "\t\t\tend\n"
        "\t\t\tif (gPmSel >= gPaFirst and gPmSel < gPaLast) then\n"
        "\t\t\t\tCmd(\"clear \"..gPmSel)\n"
        "\t\t\t\tRefresh()\n"
        "\t\t\tend\n"
        "\t\tend\n\n"
        "\t\tfunction OnDialogClose(e)\n"
        "\t\t\te:SetHandled(true)\n"
        "\t\t\tif (e:GetSender():GetValue() == \"yes\" and gPmReset ~= \"\") then\n"
        "\t\t\t\tCmd(\"reset \"..gPmReset)\n"
        "\t\t\t\tRefresh()\n"
        "\t\t\tend\n"
        "\t\t\tgPmReset = \"\"\n"
        "\t\tend\n"
        "]]>\n\t</script>\n\n</gum>\n");
    if (b.err) { free(b.p); return NULL; }
    *out_n = b.n;
    return b.p;
}

/* The main Custom page (on foot) and the sub-page (vehicles, exo, guns). */
static char* kz3_padpage_gum(size_t* out_n) { return kz3_pa_page(0, KZ3_PA_N_FOOT, 1, out_n); }
static char* kz3_padpage2_gum(size_t* out_n) { return kz3_pa_page(KZ3_PA_N_FOOT, KZ3_PA_N, 0, out_n); }

#endif /* KZ3_PADPAGE_H */

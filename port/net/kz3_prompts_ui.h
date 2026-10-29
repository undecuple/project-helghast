/*
 * KZ3 port, network pass 24 (stage 2): the Options row "Button prompts" on the game's controller settings page;
 * pass 25 (patch 4013, stage 3) adds the row "Controller layout" (Default, Modern shooter, Custom) under it.
 * ENHANCEMENT (display; default Auto).
 *
 * interface/menu/gum/common/advancedcontroller.gum (Options > Controls > Advanced Settings, in all four option menus;
 * up to network pass 33 the controller page controllersettings.gum itself) is patched when it is opened, the way port/kz3/main.cpp patches the option menus for the Keyboard row (RSX pass 37):
 * since network pass 34 one row "Layout and Prompts" at the end of Advanced Settings opens the port's own page
 * (kz3padsettings.gum, below) with the rows Button prompts (Auto / PS3 / PS4/PS5 / Xbox / Keyboard), Controller layout
 * (Default / Modern shooter / Custom) and Edit custom layout. The values go to the host through the Keyboard page's
 * Lua bridge (gPlayerProfile:SetTimeZone("kb:prompts <v>") / GetTimeZone(), main.cpp -> pad_kb_ui_cmd, cellPad.c patch
 * 4011), which stores them in keybinds.ini ([prompts] buttons = ..., [pad] preset = ...). OnPageOn selects the stored
 * values. Nothing is added when the bridge is off (KZ3_KB_PAGE=0, or an EBOOT whose bridge methods main.cpp does not
 * know): without it SetTimeZone would change the profile's time zone.
 *
 * kz3_prompts_patch_gum is pure (host test: tools/net/test_text_overrides.c); the miss hook below it needs the runtime
 * (g_ps3_vfs_miss_hook, ppu_vfs_root, vm_base) and is compiled only with KZ3_PROMPTS_UI_GLUE.
 */
#ifndef KZ3_PROMPTS_UI_H
#define KZ3_PROMPTS_UI_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "kz3_padpage.h"   /* patch 4014: the Custom layout page */

/* Net pass 28: the glue's log lines go through the port's log router (kz3_log.c), so they reach the player's log
 * file: port/spu/kz3_ppu_overrides.cpp, which includes these headers, is not compiled with the stdio redirect, and a
 * plain printf there went to the GUI exe's absent console (no [prompts] / [text] line in any live log before). C host
 * tests keep printf. */
#ifndef KZ3_NET_PRINTF
#  if defined(__cplusplus) && !defined(KZ3_LOG_REDIRECT_STDIO)
extern "C" int kz3_log_printf(const char* fmt, ...);
#    define KZ3_NET_PRINTF kz3_log_printf
#  else
#    define KZ3_NET_PRINTF printf
#  endif
#endif

#define KZ3_PROMPTS_GUM_KEY KZ3_HC_ADVCONTROLLER   /* network pass 34 (was controllersettings) */

/* Net pass 34 (the controller settings looked rough and overlapped the existing menu): the
 * controller page has room for its own three rows only; the controller picture sits right under the list (RSX pass 97:
 * the three port rows ran into it). The port's rows now live on their own page, KZ3_PADSET_KEY, built like the game's
 * advancedcontroller.gum (same panel, list and select widths, the same background and legend), and the game's
 * Advanced Settings page (Controls > Advanced Settings: a plain list, no controller picture) gets one row at the end
 * of its list that opens it, as wide as that page's selects. The controller page itself is served unchanged. */
#define KZ3_PADSET_KEY KZ3_GEN_PADSET
#define KZ3_PADSET_OPEN "open(file:///common/kz3padsettings.gum)"

static const char kz3_prompts_row[] =
    "\n\t\t\t<listitem label=\"@tr{lp.link|Layout and Prompts}\" width=\"660\" labelaccept=\"@key{menu_legend_select}\"\n"
    "\t\t\t\tonaccept=\"" KZ3_PADSET_OPEN "\"/>";

/* The controller picture of the layout in force, shared by the port's page and the game's own controller page (net
 * pass 36, so both pages agree): three panels in the place of the game's `<panel class="controller">` + include.
 * Default = the game's own include (the scheme's labels, settings:controllerconfigs/activeconfig, built at 0x8AA540);
 * Modern = the 14 label positions of that include with the game's own legend strings (menu_controller_legend_*, SP
 * keys; the L1 leader line has no label: no on-foot action there); Custom = the include's 15 positions, each filled
 * by KZ3ShowPicture with the activeconfig labels of the game buttons the Custom table puts on that pad button
 * (bridge "pa at <EButton> <i>": the on-foot action groups on that button, patch 4023; was "pm at", 4014). Texture `ControllerLayout`, so rsx 2094's PS4/PS5 and Xbox pictures
 * replace it as on the game's page. */
#define KZ3_PIC_C(cls, x, y, extra, name) \
    "\t\t\t\t<text id=\"txt_kz3_c_" name "\" class=\"" cls "\" x=\"" x "\" y=\"" y "\"" extra "></text>\n"
#define KZ3_PIC_PANELS \
    "\t\t<panel id=\"pnl_kz3_ctl_default\" class=\"controller\">\n" \
    "\t\t\t@include{file:///common/includes/controller.gum}\n" \
    "\t\t</panel>\n" \
    "\t\t<panel id=\"pnl_kz3_ctl_modern\" class=\"controller\" visible=\"false\">\n" \
    "\t\t\t<texture class=\"nofilter\" src=\"ControllerLayout\" width=\"512\" align=\"center\">\n" \
    "\t\t\t\t<text class=\"controltextright\" x=\"-252\" y=\"-26\">@key{menu_controller_legend_zoom}</text>\n" \
    "\t\t\t\t<text class=\"controltextright\" x=\"-309\" y=\"64\">@key{menu_controller_legend_sniperzoom_direction}</text>\n" \
    "\t\t\t\t<text class=\"controltextright\" x=\"-280\" y=\"102\">@key{menu_controller_legend_heavyweapon}</text>\n" \
    "\t\t\t\t<text class=\"controltextright\" x=\"-334\" y=\"140\">@key{menu_controller_legend_primaryweapon}</text>\n" \
    "\t\t\t\t<text class=\"controltextright\" x=\"-310\" y=\"178\">@key{menu_controller_legend_sidearm}</text>\n" \
    "\t\t\t\t<text class=\"controltextright\" x=\"-218\" y=\"240\">@key{menu_controller_legend_sprinttoggle}</text>\n" \
    "\t\t\t\t<text class=\"controltextleft\" x=\"406\" y=\"-26\">@key{menu_controller_legend_primaryfire}</text>\n" \
    "\t\t\t\t<text class=\"controltextleft\" x=\"406\" y=\"6\" align=\"left\">@key{menu_controller_legend_quickthrow}</text>\n" \
    "\t\t\t\t<text class=\"controltextleft\" x=\"474\" y=\"64\" align=\"left\">@key{menu_controller_legend_nextweapon}</text>\n" \
    "\t\t\t\t<text class=\"controltextleft\" x=\"442\" y=\"102\" align=\"left\">@key{menu_controller_legend_reload_pickup}</text>\n" \
    "\t\t\t\t<text class=\"controltextleft\" x=\"498\" y=\"140\">@key{menu_controller_legend_use} / @key{menu_controller_legend_cover_jetpack}</text>\n" \
    "\t\t\t\t<text class=\"controltextleft\" x=\"472\" y=\"178\">@key{menu_controller_legend_jump_boost}</text>\n" \
    "\t\t\t\t<text class=\"controltextleft\" x=\"370\" y=\"240\">@key{menu_controller_legend_closecombat_look}</text>\n" \
    "\t\t\t\t<text class=\"controltextleft\" x=\"220\" y=\"212\">@key{menu_controller_legend_objectives}</text>\n" \
    "\t\t\t</texture>\n" \
    "\t\t</panel>\n" \
    "\t\t<panel id=\"pnl_kz3_ctl_custom\" class=\"controller\" visible=\"false\">\n" \
    "\t\t\t<texture class=\"nofilter\" src=\"ControllerLayout\" width=\"512\" align=\"center\">\n" \
    KZ3_PIC_C("controltextright", "-252", "-26", "", "Shoulder_Left_2") \
    KZ3_PIC_C("controltextright", "-252", "6", "", "Shoulder_Left_1") \
    KZ3_PIC_C("controltextright", "-309", "64", "", "Up") \
    KZ3_PIC_C("controltextright", "-280", "102", "", "Right") \
    KZ3_PIC_C("controltextright", "-334", "140", "", "Left") \
    KZ3_PIC_C("controltextright", "-310", "178", "", "Down") \
    KZ3_PIC_C("controltextright", "-218", "240", "", "Left_Analog") \
    KZ3_PIC_C("controltextleft", "406", "-26", "", "Shoulder_Right_2") \
    KZ3_PIC_C("controltextleft", "406", "6", " align=\"left\"", "Shoulder_Right_1") \
    KZ3_PIC_C("controltextleft", "474", "64", " align=\"left\"", "Triangle") \
    KZ3_PIC_C("controltextleft", "442", "102", " align=\"left\"", "Square") \
    KZ3_PIC_C("controltextleft", "498", "140", "", "Circle") \
    KZ3_PIC_C("controltextleft", "472", "178", "", "Cross") \
    KZ3_PIC_C("controltextleft", "370", "240", "", "Right_Analog") \
    KZ3_PIC_C("controltextleft", "220", "212", "", "Select") \
    "\t\t\t</texture>\n" \
    "\t\t</panel>\n"

/* The picture's Lua (both pages): KZ3ShowPicture(v) shows the panel of layout v ("default" / "modern" / "custom",
 * nil = ask the host) and fills the Custom labels. Only core Lua (no string library: no GUM script uses one). */
#define KZ3_PIC_CUSTOM(name) "\t\t\t\tKZ3PicCustom(\"" name "\")\n"
#define KZ3_PIC_LUA \
    "\t\tfunction KZ3PicCmd(c)\n" \
    "\t\t\tgPlayerProfile:SetTimeZone(\"kb:\"..c)\n" \
    "\t\t\treturn gPlayerProfile:GetTimeZone()\n" \
    "\t\tend\n" \
    "\n" \
    "\t\tfunction KZ3PicCustom(p)\n" \
    "\t\t\tlocal t = \"\"\n" \
    "\t\t\tfor i = 0, 3 do\n" \
    "\t\t\t\tlocal l = KZ3PicCmd(\"pa at \"..p..\" \"..i)\n" \
    "\t\t\t\tif l == nil or l == \"\" or l == \"-\" then break end\n" \
    "\t\t\t\tif t ~= \"\" then t = t..\" / \" end\n" \
    "\t\t\t\tt = t..l\n" \
    "\t\t\tend\n" \
    "\t\t\tif p == \"Left\" or p == \"Right\" or p == \"Down\" then\n" \
    "\t\t\t\tlocal l = gRoot:GetTag(\"settings\"):GetData(\"controllerconfigs/activeconfig/\"..p..\"/text()\")\n" \
    "\t\t\t\tif l ~= nil and l ~= \"\" then\n" \
    "\t\t\t\t\tif t ~= \"\" then t = t..\" / \" end\n" \
    "\t\t\t\t\tt = t..l\n" \
    "\t\t\t\tend\n" \
    "\t\t\tend\n" \
    "\t\t\tgRoot:GetTag(\"txt_kz3_c_\"..p):SetValue(t)\n" \
    "\t\tend\n" \
    "\n" \
    "\t\tfunction KZ3ShowPicture(v)\n" \
    "\t\t\tKZ3PicCmd(\"lang \"..gPlayerProfile:GetLanguage())\n" \
    "\t\t\tif v == nil then v = KZ3PicCmd(\"padmap\") end\n" \
    "\t\t\tgRoot:GetTag(\"pnl_kz3_ctl_default\"):SetVisible(v ~= \"modern\" and v ~= \"custom\")\n" \
    "\t\t\tgRoot:GetTag(\"pnl_kz3_ctl_modern\"):SetVisible(v == \"modern\")\n" \
    "\t\t\tgRoot:GetTag(\"pnl_kz3_ctl_custom\"):SetVisible(v == \"custom\")\n" \
    "\t\t\tif v == \"custom\" then\n" \
    KZ3_PIC_CUSTOM("Shoulder_Left_2") KZ3_PIC_CUSTOM("Shoulder_Left_1") KZ3_PIC_CUSTOM("Up") KZ3_PIC_CUSTOM("Right") \
    KZ3_PIC_CUSTOM("Left") KZ3_PIC_CUSTOM("Down") KZ3_PIC_CUSTOM("Left_Analog") KZ3_PIC_CUSTOM("Shoulder_Right_2") \
    KZ3_PIC_CUSTOM("Shoulder_Right_1") KZ3_PIC_CUSTOM("Triangle") KZ3_PIC_CUSTOM("Square") KZ3_PIC_CUSTOM("Circle") \
    KZ3_PIC_CUSTOM("Cross") KZ3_PIC_CUSTOM("Right_Analog") KZ3_PIC_CUSTOM("Select") \
    "\t\t\tend\n" \
    "\t\tend\n" \
    "\n"

/* The port's page: Button prompts, Controller layout (patch 4013/4022), Edit custom layout (patch 4014). The value goes
 * to the host through the Keyboard page's Lua bridge (gPlayerProfile:SetTimeZone("kb:...") / GetTimeZone()).
 * Net pass 35 (rule in docs/guides/ui-style.md: a port page has everything a game page of its kind has):
 * built like the game's controllersettings.gum (PEDS list 800 with selects 555 / optionwidth 250 and a 320 link row,
 * the controller picture, the legend with Reset to defaults on Options and the game's confirm dialog) plus a
 * description of the focused row (one text per row, shown by the list's onchange). Net pass 36 (review of
 * the net35 pairs): the title is the controller page's own form (`hudelements/type3.gum`: the frame with the icon and
 * the marks at the top right; big title OPTIONS, the page in the subtitle, as RSX pass 104's Mouse and Keyboard pages), and
 * the description sits in the game's tooltip panel (1.14 advancedcontroller.gum: `class="tooltip"`, one text set with
 * SetValue) placed under the picture's right labels instead of at y 440 (where this page has the picture; at the top
 * right it covered the background emblem). Reset to defaults puts Button prompts on Auto and Controller layout on
 * Default (the game's pages offer one reset, to defaults, so there is no "reset to Modern" here). */
#define KZ3_PADSET_DESC_Y "542"   /* tooltip panel y: first line just under "Look / Zoom", the second level with the legend */
/* Net pass 45: a template (kz3_tr.h markers); kz3_padset_gum renders it in the game's language. */
static const char kz3_padset_page[] =
    "<gum default-focus=\"lst_options\">\n"
    "\t<texture class=\"background\"/>\n"
    "\t<movie src=\"source:movies/MenuBackground.bik\" persistid=\"movie_back\"/>\n"
    "\t<panel id=\"pnl_main\" class=\"safearea\" scrollable=\"false\"\n"
    "\t\toncancel=\"back()\" labelcancel=\"@key{menu_legend_back}\"\n"
    "\t\tonoptions=\"script(settingscript, SetToDefault)\" labeloptions=\"@key{menu_legend_default}\">\n"
    "\t\t@include{file:///hudelements/type3.gum}\n"
    "\t\t<text class=\"pagetitle\">@tr{lp.title|Change the controller layout and the button prompts}</text>\n"
    "\t\t<text class=\"sectiontitle\">@key{menu_options}</text>\n"
    "\t\t<list id=\"lst_options\" class=\"PEDS\" width=\"800\" onchange=\"script(settingscript,ShowDescription)\">\n"
    "\t\t\t<select id=\"slc_kz3_prompts\" width=\"555\" optionwidth=\"250\" label=\"@tr{lp.prompts|Button prompts}\" onchange=\"script(settingscript,KZ3ChangePrompts)\"\n"
    "\t\t\t\tdefault-value=\"auto\">\n"
    "\t\t\t\t<selectitem value=\"auto\">@tr{lp.v.auto|Auto}</selectitem>\n"
    "\t\t\t\t<selectitem value=\"ps3\">PS3</selectitem>\n"
    "\t\t\t\t<selectitem value=\"ps4\">PS4/PS5</selectitem>\n"
    "\t\t\t\t<selectitem value=\"xbox\">Xbox</selectitem>\n"
    "\t\t\t\t<selectitem value=\"keyboard\">Keyboard</selectitem>\n"
    "\t\t\t</select>\n"
    "\t\t\t<select id=\"slc_kz3_layout\" width=\"555\" optionwidth=\"250\" label=\"@tr{lp.layout|Controller layout}\" onchange=\"script(settingscript,KZ3ChangeLayout)\"\n"
    "\t\t\t\tdefault-value=\"default\">\n"
    "\t\t\t\t<selectitem value=\"default\">Default</selectitem>\n"
    "\t\t\t\t<selectitem value=\"modern\">@tr{lp.v.modern|Modern shooter}</selectitem>\n"
    "\t\t\t\t<selectitem value=\"custom\">@tr{lp.v.custom|Custom}</selectitem>\n"
    "\t\t\t</select>\n"
    "\t\t\t<listitem id=\"itm_kz3_custom\" label=\"@tr{lp.edit|Edit custom layout}\" width=\"320\" labelaccept=\"@key{menu_legend_select}\"\n"
    "\t\t\t\tonaccept=\"" KZ3_PADPAGE_OPEN "\"/>\n"
    "\t\t</list>\n"
    KZ3_PIC_PANELS
    "\t\t<panel id=\"pnl_kz3_desc\" class=\"tooltip\" y=\"" KZ3_PADSET_DESC_Y "\">\n"
    "\t\t\t<text id=\"txt_kz3_desc\"></text>\n"
    "\t\t</panel>\n"
    "\t\t@include{file:///common/includes/dialogconfirm.gum}\n"
    "\t</panel>\n"
    "\t<data id=\"settings\" src=\"object://settings\"/>\n"
    "\t<script id=\"settingscript\">\n"
    "<![CDATA[\n"
    KZ3_PIC_LUA
    "\t\tgKz3Desc = {\n"
    "\t\t\tslc_kz3_prompts = \"@trl{lp.tip.prompts|Which buttons the prompts show. Auto follows the controller or keyboard used last.}\",\n"
    "\t\t\tslc_kz3_layout = \"@trl{lp.tip.layout|Modern is the Killzone Shadow Fall layout. Menus keep their buttons.}\",\n"
    "\t\t\titm_kz3_custom = \"@trl{lp.tip.edit|Set the button for each action. A change selects the Custom layout.}\"\n"
    "\t\t}\n"
    "\n"
    "\t\tfunction KZ3PromptsCmd(c)\n"
    "\t\t\treturn KZ3PicCmd(c)\n"
    "\t\tend\n"
    "\n"
    "\t\tfunction OnPageOn()\n"
    "\t\t\tgRoot:GetTag(\"slc_kz3_prompts\"):SetValue(KZ3PromptsCmd(\"prompts\"))\n"
    "\t\t\tlocal v = KZ3PromptsCmd(\"padmap\")\n"
    "\t\t\tgRoot:GetTag(\"slc_kz3_layout\"):SetValue(v)\n"
    "\t\t\tKZ3ShowPicture(v)\n"
    "\t\t\tgRoot:GetTag(\"txt_kz3_desc\"):SetValue(gKz3Desc[\"slc_kz3_prompts\"])\n"
    "\t\tend\n"
    "\n"
    "\t\tfunction ShowDescription(e)\n"
    "\t\t\tlocal t = gKz3Desc[e:GetSender():GetSelectedItem():GetID()]\n"
    "\t\t\tif t == nil then t = \"\" end\n"
    "\t\t\tgRoot:GetTag(\"txt_kz3_desc\"):SetValue(t)\n"
    "\t\tend\n"
    "\n"
    "\t\tfunction KZ3ChangePrompts(e)\n"
    "\t\t\te:SetHandled(true)\n"
    "\t\t\tKZ3PromptsCmd(\"prompts \"..e:GetSender():GetValue())\n"
    "\t\tend\n"
    "\n"
    "\t\tfunction KZ3ChangeLayout(e)\n"
    "\t\t\te:SetHandled(true)\n"
    "\t\t\tKZ3PromptsCmd(\"padmap \"..e:GetSender():GetValue())\n"
    "\t\t\tKZ3ShowPicture(e:GetSender():GetValue())\n"
    "\t\tend\n"
    "\n"
    "\t\tfunction SetToDefault()\n"
    "\t\t\tgMenu:DoAction(\"dialog(dlg_confirmdefaults, settingscript, OnDialogClose)\")\n"
    "\t\tend\n"
    "\n"
    "\t\tfunction OnDialogClose(e)\n"
    "\t\t\te:SetHandled(true)\n"
    "\t\t\tif (e:GetSender():GetValue() == \"yes\") then\n"
    "\t\t\t\tKZ3PromptsCmd(\"prompts auto\")\n"
    "\t\t\t\tKZ3PromptsCmd(\"padmap default\")\n"
    "\t\t\t\tgRoot:GetTag(\"slc_kz3_prompts\"):SetValue(KZ3PromptsCmd(\"prompts\"))\n"
    "\t\t\t\tlocal v = KZ3PromptsCmd(\"padmap\")\n"
    "\t\t\t\tgRoot:GetTag(\"slc_kz3_layout\"):SetValue(v)\n"
    "\t\t\t\tKZ3ShowPicture(v)\n"
    "\t\t\tend\n"
    "\t\tend\n"
    "]]>\n"
    "\t</script>\n"
    "</gum>\n";
/* The Layout and Prompts page in the game's language (malloc'd, NUL-terminated, *out_n bytes; NULL without memory). */
static char* kz3_padset_gum(size_t* out_n)
{
    char* g = NULL;
    *out_n = kz3_tr_render(kz3_padset_page, sizeof kz3_padset_page - 1, &g);
    return g;
}

static const char* kz3_pu_find(const char* h, size_t n, size_t from, const char* needle)
{
    size_t k = strlen(needle);
    for (size_t i = from; i + k <= n; i++)
        if (!memcmp(h + i, needle, k)) return h + i;
    return NULL;
}

/* The Advanced Settings page with the row added at the end of its options list (malloc'd, NUL-terminated, *out_n
 * bytes), or NULL when the anchor is missing (the list lst_options and its end) or the row is already there: the page
 * is then served unchanged. */
static char* kz3_prompts_patch_gum(const char* in, size_t n, size_t* out_n)
{
    const char* list = kz3_pu_find(in, n, 0, "<list id=\"lst_options\"");
    const char* list_end = list ? kz3_pu_find(in, n, (size_t)(list - in), "</list>") : NULL;
    if (!list || !list_end || kz3_pu_find(in, n, 0, KZ3_PADSET_OPEN)) return NULL;
    /* the row goes after the last element of the list: back over the whitespace before </list> */
    size_t at_row = (size_t)(list_end - in);
    while (at_row > (size_t)(list - in) && (in[at_row - 1] == ' ' || in[at_row - 1] == '\t' || in[at_row - 1] == '\r' ||
                                            in[at_row - 1] == '\n'))
        at_row--;
    char* row = NULL;
    const size_t nr = kz3_tr_render(kz3_prompts_row, sizeof kz3_prompts_row - 1, &row);   /* network pass 45 */
    char* out = row ? (char*)malloc(n + nr + 1) : NULL;
    if (!out) { free(row); return NULL; }
    memcpy(out, in, at_row);
    memcpy(out + at_row, row, nr);
    memcpy(out + at_row + nr, in + at_row, n - at_row);
    out[n + nr] = 0;
    *out_n = n + nr;
    free(row);
    return out;
}

/* Net pass 36: the game's own controller page (Options > Controls) shows the picture of the layout in force. Its
 * labels came from settings:controllerconfigs/activeconfig only (the scheme's buttons, built at 0x8AA540), so under
 * Modern or Custom it showed buttons the player no longer has. The page's `<panel class="controller">` + include is
 * replaced by KZ3_PIC_PANELS, the script gets KZ3_PIC_LUA and calls KZ3ShowPicture() when the page opens (OnPageOn,
 * added when the page has none), after a scheme change (ChangeMapping: the Custom labels follow the scheme) and after
 * its own reset. Under Default the page is what the game draws (the same include). */
#define KZ3_CTL_GUM_KEY KZ3_HC_CONTROLLER

typedef struct { size_t at, del; const char* ins; } Kz3PuEdit;

/* The patched controller page (malloc'd, NUL-terminated, *out_n bytes), or NULL when an anchor is missing (the
 * controller panel with the include, the script's CDATA) or the page is already patched: it is then served unchanged. */
static char* kz3_ctl_patch_gum(const char* in, size_t n, size_t* out_n)
{
    static const char onpage[] = "\t\tfunction OnPageOn()\n\t\t\tKZ3ShowPicture()\n\t\tend\n\n";
    static const char call[] = "\t\t\tKZ3ShowPicture()\n";
    Kz3PuEdit e[5];
    int ne = 0, i, j;
    if (kz3_pu_find(in, n, 0, "pnl_kz3_ctl_default")) return NULL;
    const char* pc = kz3_pu_find(in, n, 0, "<panel class=\"controller\">");
    const char* pe = pc ? kz3_pu_find(in, n, (size_t)(pc - in), "</panel>") : NULL;
    const char* inc = pc ? kz3_pu_find(in, n, (size_t)(pc - in), "@include{file:///common/includes/controller.gum}") : NULL;
    const char* cd = kz3_pu_find(in, n, 0, "<![CDATA[");
    if (!pc || !pe || !inc || inc > pe || !cd || cd < pe) return NULL;
    /* the picture: from the start of the panel's line to the end of its </panel> line */
    size_t a = (size_t)(pc - in), b = (size_t)(pe - in) + 8;
    while (a > 0 && (in[a - 1] == ' ' || in[a - 1] == '\t')) a--;
    while (b < n && (in[b] == ' ' || in[b] == '\t' || in[b] == '\r')) b++;
    if (b < n && in[b] == '\n') b++;
    e[ne].at = a; e[ne].del = b - a; e[ne].ins = KZ3_PIC_PANELS; ne++;
    /* the Lua: after the CDATA line */
    size_t c = (size_t)(cd - in) + 9;
    while (c < n && in[c] != '\n') c++;
    if (c < n) c++;
    const char* op = kz3_pu_find(in, n, 0, "function OnPageOn()");
    e[ne].at = c; e[ne].del = 0; e[ne].ins = op ? KZ3_PIC_LUA : KZ3_PIC_LUA "\x01"; ne++;
    /* calls: after the lines of OnPageOn's header, the scheme save and the reset (each optional) */
    const char* const after[3] = { op ? "function OnPageOn()" : NULL, "SaveControllerConfigForID(", "SetDefaultControls(" };
    for (i = 0; i < 3; i++) {
        const char* f = after[i] ? kz3_pu_find(in, n, (size_t)(cd - in), after[i]) : NULL;
        if (!f) continue;
        size_t at = (size_t)(f - in);
        while (at < n && in[at] != '\n') at++;
        if (at >= n) continue;
        e[ne].at = at + 1; e[ne].del = 0; e[ne].ins = call; ne++;
    }
    /* apply in order (the edits never overlap: the picture is before the CDATA, the calls after it) */
    for (i = 1; i < ne; i++)
        for (j = i; j > 0 && e[j].at < e[j - 1].at; j--) { Kz3PuEdit t = e[j]; e[j] = e[j - 1]; e[j - 1] = t; }
    size_t total = n;
    for (i = 0; i < ne; i++) {
        size_t k = strlen(e[i].ins);
        int tail = k && e[i].ins[k - 1] == '\x01';
        total += (tail ? k - 1 + sizeof onpage - 1 : k) - e[i].del;
    }
    char* out = (char*)malloc(total + 1);
    if (!out) return NULL;
    size_t r = 0, w = 0;
    for (i = 0; i < ne; i++) {
        size_t k = strlen(e[i].ins);
        int tail = k && e[i].ins[k - 1] == '\x01';
        memcpy(out + w, in + r, e[i].at - r); w += e[i].at - r;
        memcpy(out + w, e[i].ins, tail ? k - 1 : k); w += tail ? k - 1 : k;
        if (tail) { memcpy(out + w, onpage, sizeof onpage - 1); w += sizeof onpage - 1; }
        r = e[i].at + e[i].del;
    }
    memcpy(out + w, in + r, n - r); w += n - r;
    out[w] = 0;
    *out_n = w;
    return out;
}

#ifdef KZ3_PROMPTS_UI_GLUE
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
extern const char* ppu_vfs_root;                           /* runtime: host dir of the PS3 mount points */
extern void (*g_ps3_vfs_miss_hook)(char* path, size_t cap);  /* sys_fs.c (patch 0004) */
#ifdef __cplusplus
}
#endif

static void (*s_pu_prev_miss)(char*, size_t);
static char s_pu_out[1100];          /* the patched page's host path, "" until written */
static int  s_pu_state;              /* 0 not tried, 1 written, -1 served unchanged */
static volatile int s_pu_lock;

static void kz3_pu_lock(void) { while (__atomic_exchange_n(&s_pu_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void kz3_pu_unlock(void) { __atomic_store_n(&s_pu_lock, 0, __ATOMIC_RELEASE); }

static const char* kz3_pu_root(void)    /* KZ3_CACHE_ROOT (kz3.ini cache_root), else the VFS root */
{
    const char* e = getenv("KZ3_CACHE_ROOT");
    return (e && *e) ? e : ppu_vfs_root;
}

static void kz3_pu_dir(char* dir, size_t cap)    /* <vfs root>/psarc-cache/kz3-ui, created */
{
    snprintf(dir, cap, "%s/psarc-cache", kz3_pu_root());
#ifdef _WIN32
    _mkdir(dir);
    snprintf(dir, cap, "%s/psarc-cache/kz3-ui", kz3_pu_root());
    _mkdir(dir);
#else
    mkdir(dir, 0755);
    snprintf(dir, cap, "%s/psarc-cache/kz3-ui", kz3_pu_root());
    mkdir(dir, 0755);
#endif
}

static char s_pp_out[1100], s_pp2_out[1100];   /* the Custom pages' host paths (patches 4014, 4023) */
static int  s_pp_state, s_pp2_state;           /* 0 not tried, 1 written, -1 not written */
static int  s_pp_lang, s_pp2_lang;             /* network pass 45: the language a page was written in */

/* Net pass 45: a page written in another language than the one in force is generated again (the game re-reads a
 * page at each open, so the change shows at the next open). */
static void kz3_pu_relang(int* state, int* lang)
{
    int now = kz3_tr_lang();
    if (*state != 0 && *lang != now) {
        if (*state == 1) KZ3_NET_PRINTF("[prompts] language %d -> %d: page generated again\n", *lang, now);
        *state = 0;
    }
    *lang = now;
}

/* Serve a generated Custom page (not in the archive: written on its first miss): padlayoutsettings.gum (on foot)
 * and kz3padlayout2.gum (vehicles, exo, guns; network pass 40). */
static void kz3_pp_serve(char* path, size_t cap, char* (*gen)(size_t*), int* state, int* lang, char* outp, size_t outcap,
                         const char* file, const char* what)
{
    kz3_pu_lock();
    kz3_pu_relang(state, lang);
    if (*state == 0) {
        size_t n = 0;
        char* g = gen(&n);
        char dir[1000];
        *state = -1;
        if (g) {
            FILE* o;
            kz3_pu_dir(dir, sizeof dir);
            snprintf(outp, outcap, "%s/%s", dir, file);
            o = fopen(outp, "wb");
            int ok = o && fwrite(g, 1, n, o) == n;
            if (o) ok = (fclose(o) == 0) && ok;
            if (ok) *state = 1;
        }
        KZ3_NET_PRINTF("[prompts] %s %s (%zu bytes)\n", what, *state == 1 ? "written" : "NOT written", n);
        fflush(stdout);
        free(g);
    }
    if (*state == 1) snprintf(path, cap, "%s", outp);
    kz3_pu_unlock();
    if (*state != 1 && s_pu_prev_miss) s_pu_prev_miss(path, cap);
}
static void kz3_padpage_serve(char* path, size_t cap)
{
    kz3_pp_serve(path, cap, kz3_padpage_gum, &s_pp_state, &s_pp_lang, s_pp_out, sizeof s_pp_out, "padlayoutsettings.gum",
                 "Custom layout page");
}
static void kz3_padpage2_serve(char* path, size_t cap)
{
    kz3_pp_serve(path, cap, kz3_padpage2_gum, &s_pp2_state, &s_pp2_lang, s_pp2_out, sizeof s_pp2_out, "kz3padlayout2.gum",
                 "Custom layout page (vehicles, exo, guns)");
}

static char s_ps_out[1100];          /* the Layout and Prompts page's host path (network pass 34) */
static int  s_ps_state;              /* 0 not tried, 1 written, -1 not written */

/* Serve interface/menu/gum/common/kz3padsettings.gum (not in the archive: written on its first miss). */
static void kz3_padset_serve(char* path, size_t cap)
{
    static int lang;
    kz3_pu_lock();
    kz3_pu_relang(&s_ps_state, &lang);
    if (s_ps_state == 0) {
        size_t n = 0;
        char* g = kz3_padset_gum(&n);
        char dir[1000];
        FILE* o;
        int ok;
        kz3_pu_dir(dir, sizeof dir);
        snprintf(s_ps_out, sizeof s_ps_out, "%s/kz3padsettings.gum", dir);
        o = g ? fopen(s_ps_out, "wb") : NULL;
        ok = o && fwrite(g, 1, n, o) == n;
        if (o) ok = (fclose(o) == 0) && ok;
        free(g);
        s_ps_state = ok ? 1 : -1;
        KZ3_NET_PRINTF("[prompts] Layout and Prompts page %s (%zu bytes)\n", ok ? "written" : "NOT written", n);
        fflush(stdout);
    }
    if (s_ps_state == 1) snprintf(path, cap, "%s", s_ps_out);
    kz3_pu_unlock();
    if (s_ps_state != 1 && s_pu_prev_miss) s_pu_prev_miss(path, cap);
}

typedef char* (*Kz3PuPatchFn)(const char*, size_t, size_t*);
static char s_pc_out[1100];          /* the patched controller page's host path (network pass 36) */
static int  s_pc_state;              /* 0 not tried, 1 written, -1 served unchanged */

/* Serve an archive page patched by fn: the first open reads the page the chain behind this hook serves, writes the
 * patched copy to psarc-cache/kz3-ui/<file> and serves it from then on; unchanged when fn refuses or the write fails. */
static void kz3_pu_patch_serve(char* path, size_t cap, int* state, int* lang, char* outp, size_t outcap, const char* file,
                               Kz3PuPatchFn fn, const char* what)
{
    kz3_pu_lock();
    kz3_pu_relang(state, lang);
    if (*state == 0) {
        char src[1100];
        snprintf(src, sizeof src, "%s", path);
        if (s_pu_prev_miss) s_pu_prev_miss(src, sizeof src);
        FILE* f = fopen(src, "rb");
        char* text = NULL;
        size_t len = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            long m = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (m > 0 && m < (1 << 20) && (text = (char*)malloc((size_t)m + 1)) != NULL) {
                len = fread(text, 1, (size_t)m, f);
                text[len] = 0;
            }
            fclose(f);
        }
        size_t pn = 0;
        char* patched = text ? fn(text, len, &pn) : NULL;
        *state = -1;
        if (patched) {
            char dir[1000];
            kz3_pu_dir(dir, sizeof dir);
            snprintf(outp, outcap, "%s/%s", dir, file);
            FILE* o = fopen(outp, "wb");
            int ok = o && fwrite(patched, 1, pn, o) == pn;
            if (o) ok = (fclose(o) == 0) && ok;
            if (ok) *state = 1;
        }
        KZ3_NET_PRINTF("[prompts] %s: %s %s\n", file, what,
               *state == 1 ? "added" : patched ? "NOT written (page served unchanged)" : "not added (page layout unknown)");
        fflush(stdout);
        free(patched);
        free(text);
        if (*state == 1) snprintf(path, cap, "%s", outp);
        else snprintf(path, cap, "%s", src);
        kz3_pu_unlock();
        return;
    }
    if (*state == 1) snprintf(path, cap, "%s", outp);
    kz3_pu_unlock();
    if (*state != 1 && s_pu_prev_miss) s_pu_prev_miss(path, cap);
}

/* Net pass 42: Options > Display gets the row "Discord status" (port/net/kz3_discord.c kd_patch_display_gum) on top
 * of the page main.cpp serves (the hook behind this one: patch 2095/2110's rows, tooltip panel and Lua). */
#include "kz3_discord.h"
static char s_pd_out[1100];
static int  s_pd_lang, s_pc_lang, s_pu_lang;   /* network pass 45: language of each patched page */
static int  s_pd_state;              /* 0 not tried, 1 written, -1 served as main.cpp serves it */

static void kz3_prompts_miss_hook(char* path, size_t cap)
{
    char low[1100];
    size_t n = strlen(path), i;
    if (n >= sizeof low) { if (s_pu_prev_miss) s_pu_prev_miss(path, cap); return; }
    for (i = 0; i <= n; i++) { char c = path[i]; c = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; low[i] = c == '\\' ? '/' : c; }
    const char* u = strstr(low, "usrdir/");
    if (u && !strcmp(u + 7, KZ3_PADPAGE_KEY)) { kz3_padpage_serve(path, cap); return; }
    if (u && !strcmp(u + 7, KZ3_PADPAGE2_KEY)) { kz3_padpage2_serve(path, cap); return; }   /* 4023 */
    if (u && !strcmp(u + 7, KZ3_PADSET_KEY)) { kz3_padset_serve(path, cap); return; }
    if (u && !strcmp(u + 7, KZ3_PROMPTS_GUM_KEY)) {
        kz3_host_composed_check(KZ3_PROMPTS_GUM_KEY, "the prompts row");   /* net 40: one-place list */
        kz3_pu_patch_serve(path, cap, &s_pu_state, &s_pu_lang, s_pu_out, sizeof s_pu_out, "advancedcontroller.gum",
                           kz3_prompts_patch_gum, "Layout and Prompts row");
        return;
    }
    if (u && !strcmp(u + 7, KZ3_HC_DISPLAY)) {
        kz3_host_composed_check(KZ3_HC_DISPLAY, "the Discord status row");
        kz3_pu_patch_serve(path, cap, &s_pd_state, &s_pd_lang, s_pd_out, sizeof s_pd_out, "displaysettings-net.gum",
                           kd_patch_display_gum, "Discord status row");
        return;
    }
    if (u && !strcmp(u + 7, KZ3_CTL_GUM_KEY)) {
        kz3_host_composed_check(KZ3_CTL_GUM_KEY, "the controller picture");
        kz3_pu_patch_serve(path, cap, &s_pc_state, &s_pc_lang, s_pc_out, sizeof s_pc_out, "controllersettings.gum",
                           kz3_ctl_patch_gum, "picture of the layout in force");
        return;
    }
    if (s_pu_prev_miss) s_pu_prev_miss(path, cap);
}

/* Once, from the first Localizer lookup (the menus and every miss hook are set up by then): chain the hook in front
 * of main.cpp's and psarc_vfs.cpp's. `bridge` = main.cpp's Keyboard-page Lua bridge is on this EBOOT. */
static void kz3_prompts_ui_install(int bridge)
{
    const char* e = getenv("KZ3_KB_PAGE");
    if (!bridge || (e && !strcmp(e, "0"))) {
        KZ3_NET_PRINTF("[prompts] Button prompts row off (%s)\n", bridge ? "KZ3_KB_PAGE=0" : "no Lua bridge on this EBOOT");
        fflush(stdout);
        return;
    }
    s_pu_prev_miss = g_ps3_vfs_miss_hook;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_ps3_vfs_miss_hook = kz3_prompts_miss_hook;
}
#endif /* KZ3_PROMPTS_UI_GLUE */

#endif /* KZ3_PROMPTS_UI_H */

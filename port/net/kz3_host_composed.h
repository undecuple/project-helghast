/* kz3_host_composed.h: the one list of game files the port rewrites (network pass 40, the RE notes s.34).
 *
 * Under KZ3_NATIVE_PATCH (default on for 1.14) the guest's own patch layer applies each 1.14 .pspatch to the file it
 * reads. A file the port rewrites in a miss hook (rows added to a menu .gum) must not get that treatment: the binary
 * delta targets the disc base, the rewritten file has shifted offsets, and the menu shows wrong rows (net 38: BUY NOW
 * on the main menu; net 39: the Layout and Prompts row missing). For these keys port/vfs/kz3_overlay.c serves update +
 * rows as the file (resolve_patched) and kz3_overlay_native_rekey leaves their .pspatch out of the native directory,
 * which happens once at init, before any rewrite runs: so the set is fixed here, at compile time, in one place.
 *
 * RULE: every rewriter takes its key from this header (KZ3_HC_* for a disc file it rewrites, KZ3_GEN_* for a page the
 * port generates, which has no .pspatch), never from a "interface/menu/gum/..." literal of its own. The host test
 * tools/net/test_host_composed.py fails on such a literal anywhere under port/ (sources and patches) and checks every
 * KZ3_HC_* macro is in kz3_host_composed_key(). A new rewrite: add a KZ3_HC_ macro and its line in the list below.
 * Add a new key in the same change as the code that rewrites that file.
 * Keys are lower case with '/' (the overlay's norm() form and the miss hooks' "usrdir/" suffix). */
#ifndef KZ3_HOST_COMPOSED_H
#define KZ3_HOST_COMPOSED_H

#include <stdio.h>

/* Disc files the port rewrites (each has, or may have, a 1.14 .pspatch). */
#define KZ3_HC_MAINMENU         "interface/menu/gum/mainmenu.gum"                    /* EXIT GAME row (main.cpp kz3_kb_miss_hook, pass 61) */
#define KZ3_HC_DISPLAY          "interface/menu/gum/common/displaysettings.gum"      /* Display mode row (patch 2095) */
#define KZ3_HC_SETTINGS         "interface/menu/gum/common/settings.gum"             /* Keyboard + Mouse rows (RSX pass 37) */
#define KZ3_HC_MAINSETTINGS     "interface/menu/gum/common/mainsettings.gum"         /* Keyboard + Mouse rows */
#define KZ3_HC_CAMPAIGNSETTINGS "interface/menu/gum/campaign/campaignsettings.gum"   /* Keyboard + Mouse rows (pause > Options) */
#define KZ3_HC_MP_OPTIONS       "interface/menu/gum/multiplayer/ingame/options.gum"  /* Keyboard + Mouse rows (MP pause) */
#define KZ3_HC_ADVCONTROLLER    "interface/menu/gum/common/advancedcontroller.gum"   /* Layout and Prompts row (port/net/kz3_prompts_ui.h) */
#define KZ3_HC_CONTROLLER       "interface/menu/gum/common/controllersettings.gum"   /* picture of the layout in force (kz3_prompts_ui.h) */
#define KZ3_HC_CAMPAIGNOPTIONS  "interface/menu/gum/campaign/campaignoptions.gum"    /* pause menu QUIT TO DESKTOP row (main.cpp, RSX pass 109) */

/* Pages the port generates (not in the archive, no .pspatch; listed so every key has one home). */
#define KZ3_GEN_KEYBOARD        "interface/menu/gum/common/keyboardsettings.gum"     /* main.cpp kz3_kb_page_gum */
#define KZ3_GEN_MOUSE           "interface/menu/gum/common/mousesettings.gum"        /* main.cpp kz3_mouse_page_gum (patch 2101) */
#define KZ3_GEN_PADLAYOUT       "interface/menu/gum/common/padlayoutsettings.gum"    /* port/net/kz3_padpage.h (patch 4014) */
#define KZ3_GEN_PADSET          "interface/menu/gum/common/kz3padsettings.gum"       /* port/net/kz3_prompts_ui.h */
#define KZ3_GEN_PADLAYOUT2      "interface/menu/gum/common/kz3padlayout2.gum"        /* kz3_padpage.h: vehicles, exo, guns (patch 4023) */

/* The host-composed set: i-th key, NULL past the end. */
static inline const char* kz3_host_composed_key(unsigned i)
{
    static const char* const k[] = {
        KZ3_HC_MAINMENU, KZ3_HC_DISPLAY, KZ3_HC_SETTINGS, KZ3_HC_MAINSETTINGS, KZ3_HC_CAMPAIGNSETTINGS,
        KZ3_HC_MP_OPTIONS, KZ3_HC_ADVCONTROLLER, KZ3_HC_CONTROLLER, KZ3_HC_CAMPAIGNOPTIONS,
    };
    return i < sizeof k / sizeof k[0] ? k[i] : (const char*)0;
}

/* 1 when `key` (any case, '/' or '\\') is in the set. */
static inline int kz3_host_composed_has(const char* key)
{
    if (!key) return 0;
    for (unsigned i = 0; kz3_host_composed_key(i); i++) {
        const char *a = key, *b = kz3_host_composed_key(i);
        for (;; a++, b++) {
            char c = *a;
            c = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c == '\\' ? '/' : c;
            if (c != *b) break;
            if (!c) return 1;
        }
    }
    return 0;
}

/* A rewriter calls this with its key before it serves a rewritten disc file: a key missing from the set is a port bug
 * (the native patch layer would re-patch the rewritten bytes) and gets one loud log line. Returns kz3_host_composed_has. */
static inline int kz3_host_composed_check(const char* key, const char* who)
{
    int ok = kz3_host_composed_has(key);
    if (!ok) fprintf(stderr, "[overlay] BUG: %s rewrites %s, which is not in kz3_host_composed.h (native patching will re-patch it)\n",
                     who ? who : "?", key ? key : "(null)");
    return ok;
}

#endif /* KZ3_HOST_COMPOSED_H */

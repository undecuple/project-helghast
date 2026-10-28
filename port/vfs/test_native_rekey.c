/* Host test for kz3_overlay_native_rekey (RE pass 168, the RE notes s.48.6). No game data: the patch
 * files are dummies named the way the update names them. Build/run: port/vfs/test_native_rekey.sh */
#include "kz3_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("[ok]   " __VA_ARGS__); else { fails++; printf("[FAIL] " __VA_ARGS__); } printf("\n"); } while (0)

static void put(const char* dir, const char* fn, const char* body)
{
    char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, fn);
    FILE* f = fopen(p, "wb"); if (f) { fputs(body, f); fclose(f); }
}
static int has(const char* dir, const char* fn, const char* body)
{
    char p[1200], b[64] = { 0 }; snprintf(p, sizeof p, "%s/%s", dir, fn);
    FILE* f = fopen(p, "rb"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    return !body || strcmp(b, body) == 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    char src[1100], dst[1100], nm[33], gum[33], gum_loose[33];
    snprintf(src, sizeof src, "%s/src", argv[1]);
    snprintf(dst, sizeof dst, "%s/native/patch_data_ps3_sp", argv[1]);
    const char* core = "LocalCachePS3/lumps/assets_description.application_application_assets.core";
    /* Known vectors: the update's real patch name for this lump (pass 165 log: 796151 -> 190270 bytes) and the key the
     * guest computes for the loose path when the archive is not mounted. */
    kz3_pspatch_name(core, nm);
    CHECK(strcmp(nm, "81AA7AF12D496E5E8ADAD77CB1A0348C") == 0, "archive key %s", nm);
    kz3_pspatch_name_loose(core, nm);
    CHECK(strcmp(nm, "8205E9EA6A40E7B054437FF51883A638") == 0, "loose key %s", nm);
    kz3_pspatch_name("interface/menu/gum/x.gum", gum);
    kz3_pspatch_name_loose("interface/menu/gum/x.gum", gum_loose);
    char fn[64];
    put(src, "81AA7AF12D496E5E8ADAD77CB1A0348C.pspatch", "A");
    snprintf(fn, sizeof fn, "%s.pspatch", gum); put(src, fn, "G");
    put(src, "0123456789ABCDEF0123456789ABCDEF.pspatch", "O");          /* no disc base (like the 45 MP-only) */
    put(src, "readme.txt", "x");
    put(dst, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF.pspatch", "stale");
    const char* names[] = { "", core, "Interface/Menu/GUM/x.gum", "localcacheps3/LUMPS/assets_description.application_application_assets.core" };
    int ren = -1;
    int n = kz3_overlay_native_rekey(src, dst, names, 4, &ren);
    CHECK(n == 3 && ren == 2, "placed %d (want 3), renamed %d (want 2)", n, ren);
    CHECK(has(dst, "8205E9EA6A40E7B054437FF51883A638.pspatch", "A"), "lump patch under the loose key");
    CHECK(!has(dst, "81AA7AF12D496E5E8ADAD77CB1A0348C.pspatch", NULL), "no copy under the archive key");
    snprintf(fn, sizeof fn, "%s.pspatch", gum_loose);
    CHECK(has(dst, fn, "G"), "gum patch under the loose key");
    CHECK(has(dst, "0123456789ABCDEF0123456789ABCDEF.pspatch", "O"), "orphan kept under its own name");
    CHECK(!has(dst, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF.pspatch", NULL), "stale patch removed");
    CHECK(!has(dst, "readme.txt", NULL), "non-patch file not copied");
    n = kz3_overlay_native_rekey(src, dst, names, 4, &ren);
    CHECK(n == 3 && ren == 2, "second run idempotent (%d, %d)", n, ren);
    /* Net pass 38: the files the port rewrites stay host-composed: their patch is not given to the guest. */
    CHECK(kz3_overlay_host_composed("Interface/Menu/GUM/MainMenu.gum") == 1 && kz3_overlay_host_composed("interface/menu/gum/x.gum") == 0,
          "host-composed keys: main menu yes (any case), another gum no");
    char mm[33], mm_l[33], ds[33]; 
    kz3_pspatch_name("interface/menu/gum/mainmenu.gum", mm); kz3_pspatch_name_loose("interface/menu/gum/mainmenu.gum", mm_l);
    kz3_pspatch_name("interface/menu/gum/common/displaysettings.gum", ds);
    snprintf(fn, sizeof fn, "%s.pspatch", mm); put(src, fn, "M");
    snprintf(fn, sizeof fn, "%s.pspatch", ds); put(src, fn, "D");
    const char* names2[] = { core, "Interface/Menu/GUM/x.gum", "interface/menu/gum/mainmenu.gum", "interface/menu/gum/common/displaysettings.gum" };
    n = kz3_overlay_native_rekey(src, dst, names2, 4, &ren);
    CHECK(n == 3 && ren == 2, "with main menu + display patches in src: placed %d (want 3), renamed %d (want 2)", n, ren);
    snprintf(fn, sizeof fn, "%s.pspatch", mm_l); CHECK(!has(dst, fn, NULL), "main menu patch not under the loose key");
    snprintf(fn, sizeof fn, "%s.pspatch", mm); CHECK(!has(dst, fn, NULL), "main menu patch not under the archive key");
    snprintf(fn, sizeof fn, "%s.pspatch", gum_loose); CHECK(has(dst, fn, "G"), "other gum patch still served");
    char missing[1200]; snprintf(missing, sizeof missing, "%s/nope", argv[1]);
    CHECK(kz3_overlay_native_rekey(missing, dst, names, 4, &ren) == -1, "missing source -> -1");
    printf(fails ? "FAIL (%d failed)\n" : "PASS (0 failed)\n", fails);
    return fails ? 1 : 0;
}

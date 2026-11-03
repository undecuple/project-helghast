/* Host test for KZ3 patch 2146 (PS3RECOMP_PRODUCT_NAME): the settings files' comment text carries the product name.
 * Built twice by test_product_name.sh: no -D (default must be "Project Helghast") and with
 * -DPS3RECOMP_PRODUCT_NAME="\"Qwark's Awakening\"" (override must replace it in both files' comments). */
#include <stdio.h>
#include <string.h>
#include "pad_keybinds.h"
#include "rsx_display_mode.h"

#ifndef EXPECT
#define EXPECT "Project Helghast"
#endif

static int fails;
#define CHECK(c, m) do { if (c) printf("[PASS] %s\n", m); else { printf("[FAIL] %s\n", m); fails++; } } while (0)

int main(void)
{
    static char kb[16384], d1[2048], d2[2048];
    PadKeybinds k; memset(&k, 0, sizeof k);
    pad_kb_defaults(&k);
    pad_kb_format(&k, kb, sizeof kb);
    rsx_dm_ini_write(d1, sizeof d1, RSX_DM_WINDOWED, 0, 0, 1);
    rsx_dm_ini_write_ext(d2, sizeof d2, RSX_DM_WINDOWED, 0, 0, 1, 100, 30);
    CHECK(!strcmp(PS3RECOMP_PRODUCT_NAME, EXPECT), "macro value");
    CHECK(!strncmp(kb, "# " EXPECT " key bindings. Written", strlen("# " EXPECT " key bindings. Written")), "keybinds.ini first line");
    CHECK(!strncmp(d1, "# " EXPECT " display settings (", strlen("# " EXPECT " display settings (")), "display.ini first line");
    CHECK(!strncmp(d2, "# " EXPECT " display settings (", strlen("# " EXPECT " display settings (")), "display.ini (ext) first line");
    if (strcmp(EXPECT, "Project Helghast") != 0)
        CHECK(!strstr(kb, "Project Helghast") && !strstr(d1, "Project Helghast") && !strstr(d2, "Project Helghast"), "override leaves no old name");
    return fails ? 1 : 0;
}

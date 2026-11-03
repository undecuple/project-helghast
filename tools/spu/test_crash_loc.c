/* Host test for the crash-line formatting (patch 1011, SPU pass 137): ps3_crash_fmt_loc / ps3_crash_fmt_line from
 * include/ps3emu/crash_loc.h. Built by test_crash_loc.sh, which extracts the header from the patch. */
#include "ps3emu/crash_loc.h"
#include <string.h>
static int fails;
#define EQ(got, want) do { if (strcmp(got, want)) { printf("FAIL line %d: got [%s] want [%s]\n", __LINE__, got, want); fails++; } } while (0)
int main(void)
{
    char loc[300], ln[400];
    ps3_crash_fmt_loc(loc, sizeof loc, "C:\\Games\\KZ3\\kz3.exe", 0x1A2B3C, 0x7FF6001A2B3CULL);
    EQ(loc, "kz3.exe+0x1A2B3C");
    ps3_crash_fmt_loc(loc, sizeof loc, "/usr/lib/libx.so", 0x10, 1);
    EQ(loc, "libx.so+0x10");
    ps3_crash_fmt_loc(loc, sizeof loc, "nvwgf2umx.dll", 0xABC, 0x7FFB0000ABCULL);
    EQ(loc, "nvwgf2umx.dll+0xABC");
    ps3_crash_fmt_loc(loc, sizeof loc, NULL, 0, 0x1F2E3D4C5B6AULL);
    EQ(loc, "? abs=0x1F2E3D4C5B6A");
    ps3_crash_fmt_loc(loc, sizeof loc, "", 5, 0x99);
    EQ(loc, "? abs=0x99");
    ps3_crash_fmt_line(ln, sizeof ln, "kz3.exe+0x10", "ppu:3 main");
    EQ(ln, "[CRASH] rip in kz3.exe+0x10 thread \"ppu:3 main\"");
    ps3_crash_fmt_line(ln, sizeof ln, "? abs=0x99", NULL);
    EQ(ln, "[CRASH] rip in ? abs=0x99 thread \"-\"");
    ps3_crash_fmt_line(ln, sizeof ln, "a+0x1", "");
    EQ(ln, "[CRASH] rip in a+0x1 thread \"-\"");
    ps3_crash_fmt_loc(loc, 8, "C:\\x\\longmodulename.dll", 0x1, 1);   /* truncates, stays terminated */
    if (strlen(loc) != 7) { printf("FAIL truncation\n"); fails++; }
    puts(fails ? "FAIL" : "PASS test_crash_loc");
    return fails != 0;
}

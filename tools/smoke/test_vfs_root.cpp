// Host test of port/kz3/kz3_vfs_root.h (the game root from the EBOOT path): built and run by test_vfs_root.sh.
#include <cstdio>
#include <set>
#include <string>
#include "kz3_vfs_root.h"

static int fails;
static void t(const char* eboot, std::set<std::string> dirs, const char* want, const char* name)
{
    const char* how = "";
    std::string got = kz3_game_root(eboot, [&](const std::string& p) { return dirs.count(p) != 0; }, &how);
    if (got != want) { fails++; printf("FAIL %s: %s -> %s (want %s; %s)\n", name, eboot, got.c_str(), want, how); }
}

int main()
{
    t("C:/Games/KZ3/PS3_GAME/USRDIR/EBOOT.elf", {}, "C:/Games/KZ3", "disc layout");
    t("C:\\Games\\KZ3\\ps3_game\\usrdir\\EBOOT.BIN", {}, "C:/Games/KZ3", "disc layout, backslashes, lower case");
    t("PS3_GAME/USRDIR/EBOOT.elf", {}, ".", "relative disc layout");
    t("./PS3_GAME/USRDIR/EBOOT.elf", {}, ".", "dot-relative disc layout");
    t("C:/KZ3/update-1.14/EBOOT-1.14.elf", {"C:/KZ3/PS3_GAME"}, "C:/KZ3", "update EBOOT beside the dump (the VM case)");
    t("update-1.14\\EBOOT-1.14.elf", {"./PS3_GAME", "update-1.14/PS3_GAME"}, "update-1.14", "nearest PS3_GAME wins");
    t("update-1.14/EBOOT-1.14.elf", {}, ".", "no PS3_GAME anywhere: working folder");
    t("C:/x/y/EBOOT.elf", {}, ".", "absolute, no PS3_GAME: working folder (not the drive root)");
    t("EBOOT.elf", {}, ".", "bare name");
    t("/srv/kz3/dev_hdd0/game/BCUS98234/USRDIR/EBOOT.BIN", {"/srv/kz3/PS3_GAME"}, "/srv/kz3", "installed update EBOOT");
    if (fails) { printf("FAIL: test_vfs_root (%d failure(s))\n", fails); return 1; }
    printf("PASS: test_vfs_root (10 cases)\n");
    return 0;
}

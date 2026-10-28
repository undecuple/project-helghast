/* The game root (the folder holding PS3_GAME, which /dev_bdvd and /app_home read) from the EBOOT path.
 *   <root>/PS3_GAME/USRDIR/EBOOT.*  -> <root> (the disc layout; folder names in any case)
 *   anything else                   -> the nearest folder above the EBOOT that holds PS3_GAME/, else "."
 * Before 2026-10-22 any EBOOT path lost three components, so an update EBOOT outside USRDIR
 * (bundle\update-1.14\EBOOT-1.14.elf) gave the drive root and /dev_hdd0 resolved under it.
 * Header only, no game state: tools/smoke/test_vfs_root.sh tests it on the host. */
#pragma once
#include <string>

/* is_dir(path) says whether path is a folder; how gets a short word for the log. */
template <class IsDir> static std::string kz3_game_root(const char* eboot, IsDir is_dir, const char** how)
{
    std::string d = eboot ? eboot : "";
    for (char& c : d)
        if (c == '\\') c = '/';
    size_t s = d.rfind('/');
    d = (s == std::string::npos) ? std::string(".") : (s == 0 ? std::string("/") : d.substr(0, s));
    auto lower = [](std::string v) {
        for (char& c : v)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return v;
    };
    auto parent = [](const std::string& p) -> std::string {
        size_t k = p.rfind('/');
        if (k == std::string::npos) return std::string();
        return k == 0 ? std::string("/") : p.substr(0, k);
    };
    std::string ld = lower(d);
    const std::string tail = "ps3_game/usrdir";
    if (ld == tail || (ld.size() > tail.size() && ld.compare(ld.size() - tail.size(), tail.size(), tail) == 0 &&
                       ld[ld.size() - tail.size() - 1] == '/')) {
        std::string r = parent(parent(d));
        if (how) *how = "disc layout";
        return r.empty() ? std::string(".") : r;
    }
    for (std::string p = d; !p.empty(); p = parent(p)) {
        if (is_dir(p + "/PS3_GAME")) {
            if (how) *how = "PS3_GAME found above the EBOOT";
            return p;
        }
        if (p == "/" || p == ".") break;
    }
    if (how) *how = "no PS3_GAME above the EBOOT: working folder";
    return ".";
}

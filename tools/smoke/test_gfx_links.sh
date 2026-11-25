#!/usr/bin/env bash
# Host test (patch 2206): kz3_kb_link_rows from port/kz3/main.cpp adds exactly one Graphics row right after the game's Display row of
# each of the four options lists (synthetic text, no game data), keeps the Keyboard / Mouse rows and every other row as they were, adds
# nothing when the page is not offered (no Graphics lever) or the list has no Display row, and a mutant (a wrong anchor) fails.
#   nix develop .#ci --command bash -c 'CXX=clang++ tools/smoke/test_gfx_links.sh'
set -euo pipefail
D="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$D/work/ps3recomp-patched}"
if [[ ! -f "$PT/libs/video/rsx_gfx_page.h" ]]; then echo "[test_gfx_links] SKIP: no patched toolkit with patch 2206 (run tools/ps3recomp-patched.sh)"; exit 77; fi
CXX="${CXX:-c++}"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
sed -n '/^static std::string kz3_kb_link_rows/,/^}/p' "$D/port/kz3/main.cpp" > "$T/fn.inc"
[[ -s "$T/fn.inc" ]] || { echo "[FAIL] kz3_kb_link_rows not found in port/kz3/main.cpp"; exit 1; }
cat > "$T/t.cpp" <<'C'
#include <string>
#include <cstring>
#include <cctype>
#include <cstdio>
#include "rsx_gfx_page.h"
#include "fn.inc"

static int fails = 0;
#define CHECK(c, ...) do { const int ok_ = (c) ? 1 : 0; printf(ok_ ? "[PASS] " : "[FAIL] "); if (!ok_) fails++; printf(__VA_ARGS__); printf("\n"); } while (0)

static int count(const std::string& h, const std::string& n) { int k = 0; for (size_t p = h.find(n); p != std::string::npos; p = h.find(n, p + n.size())) k++; return k; }

int main()
{
    const char* disp = "<listitem id=\"lst_display\" label=\"@key{menu_display}\" onaccept=\"page(file:///common/displaysettings.gum)\"/>";
    const char* disp_c = "<listitem id=\"lst_display\" class=\"campoptions\" label=\"@key{menu_display}\" onaccept=\"page(file:///common/displaysettings.gum)\" enabled=\"true\" />";
    const char* ctl = "<listitem id=\"lst_controller\" label=\"@key{menu_controller}\" onaccept=\"page(file:///common/controllersettings.gum)\"/>";
    const char* aud = "<listitem id=\"lst_audio\" label=\"@key{menu_audio}\" onaccept=\"page(file:///common/audiosettings.gum)\"/>";
    struct L { const char* name; std::string text; } lists[4] = {
        { "settings.gum",          std::string("<gum>\n\t<list id=\"lst_options\">\n\t\t") + ctl + "\n\t\t" + disp + "\n\t\t" + aud + "\n\t</list>\n</gum>\n" },
        { "mainsettings.gum",      std::string("<gum>\n\t<list id=\"lst_options\">\n\t\t") + disp + "\n\t\t" + ctl + "\n\t\t" + aud + "\n\t</list>\n</gum>\n" },
        { "campaignsettings.gum",  std::string("<gum>\n\t<list>\n\t\t") + aud + "\n\t\t" + ctl + "\n\t\t" + disp_c + "\n\t</list>\n</gum>\n" },
        { "ingame/options.gum",    std::string("<gum>\n\t<list>\n\t\t") + ctl + "\n\t\t" + aud + "\n\t\t" + disp + "\n\t\t" + aud + "\n\t</list>\n</gum>\n" },
    };
    const std::string want_label = "label=\"" RSX_GFX_LINK_LABEL "\"";
    for (auto& l : lists) {
        int added = 0, g = 0, added0 = 0, g0 = -1;
        const std::string off = kz3_kb_link_rows(l.text, &added0, false, &g0);
        const std::string on = kz3_kb_link_rows(l.text, &added, true, &g);
        const size_t dp = on.find("displaysettings.gum");
        const size_t gp = on.find("graphicssettings.gum");
        const size_t gb = on.rfind("<listitem", gp);
        CHECK(g == 1 && added == 1 && count(on, "graphicssettings.gum") == 1 && count(on, want_label) == 1, "%s: exactly one Graphics row added (g=%d)", l.name, g);
        CHECK(dp != std::string::npos && gp != std::string::npos && on.find("<listitem", on.rfind("<listitem", dp) + 1) == gb,
              "%s: the Graphics row is the very next row after Display", l.name);
        CHECK(on.find("id=", gb) == std::string::npos || on.find("id=", gb) > on.find("/>", gb), "%s: the copy carries no id of the Display row", l.name);
        CHECK(count(on, "displaysettings.gum") == 1 && on.find(std::string(l.name[0] == 'c' ? disp_c : disp)) != std::string::npos, "%s: the Display row itself is unchanged", l.name);
        CHECK(count(on, "keyboardsettings.gum") == 1 && count(on, "mousesettings.gum") == 1, "%s: the Keyboard and Mouse rows are still added once", l.name);
        CHECK(g0 == 0 && added0 == 1 && off.find("graphicssettings.gum") == std::string::npos && count(off, "keyboardsettings.gum") == 1,
              "%s: without the Graphics page (no lever) no row is added and the Keyboard and Mouse rows are as before", l.name);
        /* removing the Graphics row from the output gives the output without it: nothing else moved */
        const size_t nl = on.rfind('\n', gb);
        const size_t ge = on.find("/>", gb) + 2;
        std::string minus = on;
        minus.erase(nl, ge - nl);
        CHECK(minus == off, "%s: everything else is byte for byte what it was", l.name);
        int a2 = 0, g2 = 0;
        const std::string again = kz3_kb_link_rows(on, &a2, true, &g2);
        CHECK(g2 == 0 && count(again, "graphicssettings.gum") == 1, "%s: a second pass adds no second Graphics row", l.name);
    }
    {   /* a list without a Display row */
        const std::string nodisp = std::string("<gum>\n\t<list>\n\t\t") + ctl + "\n\t\t" + aud + "\n\t</list>\n</gum>\n";
        int a = 0, g = 5;
        const std::string o = kz3_kb_link_rows(nodisp, &a, true, &g);
        CHECK(g == 0 && a == 1 && o.find("graphicssettings.gum") == std::string::npos, "no Display row: no Graphics row, Keyboard and Mouse added");
        const std::string nothing = "<gum>\n</gum>\n";
        CHECK(kz3_kb_link_rows(nothing, &a, true, &g) == nothing && a == 0 && g == 0, "no anchors at all: served unchanged");
    }
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
C
build() { "$CXX" -std=c++17 -Wall -Wextra -I"$PT/libs/video" -I"$1" "$1/t.cpp" -o "$1/t"; }
cp "$T/fn.inc" "$T/fn.keep"
build "$T"
"$T/t" | grep -v "^\[PASS\]"
# negative controls: a wrong anchor, and a row added before Display instead of after it
mkdir -p "$T/m1" "$T/m2"; cp "$T/t.cpp" "$T/m1/"; cp "$T/t.cpp" "$T/m2/"
sed 's/low.find("displaysettings.gum)")/low.find("xdisplaysettings.gum)")/' "$T/fn.inc" > "$T/m1/fn.inc"
sed '/if (gfx \&\& out.find/,/^    return out;/ s/out.insert(e + 2, /out.insert(b, /' "$T/fn.inc" > "$T/m2/fn.inc"
for m in m1 m2; do
  cmp -s "$T/fn.inc" "$T/$m/fn.inc" && { echo "[FAIL] mutant $m did not change the function"; exit 1; }
  build "$T/$m"
  if "$T/$m/t" >/dev/null; then echo "[FAIL] negative control $m was NOT caught"; exit 1; else echo "[PASS] negative control $m fails the test"; fi
done

#!/usr/bin/env bash
# Host test (pass 109): kz3_pause_exit_rows from port/kz3/main.cpp adds QUIT TO DESKTOP after the game's Quit row of
# both pause GUMs (synthetic text, no game data) and leaves a file without the Quit row unchanged.
set -euo pipefail
D="$(cd "$(dirname "$0")/../.." && pwd)"; T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
{ echo '#include <string>'; echo '#include <cstring>'; echo '#include <cctype>'
  sed -n '/^static std::string kz3_pause_exit_rows/,/^}/p' "$D/port/kz3/main.cpp"
  cat <<'C'
#include <cstdio>
int main(){
 std::string a="<gum>\n\t\t<list>\n\t\t\t<listitem class=\"campoptions\" label=\"@key{menu_quit}\" value=\"OnQuitClose\" onaccept=\"script(optionsscript,OpenDialog)\" />\n\t\t\t<listitem label=\"@key{menu_quit_reload}\"/>\n\t\t</list>\n</gum>";
 int n=0; std::string o=kz3_pause_exit_rows(a,&n);
 size_t q=o.find("QUIT TO DESKTOP"), r=o.find("menu_quit_reload");
 if(n!=1||q==std::string::npos||q>r||o.find("OnQuitClose",o.find("QUIT TO DESKTOP")-60)!=std::string::npos&&o.find("OnQuitClose")>q||o.find("dlg_kz3_exit_no")==std::string::npos||o.find("menu_quitgame_confirmation")==std::string::npos){puts("FAIL row");return 1;}
 std::string b="<gum>\n</gum>"; n=5; if(kz3_pause_exit_rows(b,&n)!=b||n!=0){puts("FAIL unchanged");return 1;}
 puts("PASS"); return 0; }
C
} > "$T/t.cpp"
g++ -std=c++17 -o "$T/t" "$T/t.cpp" && "$T/t"

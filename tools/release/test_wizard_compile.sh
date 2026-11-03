#!/usr/bin/env bash
# Host test of tools/release/wizard_test/compile_cmd.sh (no Proton, no game data): the batch file quotes a repo path
# with a space, and a compile that wrote no setup.exe (or reported a non-zero ISCC exit) is a non-zero verdict.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=wizard_test/compile_cmd.sh
source "$here/wizard_test/compile_cmd.sh"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
fails=0
chk() { if [[ "$1" == 0 ]]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails + 1)); fi; }

write_compile_cmd "$T/c.cmd" 'Z:\home\x\Killzone 3\installer' 'Z:\p a\payload' 'Z:\o u\out' '/DVersion=1 2'
grep -q 'cd /d "Z:\\home\\x\\Killzone 3\\installer"' "$T/c.cmd"; chk $? "installer folder with a space is quoted"
grep -q '"/DPayload=Z:\\p a\\payload" "/OZ:\\o u\\out" "/DVersion=1 2" kz3.iss > "Z:\\o u\\out\\iscc.txt"' "$T/c.cmd"; chk $? "payload, output and extra args quoted"
grep -q $'\r$' "$T/c.cmd"; chk $? "CRLF line ends"

mkdir "$T/o"; : > "$T/stamp"; sleep 1
compile_verdict "$T/o" "$T/stamp" 2>/dev/null; rc=$?; chk $((rc == 1 ? 0 : 1)) "no iscc.txt fails"
printf 'Successful compile\n[exit code 0]\n' > "$T/o/iscc.txt"
compile_verdict "$T/o" "$T/stamp" 2>"$T/err"; rc=$?; grep -q "wrote no setup" "$T/err"; g=$?; chk $((rc == 1 && g == 0 ? 0 : 1)) "exit 0 but no setup.exe fails (the 2026-10 silent case)"
: > "$T/o/old.exe"; touch -d '2000-01-01' "$T/o/old.exe"
compile_verdict "$T/o" "$T/stamp" 2>/dev/null; rc=$?; chk $((rc == 1 ? 0 : 1)) "an older setup.exe does not count"
: > "$T/o/new.exe"
compile_verdict "$T/o" "$T/stamp" 2>/dev/null; chk $? "exit 0 and a new setup.exe passes"
printf 'Error\n[exit code 2]\n' > "$T/o/iscc.txt"
compile_verdict "$T/o" "$T/stamp" 2>/dev/null; rc=$?; chk $((rc == 1 ? 0 : 1)) "ISCC exit code 2 fails"
echo "test_wizard_compile: $([[ $fails == 0 ]] && echo PASS || echo "FAIL ($fails)")"
[[ $fails == 0 ]]

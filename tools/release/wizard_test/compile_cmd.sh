# shellcheck shell=bash
# Helpers of wizard_test.sh compile (sourced; also by tools/release/test_wizard_compile.sh on the host).
# The repo path may hold a space ("Killzone 3"): a one-line `cmd /c "..."` loses its quotes, so the compile is a
# batch file with every path quoted, run as `cmd /c C:\c.cmd`.

# write_compile_cmd FILE INSTALLER_WINPATH PAYLOAD_WINPATH OUT_WINPATH [ISCC args...]
# Writes the batch file (CRLF): cd to the installer folder, ISCC output to OUT\iscc.txt, the exit code appended.
write_compile_cmd() {
  local f="$1" inst="$2" payload="$3" out="$4" extra="" a; shift 4
  for a in "$@"; do extra+=" \"$a\""; done
  printf '@echo off\r\ncd /d "%s"\r\n"C:\\inno\\ISCC.exe" "/DPayload=%s" "/O%s"%s kz3.iss > "%s\\iscc.txt" 2>&1\r\necho [exit code %%errorlevel%%] >> "%s\\iscc.txt"\r\n' \
    "$inst" "$payload" "$out" "$extra" "$out" "$out" > "$f"
}

# compile_verdict OUTDIR STAMPFILE: 0 only when ISCC reported exit code 0 and wrote a setup .exe newer than
# STAMPFILE; otherwise says why on stderr and returns 1 (a compile that wrote nothing must fail loudly).
compile_verdict() {
  local out="$1" stamp="$2" code exe
  code=$(grep -o '\[exit code [0-9-]*\]' "$out/iscc.txt" 2>/dev/null | tail -1)
  exe=$(find "$out" -maxdepth 1 -name '*.exe' -newer "$stamp" 2>/dev/null | head -1)
  if [[ -z "$code" ]]; then echo "compile FAILED: ISCC left no result (no iscc.txt or no exit code line): $out" >&2; return 1; fi
  if [[ "$code" != "[exit code 0]" ]]; then echo "compile FAILED: ISCC $code (see $out/iscc.txt)" >&2; return 1; fi
  if [[ -z "$exe" ]]; then echo "compile FAILED: ISCC exited 0 but wrote no setup .exe in $out" >&2; return 1; fi
  return 0
}

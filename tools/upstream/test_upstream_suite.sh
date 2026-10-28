#!/usr/bin/env bash
# Upstream's own tests for fixes ps3recomp v0.15.0 absorbed from us (or reworked), run on the patched tree: a KZ3 patch
# that undoes one of them fails here. Each upstream test finds its tree from its own path (tools/..), so running the
# copy in work/ps3recomp-patched tests the patched sources. SKIP without the patched tree.
#   nix develop --command tools/sandbox.sh -- tools/upstream/test_upstream_suite.sh
# Ours -> upstream test (PR): 3003 -> test_ppu_tls_blocks (#253); 2019 -> test_gcm_io_mappings (#255, #248);
# 3001 -> test_savedata_user_autosave (#276); 3002 -> test_ppu_fs_imports (#257); 2060 -> test_font_hle_nids (#324);
# VFS root -> test_cellfs_vfs_root (#249).
# Not listed (they link only upstream's own source list, and the patched sources call KZ3 additions outside it):
# test_spu_context_registries (#290) and test_mfc_resume_elements need runtime/diag/kz3_wait_acct.c (patch 1032);
# test_spurs_taskset_size and test_spurs_task_exit_code_initialize (#283/#284) need spu_ll_* (1056) and the 1074 kernel
# hooks. Their cases: 1013/1083 in test_mfc_many_contexts.sh and tools/spu/test_mfc_slots.sh, #290's coherency test in
# tools/spu/test_lockline.sh, 1092's taskset layout in tools/spu/test_ts_be_layout.sh.
# test_gcm_io_mappings (v0.17.0) extracts populate_offset_table, which patch 2082/2083 extends with the weak
# ppu_store_note_pages and s_ppu_note_on: it is compiled with a stub header (-include) that declares both, off.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
P="${NEW_TREE:-$ROOT/work/ps3recomp-patched}"
[[ -d "$P/tools" ]] || { echo "SKIP: no patched toolkit at $P"; exit 0; }
rc=0; n=0
STUB="$(mktemp -d)"; trap 'rm -rf "$STUB"' EXIT
cat > "$STUB/kz3_note_stub.h" <<'H'
#include <stdint.h>
static int s_ppu_note_on;
extern int ppu_store_note_pages(uint32_t ea, uint32_t size, int on) __attribute__((weak));
H
printf '#!/bin/sh\nexec %s -include %s "$@"\n' "${CC:-cc}" "$STUB/kz3_note_stub.h" > "$STUB/cc"; chmod +x "$STUB/cc"
for t in test_ppu_tls_blocks test_gcm_io_mappings test_savedata_user_autosave test_ppu_fs_imports test_font_hle_nids \
         test_cellfs_vfs_root; do
  n=$((n + 1))
  [[ -f "$P/tools/$t.py" ]] || { echo "FAIL $t: not in $P/tools"; rc=1; continue; }
  cc="${CC:-cc}"; [[ $t == test_gcm_io_mappings ]] && cc="$STUB/cc"
  out="$(cd "$P" && CC="$cc" python3 "tools/$t.py" 2>&1)"; r=$?
  if [[ $r == 0 ]]; then echo "ok   $t"; else echo "FAIL $t (exit $r)"; tail -4 <<<"$out"; rc=1; fi
done
echo "upstream suite on the patched tree: $n tests, $([[ $rc == 0 ]] && echo PASS || echo FAIL)"
exit $rc

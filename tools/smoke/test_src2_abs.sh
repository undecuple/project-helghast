#!/usr/bin/env bash
# Host test for KZ3 patch 2057 (SRC2 abs modifier, from robbe1912/ps3recomp 0d03c8b5): a MAD whose third source has
# DWORD3 bit 18 set decodes to abs(...) with LD_SRC2_ABS=1 and is unchanged without it. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_src2_abs.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_fp_decompiler.c" || ! -d "$PT/include" ]]; then echo "[test_src2_abs] SKIP: no patched toolkit"; exit 0; fi
if ! grep -q FP_SRC2_ABS "$V/rsx_fp_decompiler.c"; then echo "[test_src2_abs] SKIP: toolkit without patch 2057"; exit 0; fi
O="$ROOT/build/smoke-host/src2_abs"; mkdir -p "$O"
cat > "$O/t.c" <<'EOC'
#include "rsx_fp_decompiler.h"
#include "rsx_test_programs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char** argv)
{
    int want = argc > 1 && argv[1][0] == '1';
    u8 fp[16]; static char h[65536];
    /* MAD r0, COL0, COL0, r1.xyzw with SRC2 abs set (DWORD3: temp register 1, identity swizzle, bit 18) */
    rsx_test_fp_instr(fp, RSX_TEST_FP_OPC(4) | RSX_TEST_FP_MASK_XYZW | RSX_TEST_FP_INSRC_COL0 | RSX_TEST_FP_END,
                      RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(fp + 8, RSX_TEST_FP_SRC_INPUT | RSX_TEST_FP_SWZ_IDENT);
    rsx_test_fp_put_word(fp + 12, (1u << 2) | RSX_TEST_FP_SWZ_IDENT | (1u << 18));
    if (rsx_fp_decompile_buffered_ex(fp, 16, 0x40, 0, h, sizeof h, NULL) <= 0) { puts("FAIL decompile"); return 1; }
    const char* plus = strstr(h, ") + (");
    if (!plus) { puts("FAIL no MAD text"); return 1; }
    const int has_abs = strncmp(plus, ") + (abs(", 9) == 0;
    if (has_abs != want) { printf("FAIL abs=%d want=%d: %.120s\n", has_abs, want, plus); return 1; }
    printf("src2 abs %s: ok\n", want ? "on" : "off");
    return 0;
}
EOC
${CC:-cc} -std=c11 -Wall -O1 -I"$V" -I"$PT/include" "$O/t.c" "$V/rsx_fp_decompiler.c" -o "$O/t" -lm 2>&1 | grep -i "error" || true
"$O/t" 0
LD_SRC2_ABS=1 "$O/t" 1

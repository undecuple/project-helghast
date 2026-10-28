#!/usr/bin/env bash
# Host test for KZ3 patches 2049/2050 (ps3recomp PR #302/#303 by tekulvw): colour-input clamp and the depth-comparison
# metadata API of the fragment decompiler. Builds the PR's test_shadow_metadata.c (copied to tools/smoke) plus a
# #303 check against the patched toolkit. No game data, no GPU.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_shadow_metadata.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_fp_decompiler.c" || ! -d "$PT/include" ]]; then
  echo "[test_shadow_metadata] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
if ! grep -q rsx_fp_shadow_desc "$V/rsx_fp_decompiler.h"; then
  echo "[test_shadow_metadata] SKIP: toolkit without patch 2050"; exit 0; fi
O="$ROOT/build/smoke-host/shadow_metadata"; mkdir -p "$O"
${CC:-cc} -std=c11 -Wall -O1 -I"$V" -I"$PT/include" "$ROOT/tools/smoke/test_shadow_metadata.c" \
  "$V/rsx_fp_decompiler.c" -o "$O/test_shadow_metadata" -lm
"$O/test_shadow_metadata"
# #303: COL0/COL1 are saturated before abs/negate, TC0 is not.
cat > "$O/col.c" <<'EOC'
#include "rsx_fp_decompiler.h"
#include "rsx_test_programs.h"
#include <stdio.h>
#include <string.h>
int main(void){u8 fp[16];static char h[65536];
 rsx_test_fp_instr(fp,RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC(2)|RSX_TEST_FP_END,
  RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ(2,1,0,3)|(1u<<29)|(1u<<17));
 if(rsx_fp_decompile_buffered_ex(fp,16,0x40,0,h,sizeof h,NULL)<=0||!strstr(h,"-(abs((saturate(input.col1)).zyxw))")){puts("FAIL col1 clamp");return 1;}
 rsx_test_fp_instr(fp,RSX_TEST_FP_OPC(RSX_TEST_FP_OP_MOV)|RSX_TEST_FP_MASK_XYZW|RSX_TEST_FP_INSRC(4)|RSX_TEST_FP_END,RSX_TEST_FP_SRC_INPUT|RSX_TEST_FP_SWZ_IDENT);
 if(rsx_fp_decompile_buffered_ex(fp,16,0x40,0,h,sizeof h,NULL)<=0||!strstr(h,"(input.tc0).xyzw")||strstr(h,"saturate(input.tc0")){puts("FAIL tc0 unbounded");return 1;}
 puts("colour input clamp: ok");return 0;}
EOC
${CC:-cc} -std=c11 -Wall -O1 -I"$V" -I"$PT/include" "$O/col.c" "$V/rsx_fp_decompiler.c" -o "$O/col" -lm
"$O/col"

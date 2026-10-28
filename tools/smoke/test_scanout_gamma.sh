#!/usr/bin/env bash
# Host test for KZ3 patch 2070 (scan-out gamma). Source-level checks on the patched toolkit (the D3D12 pass needs a GPU,
# its pixels are checked under Proton by tools/release/padrun.sh ... options-display-brightness): a ctx handler reads f1
# (no TLS on the per-call path), the shader raises to the power gamma (> 1 darkens: the game's DECREASE raises the value),
# gamma 1.0 and LD_GAMMA=0 skip the pass. Also compiles a C model of the transfer curve and checks it.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_scanout_gamma.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
RL="$PT/libs/video/rsx_live_draw.c"
if [[ ! -f "$RL" ]]; then echo "[test_scanout_gamma] SKIP: no patched toolkit"; exit 0; fi
if ! grep -q ld_gamma_present "$RL"; then echo "[test_scanout_gamma] SKIP: toolkit without patch 2070"; exit 0; fi
f=0; ck() { if grep -Eq "$2" "$3"; then echo "ok: $1"; else echo "FAIL: $1"; f=1; fi; }
ck "shader pow(c, gamma_exp)" 'pow\(max\(c\.rgb, 0\.0\), gamma_exp\)' "$RL"
ck "exponent is gamma, not 1/gamma" 'const float inv = gm, pad' "$RL"
ck "gamma 1.0 skipped" 'gm == 1\.0f' "$RL"
ck "LD_GAMMA=0 switch" 'LD_GAMMA' "$RL"
AV="$PT/libs/misc/cellAvconfExt.c"
ck "SetGamma is a ctx handler reading f1" '\(float\)ctx->fpr\[1\]' "$AV"
ck "ctx handler registered (NID 0xC7020F62)" 'ps3_hle_register_ctx\(0xC7020F62u' "$AV"
if grep -rq '__thread' "$AV" "$PT/runtime/ppu/ppu_hle.cpp" 2>/dev/null && grep -q g_ps3_hle_fpr1 "$PT/runtime/ppu/ppu_hle.cpp"; then echo "FAIL: per-call TLS publish"; f=1; else echo "ok: no per-call TLS"; fi
# Pass 81: the exponent is read at every present (no static cache) from the toolkit's one source, and with PR 355
# (patch 9355) that source is cellAvconfExt_display_gamma(); SetGamma must accept the game's whole 0.8..1.2 range.
if grep -q 'cellAvconfExt_display_gamma' "$RL"; then
  ck "present reads cellAvconfExt_display_gamma() per call" 'const float gm = cellAvconfExt_display_gamma\(\);' "$RL"
  ck "SetGamma range check is 0.8..1.2 (KZ3 slider range)" 'gamma < 0\.8f \|\| gamma > 1\.2f' "$AV"
else
  ck "present reads g_ps3_display_gamma per call (pre-9355 toolkit)" 'const float gm = g_ps3_display_gamma;' "$RL"
  echo "note: toolkit without patch 9355 (stale work/ps3recomp-patched?)"
fi
O="$ROOT/build/smoke-host/scanout_gamma"; mkdir -p "$O"
cat > "$O/t.c" <<'EOC'
#include <math.h>
#include <stdio.h>
int main(void)
{
    const float lo = 0.84f, def = 1.0f, hi = 1.16f; float v = 0.25f; int fail = 0;
    float a = powf(v, lo), b = powf(v, def), c = powf(v, hi);
    if (b != v) { puts("FAIL: gamma 1.0 not identity"); fail = 1; }
    if (!(c < b && b < a)) { puts("FAIL: gamma > 1 must darken (DECREASE), < 1 brighten"); fail = 1; }
    /* KZ3's setter (0xAE1CE8): gamma = 1.2 - 0.4 * clamp(v, 0, 1), as float; PR 355 refuses outside 0.8..1.2 */
    for (int i = 0; i <= 100; i++) {
        float g = (float)(1.2 - 0.4 * (i / 100.0));
        if (g < 0.8f || g > 1.2f) { printf("FAIL: slider %d -> %.9g outside 0.8..1.2\n", i, g); fail = 1; }
    }
    if (!fail) puts("ok: curve model, slider range inside 0.8..1.2");
    return fail;
}
EOC
cc -O1 -o "$O/t" "$O/t.c" -lm && "$O/t" || f=1
exit $f

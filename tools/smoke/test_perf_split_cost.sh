#!/usr/bin/env bash
# Host test for KZ3 patch 2203 (perf split cost): sampled decode_d recovers a known spin exactly (virtual clock), the buckets still add up,
# the negative control (scale factor dropped) must fail, plus static checks that the patched sources keep the three fixes.
#   nix develop .#ci --command bash -c 'CC=clang tools/smoke/test_perf_split_cost.sh'   (cc is the default)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if ! grep -qs rsx_split_settle "$V/rsx_o0.h"; then echo "[test_perf_split_cost] SKIP: no patched toolkit with patch 2203 ($V; run tools/ps3recomp-patched.sh)"; exit 77; fi
CC="${CC:-cc}"
O="$ROOT/build/smoke-host/perf-split-cost-$(basename "$CC")"; mkdir -p "$O"
$CC -std=gnu11 -Wall -Wextra -Werror -O2 -I"$V" "$ROOT/tools/smoke/test_perf_split_cost.c" -o "$O/t"
"$O/t"
# negative controls: each must exit non-zero and for its own reason
negctl() {   # flag, expected message
    if "$O/t" "$1" >"$O/neg.txt" 2>&1; then echo "[FAIL] negative control $1 passed"; cat "$O/neg.txt"; exit 1; fi
    grep -q "$2" "$O/neg.txt" || { echo "[FAIL] negative control $1 failed for the wrong reason (want '$2')"; cat "$O/neg.txt"; exit 1; }
    echo "[test_perf_split_cost] negative control $1 fails as required"
}
negctl --negctl 'not exactly the known spin'                 # scale factor dropped
negctl --negctl-phase 'short passes: timed'     # sampler phase reset every pass (the old code)
negctl --negctl-clamp 'went negative'           # debit taken at once, unclamped (the old scale_d)
G="$V/cellGcmSys.c"
grep -qF 'rsx_split_scale_d(s, d0, scale, owed)' "$V/rsx_o0.h" || { echo "[FAIL] dpop no longer scales"; exit 1; }
grep -qF 'rsx_split_dpop(&s_split, token, d0, s_split_dscale, s_split_owed' "$G" || { echo "[FAIL] walker does not pop with the scale"; exit 1; }
if grep -qF 'RSX_O0_ADD(g_rsx_o0.m3d' < <(sed -n '/^static void gcm_3d_method/,/^}/p' "$G"); then echo "[FAIL] per-word lock xadd back in gcm_3d_method"; exit 1; fi
grep -qF 's_perf_walk_on && !g_rsx_split_act' "$G" || { echo "[FAIL] meth_t0 pair not skipped under the split"; exit 1; }
grep -qF 'rsx_split_fmt_cost' "$G" || { echo "[FAIL] [perf-split-cost] not printed"; exit 1; }
echo "[test_perf_split_cost] static checks ok"
# The patched tree can lag the patch file, and the gating in ld_dispatch_timed lives in rsx_live_draw.c: check the PATCH FILE's added
# lines too (a revert of the rsx_split_dsel call or of the ld_dispatch_timed gate must fail here).
PF="$(ls "$ROOT"/port/patches/2203-*.patch)"
# Capture once and grep the variable: `added | grep -q` can kill the writer with SIGPIPE when grep exits on the first match,
# and pipefail then fails the check at random.
ADDED="$(grep -E '^\+' "$PF" | grep -v '^+++')"
grep -qF 'const int t = g_rsx_split_act ? rsx_split_dpush_w(&d0) : 0;' <<<"$ADDED" || { echo "[FAIL] patch: ld_dispatch_timed not gated on g_rsx_split_act"; exit 1; }
grep -qF 'if (t && g_rsx_split_act) rsx_split_dpop_w(t, d0);' <<<"$ADDED" || { echo "[FAIL] patch: ld_dispatch_timed pop not gated"; exit 1; }
grep -qF 'rsx_split_dsel(&s_split_phase, &s_split_words, &s_split_timed)' <<<"$ADDED" || { echo "[FAIL] patch: rsx_split_dpush_w does not call rsx_split_dsel on the persistent phase"; exit 1; }
grep -qF 'if (!g_rsx_split_act || RSX_SPLIT_TID() != s_split_owner) return 0;' <<<"$ADDED" || { echo "[FAIL] patch: dpush_w lost its owner gate"; exit 1; }
if grep -qE 's_split_phase *= *0' <<<"$ADDED"; then echo "[FAIL] patch: sampler phase reset"; exit 1; fi
grep -qF 'rsx_split_settle(&s_split, s_split_owed)' <<<"$ADDED" || { echo "[FAIL] patch: debit not settled at the pass end"; exit 1; }
if grep -qF 'LDS_PUSH(t, RSX_SB_DECODE_D)' < <(sed -n '/^static void ld_dispatch_timed/,/^}/p' "$V/rsx_live_draw.c"); then echo "[FAIL] tree: ld_dispatch_timed pushes decode_d on every word"; exit 1; fi
echo "[test_perf_split_cost] patch-file checks ok"

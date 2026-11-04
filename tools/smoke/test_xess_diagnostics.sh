#!/usr/bin/env bash
# XeSS diagnostic controls, lag history and draw accounting. No game data or GPU.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
O="$ROOT/build/smoke-host/xess-diagnostics"
mkdir -p "$O"
CC="${CC:-cc}"
"$CC" -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$ROOT/port/kz3" "$ROOT/tools/smoke/test_xess_diagnostics.c" \
  -lm -o "$O/test_xess_diagnostics"
"$O/test_xess_diagnostics"

PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
R="$PT/libs/video/rsx_live_draw.c"
if [[ ! -f "$R" ]]; then
  echo "[test_xess_diagnostics] SKIP: renderer environment checks need the patched toolkit"
  exit 0
fi
{
  printf '#include "xess_diagnostics.h"\n#include <stdio.h>\ntypedef uint32_t u32;\n'
  sed -n '/^static const kz3_xess_diagnostic_config\* ld_xess_diagnostic_config(void)/,/^static void ld_xess_diagnostic_draw(/p' "$R" | sed '$d'
  cat <<'C'
int main(void)
{
    const kz3_xess_diagnostic_config* c = ld_xess_diagnostic_config();
    if (c != ld_xess_diagnostic_config()) return 1;
    printf("RESULT %u %.1f %.1f %.1f %.1f %u %u\n",
           c->two_phase, c->projection_scale_x, c->projection_scale_y,
           c->execute_scale_x, c->execute_scale_y, c->execute_lag, c->trace_interval);
    return 0;
}
C
} > "$O/test_xess_env.c"
"$CC" -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$ROOT/port/kz3" "$O/test_xess_env.c" -lm -o "$O/test_xess_env"
clean=(env -u KZ3_XESS_JITTER_PATTERN -u KZ3_XESS_PROJECTION_SCALE_X -u KZ3_XESS_PROJECTION_SCALE_Y
       -u KZ3_XESS_EXECUTE_SCALE_X -u KZ3_XESS_EXECUTE_SCALE_Y
       -u KZ3_XESS_EXECUTE_JITTER_LAG -u KZ3_XESS_JITTER_TRACE)
expect_result() {
  local log="$1" expected="$2" label="$3"
  grep -Fxq "$expected" "$log" || {
    echo "[test_xess_diagnostics] FAIL: $label"; cat "$log"; exit 1; }
}
"${clean[@]}" "$O/test_xess_env" > "$O/default.log" 2>&1
expect_result "$O/default.log" 'RESULT 0 1.0 1.0 1.0 1.0 0 0' 'incorrect default settings'
if grep -Fq '[xess-diag]' "$O/default.log"; then
  echo "[test_xess_diagnostics] FAIL: defaults unexpectedly logged diagnostic settings"; exit 1
fi
"${clean[@]}" KZ3_XESS_JITTER_TRACE=1 "$O/test_xess_env" > "$O/trace.log" 2>&1
expect_result "$O/trace.log" 'RESULT 0 1.0 1.0 1.0 1.0 0 1' 'trace-only settings changed jitter'
[[ "$(grep -Fc '[xess-diag] pattern=' "$O/trace.log")" == 1 ]] || {
  echo "[test_xess_diagnostics] FAIL: trace-only settings were not logged exactly once"; exit 1; }
"${clean[@]}" KZ3_XESS_JITTER_PATTERN=two-phase KZ3_XESS_PROJECTION_SCALE_X=0.5 \
  KZ3_XESS_PROJECTION_SCALE_Y=-1 KZ3_XESS_EXECUTE_SCALE_X=0 KZ3_XESS_EXECUTE_SCALE_Y=2 \
  KZ3_XESS_EXECUTE_JITTER_LAG=1 KZ3_XESS_JITTER_TRACE=3 \
  "$O/test_xess_env" > "$O/valid.log" 2>&1
expect_result "$O/valid.log" 'RESULT 1 0.5 -1.0 0.0 2.0 1 3' 'incorrect valid settings'
[[ "$(grep -Fc '[xess-diag] pattern=' "$O/valid.log")" == 1 ]] || {
  echo "[test_xess_diagnostics] FAIL: valid settings were not logged exactly once"; exit 1; }
"${clean[@]}" KZ3_XESS_JITTER_PATTERN=invalid KZ3_XESS_PROJECTION_SCALE_X=nan \
  KZ3_XESS_PROJECTION_SCALE_Y=2.1 KZ3_XESS_EXECUTE_SCALE_X=1x KZ3_XESS_EXECUTE_SCALE_Y= \
  KZ3_XESS_EXECUTE_JITTER_LAG=3 KZ3_XESS_JITTER_TRACE=60001 \
  "$O/test_xess_env" > "$O/invalid.log" 2>&1
expect_result "$O/invalid.log" 'RESULT 0 1.0 1.0 1.0 1.0 0 0' 'invalid settings did not use defaults'
for name in KZ3_XESS_JITTER_PATTERN KZ3_XESS_PROJECTION_SCALE_X KZ3_XESS_PROJECTION_SCALE_Y \
            KZ3_XESS_EXECUTE_SCALE_X KZ3_XESS_EXECUTE_SCALE_Y KZ3_XESS_EXECUTE_JITTER_LAG \
            KZ3_XESS_JITTER_TRACE; do
  [[ "$(grep -Fc "invalid $name=" "$O/invalid.log")" == 1 ]] || {
    echo "[test_xess_diagnostics] FAIL: invalid $name was not reported exactly once"; exit 1; }
done
echo "[test_xess_diagnostics] renderer environment and explicit error reporting: PASS"

{
  cat <<'C'
#include "xess_diagnostics.h"
#include <stdio.h>
typedef uint32_t u32;
enum { RSX_DM_XESS = 1 };
static int g_dm_filter = RSX_DM_XESS;
static struct { u32 width, height; } g = {1280, 720};
static u32 g_xess_zframe = 42, g_xess_zslot = 7;
static u32 g_xess_jitter_frame = 42, g_ld_frames = 42;
static int g_xess_diagnostic_held;
static kz3_xess_diagnostic_sample g_xess_diagnostic_sample;
static kz3_xess_diagnostic_coverage g_xess_diagnostic_coverage;
static u32 g_xess_diagnostic_coverage_frame = UINT32_MAX;
static kz3_xess_diagnostic_config test_config;
static const kz3_xess_diagnostic_config* ld_xess_diagnostic_config(void) { return &test_config; }
C
  sed -n '/^static void ld_xess_diagnostic_draw(const kz3_xess_draw_info\* draw, int shifted)$/,/^static void ld_xess_output_size(/p' "$R" | sed '$d'
  cat <<'C'
int main(void)
{
    test_config = kz3_xess_diagnostic_defaults();
    test_config.trace_interval = 1;
    kz3_xess_draw_info draw = {0};
    draw.frame = 42;
    draw.target_offset = KZ3_XESS_SCENE_TARGET_OFFSET;
    draw.target_width = 1280;
    draw.target_height = 720;
    draw.target_is_local = 1;
    draw.mrt_count = 1;
    draw.full_viewport = 1;
    draw.depth_test = 1;
    draw.depth_slot = 7;

    g_xess_diagnostic_sample.active = 1;
    ld_xess_diagnostic_draw(&draw, 0);
    g_xess_diagnostic_sample.active = 0;
    g_xess_diagnostic_held = 1;
    ld_xess_diagnostic_draw(&draw, 0);
    ld_xess_diagnostic_present(0);

    draw.frame = g_ld_frames = g_xess_zframe = 43;
    g_xess_diagnostic_sample.active = 1;
    ld_xess_diagnostic_draw(&draw, 0);
    ld_xess_diagnostic_present(1);
    test_config.trace_interval = 2;
    ld_xess_diagnostic_present(1);
    g_dm_filter = 0;
    ld_xess_diagnostic_draw(&draw, 0);
    ld_xess_diagnostic_present(1);
    return g_xess_diagnostic_coverage.draws == 1 &&
           g_xess_diagnostic_coverage.unavailable == 1 &&
           g_xess_diagnostic_coverage.zero_projection == 0 ? 0 : 1;
}
C
} > "$O/test_xess_trace.c"
"$CC" -std=c11 -Wall -Wextra -Werror -O2 \
  -I"$ROOT/port/kz3" "$O/test_xess_trace.c" -lm -o "$O/test_xess_trace"
"$O/test_xess_trace" > "$O/coverage.log" 2>&1 || {
  echo "[test_xess_diagnostics] FAIL: renderer draw accounting"; cat "$O/coverage.log"; exit 1; }
for expected in \
  '^\[xess-diag\] frame=42 dispatch=fallback sample=0 held=1 .*draws=2 deferred=0 scene=2 other=0 shifted=0 partial-viewport=0 depth-disabled=0 depth-mismatch=0 zero-projection=1 unavailable=1$' \
  '^\[xess-diag\] frame=43 dispatch=ok sample=0 held=0 .*draws=1 deferred=0 scene=1 other=0 shifted=0 partial-viewport=0 depth-disabled=0 depth-mismatch=0 zero-projection=0 unavailable=1$'; do
  grep -Eq "$expected" "$O/coverage.log" || {
    echo "[test_xess_diagnostics] FAIL: renderer coverage trace"; cat "$O/coverage.log"; exit 1; }
done
[[ "$(grep -Fc '[xess-diag] frame=' "$O/coverage.log")" == 2 ]] || {
  echo "[test_xess_diagnostics] FAIL: trace filter or interval"; cat "$O/coverage.log"; exit 1; }
echo "[test_xess_diagnostics] renderer draw accounting and trace output: PASS"

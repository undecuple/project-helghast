#!/usr/bin/env bash
# Pass 125: LightProbes kd/blend/dynamic-light kernels against the lifted functions (tools/spu/native_diff.py; SKIP without
# the game lift) and the per-window gate counter parser (self-contained).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
fail=0
tmp=$(mktemp)
cat > "$tmp" <<'EOF'
[native] slot 12 job_0116d380 0x047F0 calls=1024 fallback=0 win10s(window:calls[/fallback]): 9:100 10:300 11:624
[native] slot 16 job_0116d380 0x04DD0 calls=50 fallback=2 win10s(window:calls[/fallback]): 8:40/2 9:10
[native] slot 12 job_0116d380 0x047F0 calls=2048 fallback=0 win10s(window:calls[/fallback]): 9:100 10:300 11:624 12:1024
EOF
out=$(python3 "$ROOT/tools/spu/native_counts.py" "$tmp" 2>&1)
rm -f "$tmp"
echo "$out" | grep -q "^split at 90 s" && echo "$out" | grep -q "^12 job_0116d380 0x47F0 2048 0 2048 4 0" \
  && echo "$out" | grep -q "^16 job_0116d380 0x4DD0 50 40 10 1 2" && echo "counts parser OK" || { echo "FAIL counts parser: $out"; fail=1; }
out=$(python3 "$ROOT/tools/spu/native_diff.py" --slot 8,12,16 --trials 300 2>&1); rc=$?
echo "$out" | tail -4
case "$out" in *SKIP*) ;; *) [[ $rc -eq 0 ]] || fail=1;; esac
[[ $fail -eq 0 ]] && echo "PASS" || { echo "FAIL"; exit 1; }

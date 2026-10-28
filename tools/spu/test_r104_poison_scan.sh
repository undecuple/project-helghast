#!/usr/bin/env bash
# Pass 104: upstream PR #454's test (runtime/spu/tests/test_spu_poison_scan.c, carried by patch 1121) built from the
# patched toolkit (work/ps3recomp-patched; KZ3_PATCHED_DIR = another tree) with stubs for the KZ3 additions spu_dma.h
# reaches (1026 DMA watch windows, 1056 lockline stripes, guest write hook). Checks the [POISON-DMA] lines the way the
# PR's tools/test_spu_poison_scan.py does: poison outside 0x40003000-0x40005000 is reported only with
# SPU_POISON_DMA_WATCH set; PUTs into the region are always reported with the scan result. Self-contained (no game
# data). Exit 1 on any failure.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d $T/runtime/spu ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
[[ -f $T/runtime/spu/tests/test_spu_poison_scan.c ]] || { echo "FAIL: test_spu_poison_scan.c missing (patch 1121 not applied?)"; exit 1; }
[[ $(uname -s) == Linux* || $(uname -s) == Darwin* ]] || { echo "SKIP: needs a lazily committed 1 GiB mapping"; exit 0; }
OUT="$(mktemp -d)"; fail=0
cat > "$OUT/stub.c" <<'C'
#include <stdint.h>
#define SPU_DMA_WATCH_SLOTS_STUB 8    /* = SPU_DMA_WATCH_SLOTS (patch 1026); never read (watch off) */
volatile uint32_t g_spu_dma_watch_lo[SPU_DMA_WATCH_SLOTS_STUB], g_spu_dma_watch_hi[SPU_DMA_WATCH_SLOTS_STUB];
volatile long g_spu_dma_watch_hits[SPU_DMA_WATCH_SLOTS_STUB];
volatile int g_spu_dma_watch_on;
void (*g_spu_guest_write_hook)(uint32_t ea, uint32_t size);
uint64_t spu_ll_mask(uint32_t ea, uint32_t len) { (void)ea; (void)len; return 0; }
void spu_ll_lock_mask(uint64_t m) { (void)m; }
void spu_ll_unlock_mask(uint64_t m) { (void)m; }
C
if ! (cd "$T" && ${CC:-cc} -std=gnu17 -O2 -w -pthread -I include -I runtime/spu -I runtime/platform -o "$OUT/ps" \
      runtime/spu/tests/test_spu_poison_scan.c runtime/platform/win32_compat.c "$OUT/stub.c") >"$OUT/b.log" 2>&1; then
  echo "FAIL build: $(grep -m1 -E 'error|undefined' "$OUT/b.log" | cut -c1-200)"; rm -rf "$OUT"; exit 1
fi
run() {  # mode watch expected-lines...
  local mode=$1 watch=$2; shift 2
  local got
  got="$(env -u SPU_POISON_DMA_WATCH ${watch:+SPU_POISON_DMA_WATCH=1} "$OUT/ps" "$mode" 2>&1 >/dev/null | grep '^\[POISON-DMA\]')"
  local want; want="$(printf '%s\n' "$@")"; [[ $# -eq 0 ]] && want=""
  if [[ "$got" == "$want" ]]; then echo "PASS $mode watch=${watch:-0}"
  else echo "FAIL $mode watch=${watch:-0}: got [$(tr '\n' '|' <<<"$got" | cut -c1-200)]"; fail=1; fi
}
R1='[POISON-DMA] img7 PUT ea=0x40003000 size=0x40 lsa=0x1000 hitregion=1 haspoison=0'
R2='[POISON-DMA] img7 PUT ea=0x40003100 size=0x40 lsa=0x1000 hitregion=1 haspoison=1'
run outside ""
run outside 1 '[POISON-DMA] img7 PUT ea=0x00020000 size=0x40 lsa=0x1000 hitregion=0 haspoison=1'
run region "" "$R1" "$R2"
run region 1 "$R1" "$R2"
rm -rf "$OUT"
exit $fail

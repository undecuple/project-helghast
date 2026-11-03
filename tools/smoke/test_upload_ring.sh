#!/usr/bin/env bash
# Host test for KZ3 patch 2142 (the Killzone 2 port, patch 6048): the upload ring placement. A texture level that does
# not fit in what is left of the 64 MB ring must ask for a submit, never be skipped. Also checks the patched
# rsx_live_draw.c no longer carries the silent "> UPLOAD_SIZE) break" / "return NULL" texture checks.
# No game data, no GPU. SKIP without the patched toolkit.
#   nix develop .#re --command tools/sandbox.sh -- tools/smoke/test_upload_ring.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PT="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
V="$PT/libs/video"
if [[ ! -f "$V/rsx_live_draw.c" ]]; then echo "[test_upload_ring] SKIP: no patched toolkit ($V; run tools/ps3recomp-patched.sh)"; exit 0; fi
[[ -f "$V/rsx_upload_ring.h" ]] || { echo "[test_upload_ring] FAIL: toolkit without patch 2142 (no rsx_upload_ring.h)"; exit 1; }
O="$ROOT/build/smoke-host/upload_ring"; mkdir -p "$O"
cat > "$O/t.c" <<'EOC'
#include <stdio.h>
#include "rsx_upload_ring.h"
static int fails, n;
static void check(const char* what, uint64_t used, uint64_t bytes, uint64_t size, int want, uint64_t want_start)
{
    uint64_t start = 99; n++;
    const int k = rsx_upload_place(used, bytes, size, &start);
    if (k != want || start != want_start) {
        printf("FAIL %s: used=%llu bytes=%llu -> %d start=%llu (want %d start=%llu)\n", what, (unsigned long long)used,
               (unsigned long long)bytes, k, (unsigned long long)start, want, (unsigned long long)want_start);
        fails++;
    }
}
int main(void)
{
    const uint64_t RING = 64ull << 20, BIG = 8192ull * 128;   /* DXT5 2048x512: pitch 8192, 128 block rows */
    check("empty ring", 0, BIG, RING, RSX_UP_FIT, 0);
    check("aligned up to 512", 1, 4096, RING, RSX_UP_FIT, 512);
    check("already aligned", 1024, 4096, RING, RSX_UP_FIT, 1024);
    check("exactly fills", RING - BIG, BIG, RING, RSX_UP_FIT, RING - BIG);
    check("level after a full ring (the bug)", RING - 4096, BIG, RING, RSX_UP_FLUSH, 0);
    check("one byte short", RING - BIG + 1, BIG, RING, RSX_UP_FLUSH, 0);
    check("alignment still fits", RING - BIG - 100, BIG, RING, RSX_UP_FIT, RING - BIG);
    check("whole ring, empty", 0, RING, RING, RSX_UP_FIT, 0);
    check("whole ring, not empty", 512, RING, RING, RSX_UP_FLUSH, 0);
    check("larger than the ring", 0, RING + 1, RING, RSX_UP_TOO_BIG, 0);
    check("used past the end", RING + 4096, 16, RING, RSX_UP_FLUSH, 0);
    check("zero bytes", 700, 0, RING, RSX_UP_FIT, 1024);
    uint64_t used = 0; int submits = 0;
    for (int i = 0; i < 300; i++) {   /* a frame of uploads: every level lands inside the ring, a submit restarts at 0 */
        const uint64_t bytes = (i % 7 == 0) ? BIG : (uint64_t)(i * 4099 % 300000 + 1);
        uint64_t s; const int k = rsx_upload_place(used, bytes, RING, &s);
        if (k == RSX_UP_FLUSH) { submits++; used = 0; continue; }
        if (k != RSX_UP_FIT || s % RSX_UP_ALIGN || s < used || s + bytes > RING) { printf("FAIL frame walk at %d\n", i); fails++; break; }
        used = s + bytes;
    }
    if (!fails) printf("upload ring: %d checks, frame walk ok (%d submits)\n", n, submits);
    return fails ? 1 : 0;
}
EOC
${CC:-cc} -O2 -Wall -Wextra -Werror -I"$V" -o "$O/t" "$O/t.c" || { echo "[test_upload_ring] FAIL build"; exit 1; }
"$O/t" || exit 1
if grep -En '> UPLOAD_SIZE\) break;|h \* d > UPLOAD_SIZE\) return NULL' "$V/rsx_live_draw.c" | grep -q .; then
  echo "[test_upload_ring] FAIL: a texture upload still skips silently:"; grep -En '> UPLOAD_SIZE\) break;|h \* d > UPLOAD_SIZE\) return NULL' "$V/rsx_live_draw.c" | head -5; exit 1; fi
grep -q 'ld_upload_take' "$V/rsx_live_draw.c" || { echo "[test_upload_ring] FAIL: no ld_upload_take"; exit 1; }
echo "test_upload_ring.sh: ok"

#!/usr/bin/env bash
# Pass 159: port/spu/kz3_native_snd.c (K7a mono->mono gain ramp DD80/DE48, gate slots 26/27) against an independent
# model (tools/spu/test_r159_snd_model.c, synthetic tables, no game data: 12000 states incl. refusals, early returns, in
# place / silent / raw-bit data, default and FTZ float modes, -O1 and -O2 -mfma builds), its negative control, and with game
# data native_diff.py against the lifted ovl259 code (3000 synthetic states x 3 flag sets, negative control must FAIL,
# the 1.00 twin when its lift exists). SKIP without the patched toolkit.
set -uo pipefail
. "$(dirname "$0")/test_r159_lib.sh"
r159_run k7a 26 27

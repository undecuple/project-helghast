#!/usr/bin/env bash
# Native build + run of the guest fiber ping-pong test (tools/spu/test_guest_fiber.c).
#   nix develop --command tools/sandbox.sh -- tools/spu/test_guest_fiber.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"; cd "$ROOT"
mkdir -p work/spu/test
${CC:-cc} -O2 -Wall -Wextra -o work/spu/test/test_guest_fiber tools/spu/test_guest_fiber.c port/spu/guest_fiber.c
work/spu/test/test_guest_fiber

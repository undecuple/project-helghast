#!/usr/bin/env bash
# Host test of port/kz3/kz3_vfs_root.h (game root from the EBOOT path). No game data.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=work/test-vfs-root; mkdir -p "$out"
${CXX:-g++} -std=c++17 -Wall -Wextra -Werror -Iport/kz3 -o "$out/t" tools/smoke/test_vfs_root.cpp
"$out/t"

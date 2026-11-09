#!/usr/bin/env python3
"""Behavioral cache-path regression with generated PSARC fixtures and the shipped VFS; no GPU/game data."""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
KEY = ('LocalCachePS3/lumps/levels.single_player.pyrrhus_outskirts.section_city_blocks.'
       'assets_optimized_gripsnondouble_cityblocksgpst_cityblocksartcityblockshigh_narvillekz2_allsniperr.core')


def psarc(path, name):
    body = b'synthetic ISA_M4_Revolver'
    data = [name.encode(), body]
    toc_len = 32 + 2 * 30 + 4
    entries, offset = [], toc_len
    for i, block in enumerate(data):
        digest = bytes(16) if not i else hashlib.md5(name.encode()).digest()
        entries.append(digest + struct.pack('>I', i) + len(block).to_bytes(5, 'big') + offset.to_bytes(5, 'big'))
        offset += len(block)
    header = b'PSAR' + struct.pack('>HH4s5I', 1, 4, b'zlib', toc_len, 30, 2, 65536, 0)
    path.write_bytes(header + b''.join(entries) + b''.join(struct.pack('>H', len(b)) for b in data) + b''.join(data))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, default=ROOT / 'port/kz3/psarc_vfs.cpp')
    ap.add_argument('--overlay-source', type=Path, default=ROOT / 'port/vfs/kz3_overlay.c')
    ap.add_argument('--expect-failure', action='store_true', help='behavioral control on the pre-fix source')
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='psarc-paths-') as tmp:
        tmp = Path(tmp)
        obj = tmp / 'overlay.o'; exe = tmp / 'test'
        cc, cxx = shlex.split(os.environ.get('CC', 'cc')), shlex.split(os.environ.get('CXX', 'c++'))
        subprocess.run([*cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-c',
                        str(args.overlay_source.resolve()), '-I' + str(ROOT / 'port/vfs'), '-o', str(obj)], check=True)
        subprocess.run([*cxx, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DPSARC_SOURCE="' + str(args.source.resolve()) + '"', '-I' + str(ROOT / 'port/vfs'),
                        str(ROOT / 'port/vfs/test_cache_paths.cpp'), str(obj), '-pthread',
                        '-Wl,--wrap=fopen', '-Wl,--wrap=stat', '-Wl,--wrap=rename', '-o', str(exe),
                        *([] if args.expect_failure else ['-DCHECK_CACHE_HELPER'])], check=True)
        # A normal filename works; the reported asset name crosses 260 only with the deeper cache root.
        cases = [('short', 'assets/test.core', 30), ('reported-deep', KEY, 90),
                 ('corestream', KEY[:-4] + 'corestream', 90),
                 ('extension', KEY[:-4] + 'edat', 90)]
        bad = 0
        for label, key, depth in cases:
            archive = tmp / (label + '.psarc'); psarc(archive, key)
            for layer in ('disc', 'added'):
                cache = tmp / (layer + 'x' * depth); cache.mkdir(exist_ok=True)
                result = subprocess.run([str(exe), str(archive), str(cache), key, layer], capture_output=True, text=True)
                expect_failure = args.expect_failure and label != 'short'
                passed = result.returncode == 4 if expect_failure else result.returncode == 0
                print(('PASS' if passed else 'FAIL'), layer, label, 'rc=' + str(result.returncode))
                print(result.stdout.strip() or result.stderr.strip())
                bad += not passed
        return int(bool(bad))


if __name__ == '__main__':
    raise SystemExit(main())

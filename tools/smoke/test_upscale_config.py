#!/usr/bin/env python3
"""How port/kz3/main.cpp configures the upscaler (review of PR #27, points 1 and 4); no game, GPU or Windows binary.

Extracts kz3_upscale_parse, kz3_upscale_apply and kz3_upscale_temporal from main.cpp, compiles them with the patched
toolkit's rsx_upscale.c and the harness in test_upscale_config.cpp, and runs the cases. Each mutant puts back one
defect the review found and must fail the named check: the "fsr 1" / "fsr_1" spellings refused, FSR 1 given FSR 3.1's
sharpness instead of RSX_UPSCALE_SHARPNESS, and the camera bookkeeping running with the upscaler off.
SKIP when the patched toolkit is missing.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


def repo_root():
    for path in (HERE, *HERE.parents):
        if (path / 'port/patches').is_dir():
            return path
    return Path.cwd().resolve()


def function(text, signature):
    if text.count(signature + '\n{') != 1:
        raise SystemExit(f'FAIL: unique function required in main.cpp: {signature!r}')
    start = text.index(signature + '\n{')
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise SystemExit(f'FAIL: unique mutation marker required: {old!r}')
    return text.replace(old, new, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = repo_root()
    parser.add_argument('--candidate-sdk', type=Path,
                        default=Path(os.environ.get('KZ3_PATCHED_DIR', root / 'work/ps3recomp-patched')))
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--out', type=Path, help='output directory; default is a temporary directory')
    args = parser.parse_args()
    video = args.candidate_sdk / 'libs/video'
    upscale_h = video / 'rsx_upscale.h'
    if not upscale_h.is_file() or 'rsx_upscale_register_backend' not in upscale_h.read_text(encoding='utf-8'):
        print(f'[test_upscale_config] SKIP: no toolkit with patch 2220 at {args.candidate_sdk} '
              '(run tools/ps3recomp-patched.sh)')
        return 0
    src = (root / 'port/kz3/main.cpp').read_text(encoding='utf-8').replace('\r\n', '\n')
    parse = function(src, 'static int kz3_upscale_parse(const char* text)')
    apply = function(src, 'static void kz3_upscale_apply(void)')
    temporal = function(src, 'static inline bool kz3_upscale_temporal(void)')
    harness = (HERE / 'test_upscale_config.cpp').read_text(encoding='utf-8').replace('\r\n', '\n')
    out = args.out or Path(tempfile.mkdtemp(prefix='kz3-upscale-config-'))
    out.mkdir(parents=True, exist_ok=True)
    obj = out / 'rsx_upscale.o'
    built = subprocess.run(shlex.split(args.cc) + ['-std=c99', '-O1', '-c', '-I', str(video), '-I',
                                                   str(args.candidate_sdk / 'include'), str(video / 'rsx_upscale.c'),
                                                   '-o', str(obj)], capture_output=True, text=True, timeout=60)
    if built.returncode:
        print(f'[test_upscale_config] FAIL: rsx_upscale.c did not compile\n{built.stderr}')
        return 1
    toolkit_only = parse[:parse.index('    /* the toolkit')] + '    return -1;\n}' if '    /* the toolkit' in parse else None
    if toolkit_only is None:
        print('[test_upscale_config] FAIL: kz3_upscale_parse has no fallback to the toolkit names')
        return 1
    cases = [
        ('candidate', parse, apply, temporal, set()),
        ('port-names-only', toolkit_only, apply, temporal, {'fsr-1-spelling', 'fsr_1-spelling'}),
        ('one-sharpness', parse, replace_once(apply, '''        backend == RSX_UPSCALE_FSR1
            ? s_fsr1_sharpness
            : (float)''', '        (float)'), temporal, {'fsr1-sharpness'}),
        ('camera-always', parse, apply, temporal.replace('    const LONG b', '    return true;\n    const LONG b'),
         {'camera-idle-without-temporal'}),
    ]
    failed = 0
    for label, p, a, t, expected in cases:
        source = out / f'{label}.cpp'
        binary = out / (label + ('.exe' if os.name == 'nt' else ''))
        source.write_text(replace_once(harness, '/* @@EXTRACTED@@ */', '\n'.join((p, a, t)) + '\n'), encoding='utf-8')
        command = shlex.split(args.cxx) + ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Wno-unused-function',
                                           '-Wno-unused-parameter', '-Wno-missing-field-initializers',
                                           '-Wno-unreachable-code', '-I', str(video), '-I',
                                           str(args.candidate_sdk / 'include'), str(source), str(obj),
                                           '-o', str(binary), '-lm']
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if compiled.returncode:
            print(f'[test_upscale_config] FAIL {label}: compile\n{compiled.stderr}')
            failed += 1
            continue
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        failures = {line[5:] for line in run.stdout.splitlines() if line.startswith('FAIL ')}
        if not expected:
            ok = run.returncode == 0 and not failures
            print(f'[test_upscale_config] {label}: {"PASS" if ok else "FAIL"} {run.stdout.strip().splitlines()[-1:]}')
            if not ok:
                print(run.stdout + run.stderr)
        else:
            ok = run.returncode == 1 and expected <= failures
            print(f'[test_upscale_config] {label}: negative control {"rejected" if ok else "SURVIVED"} '
                  f'(failed {sorted(failures)})')
        failed += not ok
    print('[test_upscale_config]', 'ALL PASS' if not failed else f'{failed} FAILED')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

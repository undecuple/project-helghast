#!/usr/bin/env python3
"""When the renderer may release an upscaler context (review of PR #27, point 3); no game, GPU or Windows binary.

Extracts the upscaler state block and ld_flush from the patched rsx_live_draw.c, compiles them with the mock GPU in
test_upscale_switch.c and runs the cases. The candidate must pass. Each mutant puts back one defect the review found
(destroying from the per-draw path, releasing without waiting for submitted work, no switch point at the present
flush, writing the request without the lock) and must fail the named check, so the test is shown to catch it.
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


def between(text, begin, end):
    if text.count(begin) != 1:
        raise SystemExit(f'FAIL: unique source marker required: {begin!r}')
    start = text.index(begin)
    return text[start:text.index(end, start)]


def function(text, signature):
    if text.count(signature + '\n{') != 1:
        raise SystemExit(f'FAIL: unique function required: {signature!r}')
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


def extracted(live):
    block = between(live, 'static rsx_upscale* g_up;\n', 'static int g_dm_failed;')
    flush = function(live, 'static void ld_flush(ld_flush_reason reason)')
    return block, flush


def mutants(block, flush):
    yield 'candidate', block, flush, set()
    # the code under review: the per-draw path waited for submitted work only and tore the context down at once,
    # with this frame's recorded capture still in the open command list
    yield 'destroy-in-get', replace_once(block, '            g_up_switch_pending = 1;\n',
                                         '            ld_wait_fence(g.fence_value);\n            ld_up_destroy();\n'
                                         '            g_up_config = req;\n            ld_up_create();\n'), \
        flush, {'no-destroy-mid-frame'}
    yield 'no-fence-wait', replace_once(block, '    if (g_up && !ld_wait_fence(g.fence_value)) return;', ''), \
        flush, {'destroy-after-gpu-drained'}
    no_switch = flush.replace(
        'if (reason == LD_FLUSH_PRESENT || reason == LD_FLUSH_MOVIE_PRESENT) ld_up_apply_pending();', '')
    if no_switch == flush:
        raise SystemExit('FAIL: ld_flush does not call ld_up_apply_pending')
    yield 'no-flush-switch', block, no_switch, {'switch-at-present-flush'}
    yield 'sharpness-waits', replace_once(block, '            if (g_up) rsx_upscale_set_sharpness(g_up, req.sharpness);\n', ''), \
        flush, {'sharpness-applies-at-once'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate-sdk', type=Path,
                        default=Path(os.environ.get('KZ3_PATCHED_DIR', repo_root() / 'work/ps3recomp-patched')))
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--out', type=Path, help='output directory; default is a temporary directory')
    args = parser.parse_args()
    live_path = args.candidate_sdk / 'libs/video/rsx_live_draw.c'
    if not live_path.is_file():
        print(f'[test_upscale_switch] SKIP: no patched toolkit at {args.candidate_sdk} (run tools/ps3recomp-patched.sh)')
        return 0
    live = live_path.read_text(encoding='utf-8').replace('\r\n', '\n')
    if 'static void ld_up_apply_pending(void)\n{' not in live:
        print('[test_upscale_switch] FAIL: rsx_live_draw.c has no ld_up_apply_pending (patch 2291 is not applied)')
        return 1
    block, flush = extracted(live)
    harness = (HERE / 'test_upscale_switch.c').read_text(encoding='utf-8').replace('\r\n', '\n')
    out = args.out or Path(tempfile.mkdtemp(prefix='kz3-upscale-switch-'))
    out.mkdir(parents=True, exist_ok=True)
    failed = 0
    for label, b, f, expected in mutants(block, flush):
        source = out / f'{label}.c'
        binary = out / (label + ('.exe' if os.name == 'nt' else ''))
        source.write_text(replace_once(harness, '/* @@EXTRACTED@@ */', b + '\n' + f + '\n'), encoding='utf-8')
        command = shlex.split(args.cc) + ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-function',
                                          '-Wno-unused-parameter', '-I', str(args.candidate_sdk / 'libs/video'),
                                          str(source), '-o', str(binary), '-pthread']
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if compiled.returncode:
            print(f'[test_upscale_switch] FAIL {label}: compile\n{compiled.stderr}')
            failed += 1
            continue
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        failures = {line[5:] for line in run.stdout.splitlines() if line.startswith('FAIL ')}
        if not expected:
            ok = run.returncode == 0 and not failures
            print(f'[test_upscale_switch] {label}: {"PASS" if ok else "FAIL"} {run.stdout.strip().splitlines()[-1:]}')
            if not ok:
                print(run.stdout + run.stderr)
        else:
            ok = run.returncode == 1 and expected <= failures
            print(f'[test_upscale_switch] {label}: negative control {"rejected" if ok else "SURVIVED"} '
                  f'(failed {sorted(failures)})')
        failed += not ok
    print('[test_upscale_switch]', 'ALL PASS' if not failed else f'{failed} FAILED')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

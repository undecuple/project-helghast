#!/usr/bin/env python3
"""Test the shipped ring submission hook against actual-reader lifetime cases and mutants."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def hook(source):
    match = re.search(r'static void ld_rm_note_submit\(u64 fence\)\n\{.*?\n\}', source, re.S)
    if not match:
        raise ValueError('missing ring submission hook')
    return match.group()


def main():
    sdk = Path(os.environ.get('KZ3_PATCHED_DIR', ROOT / 'work/ps3recomp-patched'))
    video = sdk / 'libs/video'
    source = (video / 'rsx_live_draw.c').read_text()
    header = (video / 'rsx_ring_marks.h').read_text()
    output = ROOT / 'build/smoke-host'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='ring-lifetime-', dir=output) as tmp:
        tmp = Path(tmp)
        baseline = tmp / 'sdk'
        (baseline / 'libs/video').mkdir(parents=True)
        (baseline / 'libs/video/rsx_live_draw.c').write_text(source)
        (baseline / 'libs/video/rsx_ring_marks.h').write_text(header)
        patch = ROOT / 'port/patches/2155-live-draw-ring-live-prefix-fences.patch'
        directory = f'--directory={baseline.relative_to(ROOT)}'
        subprocess.run(['git', 'apply', '--reverse', directory, str(patch)], cwd=ROOT, check=True)
        subprocess.run(['git', 'apply', '--check', directory, str(patch)], cwd=ROOT, check=True)
        subprocess.run(['patch', '--dry-run', '-p1', '-d', str(baseline), '-i', str(patch)], check=True)
        (tmp / 'rsx_ring_marks.h').write_text(header)
        compiler = shlex.split(os.environ.get('CC', 'cc'))

        def run(body):
            (tmp / 'submission.h').write_text(body)
            subprocess.run([*compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                            '-I', str(tmp), str(ROOT / 'tools/smoke/test_ld_ring_lifetime.c'),
                            '-o', str(tmp / 'test')], check=True)
            return subprocess.run([str(tmp / 'test')], capture_output=True, text=True, timeout=30)

        body = hook(source)
        result = run(body)
        print(result.stdout, end='')
        if result.returncode:
            raise SystemExit(result.stderr or f'FAIL: exit {result.returncode}')
        if 'rsx_rm_submit_live(' not in body:
            raise SystemExit('FAIL: missing lifetime-refresh control')
        for ring in ('LD_RM_SRV', 'LD_RM_SMP', 'LD_RM_PSCB'):
            mutant = body.replace('rsx_rm_submit_live(', f'(r == {ring} ? rsx_rm_submit : rsx_rm_submit_live)(')
            result = run(mutant)
            if result.returncode != 1:
                raise SystemExit(f'FAIL: {ring} allocation-only mutant was not detected')
            print(f'PASS: {ring} allocation-only mutant rejected')
        result = run(body.replace('rsx_rm_submit_live(', 'rsx_rm_submit('))
        if result.returncode != 1:
            raise SystemExit('FAIL: old allocation-only control was not detected')
        print('PASS: old allocation-only control rejected')


if __name__ == '__main__':
    main()

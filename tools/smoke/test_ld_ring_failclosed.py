#!/usr/bin/env python3
"""Patch 2157 host test: shipped ring fail-closed code against injected failures, plus the pre-2157 text as a
negative control (it must fail). SKIP without the patched toolkit or without patch 2157."""
import os, re, shlex, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAMES = ('ld_wait_fence', 'ld_rm_ensure', 'ld_srv_take', 'ld_smp_take', 'ld_rm_try_wrap')


def extract(source):
    out = []
    for n in NAMES:
        m = re.search(r'^static (?:int|void|u32) ' + n + r'\([^;{]*?\)\n\{.*?\n\}\n', source, re.S | re.M)
        if not m:
            raise SystemExit(f'FAIL: cannot extract {n}')
        out.append(m.group())
    return '\n'.join(out)


def main():
    sdk = Path(os.environ.get('KZ3_PATCHED_DIR', ROOT / 'work/ps3recomp-patched'))
    video = sdk / 'libs/video'
    if not (video / 'rsx_live_draw.c').exists():
        print('[test_ld_ring_failclosed] SKIP: no patched toolkit'); return
    source = (video / 'rsx_live_draw.c').read_text()
    if '#define LD_TAKE_FAIL 0xFFFFFFFFu' not in source:
        print('[test_ld_ring_failclosed] SKIP: toolkit without patch 2157'); return
    header = (video / 'rsx_ring_marks.h').read_text()
    out = ROOT / 'build/smoke-host'; out.mkdir(parents=True, exist_ok=True)
    cc = shlex.split(os.environ.get('CC', 'cc'))
    with tempfile.TemporaryDirectory(prefix='ring-failclosed-', dir=out) as tmp:
        tmp = Path(tmp)
        base = tmp / 'sdk'; (base / 'libs/video').mkdir(parents=True)
        (base / 'libs/video/rsx_live_draw.c').write_text(source)
        (base / 'libs/video/rsx_ring_marks.h').write_text(header)
        patch = ROOT / 'port/patches/2157-live-draw-ring-fail-closed.patch'
        subprocess.run(['patch', '-R', '-s', '-p1', '-d', str(base), '-i', str(patch)], check=True)
        (tmp / 'rsx_ring_marks.h').write_text(header)

        def run(body, name):
            (tmp / 'shipped.h').write_text(body)
            exe = tmp / name
            subprocess.run([*cc, '-std=gnu11', '-Wall', '-Wno-unused', '-O1', '-I', str(tmp),
                            str(ROOT / 'tools/smoke/test_ld_ring_failclosed.c'), '-o', str(exe)], check=True)
            return subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)

        r = run(extract(source), 'new')
        print(r.stdout, end='')
        if r.returncode:
            raise SystemExit('FAIL: shipped code:\n' + r.stderr)
        # negative control: the pre-2157 functions (no failure check, void ensure, marks retired first)
        old = extract((base / 'libs/video/rsx_live_draw.c').read_text())
        r = run(old, 'old')
        if r.returncode != 1:
            raise SystemExit('FAIL: pre-2157 code was not rejected by the test')
        print(f'PASS: pre-2157 negative control rejected ({r.stderr.count("FAIL line")} checks failed)')
        print('[test_ld_ring_failclosed] PASS')


if __name__ == '__main__':
    main()

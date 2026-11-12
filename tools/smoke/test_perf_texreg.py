#!/usr/bin/env python3
"""Patch 2158 host test: the shipped ld_trg_draw / ld_trg_report (RSX_PERF_TEXREG counters) against a scripted register
sequence with known expected counts, plus a mutated copy as a negative control (it must fail). SKIP without the
patched toolkit or without patch 2158."""
import os, shlex, subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint64_t u64; typedef uint32_t u32;
static u32 regs[16][9]; static u32 g_ld_fp_units_cur = 0xFFFFu; static int walk_on = 1;
static int rsx_perf_walk_on(void) { return walk_on; }
static void ld_tm_raw(u32 u, u32 raw[9]) { memcpy(raw, regs[u], sizeof regs[u]); }
#include "shipped.h"
static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)
int main(void)
{
    setenv("RSX_PERF_TEXREG", "1", 1);
    regs[0][3] = 0x80000000u; regs[1][3] = 0x80000000u;
    ld_trg_draw();                              /* first draw: snapshot only */
    CHECK(g_ld_trg.draws == 0);
    ld_trg_draw();                              /* nothing changed */
    CHECK(g_ld_trg.draws == 1 && g_ld_trg.all_same == 1 && g_ld_trg.fp_same == 1);
    CHECK(g_ld_trg.units_en == 2 && g_ld_trg.en_same == 2);
    regs[0][0] = 0x1000;                        /* unit 0 offset only */
    ld_trg_draw();
    CHECK(g_ld_trg.all_same == 1);
    CHECK(g_ld_trg.en_off == 1 && g_ld_trg.chg[0] == 1);
    CHECK(g_ld_trg.fp_same == 1);               /* offset change of a used unit is a change: fp_same must not grow */
    regs[1][5] = 7;                             /* unit 1 filter word */
    g_ld_fp_units_cur = 0x1;                    /* only unit 0 is used by the program: the unit-1 change is not counted */
    ld_trg_draw();
    CHECK(g_ld_trg.en_chg == 1 && g_ld_trg.fp_same == 2 && g_ld_trg.all_same == 1);
    regs[0][3] = 0; regs[1][3] = 0;             /* both disabled */
    ld_trg_draw();
    ld_trg_draw();
    CHECK(g_ld_trg.draws_en0 == 2 && g_ld_trg.draws == 5);
    walk_on = 0; ld_trg_draw(); CHECK(g_ld_trg.draws == 5);   /* outside the window nothing counts */
    walk_on = 1;
    ld_trg_report();
    CHECK(g_ld_trg.draws == 0);                 /* the report resets */
    return bad ? 1 : 0;
}
'''


def extract(src):
    a = src.index('static struct { u64 draws, all_same')
    b = src.index('static void ld_trg_report(void)\n{')
    b = src.index('\n}\n', b) + 3
    return src[a:b]


def main():
    sdk = Path(os.environ.get('KZ3_PATCHED_DIR', ROOT / 'work/ps3recomp-patched'))
    f = sdk / 'libs/video/rsx_live_draw.c'
    if not f.exists():
        print('[test_perf_texreg] SKIP: no patched toolkit'); return
    src = f.read_text()
    if 'ld_trg_draw' not in src:
        print('[test_perf_texreg] SKIP: toolkit without patch 2158'); return
    out = ROOT / 'build/smoke-host'; out.mkdir(parents=True, exist_ok=True)
    cc = shlex.split(os.environ.get('CC', 'cc'))
    with tempfile.TemporaryDirectory(prefix='texreg-', dir=out) as tmp:
        tmp = Path(tmp)
        (tmp / 'h.c').write_text(HARNESS)

        def run(body, name):
            (tmp / 'shipped.h').write_text(body)
            exe = tmp / name
            c = subprocess.run([*cc, '-std=gnu11', '-Wall', '-Wno-unused', '-O1', '-I', str(tmp), str(tmp / 'h.c'), '-o', str(exe)], capture_output=True, text=True)
            if c.returncode: raise SystemExit('compile failed:\n' + c.stderr[:1500])
            return subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)

        body = extract(src)
        r = run(body, 'new'); print(r.stdout, end='')
        if r.returncode:
            raise SystemExit('FAIL: shipped code:\n' + r.stdout)
        print('PASS: counts as scripted')
        mut = body.replace('if (k) diff_other = 1;', 'diff_other = 0;')   # offset-only no longer told apart
        assert mut != body
        r = run(mut, 'mut')
        if r.returncode != 1:
            raise SystemExit('FAIL: mutated counter was not rejected')
        print('PASS: negative control rejected')
        print('[test_perf_texreg] PASS')


if __name__ == '__main__':
    main()

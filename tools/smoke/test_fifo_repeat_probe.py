#!/usr/bin/env python3
"""Host test for tools/smoke/fifo_repeat_probe.py: builds a tiny synthetic capture (records without payload checks; the probe
reads CMD and FLIP only) and checks the numbers. Synthetic data only; a FIFO capture from a play-through is not in the repo, and
the test SKIPs nothing because it needs no game data."""
import os, struct, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'smoke'))
import fifo_repeat_probe as P

seq = [0]
def rec(t, payload=b''):
    r = struct.pack('<IIQQ', t, len(payload), seq[0], 0) + payload; seq[0] += 1; return r
def cmd(getoff, hw, words): return rec(4, struct.pack('<3I', getoff, hw, len(words)) + b''.join(struct.pack('<I', w) for w in words))
def hdr(m, n): return (n << 18) | m
JUMP = lambda to: 0x20000000 | to

def build(path, frames):
    with open(path, 'wb') as f:
        f.write(b'KZ3FIFOC' + struct.pack('<6I', 1, 32, 0, len(frames), 0, 0))
        f.write(rec(1, b'\0' * 40))
        for fr in frames:
            for c in fr: f.write(c)
            f.write(rec(9, struct.pack('<3I', 0, 0, 0)))

def frame(base, tweak=0, extra=False):
    ring = 0x300000 + base                    # ring in its own page
    pool = 0x500400 + base                    # pool chunk head (0x400 mod 0x10000), entered by JUMP
    out = [cmd(ring, hdr(0x0208, 1), [7]), cmd(ring + 8, JUMP(pool), [])]
    off = pool
    for i in range(4):                        # four vertex-constant commands of 4 words
        w = [i, i + 1, i + 2, 100 + tweak if i == 3 else i + 3]
        out.append(cmd(off, hdr(0x1EFC, 4), w)); off += 4 + 16
    if extra: out.append(cmd(off, hdr(0x1808, 1), [5])); off += 8
    out.append(cmd(off + 0x100, hdr(0x0B80, 2), [1, 2]))   # discontinuity, no link before it: a park patch
    return out

def main():
    d = tempfile.mkdtemp(dir=os.path.join(ROOT, 'build') if os.path.isdir(os.path.join(ROOT, 'build')) else None)
    p = os.path.join(d, 'cap.kz3fc')
    build(p, [frame(0), frame(0x1000, tweak=1)])
    rep = P.run(p)
    a, b = rep
    assert a['words'] == 2 + 1 + 4 * 5 + 3 and a['segments'] == 3, a
    assert a['words_by_kind']['ring'] == 3 and a['words_by_kind']['pool_jump_head'] == 20 and a['words_by_kind']['pool_park'] == 3, a['words_by_kind']
    assert a['pool_entry_links'] == {'jump': 1, 'park': 1}, a['pool_entry_links']
    assert a['words_by_method_class']['vertex_const'] == 20, a['words_by_method_class']
    assert a['repeat']['N-1'] is None and a['repeat']['N-2'] is None
    r = b['repeat']['N-1']; assert b['repeat']['N-2'] is None
    # the JUMP's target differs (the pool page moves), so that one header is unmatched; one payload word differs too
    assert abs(r['structure'] - 25 / 26) < 1e-9, r
    assert abs(r['raw'] - 24 / 26) < 1e-9, r
    # entry state: flip 0's ring segment starts with an empty shadow, flip 1's with 0x0208 set; the pool segments agree
    assert abs(r['structure_entry_state'] - 23 / 26) < 1e-9, r
    # identical frames: every rate is 1.0 except entry state, which only needs equal shadows
    build(p, [frame(0), frame(0)])
    c = P.run(p)[1]['repeat']['N-1']
    assert c['raw'] == 1.0 and c['structure'] == 1.0, c
    # an extra command: structure drops, raw drops by the same words
    build(p, [frame(0), frame(0, extra=True)])
    e = P.run(p)[1]['repeat']['N-1']
    assert abs(e["structure"] - 26 / 28) < 1e-9 and abs(e["raw"] - 26 / 28) < 1e-9, e   # N has 28 words; the extra command (2 words) is unmatched
    # N-2 exists with three flips
    build(p, [frame(0), frame(0), frame(0)])
    f3 = P.run(p)[2]['repeat']; assert f3['N-1']['raw'] == 1.0 and f3['N-2']['raw'] == 1.0, f3
    print('[test_fifo_repeat_probe] ok')
main()

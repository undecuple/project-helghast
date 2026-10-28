#!/usr/bin/env python3
"""Host test for tools/smoke/fp_eval.py (RSX pass 43): assembles a small NV40 fragment program, writes a synthetic
patch-2043 LD_DRAW_INPUTS dump (one 4x4 A8R8G8B8 linear texture) and checks the evaluator's pixels.
No game data, no GPU. Run by tools/smoke/test_fp_eval.sh.
"""
import os, struct, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fp_eval as F  # noqa: E402

OUT = os.path.join(HERE, '..', '..', 'build', 'smoke-host', 'fp_eval')
os.makedirs(OUT, exist_ok=True)
fails = 0


def check(name, cond, detail=''):
    global fails
    print('[test_fp_eval] %s: %s %s' % ('ok  ' if cond else 'FAIL', name, detail))
    if not cond: fails += 1


def word(w):
    """Store a 32-bit ucode word the way the RSX reads it: half-word swapped, big-endian."""
    return struct.pack('>I', ((w << 16) | (w >> 16)) & 0xFFFFFFFF)


def src(kind, idx=0, swz='xyzw', neg=0, abs_=0, half=0):
    t = {'r': 0, 'h': 0, 'in': 1, 'c': 2}[kind]
    w = t | (idx << 2) | ((1 if kind == 'h' else half) << 8) | (neg << 17)
    for i, ch in enumerate(swz):
        w |= 'xyzw'.index(ch) << (9 + 2 * i)
    return w, abs_


def ins(op, dst, s0, s1=None, s2=None, const=None, end=0, sat=0, scale=0, tex=0, inp=0, prec=0):
    """dst = ('r'|'h', idx, 'xyz') or None."""
    w0 = end
    if dst is None: w0 |= 1 << 30
    else:
        w0 |= (dst[1] << 1) | ((1 if dst[0] == 'h' else 0) << 7)
        for ch in dst[2]: w0 |= 1 << (9 + 'xyzw'.index(ch))
    w0 |= (inp << 13) | (tex << 17) | (prec << 22) | (op << 24) | (sat << 31)
    s1 = s1 or src('r'); s2 = s2 or src('r')
    w1 = s0[0] | (7 << 18) | (0xE4 << 21) | (s0[1] << 29)       # exec always, cond swizzle xyzw
    w2 = s1[0] | (s1[1] << 18) | (scale << 28)
    w3 = s2[0] | (s2[1] << 18)
    b = word(w0) + word(w1) + word(w2) + word(w3)
    if const is not None:
        b += b''.join(word(struct.unpack('<I', struct.pack('<f', v))[0]) for v in const)
    return b


# 4x4 texture, A8R8G8B8 linear (bytes A,R,G,B), values chosen so bilinear/nearest confusion shows up
tex = np.zeros((4, 4, 4), np.uint8)
for y in range(4):
    for x in range(4):
        tex[y, x] = (255, 16 * (x + 1), 32 * (y + 1), 128 if (x + y) % 2 else 64)
tex.tofile(os.path.join(OUT, 'inputs_f000001_i0000_u00.bin'))

# program: r0.xy = WPOS.xy * (1/4, 1/4) ; h0 = tex0(r0) ; h1.xyz = (|-h0| * 0.5 + 0.25) x2 sat ; end
prog = b''
prog += ins(0x02, ('r', 0, 'xy'), src('in'), src('c'), const=[0.25, 0.25, 0, 0], inp=0)
prog += ins(0x17, ('h', 0, 'xyzw'), src('r', 0), tex=0)
prog += ins(0x04, ('h', 1, 'xyz'), src('h', 0, neg=1, abs_=1), src('c', swz='xxxx'), src('c', swz='yyyy'),
            const=[0.5, 0.25, 0, 0], scale=1, sat=1, end=1)
open(os.path.join(OUT, 'inputs_f000001_i0000_fp.bin'), 'wb').write(prog)


def write_inputs(wrap):
    with open(os.path.join(OUT, 'inputs_f000001_i0000.txt'), 'w') as f:
        f.write('draw frame=1 idx=0 surf=00010000 prim=6\n')
        f.write('target loc=0 off=00010000 pitch=16 fmt=8 ct=1 clip=4x4\n')
        f.write('blend en=0 sf=1/1 df=1/1 eq=8006/8006 cmask=01010101 at=0/207/0\n')
        f.write('viewport 0 0 4 4 scale 2 -2 0.5 trans 2 2 0.5\n')
        f.write('reg shader_ctrl=00000000 shader_window=00000004 scissor=00040000/00040000 blend_color=00000000\n')
        f.write('unit 0 loc=0 off=00100000 fmt=A5 dim=2 cube=0 mips=1 w=4 h=4 pitch=16 depth=1 wrap=%08X '
                'remap=0000AAE4 filter=01011000 ctl0=80078000 border=00000000 raw=0 0 0 0 0 0 0 0 ctl3=00000010\n' % wrap)
        f.write('unit 0 span=64 saved=1 surface=-1\n')
        f.write('fp loc=1 off=00000000 size=%d\nfpconst count=0\nregs\n' % len(prog))


def run(extra):
    out = os.path.join(OUT, 'eval.ppm')
    r = subprocess.run([sys.executable, os.path.join(HERE, 'fp_eval.py'), os.path.join(OUT, 'inputs_f000001_i0000.txt'),
                        '--out', out] + extra, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-800:], r.stderr[-800:])
        return None
    return F.read_ppm(out).astype(float)


# 1. disassembly names the three instructions
p = F.decode(prog)
check('decode: 3 instructions', len(p) == 3, str(len(p)))
check('decode: opcodes MUL TEX MAD', [F.OPS[i['op']] for i in p] == ['MUL', 'TEX', 'MAD'])
check('decode: src0 abs+neg, scale x2, sat on the MAD',
      p[2]['src'][0]['abs'] == 1 and p[2]['src'][0]['neg'] == 1 and p[2]['scale'] == 1 and p[2]['sat'] == 1)

# 2. nearest sampling, no gamma: h1 = sat(2 * (-|t| * 0.5 + 0.25)) = sat(0.5 - t) (abs before negate)
write_inputs(0x00000303)   # clamp, gamma 0
img = run(['--export', 'h1', '--gamma', 'rgb'])
if img is not None:
    exp = np.clip(0.5 - tex[..., 1:4].astype(float) / 255.0, 0, 1) * 255.0
    check('modifiers: sat(2*(-|t|*0.5+0.25)) = sat(0.5-t)', np.abs(img - exp).max() <= 1.0,
          'max diff %.2f' % np.abs(img - exp).max())
    img0 = run(['--export', 'h0', '--gamma', 'rgb'])
    check('TEX nearest copies the texture', np.abs(img0 - tex[..., 1:4]).max() <= 1.0)

# 3. gamma bits 7 decode R,G,B under --gamma rgb and not under --gamma none
write_inputs(0x00700303)
g = run(['--export', 'h0', '--gamma', 'rgb'])
n = run(['--export', 'h0', '--gamma', 'none'])
if g is not None and n is not None:
    exp = F.srgb_to_linear(tex[..., 1:4].astype(float) / 255.0) * 255.0
    check('gamma 7 under rgb decodes R,G,B', np.abs(g - exp).max() <= 1.0, 'max diff %.2f' % np.abs(g - exp).max())
    check('gamma 7 under none leaves the texel', np.abs(n - tex[..., 1:4]).max() <= 1.0)

# 4. the storage hypothesis with a rotating remap (0x93): bit 0 names storage R, which is the output G
with open(os.path.join(OUT, 'inputs_f000001_i0000.txt')) as f: txt = f.read()
open(os.path.join(OUT, 'inputs_f000001_i0000.txt'), 'w').write(txt.replace('remap=0000AAE4', 'remap=0000AA93')
                                                                .replace('wrap=00700303', 'wrap=00100303'))
s = run(['--export', 'h0', '--gamma', 'storage'])
r = run(['--export', 'h0', '--gamma', 'rgb'])
if s is not None and r is not None:
    # remap 0x93: out R <- storage A (255), out G <- storage R, out B <- storage G
    expected_s = np.stack([np.full((4, 4), 255.0), F.srgb_to_linear(tex[..., 1] / 255.0) * 255.0, tex[..., 2].astype(float)], -1)
    expected_r = np.stack([F.srgb_to_linear(np.ones((4, 4))) * 255.0, tex[..., 1].astype(float), tex[..., 2].astype(float)], -1)
    check('storage mode: gamma bit 0 decodes the remapped G', np.abs(s - expected_s).max() <= 1.0, 'max diff %.2f' % np.abs(s - expected_s).max())
    check('rgb mode: gamma bit 0 decodes the output R', np.abs(r - expected_r).max() <= 1.0, 'max diff %.2f' % np.abs(r - expected_r).max())

print('[test_fp_eval] %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
sys.exit(1 if fails else 0)

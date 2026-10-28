#!/usr/bin/env python3
"""Run one RSX (NV40) fragment program on the CPU from a patch-2043 LD_DRAW_INPUTS dump.

Independent of the port's decompiler: decodes the ucode words itself (RPCS3 RSXFragmentProgram.h layout), samples
the dumped textures with the unit's wrap/filter/remap/gamma state and writes the result as a PPM next to the dump,
so each instruction's value can be compared with the port's HLSL and the draw's actual target (LD_DRAW_DUMP).

  fp_eval.py DIR/inputs_fFFFFFF_iIIII.txt [--out X.ppm] [--step N] [--gamma rgb|storage|none|argb]
             [--fp16] [--prec] [--trace X,Y] [--compare target.ppm]

--step N      evaluate every Nth pixel (default 1 = full target).
--gamma MODE  which channels the TEXTURE_ADDRESS gamma bits decode: rgb (bit 0 R, 1 G, 2 B of the sampled,
              remapped colour; the port's patch 2009), storage (the bits name the format's own A/R/G/B channels
              before the remap), argb (bit 0 A, 1 R, 2 G, 3 B after the remap), none.
--fp16        round 'h' register writes to fp16.
--prec        apply the OPDEST precision clamp (fixed12 [-2,2], fixed9 [-1,1]).
--trace X,Y   print every instruction's result for that pixel.
--compare P   mean/max absolute difference per channel against a PPM (the draw's target after the draw).
--disasm      print the decoded program and exit.

Exit code 0; the host test tools/smoke/test_fp_eval.sh runs it on a synthetic dump (no game data).
"""
import argparse, math, os, struct
import numpy as np

OPS = {0x00: 'NOP', 0x01: 'MOV', 0x02: 'MUL', 0x03: 'ADD', 0x04: 'MAD', 0x05: 'DP3', 0x06: 'DP4', 0x07: 'DST',
       0x08: 'MIN', 0x09: 'MAX', 0x0A: 'SLT', 0x0B: 'SGE', 0x0C: 'SLE', 0x0D: 'SGT', 0x0E: 'SNE', 0x0F: 'SEQ',
       0x10: 'FRC', 0x11: 'FLR', 0x12: 'KIL', 0x13: 'PK4', 0x14: 'UP4', 0x15: 'DDX', 0x16: 'DDY', 0x17: 'TEX',
       0x18: 'TXP', 0x19: 'TXD', 0x1A: 'RCP', 0x1B: 'RSQ', 0x1C: 'EX2', 0x1D: 'LG2', 0x1E: 'LIT', 0x1F: 'LRP',
       0x20: 'STR', 0x21: 'SFL', 0x22: 'COS', 0x23: 'SIN', 0x24: 'PK2', 0x25: 'UP2', 0x26: 'POW', 0x27: 'PKB',
       0x28: 'UPB', 0x29: 'PK16', 0x2A: 'UP16', 0x2B: 'BEM', 0x2C: 'PKG', 0x2D: 'UPG', 0x2E: 'DP2A', 0x2F: 'TXL',
       0x31: 'TXB', 0x33: 'TEXBEM', 0x34: 'TXPBEM', 0x35: 'BEMLUM', 0x36: 'REFL', 0x37: 'TIMESWTEX', 0x38: 'DP2',
       0x39: 'NRM', 0x3A: 'DIV', 0x3B: 'DIVSQ', 0x3C: 'LIF', 0x3D: 'FENCT', 0x3E: 'FENCB', 0x40: 'BRK', 0x41: 'CAL',
       0x42: 'IFE', 0x43: 'LOOP', 0x44: 'REP', 0x45: 'RET'}   # RPCS3 RSXFragmentProgram.h
PREC = {0: 'fp32', 1: 'fp16', 2: 'fixed12', 3: 'fixed9'}
SCALE = {0: 1.0, 1: 2.0, 2: 4.0, 3: 8.0, 4: 1.0, 5: 0.5, 6: 0.25, 7: 0.125}
SWZ = 'xyzw'
FP16_ORDER = '>'   # big-endian halves, see the 0x9A upload in rsx_live_draw.c
INPUTS = {0: 'WPOS', 1: 'COL0', 2: 'COL1', 3: 'FOGC', 4: 'TC0', 5: 'TC1', 6: 'TC2', 7: 'TC3', 8: 'TC4', 9: 'TC5',
          0xA: 'TC6', 0xB: 'TC7', 0xC: 'TC8', 0xD: 'TC9', 0xE: 'SSA', 0xF: 'VFACE'}


def read_word(b, off):
    be = struct.unpack_from('>I', b, off)[0]
    return ((be << 16) | (be >> 16)) & 0xFFFFFFFF


def decode_src(w):
    return dict(type=w & 3, idx=(w >> 2) & 63, half=(w >> 8) & 1,
                swz=[(w >> (9 + 2 * i)) & 3 for i in range(4)], neg=(w >> 17) & 1)


def decode(ucode):
    """List of instruction dicts; CONST sources carry the inline float4 that follows the instruction."""
    prog, off = [], 0
    while off + 16 <= len(ucode):
        w = [read_word(ucode, off + 4 * i) for i in range(4)]
        off += 16
        ins = dict(off=off - 16, end=w[0] & 1, dst=(w[0] >> 1) & 63, dhalf=(w[0] >> 7) & 1,
                   set_cond=(w[0] >> 8) & 1, mask=[(w[0] >> (9 + i)) & 1 for i in range(4)],
                   input=(w[0] >> 13) & 15, tex=(w[0] >> 17) & 15, exp_tex=(w[0] >> 21) & 1,
                   prec=(w[0] >> 22) & 3, op=(w[0] >> 24) & 63, no_dest=(w[0] >> 30) & 1,
                   sat=(w[0] >> 31) & 1, src=[decode_src(w[1]), decode_src(w[2]), decode_src(w[3])],
                   exec_lt=(w[1] >> 18) & 1, exec_eq=(w[1] >> 19) & 1, exec_gr=(w[1] >> 20) & 1,
                   cond_swz=[(w[1] >> (21 + 2 * i)) & 3 for i in range(4)], cond_mod_reg=(w[1] >> 30) & 1,
                   cond_reg=(w[1] >> 31) & 1, scale=(w[2] >> 28) & 7, branch=(w[2] >> 31) & 1, words=w)
        ins['src'][0]['abs'] = (w[1] >> 29) & 1
        ins['src'][1]['abs'] = (w[2] >> 18) & 1
        ins['src'][2]['abs'] = (w[3] >> 18) & 1
        if ins['src'][2]['type'] == 2 and ins['branch']:
            pass
        ins['const'] = None
        if any(s['type'] == 2 for s in ins['src']) and off + 16 <= len(ucode):
            cw = [read_word(ucode, off + 4 * i) for i in range(4)]
            ins['const'] = np.array([struct.unpack('<f', struct.pack('<I', c))[0] for c in cw], dtype=np.float32)
            off += 16
        if not ins['mask'][0] and not ins['mask'][1] and not ins['mask'][2] and not ins['mask'][3]:
            ins['mask'] = [1, 1, 1, 1]
        prog.append(ins)
        if ins['end']:
            break
    return prog


def disasm(ins):
    op = OPS.get(ins['op'], '?%02X' % ins['op'])
    m = ''.join(SWZ[i] for i in range(4) if ins['mask'][i])
    d = '-' if ins['no_dest'] else '%s%d.%s' % ('h' if ins['dhalf'] else 'r', ins['dst'], m)
    srcs = []
    nsrc = {'MOV': 1, 'TEX': 1, 'TXP': 1, 'RCP': 1, 'RSQ': 1, 'EX2': 1, 'LG2': 1, 'FRC': 1, 'FLR': 1, 'COS': 1,
            'SIN': 1, 'NRM': 1, 'MAD': 3, 'LRP': 3, 'DP2A': 3, 'TXL': 2, 'TXB': 2, 'TXD': 3}.get(op, 2)
    for k in range(nsrc):
        s = ins['src'][k]
        if s['type'] == 0: base = '%s%d' % ('h' if s['half'] else 'r', s['idx'])
        elif s['type'] == 1: base = INPUTS.get(ins['input'], 'in%d' % ins['input'])
        else: base = 'c(%s)' % ','.join('%g' % v for v in ins['const'])
        sw = ''.join(SWZ[i] for i in s['swz'])
        t = base + ('.' + sw if sw != 'xyzw' else '')
        if s['abs']: t = '|%s|' % t
        if s['neg']: t = '-' + t
        srcs.append(t)
    flags = []
    if op in ('TEX', 'TXP', 'TXL', 'TXB', 'TXD'): flags.append('unit%d' % ins['tex'])
    if ins['sat']: flags.append('sat')
    if ins['scale']: flags.append('x%g' % SCALE[ins['scale']])
    if ins['prec']: flags.append(PREC[ins['prec']])
    if not (ins['exec_lt'] and ins['exec_eq'] and ins['exec_gr']):
        flags.append('if(%s%s%s cc%d.%s)' % ('<' if ins['exec_lt'] else '', '=' if ins['exec_eq'] else '',
                                             '>' if ins['exec_gr'] else '', ins['cond_reg'],
                                             ''.join(SWZ[i] for i in ins['cond_swz'])))
    if ins['set_cond']: flags.append('cc%d=' % ins['cond_mod_reg'])
    return '%04X %-5s %-10s %s %s' % (ins['off'], op, d, ', '.join(srcs), ' '.join(flags))


# ---- textures -------------------------------------------------------------------------------------------------

def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, np.power((np.maximum(c, 0.0) + 0.055) / 1.055, 2.4))


def swizzle_offsets(w, h):
    """Texel index of each (y, x) in an RSX swizzled (Morton) image (RPCS3 convert_linear_swizzle)."""
    lw, lh = int(math.log2(w)), int(math.log2(h))
    lim = min(lw, lh)
    ys, xs = np.mgrid[0:h, 0:w]
    off = np.zeros((h, w), dtype=np.int64)
    for i in range(lim):
        off |= ((xs >> i) & 1) << (2 * i)
        off |= ((ys >> i) & 1) << (2 * i + 1)
    if lw > lim: off |= (xs >> lim) << (2 * lim)
    if lh > lim: off |= (ys >> lim) << (2 * lim)
    return off


def load_unit(d, unit, dumpdir, base):
    """RGBA float image (A,R,G,B storage order kept as 'storage' array, remapped array returned) for one unit."""
    fmt, w, h = d['fmt'] & 0xFF, d['w'], d['h']
    linear = bool(d['fmt'] & 0x20)
    base_fmt = fmt & 0x9F
    ppm = [f for f in os.listdir(dumpdir) if f.startswith('%s_u%02d_surf' % (base, unit)) and f.endswith('.ppm')]
    storage = None
    if ppm:
        img = read_ppm(os.path.join(dumpdir, ppm[0])).astype(np.float32) / 255.0
        if img.shape[0] != h or img.shape[1] != w:
            print('unit %d: surface %dx%d but texture %dx%d, resizing by nearest' % (unit, img.shape[1], img.shape[0], w, h))
            ys = (np.arange(h) * img.shape[0] // h); xs = (np.arange(w) * img.shape[1] // w)
            img = img[ys][:, xs]
        storage = np.concatenate([np.ones(img.shape[:2] + (1,), np.float32), img], axis=2)  # A,R,G,B
        src = 'surface ' + ppm[0]
    else:
        raw = np.fromfile(os.path.join(dumpdir, '%s_u%02d.bin' % (base, unit)), dtype=np.uint8)
        src = 'guest bytes'
        if base_fmt == 0x85:
            if linear:
                pitch = d['pitch'] or w * 4
                px = raw[:pitch * h].reshape(h, pitch)[:, :w * 4].reshape(h, w, 4)
            else:
                px = raw[:w * h * 4].reshape(-1, 4)[swizzle_offsets(w, h)]
            storage = px.astype(np.float32) / 255.0                     # bytes A,R,G,B
        elif base_fmt == 0x9A:
            cnt = w * h * 4
            hv = raw[:cnt * 2].view(FP16_ORDER + 'f2')
            px = hv.astype(np.float32).reshape(-1, 4)
            if not linear and w > 1 and h > 1: px = px[swizzle_offsets(w, h)]
            else: px = px.reshape(h, w, 4)
            # W16_Z16_Y16_X16: halves X,Y,Z,W in memory = R,G,B,A (the port's ld_float_tex_mapping). Storage A,R,G,B:
            storage = px.reshape(h, w, 4)[..., [3, 0, 1, 2]]
        elif base_fmt == 0x81:
            px = raw[:w * h].reshape(h, w).astype(np.float32) / 255.0
            storage = np.stack([np.ones_like(px), px, px, px], axis=2)
        else:
            raise SystemExit('unit %d: format %02X not handled' % (unit, fmt))
    # remap: out comp c (0 A,1 R,2 G,3 B) <- op (bits 8+2c), sel (bits 2c)
    remap = d['remap']
    out = np.zeros_like(storage)
    for c in range(4):
        op = (remap >> (8 + 2 * c)) & 3; sel = (remap >> (2 * c)) & 3
        out[..., c] = 0.0 if op == 0 else 1.0 if op == 1 else storage[..., sel]
    return dict(storage=storage, argb=out, w=w, h=h, wrap_s=d['wrap'] & 0xF, wrap_t=(d['wrap'] >> 8) & 0xF,
                gamma=(d['wrap'] >> 20) & 0xF, remap=remap, mag=(d['filter'] >> 24) & 7, min=(d['filter'] >> 16) & 7,
                src=src, fmt=fmt)


def apply_gamma(tex, mode):
    """Return the RGBA (x=R,y=G,z=B,w=A) image after the gamma decode under one hypothesis."""
    argb = tex['argb'].copy()
    g = tex['gamma']
    if g and mode != 'none':
        if mode == 'rgb':          # bit 0 R, 1 G, 2 B of the remapped colour (patch 2009)
            for bit, c in ((0, 1), (1, 2), (2, 3)):
                if g & (1 << bit): argb[..., c] = srgb_to_linear(argb[..., c])
        elif mode == 'argb':       # bit 0 A, 1 R, 2 G, 3 B of the remapped colour
            for bit, c in ((0, 0), (1, 1), (2, 2), (3, 3)):
                if g & (1 << bit): argb[..., c] = srgb_to_linear(argb[..., c])
        elif mode == 'storage':    # bits name the storage channels R,G,B,A before the remap
            st = tex['storage'].copy()
            for bit, c in ((0, 1), (1, 2), (2, 3), (3, 0)):
                if g & (1 << bit): st[..., c] = srgb_to_linear(st[..., c])
            remap = tex['remap']
            for c in range(4):
                op = (remap >> (8 + 2 * c)) & 3; sel = (remap >> (2 * c)) & 3
                argb[..., c] = 0.0 if op == 0 else 1.0 if op == 1 else st[..., sel]
    return argb[..., [1, 2, 3, 0]]


def wrap_coord(i, n, mode):
    if mode == 1:   # WRAP
        return np.mod(i, n)
    if mode == 2:   # MIRROR
        p = np.mod(i, 2 * n); return np.where(p >= n, 2 * n - 1 - p, p)
    return np.clip(i, 0, n - 1)   # CLAMP_TO_EDGE and the others (no border colour here)


def sample(tex, img, u, v, bilinear):
    w, h = tex['w'], tex['h']
    x = u * w - 0.5; y = v * h - 0.5
    if not bilinear:
        xi = wrap_coord(np.floor(x + 0.5).astype(np.int64), w, tex['wrap_s'])
        yi = wrap_coord(np.floor(y + 0.5).astype(np.int64), h, tex['wrap_t'])
        return img[yi, xi]
    x0 = np.floor(x); y0 = np.floor(y); fx = (x - x0)[..., None]; fy = (y - y0)[..., None]
    x0 = x0.astype(np.int64); y0 = y0.astype(np.int64)
    xa = wrap_coord(x0, w, tex['wrap_s']); xb = wrap_coord(x0 + 1, w, tex['wrap_s'])
    ya = wrap_coord(y0, h, tex['wrap_t']); yb = wrap_coord(y0 + 1, h, tex['wrap_t'])
    return ((img[ya, xa] * (1 - fx) + img[ya, xb] * fx) * (1 - fy) + (img[yb, xa] * (1 - fx) + img[yb, xb] * fx) * fy)


# ---- dump parsing ----------------------------------------------------------------------------------------------

def read_ppm(p):
    d = open(p, 'rb').read()
    parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], dtype=np.uint8)[:w * h * 3].reshape(h, w, 3)


def write_ppm(p, img8):
    with open(p, 'wb') as f:
        f.write(b'P6\n%d %d\n255\n' % (img8.shape[1], img8.shape[0])); f.write(img8.astype(np.uint8).tobytes())


def parse_inputs(path):
    info = dict(units={}, consts=[])
    for line in open(path):
        t = line.split()
        if not t: continue
        if t[0] == 'unit' and 'loc=' in line:
            d = {k: int(v, 16) if k in ('off', 'wrap', 'remap', 'filter', 'ctl0', 'ctl3', 'border', 'fmt') else int(v)
                 for k, v in (kv.split('=') for kv in t[2:] if '=' in kv and kv.split('=')[0] != 'raw')}
            info['units'][int(t[1])] = d
        elif t[0] == 'target':
            info['target'] = {k: v for k, v in (kv.split('=') for kv in t[1:])}
        elif t[0] == 'blend':
            info['blend'] = {k: v for k, v in (kv.split('=') for kv in t[1:])}
        elif t[0] == 'reg':
            info['reg'] = {k: int(v.split('/')[0], 16) for k, v in (kv.split('=') for kv in t[1:])}
        elif t[0] == 'regs':
            break
    return info


# ---- the interpreter -------------------------------------------------------------------------------------------

def fp16(a):
    return a.astype(np.float16).astype(np.float32)


def evaluate(prog, info, texs, W, H, step, args, trace=None):
    ys, xs = np.mgrid[0:H:step, 0:W:step]
    shape = xs.shape
    wpos = np.stack([xs + 0.5, ys + 0.5, np.zeros(shape), np.ones(shape)], axis=-1).astype(np.float32)
    inputs = {0: wpos}
    for i in range(1, 16):
        inputs[i] = np.zeros(shape + (4,), np.float32)
    if args.tc0 == 'uv':
        inputs[4] = np.stack([(xs + 0.5) / W, (ys + 0.5) / H, np.zeros(shape), np.ones(shape)], axis=-1).astype(np.float32)
    inputs[1] = np.broadcast_to(np.array([float(v) for v in args.col0.split(',')], np.float32), shape + (4,)).copy()
    r = [np.zeros(shape + (4,), np.float32) for _ in range(64)]
    h = [np.zeros(shape + (4,), np.float32) for _ in range(64)]
    cc = [np.zeros(shape + (4,), np.float32), np.zeros(shape + (4,), np.float32)]
    texcache = {}
    if trace:
        tx, ty = trace; ti = (ty // step, tx // step)

    def src_val(ins, k):
        s = ins['src'][k]
        if s['type'] == 0: v = (h if s['half'] else r)[s['idx']]
        elif s['type'] == 1: v = inputs[ins['input']]
        else: v = np.broadcast_to(ins['const'], shape + (4,))
        v = v[..., s['swz']]
        if s['abs']: v = np.abs(v)
        if s['neg']: v = -v
        return v.astype(np.float32)

    def tex_sample(ins, coord, proj=False):
        u = ins['tex']
        if u not in texs: raise SystemExit('unit %d sampled but not dumped' % u)
        t = texs[u]
        if u not in texcache:
            mode = args.surf_gamma if (args.surf_gamma and t.get('is_surface')) else args.gamma
            for ug in args.unit_gamma:
                if int(ug.split('=')[0]) == u: mode = ug.split('=')[1]
            texcache[u] = apply_gamma(t, mode)
            for us in args.unit_scale:
                if int(us.split('=')[0]) == u: texcache[u] = texcache[u] * float(us.split('=')[1])
        img = texcache[u]
        uu, vv = coord[..., 0], coord[..., 1]
        if proj: uu, vv = uu / coord[..., 3], vv / coord[..., 3]
        return sample(t, img, uu, vv, t['mag'] >= 2).astype(np.float32)

    for n, ins in enumerate(prog):
        op = OPS.get(ins['op'], '?')
        if op in ('NOP', 'FENCT', 'FENCB') or ins['op'] >= 0x40:   # fences/branches: not needed for the programs this is used on
            if ins['end']: break
            continue
        a = src_val(ins, 0); b = src_val(ins, 1) if op not in ('MOV', 'TEX', 'TXP', 'RCP', 'RSQ', 'EX2', 'LG2', 'FRC', 'FLR', 'COS', 'SIN', 'NRM', 'KIL') else None
        c = src_val(ins, 2) if op in ('MAD', 'LRP', 'DP2A', 'TXD') else None
        if op == 'MOV': v = a
        elif op == 'MUL': v = a * b
        elif op == 'ADD': v = a + b
        elif op == 'MAD': v = a * b + c
        elif op == 'DP3': v = np.repeat((a[..., :3] * b[..., :3]).sum(-1, keepdims=True), 4, -1)
        elif op == 'DP4': v = np.repeat((a * b).sum(-1, keepdims=True), 4, -1)
        elif op == 'DP2': v = np.repeat((a[..., :2] * b[..., :2]).sum(-1, keepdims=True), 4, -1)
        elif op == 'DP2A': v = np.repeat((a[..., :2] * b[..., :2]).sum(-1, keepdims=True) + c[..., :1], 4, -1)
        elif op == 'MIN': v = np.minimum(a, b)
        elif op == 'MAX': v = np.maximum(a, b)
        elif op == 'SLT': v = (a < b).astype(np.float32)
        elif op == 'SGE': v = (a >= b).astype(np.float32)
        elif op == 'SLE': v = (a <= b).astype(np.float32)
        elif op == 'SGT': v = (a > b).astype(np.float32)
        elif op == 'SNE': v = (a != b).astype(np.float32)
        elif op == 'SEQ': v = (a == b).astype(np.float32)
        elif op == 'FRC': v = a - np.floor(a)
        elif op == 'FLR': v = np.floor(a)
        elif op == 'RCP': v = np.repeat(1.0 / a[..., :1], 4, -1)
        elif op == 'RSQ': v = np.repeat(1.0 / np.sqrt(np.abs(a[..., :1])), 4, -1)
        elif op == 'EX2': v = np.repeat(np.exp2(a[..., :1]), 4, -1)
        elif op == 'LG2': v = np.repeat(np.log2(np.abs(a[..., :1])), 4, -1)
        elif op == 'COS': v = np.repeat(np.cos(a[..., :1]), 4, -1)
        elif op == 'SIN': v = np.repeat(np.sin(a[..., :1]), 4, -1)
        elif op == 'LRP': v = a * b + (1 - a) * c
        elif op == 'POW': v = np.repeat(np.power(np.abs(a[..., :1]), b[..., :1]), 4, -1)
        elif op == 'DIV': v = a / b[..., :1]
        elif op == 'DIVSQ': v = a / np.sqrt(np.abs(b[..., :1]))
        elif op == 'NRM': v = a / np.sqrt((a[..., :3] ** 2).sum(-1, keepdims=True))
        elif op == 'TEX': v = tex_sample(ins, a)
        elif op == 'TXP': v = tex_sample(ins, a, proj=True)
        elif op in ('TXL', 'TXB'): v = tex_sample(ins, a)
        elif op == 'KIL':
            if ins['end']: break
            continue
        else:
            raise SystemExit('opcode %s at %04X not implemented' % (op, ins['off']))
        v = (v * SCALE[ins['scale']]).astype(np.float32)
        if ins['sat']: v = np.clip(v, 0.0, 1.0)
        elif args.prec and ins['prec'] == 2: v = np.clip(v, -2.0, 2.0)
        elif args.prec and ins['prec'] == 3: v = np.clip(v, -1.0, 1.0)
        if args.fp16 and ins['dhalf']: v = fp16(v)
        # exec condition (per component sign test of the swizzled CC register)
        allc = ins['exec_lt'] and ins['exec_eq'] and ins['exec_gr']
        if not allc:
            ccv = cc[ins['cond_reg']][..., ins['cond_swz']]
            ok = np.zeros(ccv.shape, bool)
            if ins['exec_lt']: ok |= ccv < 0
            if ins['exec_eq']: ok |= ccv == 0
            if ins['exec_gr']: ok |= ccv > 0
        if not ins['no_dest']:
            dst = (h if ins['dhalf'] else r)[ins['dst']]
            for ci in range(4):
                if ins['mask'][ci]:
                    if allc: dst[..., ci] = v[..., ci]
                    else: dst[..., ci] = np.where(ok[..., ci], v[..., ci], dst[..., ci])
        if ins['set_cond']:
            dcc = cc[ins['cond_mod_reg']]
            for ci in range(4):
                if ins['mask'][ci]: dcc[..., ci] = v[..., ci]
        if trace:
            print('%-60s = %s' % (disasm(ins), ' '.join('%.5f' % x for x in v[ti])))
        if ins['end']: break
    return r, h, cc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inputs')
    ap.add_argument('--out'); ap.add_argument('--step', type=int, default=1)
    ap.add_argument('--gamma', default='rgb', choices=['rgb', 'storage', 'argb', 'none'])
    ap.add_argument('--fp16', action='store_true'); ap.add_argument('--prec', action='store_true')
    ap.add_argument('--trace'); ap.add_argument('--compare'); ap.add_argument('--disasm', action='store_true')
    ap.add_argument('--export', default='h0', help='register holding colour 0 (h0 for half exports, r0 for 32-bit)')
    ap.add_argument('--size', help='evaluate WxH pixels (default: the target clip)')
    ap.add_argument('--tc0', default='zero', choices=['zero', 'uv'],
                    help='TC0 input: zero, or uv = the pixel centre over the evaluated size (a full-screen quad)')
    ap.add_argument('--col0', default='0,0,0,0', help='COL0 input, four comma-separated floats')
    ap.add_argument('--unit-scale', action='append', default=[], metavar='U=K', help='multiply one unit (after gamma)')
    ap.add_argument('--unit-gamma', action='append', default=[], metavar='U=MODE', help='gamma mode for one unit')
    ap.add_argument('--surf-gamma', choices=['rgb', 'storage', 'argb', 'none'],
                    help='gamma mode for units that alias a live surface (default: --gamma)')
    ap.add_argument('--unit', action='append', default=[], metavar='U=PPM',
                    help='replace unit U\'s image with this PPM (e.g. an earlier pass evaluated by this tool)')
    args = ap.parse_args()
    dumpdir = os.path.dirname(os.path.abspath(args.inputs))
    base = os.path.basename(args.inputs)[:-4]
    info = parse_inputs(args.inputs)
    ucode = open(os.path.join(dumpdir, base + '_fp.bin'), 'rb').read()
    prog = decode(ucode)
    if args.disasm:
        for ins in prog: print(disasm(ins))
        print('%d instructions' % len(prog))
        return
    W, H = (int(x) for x in (args.size or info['target']['clip']).split('x'))
    used = sorted({ins['tex'] for ins in prog if OPS.get(ins['op']) in ('TEX', 'TXP', 'TXL', 'TXB', 'TXD')})
    texs = {}
    for u in used:
        if u in info['units']:
            texs[u] = load_unit(info['units'][u], u, dumpdir, base)
            t = texs[u]
            for ov in args.unit:
                if int(ov.split('=')[0]) == u:
                    img = read_ppm(ov.split('=', 1)[1]).astype(np.float32) / 255.0
                    t['storage'] = np.concatenate([np.ones(img.shape[:2] + (1,), np.float32), img], axis=2)
                    t['argb'] = t['storage'].copy(); t['w'], t['h'] = img.shape[1], img.shape[0]; t['src'] = ov
            t['is_surface'] = t['src'].startswith('surface') or '=' in t['src']
            print('unit %d: %dx%d fmt %02X wrap %d/%d gamma %X remap %04X filter mag %d min %d from %s' %
                  (u, t['w'], t['h'], t['fmt'], t['wrap_s'], t['wrap_t'], t['gamma'], t['remap'] & 0xFFFF, t['mag'], t['min'], t['src']))
    trace = tuple(int(x) for x in args.trace.split(',')) if args.trace else None
    r, h, cc = evaluate(prog, info, texs, W, H, args.step, args, trace)
    if args.export == 'auto':   # the register the program wrote last among h0 / r0 (the port's fp_export_target rule)
        wrote = [(ins['dhalf'], ins['dst']) for ins in prog if not ins['no_dest'] and ins['dst'] == 0]
        args.export = 'h0' if (not wrote or wrote[-1][0]) else 'r0'
        print('export %s' % args.export)
    export = (h if args.export[0] == 'h' else r)[int(args.export[1:])]
    img8 = np.clip(export[..., :3] * 255.0 + 0.5, 0, 255).astype(np.uint8)
    out = args.out or os.path.join(dumpdir, base + '_eval_%s.ppm' % args.gamma)
    write_ppm(out, img8)
    print('wrote %s (%dx%d, step %d): mean RGB %s max %s' % (out, img8.shape[1], img8.shape[0], args.step,
          img8.reshape(-1, 3).mean(0).round(2), img8.reshape(-1, 3).max(0)))
    if args.compare:
        ref = read_ppm(args.compare)[::args.step, ::args.step].astype(np.float32)
        d = img8.astype(np.float32) - ref
        print('vs %s: ref mean %s, diff mean %s, |diff| mean %s max %s' % (os.path.basename(args.compare),
              ref.reshape(-1, 3).mean(0).round(2), d.reshape(-1, 3).mean(0).round(3),
              np.abs(d).reshape(-1, 3).mean(0).round(3), np.abs(d).reshape(-1, 3).max(0)))


if __name__ == '__main__':
    main()

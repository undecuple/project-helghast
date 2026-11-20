#!/usr/bin/env python3
"""Offline repeat probe on a LD_FIFO_CAPTURE file (command-thread overhaul O0, patch 2188 round).

  python3 -I tools/smoke/fifo_repeat_probe.py work/captures/fifo-capture.kz3fc [--json out.json]

Reads the CMD (type 4) and FLIP (type 9) records only (payloads of the other records are skipped, so a 500 MB capture
reads in seconds). A "flip" is the stream between two FLIP records. Output, per flip:

 1. Words attributed by segment kind. A segment is a maximal run of commands whose get offsets are consecutive; a
    discontinuity starts a new one. The capture does not tag the writer, so the kinds are
    address-and-link classes, ASSUMPTION: a segment is the ring when it lies in the 1 MB IO page of the stream's first
    segment (the title's command buffer; it moves between flips); every other segment is a pool segment (the SPU jobs'
    push-buffer chunks), split by how the walker entered it: jump (entered by a JUMP), call (CALL), ret (RET), park (the
    previous command is no link: the walker waited at a park and the SPU patched it, "park patch"). Pool segments that
    start at offset 0x400 mod 0x10000 are the chunk heads. Separately, words by method class (vertex constants,
    vertex-program upload, texture, draw, user command 0xEB00, other) and the link types that entered pool segments.
 2. Repeat rates of flip N against N-1 and N-2 (only the flips that exist; a typical capture holds 2 flips, so N-2 is n/a):
      raw        words equal, at the same position inside a command that the alignment matched, / all words of N
      structure  words of commands the alignment matched (same header word, so same method and count) / all words of N
      struct+st  structure-matched commands whose segment-entry state equals the matched command's (every register), / all
                 words of N; "similarity" is the same weighted by the share of registers that are equal at entry. Any
                 per-frame pointer (vertex array, texture, label) makes the strict rate 0, so the differing registers are listed.
    The alignment is an LCS (difflib) over the command header words (offsets are ignored: a flip's pages move). The entry
    state of a segment is a hash of the shadow of every NV4097 method register written before the segment, excluding the
    vertex-program upload (0x0B80..0x0BFF) and constant (0x1EFC..0x1FFF) ranges, which are the payload under test.
Exit status 0; nothing is written except the report (and --json)."""
import argparse, collections, difflib, json, struct, sys

def read(path):
    flips = [[]]
    with open(path, 'rb') as f:
        h = f.read(32)
        if h[:8] != b'KZ3FIFOC': sys.exit('not a KZ3 FIFO capture: ' + path)
        while True:
            h = f.read(24)
            if len(h) < 24: break
            t, ln, seq, chk = struct.unpack('<IIQQ', h)
            if t == 4:
                p = f.read(ln); g, hw, n = struct.unpack('<3I', p[:12])
                flips[-1].append((g, hw, struct.unpack('<%dI' % n, p[12:12 + 4 * n]) if n else ()))
            elif t == 9:
                f.seek(ln, 1); flips.append([])
            else:
                f.seek(ln, 1)
    return [x for x in flips if x]

def link(hw):
    if hw == 0x00020000: return 'ret'
    if (hw & 0xE0000003) == 0x20000000 or (hw & 3) == 1 and hw >> 29 == 0 and False: return 'jump'
    if (hw & 3) == 2: return 'call'
    return None

def method_of(hw): return hw & 0x1FFC if hw >> 29 in (0, 2) and (hw & 3) == 0 else None

def mclass(hw):
    m = method_of(hw)
    if m is None: return 'link/flip'
    if 0x1EFC <= m <= 0x1FFC: return 'vertex_const'
    if 0x0B80 <= m <= 0x0BFC: return 'vp_upload'
    if 0x1A00 <= m <= 0x1AFC or 0x0900 <= m <= 0x0980: return 'texture'
    if m in (0x1808, 0x1814, 0x1800, 0x1810, 0x1D94): return 'draw/clear/report'
    return 'other'

def segments(cmds):
    """list of dict(start, cmds=[idx], enter) ; consecutive get offsets inside one segment."""
    segs = []; prev = None
    for i, (g, hw, pl) in enumerate(cmds):
        if prev is None or g != prev[0] + 4 + 4 * len(prev[2]):
            segs.append({'start': g, 'idx': [], 'enter': 'start' if prev is None else (link(prev[1]) or 'park')})
        segs[-1]['idx'].append(i); prev = (g, hw, pl)
    return segs

def kinds(cmds, segs):
    ring_page = segs[0]['start'] >> 20 if segs else None
    out = collections.Counter(); cnt = collections.Counter(); enter = collections.Counter(); mc = collections.Counter()
    seg_kind = []
    for s in segs:
        words = sum(1 + len(cmds[i][2]) for i in s['idx'])
        if s['start'] >> 20 == ring_page: k = 'ring'
        else:
            k = 'pool_' + s['enter'] + ('_head' if (s['start'] & 0xFFFF) == 0x400 else '')
            enter[s['enter']] += 1
        seg_kind.append(k); out[k] += words; cnt[k] += 1
    for g, hw, pl in cmds:
        w = 1 + len(pl)
        mc['user_cmd_0xEB00' if method_of(hw) == 0xEB00 else mclass(hw)] += w
    return out, cnt, enter, mc, seg_kind

def entry_states(cmds, segs, shadow):
    """hash of the shadow registers at each segment start; advances `shadow` through the flip."""
    res = []
    for s in segs:
        res.append(dict(shadow))
        for i in s['idx']:
            g, hw, pl = cmds[i]; m = method_of(hw)
            if m is None: continue
            inc = hw >> 29 == 0
            for k, v in enumerate(pl):
                mm = m + (4 * k if inc else 0)
                if 0x0B80 <= mm <= 0x0BFC or 0x1EFC <= mm <= 0x1FFC: continue
                shadow[mm] = v
    return res

def compare(cur, prev, st_cur, st_prev, segc, segp):
    hc = [c[1] for c in cur]; hp = [c[1] for c in prev]
    sm = difflib.SequenceMatcher(None, hp, hc, autojunk=False)
    cur_seg = {}; prev_seg = {}
    for si, s in enumerate(segc):
        for i in s['idx']: cur_seg[i] = si
    for si, s in enumerate(segp):
        for i in s['idx']: prev_seg[i] = si
    total = sum(1 + len(c[2]) for c in cur); raw = struct_w = st_w = 0; sim_w = 0.0
    sim_cache = {}; differ = collections.Counter()
    for a, b, n in sm.get_matching_blocks():
        for k in range(n):
            ci, pi = b + k, a + k
            w = 1 + len(cur[ci][2]); struct_w += w
            eq = 1 + sum(1 for x, y in zip(cur[ci][2], prev[pi][2]) if x == y)
            raw += eq
            key = (cur_seg[ci], prev_seg[pi]); a_, b_ = st_cur[key[0]], st_prev[key[1]]
            if key not in sim_cache:
                regs = set(a_) | set(b_); bad = [x for x in regs if a_.get(x) != b_.get(x)]
                sim_cache[key] = 1.0 - len(bad) / len(regs) if regs else 1.0
                if bad:
                    for x in bad: differ[x] += 1
            if a_ == b_: st_w += w
            sim_w += w * sim_cache[key]
    return {'words': total, 'raw': raw / total, 'structure': struct_w / total, 'structure_entry_state': st_w / total,
            'structure_state_similarity': sim_w / total, 'segment_pairs': len(sim_cache),
            'top_differing_registers': [('0x%X' % x, c) for x, c in differ.most_common(12)]}

def run(path):
    flips = read(path); shadow = {}; info = []
    for fi, cmds in enumerate(flips):
        segs = segments(cmds); k, c, e, mc, sk = kinds(cmds, segs)
        st = entry_states(cmds, segs, shadow)
        info.append({'cmds': cmds, 'segs': segs, 'kinds': k, 'segs_n': c, 'enter': e, 'mclass': mc, 'st': st})
    rep = []
    for fi, d in enumerate(info):
        r = {'flip_index': fi, 'cmds': len(d['cmds']), 'words': sum(1 + len(c[2]) for c in d['cmds']), 'segments': len(d['segs']),
             'words_by_kind': dict(d['kinds']), 'segments_by_kind': dict(d['segs_n']), 'pool_entry_links': dict(d['enter']),
             'words_by_method_class': dict(d['mclass']), 'repeat': {}}
        for back in (1, 2):
            if fi - back < 0: r['repeat']['N-%d' % back] = None; continue
            p = info[fi - back]
            r['repeat']['N-%d' % back] = compare(d['cmds'], p['cmds'], d['st'], p['st'], d['segs'], p['segs'])
        rep.append(r)
    return rep

def show(rep):
    for r in rep:
        print('flip #%d: %d commands, %d words, %d segments' % (r['flip_index'], r['cmds'], r['words'], r['segments']))
        for k, v in sorted(r['words_by_kind'].items(), key=lambda x: -x[1]):
            print('  kind %-18s words %7d (%5.1f %%) segments %d' % (k, v, 100.0 * v / r['words'], r['segments_by_kind'][k]))
        print('  pool entry links:', r['pool_entry_links'])
        for k, v in sorted(r['words_by_method_class'].items(), key=lambda x: -x[1]):
            print('  method class %-18s words %7d (%5.1f %%)' % (k, v, 100.0 * v / r['words']))
        for k, v in r['repeat'].items():
            if v is None: print('  repeat vs %s: n/a (not in the capture)' % k)
            else:
                print('  repeat vs %s: raw %.3f  structure %.3f  structure+entry-state (all registers equal) %.3f  (register similarity at entry, weighted %.3f)'
                      % (k, v['raw'], v['structure'], v['structure_entry_state'], v['structure_state_similarity']))
                print('    segment pairs %d, registers that differ most often at entry: %s' % (v['segment_pairs'], v['top_differing_registers']))

if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('capture'); ap.add_argument('--json')
    a = ap.parse_args(); rep = run(a.capture); show(rep)
    if a.json: json.dump(rep, open(a.json, 'w'), indent=1)

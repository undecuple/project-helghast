#!/usr/bin/env python3
"""KZ3 pipeline template tool (patch 2071, RSX pass 67): merge and inspect template files.

A template (libs/video/rsx_pso_template.h) is a header plus fixed-size records of hashes and numeric state, no shader
text and no game code. The game writes one with LD_PSO_TEMPLATE_OUT=<file> and reads one with LD_PSO_TEMPLATE=<file>.

  pso_template.py merge OUT IN [IN...]   union by pipeline key (the first file wins on a clash), sorted by key;
                                         fails when records of one key differ in pipeline state (NOT_PIPELINE)
  pso_template.py info FILE [FILE...]    counts and distinct programs
  pso_template.py check FILE             exit 0 when the file is exactly a header plus records of the known layout
"""
import struct
import sys

MAGIC = 0x54505A4B
VERSION = 1
KEYVER = 1
HDR = struct.Struct("<6I")                      # magic, version, rec_size, key_version, flags, count
REC = struct.Struct("<Q5Q7I4I3II16s16H32I16s64I")
F_BUFFERED = 8
MAX_RECS = 65536
NAMES = ("key vp_hash vp_first fp_hash fp_first gamma_mask "
         "vp_instrs vp_start fp_size fp_ctrl cube_mask vtex_mask srgb_out "
         "shadow_mask argb_mask unnorm_mask flags layout_mask layout_stride layout_count rs_words").split()
RS0 = len(REC.unpack(bytes(REC.size))) - 64     # index of rs[0] (the render_state_t words) in REC.unpack
# Fields that never select a different pipeline, so two records of one key may differ in them (RSX pass 73):
#  vp_start: the VP slot; it reaches the HLSL only through a branching program's targets, and since patch 2077 the
#            key hashes it for exactly those programs (rsx_vp_key_branch_base), so a clash in it is branch-free.
#  rs[2], rs[3]: alpha_ref_raw, alpha_ref_format; with buffered fragment constants the alpha reference is a constant
#            (g.fp_alpha_ref), not shader text or PSO state; ld_hash_structural_render_state leaves both out.
NOT_PIPELINE = {NAMES.index("vp_start"): "vp_start", RS0 + 2: "alpha_ref_raw", RS0 + 3: "alpha_ref_format"}


def read(path, strict=False):
    """-> (flags, [record bytes]); raises ValueError on a bad file. strict (the `check` command, which gates what the
    release ships): the file must be exactly the header plus its records, nothing appended."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < HDR.size:
        raise ValueError(f"{path}: too short")
    magic, ver, rsz, kver, flags, count = HDR.unpack_from(data)
    if magic != MAGIC or ver != VERSION:
        raise ValueError(f"{path}: not a template (magic/version)")
    if kver != KEYVER:
        raise ValueError(f"{path}: key version {kver}, this tool knows {KEYVER}")
    if rsz != REC.size:
        raise ValueError(f"{path}: record size {rsz}, expected {REC.size}")
    if count > MAX_RECS or len(data) < HDR.size + count * rsz:
        raise ValueError(f"{path}: truncated or too many records ({count})")
    if strict and len(data) != HDR.size + count * rsz:
        raise ValueError(f"{path}: {len(data) - HDR.size - count * rsz} bytes after the last record")
    recs = [data[HDR.size + i * rsz: HDR.size + (i + 1) * rsz] for i in range(count)]
    return flags, recs


def write(path, flags, recs):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(HDR.pack(MAGIC, VERSION, REC.size, KEYVER, flags, len(recs)))
        for r in recs:
            f.write(r)
    import os
    os.replace(tmp, path)


def key_of(rec):
    return struct.unpack_from("<Q", rec)[0]


def fields(rec):
    t = REC.unpack(rec)
    return dict(zip(NAMES, t[:len(NAMES)]))


def merge(out, ins):
    """Union by key, first record kept. Records of one key may differ only in NOT_PIPELINE fields; any other difference
    means the key misses state the pipeline depends on, so the merge fails and writes nothing."""
    seen = {}
    flags = F_BUFFERED
    clash = total = 0
    diff = {}   # field index -> records of a later file that differ from the kept one
    for p in ins:
        fl, recs = read(p)
        flags &= fl
        for r in recs:
            total += 1
            k = key_of(r)
            if k in seen:
                if seen[k] != r:
                    clash += 1
                    for i, (x, y) in enumerate(zip(REC.unpack(seen[k]), REC.unpack(r))):
                        if x != y:
                            diff[i] = diff.get(i, 0) + 1
            else:
                seen[k] = r
    names = {i: NOT_PIPELINE.get(i) or (NAMES[i] if i < len(NAMES) else f"rs[{i - RS0}]" if i >= RS0 else f"field#{i}")
             for i in diff}
    bad = sorted(i for i in diff if i not in NOT_PIPELINE)
    if bad:
        raise ValueError("records of one key differ in pipeline state ("
                         + ", ".join(f"{names[i]} x{diff[i]}" for i in bad) + "): the pipeline key misses it")
    if len(seen) > MAX_RECS:
        raise ValueError(f"{len(seen)} records exceed the format limit {MAX_RECS}")
    write(out, flags, [seen[k] for k in sorted(seen)])
    print(f"merged {len(ins)} files, {total} records -> {len(seen)} pipelines "
          f"({total - len(seen)} duplicates, {clash} of them with different contents) -> {out}")
    if diff:
        print("  differing fields among clashes (none reaches the pipeline): "
              + ", ".join(f"{names[i]} x{n}" for i, n in sorted(diff.items())))
    return 0


def info(paths):
    for p in paths:
        fl, recs = read(p)
        fs = [fields(r) for r in recs]
        print(f"{p}: {len(recs)} pipelines, {len({f['vp_hash'] for f in fs})} vertex programs, "
              f"{len({f['fp_hash'] for f in fs})} fragment programs, flags 0x{fl:x}")
    return 0


def main(argv):
    if len(argv) >= 4 and argv[1] == "merge":
        try:
            return merge(argv[2], argv[3:])
        except (ValueError, OSError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
    if len(argv) >= 3 and argv[1] == "info":
        try:
            return info(argv[2:])
        except (ValueError, OSError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
    if len(argv) == 3 and argv[1] == "check":
        try:
            read(argv[2], strict=True)
            return 0
        except (ValueError, OSError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))

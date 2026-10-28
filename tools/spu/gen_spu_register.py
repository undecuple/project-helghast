#!/usr/bin/env python3
"""Generate the KZ3 SPU registration TU for ps3recomp's runtime from the lift in work/spu/lift.

    gen_spu_register.py [--lift work/spu] [--out work/spu/gen]

Run after tools/spu/lift_spu.sh, inside the sandbox (reads the decrypted EBOOT and the SPU ELF dumps):

    nix develop .#re --command tools/sandbox.sh -- python3 tools/spu/gen_spu_register.py

Writes (derived from the binary, so not committed):
  <out>/kz3_spu_register.c   kz3_spu_register_all() + a constructor that calls it
  <out>/kz3_spu_manifest.json  image ids, fingerprints, signatures, entry symbols
  <out>/sources.txt          names of the registered images; the library compiles lift/<name>/spu_recomp.c

How each kind is found at run time (ps3recomp v0.13.0 runtime, read in runtime/spu/ and libs/spurs/):
  * SPURS policy module (pm_*): cellSpurs.c spurs_resolve_pm() fingerprints `sizePm` bytes at the guest EA
    (FNV-1a-64, spu_workload_fingerprint) and calls spu_workload_find_img(). Registered with
    spu_workload_register_img(fp, entry 0xA00, image id). The WWS job manager goes in as image id 2: the
    runtime's WWS job-code dispatch (spu_channels.c spu_indirect_branch: `ctx->image_id == 2`, code buffer
    pointer at LS 0x1320) is keyed to that id. KZ3's PM keeps the code buffer pointer at LS 0x1320 too
    (`lqa $r0,0x1320; lqd $r4,0x10($r0); a $r5,$r4,$r0; bisl $r0,$r5` at LS 0x31F0-0x32AC).
  * WWS job modules (job_*): streamed into the job code buffer; the runtime matches the first 16 bytes
    (the four `ila` signature) against spu_overlay_register_sig() entries and dispatches the module's lifted
    functions under that image id. Image ids 200+ (ps3recomp's lift_jobmods.py convention).
  * Located job lifts (job_*_atLLLLL, kind "jobloc", pass 61): a second lift of a job module at the LS address
    another job loads it to (lift_spu.py LOCATED). Registered with spu_overlay_register_located(bytes, lsa, size,
    id) (patch 1078): a dispatch miss in that range uses it while LS holds the module's header and the word at pc.
    Ids after the job modules.
  * SPU ELFs (elf_*): SPURS tasks / raw SPU images are dispatched by spu_workload_dispatch*() with the
    fingerprint over spu_elf_image_size() bytes; also registered under the fingerprint of the executable
    PT_LOAD (raw SPU proxy-DMA loads, spu_raw.c), exactly like ps3recomp's build_spu_workloads.py.
    Image ids 30+ (clear of the ids the runtime special-cases: 1, 2, 22, 23, 60-65, 100, 200+).
Image id 1 stays unused on purpose: spu_indirect_branch retries PM misses inside 0x4000-0x3E000 against
image 1 (LBP's staged job), which must not resolve to an unrelated KZ3 image.
"""
import argparse, json, os, struct, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kz3ver  # noqa: E402  KZ3_EBOOT_VER=100|114 (image names below are v1.00 names, translated for 1.14)
from kz3ver import EBOOT, SPU_DIR, name as vname  # noqa: E402

PM_IMAGE_ID = {vname("pm_012af080"): 2}    # WWS job manager: id 2 is what the runtime's WWS hooks expect
ELF_IMAGE_BASE = 30
JOB_IMAGE_BASE = 200
PM_ENTRY = 0xA00
RESERVED = {1, 22, 23, 100} | set(range(60, 66))


def be16(b, o): return (b[o] << 8) | b[o + 1]
def be32(b, o): return struct.unpack_from(">I", b, o)[0]


def fnv1a64(b):
    """runtime/spu/spu_workload.c spu_workload_fingerprint (FNV-1a 64)."""
    h = 1469598103934665603
    for x in b:
        h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def elf_image_size(b):
    """runtime/spu/spu_workload.c spu_elf_image_size (same as build_spu_workloads.py img_size)."""
    e_phoff, e_shoff = be32(b, 0x1C), be32(b, 0x20)
    e_phentsize, e_phnum = be16(b, 0x2A), be16(b, 0x2C)
    e_shentsize, e_shnum = be16(b, 0x2E), be16(b, 0x30)
    end = e_shoff + e_shnum * e_shentsize
    for k in range(e_phnum):
        po = e_phoff + k * e_phentsize
        if po + 0x14 > len(b):
            break
        end = max(end, be32(b, po + 4) + be32(b, po + 0x10))
    for k in range(e_shnum):
        so = e_shoff + k * e_shentsize
        if so + 0x18 > len(b):
            break
        if be32(b, so + 4) != 8:
            end = max(end, be32(b, so + 0x10) + be32(b, so + 0x14))
    return min(end, len(b))


def text_fp(b):
    """Fingerprint of the executable PT_LOAD's file bytes (build_spu_workloads.py text_fp)."""
    phoff, phes, phn = be32(b, 0x1C), be16(b, 0x2A), be16(b, 0x2C)
    for i in range(phn):
        o = phoff + i * phes
        if be32(b, o) == 1 and be32(b, o + 0x18) & 1:
            off, fsz = be32(b, o + 4), be32(b, o + 0x10)
            return fnv1a64(b[off:off + fsz])
    return None


def has_symbol(lift, name, sym):
    h = os.path.join(lift, "lift", name, "spu_recomp.h")
    return os.path.exists(h) and f"{sym}(" in open(h).read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lift", default=kz3ver.SPU_OUT, help="lift_spu.py --out directory (default: this version's, kz3ver.SPU_OUT)")
    ap.add_argument("--out", default=None, help="default: <lift>/gen")
    ap.add_argument("--only", default="", help="comma list of image names to register (default: all)")
    a = ap.parse_args()
    kz3ver.check_lift(a.lift)
    a.out = a.out or os.path.join(a.lift, "gen")
    R = json.load(open(os.path.join(a.lift, "spu_lift.json")))
    bad = [r["name"] for r in R if not r["ok"]]
    if bad:
        sys.exit(f"lift failed for {bad}; rerun tools/spu/lift_spu.sh")
    eboot = open(EBOOT, "rb").read()
    images, problems = [], []

    pms = [r for r in R if r["kind"] == "pm"]
    elfs = sorted((r for r in R if r["kind"] == "elf"), key=lambda r: r["file_offset"])
    jobs = sorted((r for r in R if r["kind"] == "job"), key=lambda r: r["file_offset"])

    for r in pms:
        raw = eboot[r["file_offset"]:r["file_offset"] + r["size"]]
        sym = f"{r['name']}_spu_func_{PM_ENTRY:08X}"
        images.append(dict(name=r["name"], kind="pm", image_id=PM_IMAGE_ID[r["name"]], fp=fnv1a64(raw),
                           size=len(raw), ea=r["file_offset"] + 0x10000, entry=PM_ENTRY, entry_sym=sym))

    for i, r in enumerate(elfs):
        b = open(os.path.join(SPU_DIR, f"spu_{r['file_offset']:08x}.elf"), "rb").read()
        if b[:4] != b"\x7fELF" or be16(b, 0x12) != 0x17:
            problems.append(f"{r['name']}: not an SPU ELF")
            continue
        ent = be32(b, 0x18)
        sz = elf_image_size(b)
        images.append(dict(name=r["name"], kind="elf", image_id=ELF_IMAGE_BASE + i, fp=fnv1a64(b[:sz]),
                           size=sz, text_fp=text_fp(b), ea=r["file_offset"] + 0x10000, entry=ent,
                           entry_sym=f"{r['name']}_spu_func_{ent:08X}"))

    # Extra executable segments of an ELF (lift_spu.py kind "elfseg"): their functions go in under the parent
    # ELF's image id; no fingerprint of their own (the runtime dispatches the parent).
    elf_id = {m["name"]: m["image_id"] for m in images if m["kind"] == "elf"}
    for r in sorted((r for r in R if r["kind"] == "elfseg"), key=lambda r: r["name"]):
        if r["parent"] not in elf_id:
            problems.append(f"{r['name']}: parent {r['parent']} not registered")
            continue
        images.append(dict(name=r["name"], kind="elfseg", image_id=elf_id[r["parent"]], parent=r["parent"],
                           size=r["size"], ea=r["file_offset"] + 0x10000, entry=r["base"],
                           entry_sym=f"{r['name']}_spu_func_{r['base']:08X}"))

    for i, r in enumerate(jobs):
        raw = eboot[r["file_offset"]:r["file_offset"] + r["size"]]
        images.append(dict(name=r["name"], kind="job", image_id=JOB_IMAGE_BASE + i, sig=raw[:16].hex(),
                           size=len(raw), ea=r["file_offset"] + 0x10000, base=r["base"],
                           hdr_base=r["hdr_base"], flags=r["flags"], entry=r["base"] + r["entry_off"],
                           entry_sym=f"{r['name']}_spu_func_{r['base'] + r['entry_off']:08X}"))

    # Pass 61 (patch 1078): second lifts of job modules at the LS address another job loads them to (lift_spu.py
    # LOCATED, kind "jobloc"); ids after the job modules, registered by header + load address + size.
    for k, r in enumerate(sorted((r for r in R if r["kind"] == "jobloc"), key=lambda r: r["name"])):
        raw = eboot[r["file_offset"]:r["file_offset"] + r["size"]]
        images.append(dict(name=r["name"], kind="jobloc", image_id=JOB_IMAGE_BASE + len(jobs) + k, sig=raw[:16].hex(),
                           raw=raw, size=len(raw), ea=r["file_offset"] + 0x10000, base=r["base"], parent=r["parent"],
                           entry=r["base"] + r["entry_off"],
                           entry_sym=f"{r['name']}_spu_func_{r['base'] + r['entry_off']:08X}"))

    # Registration order: Edge zlib inflate task first (it gates the PSARC TOC reads at boot), then the WWS
    # job manager policy module, then the other ELFs and the job modules.
    first = {vname("elf_01000900"): 0, vname("pm_012af080"): 1}
    images.sort(key=lambda m: first.get(m["name"], 2))
    images.sort(key=lambda m: m["kind"] == "elfseg")  # stable: segments after every primary image
    if a.only:
        keep = set(a.only.split(","))
        images = [m for m in images if m["name"] in keep]

    # Checks: entry symbols exist, ids unique and not reserved, signatures and fingerprints unique.
    for m in images:
        if not has_symbol(a.lift, m["name"], m["entry_sym"]):
            problems.append(f"{m['name']}: entry symbol {m['entry_sym']} not in the lift")
    ids = [m["image_id"] for m in images if m["kind"] != "elfseg"]
    if len(set(ids)) != len(ids):
        problems.append("duplicate image ids")
    for m in images:
        if m["image_id"] in RESERVED:
            problems.append(f"{m['name']}: image id {m['image_id']} is special-cased by the runtime")
    sigs = {}
    for m in images:
        if m["kind"] == "job":
            sigs.setdefault(m["sig"], []).append(m["name"])
    dup_sigs = {k: v for k, v in sigs.items() if len(v) > 1}
    fps = {}
    for m in images:
        for k in ("fp", "text_fp"):
            if m.get(k) is not None:
                fps.setdefault(m[k], set()).add(m["name"])
    dup_fps = {f"{k:016X}": sorted(v) for k, v in fps.items() if len(v) > 1}
    if dup_sigs:
        problems.append(f"job signature collisions: {dup_sigs}")
    if dup_fps:
        problems.append(f"fingerprint collisions: {dup_fps}")
    if problems:
        sys.exit("\n".join(problems))

    L = ["/* kz3_spu_register.c -- GENERATED by tools/spu/gen_spu_register.py from the KZ3 SPU lift. Do not edit.",
         " * Registers every lifted KZ3 SPU image with ps3recomp's runtime: the SPURS policy module(s) and SPU ELFs",
         " * by FNV-1a-64 image fingerprint (spu_workload_register_img), the WWS job modules by their 16-byte",
         " * header signature (spu_overlay_register_sig). Each image's functions go in under its own image id",
         " * (spu_begin_image before its spu_recomp_register). */",
         "#include <stdint.h>",
         '#include "spu_workload.h"',
         '#include "kz3_spu_skip.h"   /* SPU pass 30: KZ3_SPU_SKIP_OVL / KZ3_CULL_AB name table */',
         "",
         "#ifndef SPU_ABI   /* patch 1099: lifted SPU functions' calling convention (spu_context.h) */",
         "#define SPU_ABI",
         "#endif",
         "extern void spu_begin_image(int image_id);",
         "extern void spu_overlay_register_sig(const uint8_t sig[16], int image_id);",
         "extern void spu_overlay_register_located(const uint8_t* code, uint32_t lsa, uint32_t span, int image_id);",
         ""]
    for m in images:   # patch 1078: the located lift's input bytes (the runtime checks LS against them before dispatch)
        if m["kind"] == "jobloc":
            raw = m.pop("raw")
            L.append(f"static const uint8_t kz3_loc_{m['image_id']}[{len(raw)}] = {{")
            L += ["    " + ",".join(f"0x{x:02X}" for x in raw[i:i + 24]) + "," for i in range(0, len(raw), 24)]
            L += ["};", ""]
    for m in images:
        L.append(f"extern void {m['name']}_spu_recomp_register(void);")
        if m["kind"] in ("pm", "elf"):
            L.append(f"extern SPU_ABI void {m['entry_sym']}(spu_context*);")
    L += ["", "unsigned kz3_spu_image_count = %du;" % sum(m["kind"] != "elfseg" for m in images), "",
          "void kz3_spu_register_all(void)", "{",
          "    static int done;", "    if (done) return;", "    done = 1;"]
    for m in images:
        tag = f"{m['kind']} EA 0x{m['ea']:08X}, {m['size']} bytes"
        L.append(f"    /* {m['name']}: {tag} */")
        L.append(f"    spu_begin_image({m['image_id']}); {m['name']}_spu_recomp_register();")
        if m["kind"] in ("pm", "elf"):
            L.append(f"    spu_workload_register_img(0x{m['fp']:016X}ULL, {m['entry_sym']}, {m['image_id']}, "
                     f"\"{m['name']}\");")
        if m.get("text_fp") is not None:
            L.append(f"    spu_workload_register_img(0x{m['text_fp']:016X}ULL, {m['entry_sym']}, "
                     f"{m['image_id']}, \"{m['name']}.text\");")
        if m["kind"] == "job":
            arr = ",".join(f"0x{x:02X}" for x in bytes.fromhex(m["sig"]))
            L.append(f"    {{ static const uint8_t sig[16] = {{{arr}}};")
            L.append(f"      spu_overlay_register_sig(sig, {m['image_id']}); }}")
        if m["kind"] == "jobloc":   # patch 1078: found by its bytes at its load address, not by a WWS list entry
            L.append(f"    spu_overlay_register_located(kz3_loc_{m['image_id']}, 0x{m['base']:05X}u, {m['size']}u, "
                     f"{m['image_id']});")
    L += ["    spu_begin_image(0);", "    kz3_spu_apply_skip_env();", "}", "",
          "/* Image names for port/spu/kz3_spu_skip.c (KZ3_SPU_SKIP_OVL=<name|id>, KZ3_CULL_AB=skipNN by job EA). */",
          "const kz3_spu_image_name kz3_spu_image_names[] = {"]
    L += [f'    {{"{m["name"]}", {m["image_id"]}, 0x{m["ea"]:08X}u}},' for m in images if m["kind"] != "elfseg"]
    L += ["};", "const unsigned kz3_spu_image_names_n = %du;" % sum(m["kind"] != "elfseg" for m in images), "",
          "/* Registers before main(), i.e. before the game adds any SPURS workload. The library is an OBJECT",
          " * library so this TU is always linked in. */",
          "__attribute__((constructor)) static void kz3_spu_register_ctor(void)", "{",
          "    kz3_spu_register_all();", "}", ""]
    os.makedirs(a.out, exist_ok=True)
    open(os.path.join(a.out, "kz3_spu_register.c"), "w").write("\n".join(L))
    for m in images:
        for k in ("fp", "text_fp"):
            if m.get(k) is not None:
                m[k] = f"0x{m[k]:016X}"
    json.dump(images, open(os.path.join(a.out, "kz3_spu_manifest.json"), "w"), indent=1)
    # Image names, one per line: port/spu/CMakeLists.txt compiles <KZ3_SPU_DIR>/lift/<name>/spu_recomp.c for each.
    open(os.path.join(a.out, "sources.txt"), "w").write("\n".join(m["name"] for m in images) + "\n")
    # The generator's own content hash: build-kz3.sh and kz3remote.py regenerate/refuse on a hash change, not on mtimes
    # (a lane rebase touches this file's mtime without changing it and tripped the guard, 2026-10-21).
    import hashlib
    open(os.path.join(a.out, ".generator-sha256"), "w").write(
        hashlib.sha256(open(os.path.abspath(__file__), "rb").read()).hexdigest() + "\n")
    n = {k: sum(1 for m in images if m["kind"] == k) for k in ("pm", "elf", "job", "jobloc", "elfseg")}
    print(f"registered {len(images) - n['elfseg']} images: {n['pm']} policy module(s), {n['elf']} ELFs, "
          f"{n['job']} job modules, {n['jobloc']} located job lift(s); {n['elfseg']} extra ELF code segment(s)")
    for m in images:
        if m["kind"] == "pm":
            print(f"  {m['name']}: EA 0x{m['ea']:08X} size {m['size']} fp {m['fp']} image id {m['image_id']}")
    print(f"wrote {a.out}/kz3_spu_register.c, kz3_spu_manifest.json, sources.txt")


if __name__ == "__main__":
    main()

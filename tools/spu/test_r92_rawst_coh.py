#!/usr/bin/env python3
"""Host test for KZ3 patch 1111 (SPU pass 92; from JonathanDC64/ps3recomp PR #38, adapted): the lifted PPU's raw
stores (stvx, stvebx/ehx/ewx, stvlx/stvrx, dcbz, stwbrx/sthbrx/stdbrx) must commit through ppu_st_coh -- vm_write*'s
locked, reservation-breaking commit -- on a line an SPU has reserved, and store the right bytes everywhere.

Lifts each mnemonic with the patched toolkit's ppu_lifter.py, compiles the emitted C with the lifter's raw-store
preamble block against stubs (one reserved line), runs it. No game data.
Usage: test_r92_rawst_coh.py PATCHED_DIR OUT_DIR   (exit 1 on a failed check)"""
import subprocess
import sys
from pathlib import Path

pt, od = Path(sys.argv[1]), Path(sys.argv[2])
od.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(pt / "tools"))
import ppu_lifter  # noqa: E402
from ppu_disasm import Instruction  # noqa: E402

pre = ppu_lifter.HEADER_PREAMBLE
a = pre.index("#define VM_RAW_LD")
b = pre.index("extern int      vm_inline_ok;")
block = pre[a:b]

L = ppu_lifter.PPULifter()


def lift(mn, ops):
    return L._translate_op(Instruction(0x10000, 0x7C000000, mn, ops), None)


# (name, mnemonic, operands, r3 = EA base, r4 = EA offset, expected bytes written: (offset in buf, values))
RES = 0x1000            # the reserved line
FREE = 0x2000           # a line no SPU holds
cases = []
for base, tag in ((RES, "reserved"), (FREE, "free")):
    cases += [
        (f"stvx {tag}", "stvx", "v2,r3,r4", base, 0x30, [(base + 0x30 + i, 0xA0 + i) for i in range(16)]),
        (f"stvewx {tag}", "stvewx", "v2,r3,r4", base, 0x38, [(base + 0x38 + i, 0xA8 + i) for i in range(4)]),
        (f"stvehx {tag}", "stvehx", "v2,r3,r4", base, 0x36, [(base + 0x36 + i, 0xA6 + i) for i in range(2)]),
        (f"stvebx {tag}", "stvebx", "v2,r3,r4", base, 0x35, [(base + 0x35, 0xA5)]),
        (f"stvlx {tag}", "stvlx", "v2,r3,r4", base, 0x2B, [(base + 0x2B + i, 0xA0 + i) for i in range(5)]),
        (f"stvrx {tag}", "stvrx", "v2,r3,r4", base, 0x2B, [(base + 0x20 + i, 0xA0 + 5 + i) for i in range(11)]),
        (f"stwbrx {tag}", "stwbrx", "r5,r3,r4", base, 0x40, [(base + 0x40 + i, v) for i, v in enumerate((0x44, 0x33, 0x22, 0x11))]),
        (f"sthbrx {tag}", "sthbrx", "r5,r3,r4", base, 0x48, [(base + 0x48, 0x44), (base + 0x49, 0x33)]),
        (f"stdbrx {tag}", "stdbrx", "r5,r3,r4", base, 0x50, [(base + 0x50 + i, 0x44 - 0x11 * i if i < 4 else 0x55 + 0x11 * (i - 4)) for i in range(8)]),
        (f"dcbz {tag}", "dcbz", "r3,r4", base, 0x44, [(base + i, 0) for i in range(128)]),
    ]

body = []
for i, (name, mn, ops, base, off, exp) in enumerate(cases):
    code = lift(mn, ops)
    chk = " && ".join(f"g_mem[0x{o:X}] == 0x{v:02X}" for o, v in exp)
    lo, hi = min(o for o, _ in exp), max(o for o, _ in exp) + 1
    coh = "1" if base == RES else "0"
    body.append(f"""
  {{ reset(); ctx->gpr[3] = 0x{base:X}; ctx->gpr[4] = 0x{off:X};
    {code}
    int ok_bytes = ({chk});
    int ok_rest = untouched(0x{lo:X}, 0x{hi:X});
    int ok_coh = {coh} ? (g_coh_n > 0 && g_coh_lo <= 0x{lo:X} && g_coh_hi >= 0x{hi:X} && !g_raw_on_reserved()) : 1;
    check("{name}", ok_bytes && ok_rest, ok_coh); }}""")

src = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static uint8_t g_mem[0x4000];
extern "C" { uint8_t* vm_base = g_mem; }
#ifndef PPU_ST_NOTE
#define PPU_ST_NOTE(ea, n) ((void)0)
#endif
extern "C" int spu_coh_is_reserved(uint32_t addr) { return (addr & ~127u) == 0x1000u; }
static int g_coh_n; static uint32_t g_coh_lo, g_coh_hi;
static uint8_t g_shadow[0x4000];   /* what the committed bytes should look like */
extern "C" void ppu_st_coh(uint32_t ea, const void* src, uint32_t n) {
  if (!g_coh_n || ea < g_coh_lo) g_coh_lo = ea;
  if (!g_coh_n || ea + n > g_coh_hi) g_coh_hi = ea + n;
  g_coh_n++; memcpy(g_mem + ea, src, n); memcpy(g_shadow + ea, src, n); }
/* A byte of the reserved line that changed without passing through ppu_st_coh. */
static int g_raw_on_reserved(void) { return memcmp(g_mem + 0x1000, g_shadow + 0x1000, 128) != 0; }
""" + "extern \"C\" {\n" + block + "}\n" + r"""
struct vr_t { uint8_t b[16]; } __attribute__((aligned(16)));
struct ctx_t { uint64_t gpr[32]; vr_t vr[32]; };
static ctx_t g_ctx; static ctx_t* ctx = &g_ctx;
static int g_fail, g_n;
static void reset(void) {
  memset(g_mem, 0x5A, sizeof g_mem); memcpy(g_shadow, g_mem, sizeof g_mem);
  g_coh_n = 0; g_coh_lo = g_coh_hi = 0;
  memset(&g_ctx, 0, sizeof g_ctx);
  for (int i = 0; i < 16; i++) g_ctx.vr[2].b[i] = (uint8_t)(0xA0 + i);
  g_ctx.gpr[5] = 0x8877665511223344ull;   /* st*brx: the low bytes, reversed */
}
static int untouched(uint32_t lo, uint32_t hi) {
  for (uint32_t i = 0; i < sizeof g_mem; i++) if ((i < lo || i >= hi) && g_mem[i] != 0x5A) return 0;
  return 1; }
static void check(const char* name, int bytes, int coh) {
  g_n++;
  if (bytes && coh) { printf("  ok   %s\n", name); return; }
  g_fail++; printf("  FAIL %s:%s%s\n", name, bytes ? "" : " wrong bytes", coh ? "" : " not committed through ppu_st_coh");
}
int main(void) {
""" + "".join(body) + r"""
  printf("[test_r92_rawst_coh] %d/%d passed\n", g_n - g_fail, g_n);
  return g_fail ? 1 : 0;
}
"""
(od / "t.cpp").write_text(src)
exe = od / "t"
subprocess.run(["g++", "-std=gnu++17", "-O1", "-w", "-o", str(exe), str(od / "t.cpp")], check=True)
sys.exit(subprocess.run([str(exe)]).returncode)

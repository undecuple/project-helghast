#!/usr/bin/env python3
"""Exercise shipped helper bindings and draw memo guards through a fake D3D12 list.

No game data/GPU. Requires the assembled SDK (KZ3_PATCHED_DIR overrides it).
Removing either helper's invalidation must break its draw/helper/draw sequence.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def between(text, start, end):
    first = text.index(start)
    return text[first:text.index(end, first)]


def harness(source):
    helpers = []
    for name, end in (("ld_downsample", "    const u32 c[5]"),
                      ("ld_scaled_blit", "    const float r[4]")):
        body = between(source, f"static int {name}(", "\n}\n")
        bindings = between(body, "    g.list->lpVtbl->OMSetRenderTargets", end)
        helpers.append(f"static void {name}(void) {{\n"
                       "ID3D12PipelineState* pso = &helper_pso;\n"
                       "D3D12_CPU_DESCRIPTOR_HANDLE rtv = {22};\n" + bindings + "}\n")
    memo = between(source, "static struct { u32 gen; ID3D12PipelineState* pso;", "static int ld_state_memo_on")
    draw = between(source, "static void sink_end_impl(", "\n}\n")
    generation = re.search(r"    if \(sm_on && g_ld_sm.gen != g_ld_sm_gen\)[^\n]+", draw).group()
    targets = between(draw, "        int same = sm_on &&", "    LDW_ADD(RSX_PERF_W_D_OMRT_T")
    heaps = between(draw, "    ID3D12DescriptorHeap* heaps[]", "    LDW_T0(srvtbl_t0)")
    pipeline = between(draw, "    if (!sm_on || g_ld_sm.pso != pso)",
                       "    g.list->lpVtbl->SetGraphicsRootConstantBufferView")
    normal = "static void normal_draw(int sm_on) {\n" + generation + "\n{\n" + """
    const u32 n_mrt = 1;
    const int have_dsv = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {{11}}, dsv = {0};
""" + targets + heaps + "ID3D12PipelineState* pso = &normal_pso;\n" + pipeline + "}\n"
    return PREAMBLE + memo + "\n" + "\n".join(helpers) + normal + MAIN


PREAMBLE = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uintptr_t SIZE_T;
typedef struct { int id; } ID3D12PipelineState;
typedef struct { int id; } ID3D12RootSignature;
typedef struct { int id; } ID3D12DescriptorHeap;
typedef struct { SIZE_T ptr; } D3D12_CPU_DESCRIPTOR_HANDLE;
typedef struct List List;
struct Vtable {
    void (*OMSetRenderTargets)(List*, u32, const D3D12_CPU_DESCRIPTOR_HANDLE*, int,
                               const D3D12_CPU_DESCRIPTOR_HANDLE*);
    void (*SetDescriptorHeaps)(List*, u32, ID3D12DescriptorHeap* const*);
    void (*SetPipelineState)(List*, ID3D12PipelineState*);
    void (*SetGraphicsRootSignature)(List*, ID3D12RootSignature*);
};
struct List { const struct Vtable* lpVtbl; };
static ID3D12PipelineState normal_pso = {1}, helper_pso = {2};
static ID3D12RootSignature normal_root = {1}, downsample_root = {2}, scaled_root = {3};
static ID3D12DescriptorHeap heap0 = {1}, heap1 = {2};
static struct {
    ID3D12PipelineState* pso;
    ID3D12RootSignature* root;
    SIZE_T rtv;
    ID3D12DescriptorHeap* heap[2];
    unsigned calls;
} device;
static void targets(List* l, u32 n, const D3D12_CPU_DESCRIPTOR_HANDLE* r, int single,
                    const D3D12_CPU_DESCRIPTOR_HANDLE* d) {
    (void)l; (void)single; (void)d;
    if (n == 1) device.rtv = r[0].ptr;
    device.calls++;
}
static void heaps(List* l, u32 n, ID3D12DescriptorHeap* const* h) {
    (void)l;
    if (n == 2) { device.heap[0] = h[0]; device.heap[1] = h[1]; }
    device.calls++;
}
static void pipeline(List* l, ID3D12PipelineState* p) { (void)l; device.pso = p; device.calls++; }
static void signature(List* l, ID3D12RootSignature* r) { (void)l; device.root = r; device.calls++; }
static const struct Vtable vtable = {targets, heaps, pipeline, signature};
static List list = {&vtable};
static struct {
    List* list;
    ID3D12DescriptorHeap* srv_heap;
    ID3D12DescriptorHeap* smp_heap;
    ID3D12RootSignature* rootsig_x;
    ID3D12RootSignature* downsample_rootsig;
} g = {&list, &heap0, &heap1, &normal_root, &downsample_root};
static ID3D12RootSignature* g_sblit_rs = &scaled_root;
#define FALSE 0
#define RSX_PERF_W_D_SM_HIT_N 0
static int rsx_perf_walk_on(void) { return 0; }
static void rsx_perf_add(int i, int n) { (void)i; (void)n; }
"""

MAIN = r"""
static int normal_state(void) {
    return device.pso == &normal_pso && device.root == &normal_root && device.rtv == 11 &&
           device.heap[0] == &heap0 && device.heap[1] == &heap1;
}
int main(void) {
    void (*helper[])(void) = {ld_downsample, ld_scaled_blit};
    const char* name[] = {"ld_downsample", "ld_scaled_blit"};
    for (int memo = 0; memo <= 1; memo++) for (int h = 0; h < 2; h++) {
        memset(&g_ld_sm, 0, sizeof g_ld_sm);
        memset(&device, 0, sizeof device);
        g_ld_sm_gen = 1;
        normal_draw(memo);
        if (!normal_state()) return 2;
        unsigned before = device.calls;
        normal_draw(memo);
        if (memo && device.calls != before) return 3; /* identical normal draws still memoize */
        for (int repeat = 0; repeat < 3; repeat++) {
            helper[h]();
            if (device.pso != &helper_pso || device.rtv != 22 || device.root == &normal_root) return 4;
            normal_draw(memo);
            if (!normal_state()) {
                fprintf(stderr, "FAIL %s memo=%d: ordinary draw did not restore bindings\n", name[h], memo);
                return 1;
            }
        }
        printf("PASS %s memo=%d: bindings restored\n", name[h], memo);
    }
    return 0;
}
"""


def main():
    sdk = Path(os.environ.get("KZ3_PATCHED_DIR", ROOT / "work/ps3recomp-patched"))
    source = (sdk / "libs/video/rsx_live_draw.c").read_text()
    output = ROOT / "build/smoke-host"
    output.mkdir(parents=True, exist_ok=True)
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="blit-memo-", dir=output) as tmp:
        cfile, exe = Path(tmp) / "test.c", Path(tmp) / "test"

        def run(text):
            cfile.write_text(harness(text))
            subprocess.run([*compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                            str(cfile), "-o", str(exe)], check=True)
            return subprocess.run([str(exe)], capture_output=True, text=True)

        result = run(source)
        print(result.stdout, end="")
        if result.returncode:
            raise SystemExit(result.stderr or f"FAIL: harness exit {result.returncode}")
        for name in ("ld_downsample", "ld_scaled_blit"):
            body = between(source, f"static int {name}(", "\n}\n")
            mutated = re.sub(r"LD_SM_INVALIDATE\(\);", "(void)0;", body)
            if mutated == body or run(source.replace(body, mutated, 1)).returncode != 1:
                raise SystemExit(f"FAIL: removing {name}'s invalidation was not detected")
            print(f"PASS mutation: missing {name} invalidation detected")


if __name__ == "__main__":
    main()

/*
 * Host test for KZ3 patch 2183 (RSX pass 161): the LD_CEIL ceiling-probe switch.
 * Part 1: the pure parser rsx_ceil_parse (tokens, separators, unknown tokens, no substring matches).
 * Part 2 (argv[1] = "convert" or "skip"): rsx_vertex_fetch_many, the one place LD_CEIL=vtx acts, run in a process
 * whose LD_CEIL the shell script sets: "convert" = the output equals rsx_vertex_fetch_one byte for byte and the
 * sentinel is gone; "skip" = return 1 and the output buffer is untouched (sentinel intact).
 * No D3D12, no game data. Built by tools/smoke/test_ceil.sh (Linux cc).
 */
#define _POSIX_C_SOURCE 200809L
#include "rsx_vertex_compact.h"
#include "rsx_vertex_formats.h"
#include "rsx_ceil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u32 rsx_dsp_vertex_data_base_offset(const rsx_dispatch* rsx) { (void)rsx; return 0; }
void rsx_dsp_get_vertex_attr(const rsx_dispatch* rsx, u32 index, rsx_dsp_vertex_attr* out) { (void)rsx; (void)index; memset(out, 0, sizeof(*out)); }
void rsx_dsp_vertex_default(const rsx_dispatch* rsx, u32 attr, float out[4]) { (void)rsx; (void)attr; out[0] = out[1] = out[2] = 0; out[3] = 1; }

static int g_fail;
#define CHECK(cond, ...) do { if (cond) printf("[PASS] " __VA_ARGS__); else { printf("[FAIL] " __VA_ARGS__); g_fail = 1; } printf("\n"); } while (0)

static u8 g_src[4096];
static const u8* guest_ptr(void* user, u32 location, u32 offset, u32 min_bytes)
{
    (void)user;
    if (location != 0 || (u64)offset + min_bytes > sizeof g_src) return NULL;
    return g_src + offset;
}

static void parser(void)
{
    unsigned bad = 99;
    CHECK(rsx_ceil_parse(NULL, &bad) == 0 && bad == 0, "NULL -> 0");
    CHECK(rsx_ceil_parse("", &bad) == 0 && bad == 0, "empty -> 0");
    CHECK(rsx_ceil_parse("vtx", &bad) == RSX_CEIL_VTX && bad == 0, "vtx");
    CHECK(rsx_ceil_parse("texhash", &bad) == RSX_CEIL_TEXHASH && bad == 0, "texhash");
    CHECK(rsx_ceil_parse("wb", &bad) == RSX_CEIL_WB && bad == 0, "wb");
    CHECK(rsx_ceil_parse("rb", &bad) == RSX_CEIL_RB && bad == 0, "rb");
    CHECK(rsx_ceil_parse("vtx,texhash,wb,rb", &bad) == 15u && bad == 0, "all four");
    CHECK(rsx_ceil_parse(" wb ,, rb ", &bad) == (RSX_CEIL_WB | RSX_CEIL_RB) && bad == 0, "spaces and empty items");
    CHECK(rsx_ceil_parse("vtx vtx,vtx", &bad) == RSX_CEIL_VTX && bad == 0, "duplicates");
    CHECK(rsx_ceil_parse("vtxx", &bad) == 0 && bad == 1, "no prefix match (vtxx)");
    CHECK(rsx_ceil_parse("tex", &bad) == 0 && bad == 1, "no prefix match (tex)");
    CHECK(rsx_ceil_parse("VTX", &bad) == 0 && bad == 1, "case sensitive");
    CHECK(rsx_ceil_parse("vtx,bogus,rb", &bad) == (RSX_CEIL_VTX | RSX_CEIL_RB) && bad == 1, "unknown token ignored, rest kept");
    CHECK(rsx_ceil_parse("wb", NULL) == RSX_CEIL_WB, "bad == NULL allowed");
}

static void fetch(int expect_skip)
{
    for (u32 i = 0; i < sizeof g_src; i++) g_src[i] = (u8)(i * 2654435761u >> 13);
    rsx_vertex_fetch_plan plan;
    memset(&plan, 0, sizeof plan);
    rsx_vertex_layout_plan_init(&plan.layout, 0x1u);   /* attribute 0 only */
    plan.guest_ptr = guest_ptr;
    rsx_vertex_fetch_attr* f = &plan.attr[0];
    f->desc.type = RSX_VTX_TYPE_FLOAT; f->desc.size = 3; f->desc.stride = 12; f->desc.offset = 0; f->desc.location = 0;
    f->elem_size = rsx_vertex_attrib_size(RSX_VTX_TYPE_FLOAT, 3);
    f->stride = 12;
    enum { N = 64 };
    rsx_vertex_ref refs[N];
    for (u32 i = 0; i < N; i++) { refs[i].vertex_id = i; refs[i].base_index = 0; }
    const u32 stride = plan.layout.stride;
    u8 want[N * 16 + 16], got[N * 16 + 16];
    memset(want, 0xA5, sizeof want); memset(got, 0xA5, sizeof got);
    int ok1 = 1;
    for (u32 i = 0; i < N && ok1; i++) ok1 = rsx_vertex_fetch_one(&plan, &refs[i], want + (u64)i * stride);
    const int rc = rsx_vertex_fetch_many(&plan, refs, N, got);
    CHECK(ok1 && rc == 1, "fetch_one ok and fetch_many returns 1 (LD_CEIL=%s)", getenv("LD_CEIL") ? getenv("LD_CEIL") : "unset");
    int sentinel = 1;
    for (u32 i = 0; i < sizeof got; i++) if (got[i] != 0xA5) { sentinel = 0; break; }
    if (expect_skip) CHECK(sentinel, "skip: output buffer untouched");
    else CHECK(!sentinel && !memcmp(want, got, sizeof got), "convert: output equals fetch_one byte for byte");
}

int main(int argc, char** argv)
{
    if (argc >= 2) { fetch(!strcmp(argv[1], "skip")); return g_fail; }
    parser();
    return g_fail;
}

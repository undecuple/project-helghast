/* Host test for KZ3 patch 2006: the FP16 blend constant. R/G come from SET_BLEND_COLOR (0x031C), B/A from
 * SET_BLEND_COLOR2 (0x037C; 0x0368 is SET_SHADE_MODE). The code under test (M_BLEND_COLOR* defines, ld_half and
 * ld_set_blend_factor) is cut verbatim out of the patched libs/video/rsx_live_draw.c by test_blend_color2.sh into
 * blend_extract.inc; only the register file and the D3D12 command list are stubbed here. No game data, no GPU. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef uint32_t u32;

static u32 regs[0x2000];
typedef struct { int n; float f[4]; } blend_rec;
static blend_rec rec;
struct vt { void (*OMSetBlendFactor)(void* l, const float* f); };
static void om_set_blend_factor(void* l, const float* f) { (void)l; rec.n++; memcpy(rec.f, f, sizeof rec.f); }
static struct vt vtbl = { om_set_blend_factor };
struct list { struct vt* lpVtbl; };
static struct list the_list = { &vtbl };
static struct { struct list* list; struct { int dummy; } rsx; } g = { &the_list, { 0 } };
static u32 rsx_dsp_reg(const void* r, u32 off) { (void)r; return regs[off >> 2]; }

#include "blend_extract.inc"

static int fails;
static void expect(const char* what, int ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

int main(void)
{
    /* half bit patterns: 1.0 = 0x3C00, 0.5 = 0x3800, 2.0 = 0x4000, 0.25 = 0x3400 */
    memset(regs, 0, sizeof regs);
    regs[0x031C >> 2] = 0x3800u | (0x3C00u << 16);   /* R=0.5  G=1.0  */
    regs[0x037C >> 2] = 0x4000u | (0x3400u << 16);   /* BLEND_COLOR2: B=2.0  A=0.25 */
    regs[0x0368 >> 2] = 0x3C00u | (0x3C00u << 16);   /* SHADE_MODE: decoy, B=A=1.0 */
    rec.n = 0; ld_set_blend_factor(1);
    expect("fp16 set the blend factor once", rec.n == 1);
    expect("fp16 R from 0x031C low half", rec.f[0] == 0.5f);
    expect("fp16 G from 0x031C high half", rec.f[1] == 1.0f);
    expect("fp16 B from 0x037C low half (not the shade-mode word 0x0368)", rec.f[2] == 2.0f);
    expect("fp16 A from 0x037C high half (not the shade-mode word 0x0368)", rec.f[3] == 0.25f);
    expect("M_BLEND_COLOR2 is 0x037C", M_BLEND_COLOR2 == 0x037Cu);

    /* 8-bit surfaces ignore the second register entirely */
    memset(regs, 0, sizeof regs);
    regs[0x031C >> 2] = 0xFF000000u | (0x80u << 16) | (0x40u << 8) | 0x20u;
    regs[0x037C >> 2] = 0x12345678u;
    regs[0x0368 >> 2] = 0x9ABCDEF0u;
    rec.n = 0; ld_set_blend_factor(0);
    expect("a8r8g8b8 uses only 0x031C (A=1, R=0x80/255)", rec.f[3] == 1.0f && rec.f[0] == 128.0f / 255.0f
           && rec.f[1] == 64.0f / 255.0f && rec.f[2] == 32.0f / 255.0f);
    return fails ? 1 : 0;
}

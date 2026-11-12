/* The Python driver appends source-extracted ownership helpers and selection
 * snippets before test_provenance_cases.inc. No D3D12, assets or game process. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "rsx_ui_provenance.h"
#include "rsx_present_geometry.h"
#include "rsx_hud169.h"
#include "rsx_display_mode.h"
typedef unsigned u32;
#define MAX_SURFACES 4
typedef struct {
    rsx_ui_provenance ui;
    unsigned w, h;
    unsigned swin_wgen;
    unsigned last_write_generation, last_write_kind, last_create_generation, last_draw_generation;
    unsigned last_clear_generation, last_copy_generation, last_blit_generation, last_resolve_generation,
        last_other_generation;
} surface_t;
static struct {
    surface_t surfaces[MAX_SURFACES];
    rsx_ui_provenance movie_overlay_ui;
    int movie_overlay_valid;
    unsigned width, height;
} g;
static unsigned g_ld_frames = 10, g_ld_surface_generation;
static unsigned g_dm_bb_w = 2560, g_dm_bb_h = 1080;
static int ld_h169_on(void)
{
    return 1;
}
static int rsx_display_get_aspect(void)
{
    return RSX_DM_ASPECT_SCREEN;
}
static int ld_dm_scaling_now(void)
{
    return RSX_DM_STRETCH;
}
static void ld_gv_note_gpu_write(u32 target)
{
    (void)target;
}

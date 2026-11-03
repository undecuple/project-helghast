/* Host test of the upscaler interface (KZ3 patch 2145): backend selection, size and quality math, the off path (no
 * pass), FSR 1 constants against the formulas of ffx_fsr1.h, the pre-HUD tap, and the embedded shader text against the
 * vendored headers. A fake backend counts calls; no D3D12, no GPU, no game data. argv[1] = libs/video/fsr1 dir. */
#define _POSIX_C_SOURCE 200112L
#include "rsx_upscale.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (c) printf("ok   %s\n", #c); else { printf("FAIL %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

static int g_create, g_destroy, g_eval;
static void* fk_create(void* dev, const rsx_upscale_config* cfg) { (void)cfg; g_create++; return dev ? dev : (void*)&g_create; }
static void fk_destroy(void* be, void (*r)(void*, void*), void* u) { (void)be; (void)r; (void)u; g_destroy++; }
static int fk_eval(void* be, const rsx_upscale_eval* e, float sh, rsx_upscale_result* res, void (*r)(void*, void*), void* u)
{
    (void)be; (void)sh; (void)r; (void)u;
    g_eval++;
    res->resource = (void*)e->input.resource;
    res->width = e->output_width; res->height = e->output_height;
    return 1;
}
static const rsx_upscale_ops fake = { fk_create, fk_destroy, fk_eval };

static float bits_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static int near(float a, float b) { return fabsf(a - b) <= 1e-6f * (fabsf(b) + 1.0f); }

static char* slurp(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* b = (char*)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}

int main(int argc, char** argv)
{
    /* backend names and parsing */
    CHECK(rsx_upscale_parse_backend(NULL) == RSX_UPSCALE_NONE);
    CHECK(rsx_upscale_parse_backend("") == RSX_UPSCALE_NONE);
    CHECK(rsx_upscale_parse_backend("fsr1") == RSX_UPSCALE_FSR1);
    CHECK(rsx_upscale_parse_backend("FSR1") == RSX_UPSCALE_FSR1);
    CHECK(rsx_upscale_parse_backend("fsr") == RSX_UPSCALE_FSR1);
    CHECK(rsx_upscale_parse_backend("dlss") == RSX_UPSCALE_DLSS);
    CHECK(rsx_upscale_parse_backend("xess") == RSX_UPSCALE_XESS);
    CHECK(rsx_upscale_parse_backend("off") == RSX_UPSCALE_NONE);
    CHECK(rsx_upscale_parse_backend("bogus") == RSX_UPSCALE_NONE);
    for (int i = 0; i < RSX_UPSCALE_BACKEND_COUNT; i++)
        CHECK(rsx_upscale_parse_backend(rsx_upscale_backend_name((rsx_upscale_backend)i)) == (rsx_upscale_backend)i);
    CHECK(rsx_upscale_backend_implemented(RSX_UPSCALE_NONE) && rsx_upscale_backend_implemented(RSX_UPSCALE_FSR1));
    CHECK(!rsx_upscale_backend_implemented(RSX_UPSCALE_DLSS) && !rsx_upscale_backend_implemented(RSX_UPSCALE_FSR3));

    /* environment */
    rsx_upscale_config c;
    unsetenv("RSX_UPSCALE"); unsetenv("RSX_UPSCALE_SHARPNESS");
    rsx_upscale_config_from_env(&c);
    CHECK(c.backend == RSX_UPSCALE_NONE && near(c.sharpness, 0.2f));
    setenv("RSX_UPSCALE", "fsr1", 1);
    rsx_upscale_config_from_env(&c);
    CHECK(c.backend == RSX_UPSCALE_FSR1 && near(c.sharpness, 0.2f));
    setenv("RSX_UPSCALE_SHARPNESS", "1.5", 1);
    rsx_upscale_config_from_env(&c);
    CHECK(near(c.sharpness, 1.5f));
    setenv("RSX_UPSCALE_SHARPNESS", "9", 1);
    rsx_upscale_config_from_env(&c);
    CHECK(near(c.sharpness, 2.0f));
    setenv("RSX_UPSCALE_SHARPNESS", "-3", 1);
    rsx_upscale_config_from_env(&c);
    CHECK(near(c.sharpness, 0.0f));
    setenv("RSX_UPSCALE_SHARPNESS", "x", 1);
    rsx_upscale_config_from_env(&c);
    CHECK(near(c.sharpness, 0.2f));
    setenv("RSX_UPSCALE", "dlss", 1);                     /* reserved: falls back to none */
    rsx_upscale_config_from_env(&c);
    CHECK(c.backend == RSX_UPSCALE_NONE);
    CHECK(near(rsx_upscale_clamp_sharpness(NAN), 0.2f));

    /* quality and render size */
    CHECK(near(rsx_upscale_quality_ratio(RSX_UPSCALE_Q_NATIVE), 1.0f));
    CHECK(near(rsx_upscale_quality_ratio(RSX_UPSCALE_Q_ULTRA_QUALITY), 1.3f));
    CHECK(near(rsx_upscale_quality_ratio(RSX_UPSCALE_Q_QUALITY), 1.5f));
    CHECK(near(rsx_upscale_quality_ratio(RSX_UPSCALE_Q_BALANCED), 1.7f));
    CHECK(near(rsx_upscale_quality_ratio(RSX_UPSCALE_Q_PERFORMANCE), 2.0f));
    CHECK(rsx_upscale_parse_quality("balanced") == RSX_UPSCALE_Q_BALANCED && rsx_upscale_parse_quality("x") == -1);
    uint32_t rw, rh;
    rsx_upscale_render_size(RSX_UPSCALE_Q_QUALITY, 1920, 1080, &rw, &rh);       CHECK(rw == 1280 && rh == 720);
    rsx_upscale_render_size(RSX_UPSCALE_Q_PERFORMANCE, 1920, 1080, &rw, &rh);   CHECK(rw == 960 && rh == 540);
    rsx_upscale_render_size(RSX_UPSCALE_Q_PERFORMANCE, 3840, 2160, &rw, &rh);   CHECK(rw == 1920 && rh == 1080);
    rsx_upscale_render_size(RSX_UPSCALE_Q_ULTRA_QUALITY, 1920, 1080, &rw, &rh); CHECK(rw == 1476 && rh == 830);
    rsx_upscale_render_size(RSX_UPSCALE_Q_NATIVE, 2560, 1080, &rw, &rh);        CHECK(rw == 2560 && rh == 1080);
    rsx_upscale_render_size(RSX_UPSCALE_Q_PERFORMANCE, 1, 1, &rw, &rh);         CHECK(rw == 1 && rh == 1);

    /* off: no context, no pass, nothing called */
    rsx_upscale_set_test_ops(&fake);
    rsx_upscale_config off = { RSX_UPSCALE_NONE, 0.2f };
    CHECK(rsx_upscale_create(&off, (void*)1) == NULL && g_create == 0);
    CHECK(rsx_upscale_create(NULL, (void*)1) == NULL);
    rsx_upscale_config dl = { RSX_UPSCALE_DLSS, 0.2f };
    CHECK(rsx_upscale_create(&dl, (void*)1) == NULL && g_create == 0);
    rsx_upscale_eval ev; rsx_upscale_result res;
    memset(&ev, 0, sizeof ev);
    ev.cmd_list = (void*)1; ev.input.resource = (void*)2; ev.input.width = 1280; ev.input.height = 720;
    ev.output_width = 1920; ev.output_height = 1080; ev.sharpness = -1.0f;
    CHECK(rsx_upscale_evaluate(NULL, &ev, &res) == 0 && g_eval == 0);
    CHECK(rsx_upscale_pre_hud(NULL, &ev, &res) == 0 && rsx_upscale_tapped(NULL) == 0 && rsx_upscale_passes(NULL) == 0);
    CHECK(rsx_upscale_wants_pass(NULL, 1280, 720, 1920, 1080) == 0 && rsx_upscale_get_backend(NULL) == RSX_UPSCALE_NONE);
    rsx_upscale_destroy(NULL); rsx_upscale_frame_end(NULL); rsx_upscale_set_retire(NULL, NULL, NULL);

    /* FSR 1 through the fake backend: which size pairs run */
    rsx_upscale_config on = { RSX_UPSCALE_FSR1, 0.2f };
    rsx_upscale* u = rsx_upscale_create(&on, (void*)7);
    CHECK(u != NULL && g_create == 1 && rsx_upscale_get_backend(u) == RSX_UPSCALE_FSR1);
    CHECK(rsx_upscale_wants_pass(u, 1280, 720, 1920, 1080) == 1);
    CHECK(rsx_upscale_wants_pass(u, 1280, 720, 1280, 720) == 0);        /* equal: FSR 1 only enlarges */
    CHECK(rsx_upscale_wants_pass(u, 1280, 720, 960, 540) == 0);         /* smaller */
    CHECK(rsx_upscale_wants_pass(u, 1280, 720, 1920, 720) == 1);        /* one axis larger */
    CHECK(rsx_upscale_wants_pass(u, 1280, 720, 1920, 540) == 0);        /* larger in one, smaller in the other */
    CHECK(rsx_upscale_wants_pass(u, 0, 720, 1920, 1080) == 0 && rsx_upscale_wants_pass(u, 1280, 720, 0, 0) == 0);
    ev.output_width = ev.input.width; ev.output_height = ev.input.height;
    CHECK(rsx_upscale_evaluate(u, &ev, &res) == 0 && g_eval == 0);      /* off for this size: backend not called */
    ev.output_width = 1920; ev.output_height = 1080;
    CHECK(rsx_upscale_evaluate(u, &ev, &res) == 1 && g_eval == 1 && res.width == 1920 && res.height == 1080);
    CHECK(rsx_upscale_passes(u) == 1);
    ev.cmd_list = NULL;
    CHECK(rsx_upscale_evaluate(u, &ev, &res) == 0 && g_eval == 1);      /* no command list: nothing recorded */
    ev.cmd_list = (void*)1;
    /* pre-HUD tap: the present step then runs no second pass this frame */
    CHECK(rsx_upscale_tapped(u) == 0);
    CHECK(rsx_upscale_pre_hud(u, &ev, &res) == 1 && rsx_upscale_tapped(u) == 1 && g_eval == 2);
    CHECK(rsx_upscale_wants_pass(u, 1280, 720, 1920, 1080) == 0);
    CHECK(rsx_upscale_evaluate(u, &ev, &res) == 0 && g_eval == 2);
    rsx_upscale_frame_end(u);
    CHECK(rsx_upscale_tapped(u) == 0 && rsx_upscale_wants_pass(u, 1280, 720, 1920, 1080) == 1);
    rsx_upscale_destroy(u);
    CHECK(g_destroy == 1);
    rsx_upscale_set_test_ops(NULL);

    /* FSR 1 constants: the formulas of FsrEasuCon / FsrRcasCon, spelled out here */
    uint32_t easu[16], rcas[4];
    rsx_upscale_fsr1_constants(1280, 720, 1920, 1080, 0.2f, easu, rcas);
    CHECK(near(bits_f(easu[0]), 1280.0f / 1920.0f) && near(bits_f(easu[1]), 720.0f / 1080.0f));
    CHECK(near(bits_f(easu[2]), 0.5f * 1280.0f / 1920.0f - 0.5f) && near(bits_f(easu[3]), 0.5f * 720.0f / 1080.0f - 0.5f));
    CHECK(near(bits_f(easu[4]), 1.0f / 1280.0f) && near(bits_f(easu[5]), 1.0f / 720.0f));
    CHECK(near(bits_f(easu[6]), 1.0f / 1280.0f) && near(bits_f(easu[7]), -1.0f / 720.0f));
    CHECK(near(bits_f(easu[8]), -1.0f / 1280.0f) && near(bits_f(easu[9]), 2.0f / 720.0f));
    CHECK(near(bits_f(easu[10]), 1.0f / 1280.0f) && near(bits_f(easu[11]), 2.0f / 720.0f));
    CHECK(near(bits_f(easu[12]), 0.0f) && near(bits_f(easu[13]), 4.0f / 720.0f) && easu[14] == 0 && easu[15] == 0);
    CHECK(near(bits_f(rcas[0]), powf(2.0f, -0.2f)) && rcas[2] == 0 && rcas[3] == 0);
    CHECK((rcas[1] & 0xffffu) == (rcas[1] >> 16) && (rcas[1] & 0xffffu) != 0);   /* packed half pair of the same value */
    rsx_upscale_fsr1_constants(1280, 720, 1920, 1080, 0.0f, easu, rcas);
    CHECK(near(bits_f(rcas[0]), 1.0f));                                          /* 0 stops = full strength */
    rsx_upscale_fsr1_constants(1280, 720, 1920, 1080, 2.0f, easu, rcas);
    CHECK(near(bits_f(rcas[0]), 0.25f));

    /* the HLSL: assembled from the embedded headers, which must equal the vendored files */
    size_t n = 0;
    char* src = rsx_upscale_fsr1_hlsl_source(&n);
    CHECK(src != NULL && n == strlen(src) && n > 100000);
    CHECK(src && strncmp(src, "cbuffer cb", 10) == 0 && strstr(src, "mainCS") && strstr(src, "FsrEasuF(c, pos") && strstr(src, "FsrRcasF(c.r"));
    CHECK(src && strstr(src, "Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved."));
    CHECK(src && strstr(src, "Copyright (c) 2014 Michal Drobot"));
    if (argc > 1) {
        const char* names[2] = { "ffx_a.h", "ffx_fsr1.h" };
        for (int i = 0; i < 2 && src; i++) {
            char path[1024]; snprintf(path, sizeof path, "%s/%s", argv[1], names[i]);
            char* f = slurp(path);
            if (!f) { printf("FAIL cannot read %s\n", path); fails++; continue; }
            printf("%s: ", names[i]);
            CHECK(strstr(src, f) != NULL);          /* the whole vendored file, byte for byte, is inside the source */
            free(f);
        }
    } else printf("note: no fsr1 dir given, embed-vs-file check skipped\n");
    free(src);

    printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}

/* Host test for KZ3 patch 2202: libs/video/rsx_views.h (RSX_VIEWS=1 split-screen per-view counter). Synthetic command
 * streams with 1, 2 and 3 views, built the way the walker feeds the core: method words, then a draw packet with the
 * surface offsets, target size, raw scissor registers and enabled texture units. Built and mutated by test_views.sh. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rsx_views.h"

static int fails;
#define CHECK(name, cond)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s (line %d)\n", name, __LINE__);                                                             \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

#define SC(x, y, w, h) ((uint32_t)(x) | ((uint32_t)(w) << 16)), ((uint32_t)(y) | ((uint32_t)(h) << 16))

/* n draw packets, `words` method words before each */
static void burst(rsx_views* v, int n, uint32_t col, uint32_t zeta, uint32_t tw, uint32_t th, uint32_t sh, uint32_t sv,
                  int written, uint32_t binds, int words)
{
    for (int i = 0; i < n; i++) {
        for (int w = 0; w < words; w++) rsx_views_word(v);
        rsx_views_draw(v, col, zeta, tw, th, sh, sv, written, binds);
    }
}

/* the geometry of a frame: `views` main-target views (1 = unscissored, 2 = split halves, 3 = columns), plus the passes
 * that must not count: a shadow target, a half-resolution post target, small light-volume scissors, a UI scissor */
static void frame(rsx_views* v, int views)
{
    burst(v, 20, 0x100000, 0x200000, 1024, 1024, SC(0, 0, 1024, 1024), 1, 3, 2); /* shadow map */
    if (views == 1) {
        burst(v, 50, 0x300000, 0x400000, 1280, 720, 0, 0, 0, 4, 3); /* scissor never written = whole target */
    } else if (views == 2) {
        burst(v, 30, 0x300000, 0x400000, 1280, 720, SC(0, 0, 640, 560), 1, 4, 3);
        burst(v, 28, 0x300000, 0x400000, 1280, 720, SC(640, 160, 640, 560), 1, 5, 2);
        /* the two uncovered bands (the HUD fills the clearports): disjoint from both views, 11 % of the target each */
        burst(v, 10, 0x300000, 0x400000, 1280, 720, SC(640, 0, 640, 160), 1, 1, 1);
        burst(v, 9, 0x300000, 0x400000, 1280, 720, SC(0, 560, 640, 160), 1, 1, 1);
    } else {
        burst(v, 20, 0x300000, 0x400000, 1280, 720, SC(0, 0, 426, 720), 1, 4, 3);
        burst(v, 19, 0x300000, 0x400000, 1280, 720, SC(427, 0, 426, 720), 1, 4, 3);
        burst(v, 18, 0x300000, 0x400000, 1280, 720, SC(854, 0, 426, 720), 1, 4, 3);
    }
    /* light volumes: three disjoint small scissors with >= min_draws each (would be views without the area rule) */
    burst(v, 10, 0x500000, 0x400000, 1280, 720, SC(100, 100, 120, 100), 1, 6, 1);
    burst(v, 10, 0x500000, 0x400000, 1280, 720, SC(300, 100, 120, 100), 1, 6, 1);
    burst(v, 10, 0x500000, 0x400000, 1280, 720, SC(500, 400, 120, 100), 1, 6, 1);
    burst(v, 9, 0x600000, 0, 640, 360, SC(0, 0, 640, 360), 1, 2, 0); /* half-resolution post target */
    /* a slightly smaller full-screen scissor on the main target (overlaps the view rects) */
    burst(v, 15, 0x700000, 0, 1280, 720, SC(8, 8, 1264, 704), 1, 2, 0);
}

static uint32_t run(rsx_views* v, char* line, size_t cap)
{
    uint32_t nv = 99;
    line[0] = 0;
    rsx_views_flip(v, line, cap, &nv);
    return nv;
}

int main(void)
{
    rsx_views v;
    char line[1024];
    uint32_t nv;

    /* ---- one view ---- */
    rsx_views_init(&v, 8, 100);
    frame(&v, 1);
    nv = run(&v, line, sizeof line);
    CHECK("one-view-count", nv == 1);
    CHECK("one-view-line",
          strstr(line, "views=1 ") && strstr(line, "| v0 1280x720 rect=0,0-1280,720 draws=50 words=150 binds=200"));
    CHECK("one-view-no-v1", !strstr(line, "| v1 "));

    /* ---- two views (the 640x560 staggered halves of VerticalSplit) ---- */
    rsx_views_init(&v, 8, 100);
    frame(&v, 2);
    {
        uint32_t sel[RSX_VIEWS_MAXSEL];
        const uint32_t n = rsx_views_select(&v, sel);
        CHECK("two-view-select-count", n == 2);
        if (n == 2) {
            CHECK("two-view-left",
                  v.g[sel[0]].x0 == 0 && v.g[sel[0]].y0 == 0 && v.g[sel[0]].x1 == 640 && v.g[sel[0]].y1 == 560);
            CHECK("two-view-right",
                  v.g[sel[1]].x0 == 640 && v.g[sel[1]].y0 == 160 && v.g[sel[1]].x1 == 1280 && v.g[sel[1]].y1 == 720);
            CHECK("two-view-left-numbers",
                  v.g[sel[0]].draws == 30 && v.g[sel[0]].words == 90 && v.g[sel[0]].binds == 120);
            CHECK("two-view-right-numbers",
                  v.g[sel[1]].draws == 28 && v.g[sel[1]].words == 56 && v.g[sel[1]].binds == 140);
        }
    }
    nv = run(&v, line, sizeof line);
    CHECK("two-view-count", nv == 2);
    CHECK("two-view-line-left", strstr(line, "| v0 1280x720 rect=0,0-640,560 draws=30 words=90 binds=120"));
    CHECK("two-view-line-right", strstr(line, "| v1 1280x720 rect=640,160-1280,720 draws=28 words=56 binds=140"));
    CHECK("two-view-hist", strstr(line, "hist=0:0 1:0 2:1 3+:0"));

    /* ---- three views ---- */
    rsx_views_init(&v, 8, 100);
    frame(&v, 3);
    CHECK("three-view-count", run(&v, line, sizeof line) == 3);

    /* ---- alternative signals: surface/zeta changes and scissor changes, resets per flip ---- */
    rsx_views_init(&v, 8, 100);
    frame(&v, 2);
    CHECK("surf-changes", v.surf_changes == 4); /* shadow>left, right>light, light>post, post>big */
    CHECK("scissor-changes", v.scissor_changes == 9);
    run(&v, line, sizeof line);
    CHECK("per-flip-reset",
          v.draws == 0 && v.ng == 0 && v.surf_changes == 0 && v.scissor_changes == 0 && v.pend_words == 0);

    /* ---- words after the last draw are the tail ---- */
    rsx_views_init(&v, 8, 100);
    burst(&v, 10, 1, 2, 1280, 720, 0, 0, 0, 1, 0);
    for (int i = 0; i < 7; i++) rsx_views_word(&v);
    run(&v, line, sizeof line);
    CHECK("tail-words", strstr(line, "tail_words=7"));

    /* ---- a flip without draws prints nothing and counts as hist[0] ---- */
    rsx_views_init(&v, 8, 100);
    CHECK("idle-flip-silent", run(&v, line, sizeof line) == 0 && line[0] == 0 && v.hist[0] == 1);

    /* ---- the NV40 reset scissor 4096x4096 and an overlarge rect are clamped to the target ---- */
    rsx_views_init(&v, 8, 100);
    burst(&v, 9, 1, 2, 1280, 720, SC(0, 0, 4096, 4096), 1, 1, 0);
    nv = run(&v, line, sizeof line);
    CHECK("clamp", nv == 1 && strstr(line, "rect=0,0-1280,720 draws=9"));

    /* ---- min_draws: a rect with min-1 packets is not a view ---- */
    rsx_views_init(&v, 8, 100);
    burst(&v, 7, 1, 2, 1280, 720, 0, 0, 0, 1, 0);
    CHECK("min-draws-below", run(&v, line, sizeof line) == 0);
    rsx_views_init(&v, 8, 100);
    burst(&v, 8, 1, 2, 1280, 720, 0, 0, 0, 1, 0);
    CHECK("min-draws-at", run(&v, line, sizeof line) == 1);

    /* ---- a target size with its own rect must not add a view to a single-view frame (per-size selection) ---- */
    rsx_views_init(&v, 8, 100);
    burst(&v, 40, 1, 2, 1280, 720, 0, 0, 0, 1, 0);
    burst(&v, 30, 3, 0, 2048, 2048, SC(1280, 0, 768, 2048), 1, 1,
          0); /* e.g. an atlas viewport, disjoint from the main rect */
    CHECK("per-size-selection", run(&v, line, sizeof line) == 1);

    /* ---- overlapping big rects on one target are one view ---- */
    rsx_views_init(&v, 8, 100);
    burst(&v, 40, 1, 2, 1280, 720, 0, 0, 0, 1, 0);
    burst(&v, 20, 1, 2, 1280, 720, SC(64, 36, 1152, 648), 1, 1, 0);
    CHECK("overlap-is-one-view", run(&v, line, sizeof line) == 1);

    /* ---- group overflow does not crash and keeps counting ---- */
    rsx_views_init(&v, 8, 100);
    for (int i = 0; i < 200; i++) burst(&v, 1, 1, 2, 1280, 720, SC(i, 0, 10, 10), 1, 1, 1);
    nv = run(&v, line, sizeof line);
    CHECK("overflow", nv == 0 && strstr(line, "draws=200"));

    /* ---- line cap: detailed lines stop at max_lines, then a hist line every 300 flips ---- */
    rsx_views_init(&v, 8, 2);
    int printed = 0;
    for (int f = 0; f < 700; f++) {
        frame(&v, 2);
        if (rsx_views_flip(&v, line, sizeof line, &nv)) {
            printed++;
            if (printed > 2) CHECK("summary-has-no-views-detail", !strstr(line, "| v0 "));
        }
    }
    CHECK("line-cap", printed == 2 + 2); /* flips 300 and 600 */

    if (fails) {
        printf("test_views: %d check(s) FAILED\n", fails);
        return 1;
    }
    printf("test_views: all checks pass\n");
    return 0;
}

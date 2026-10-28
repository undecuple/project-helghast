/* Host test for KZ3 patch 2069 (native mouse look, libs/input/pad_mouse.h + pad_mouse.c): the arithmetic of one look
 * update (signs, weapon scale, zoom factor, inversion, pitch clamp, idle = no change), the KZ3_MOUSE_SCRIPT parser
 * and its per-update shares, and the accumulator (counts summed between takes, cleared by a take). Patch 2099 (rsx
 * pass 94): degrees-per-count sensitivity, the kz3.ini [input] keys, and the frame-stamped accumulator (no counts
 * dropped or doubled while a look update runs every frame, the full motion of a stalled frame kept, stale counts of
 * an idle stretch dropped). Patch 2108 (RSX pass 103): arrival-time takes up to a cut (look counts on the game's
 * clock) and the KZ3_MOUSE_FEED rate. No game data. */
#include "pad_mouse.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
static void check(const char* what, int ok) { printf("%s: %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fails++; }
static int near(float a, float b) { return fabsf(a - b) < 1e-5f; }
/* the runtime's frame clock (cellGcmSys.c in the exe); the test drives it */
static unsigned s_frame;
unsigned cellGcm_flip_request_count(void) { return s_frame; }

int main(void)
{
    PadMouseCfg c = { 0.003f, 0.5f, 0 };
    float y = 0.25f, p = 0.1f;
    check("idle: no change", !pad_mouse_look_apply(&c, 0, 0, 1.0f, 0, -1.0f, 1.0f, &y, &p) && y == 0.25f && p == 0.1f);
    y = 0.0f; p = 0.0f;
    pad_mouse_look_apply(&c, 100, 0, 1.0f, 0, -1.0f, 1.0f, &y, &p);
    check("right = negative yaw delta, 100 counts = 0.3 rad", near(y, -0.3f) && p == 0.0f);
    y = 0.02f; p = 0.0f;
    pad_mouse_look_apply(&c, 0, -100, 1.0f, 0, -1.0f, 1.0f, &y, &p);
    check("up (dy < 0) = positive pitch; the game's yaw kept", near(p, 0.3f) && near(y, 0.02f));
    c.invert = 1; p = 0.0f;
    pad_mouse_look_apply(&c, 0, -100, 1.0f, 0, -1.0f, 1.0f, &y, &p);
    check("inverted: up = negative pitch", near(p, -0.3f));
    c.invert = 0; y = 0.0f; p = 0.0f;
    pad_mouse_look_apply(&c, 100, 100, 0.4f, 0, -1.0f, 1.0f, &y, &p);
    check("weapon scale 0.4 on both axes", near(y, -0.12f) && near(p, -0.12f));
    y = 0.0f; p = 0.0f;
    pad_mouse_look_apply(&c, 100, 0, 0.4f, 1, -1.0f, 1.0f, &y, &p);
    check("zoomed: weapon scale x zoom factor", near(y, -0.06f));
    y = 0.0f; p = 0.0f;
    pad_mouse_look_apply(&c, 100, 0, NAN, 0, -1.0f, 1.0f, &y, &p);
    check("bad weapon scale counts as 1", near(y, -0.3f));
    p = 0.9f;
    pad_mouse_look_apply(&c, 0, -1000, 1.0f, 0, -1.2f, 1.0f, &y, &p);
    check("pitch clamped to the max", p == 1.0f);
    p = -1.1f;
    pad_mouse_look_apply(&c, 0, 1000, 1.0f, 0, -1.2f, 1.0f, &y, &p);
    check("pitch clamped to the min", p == -1.2f);
    p = 0.0f;
    pad_mouse_look_apply(&c, 0, -1000, 1.0f, 0, NAN, 1.0f, &y, &p);
    check("no clamp with unknown limits", near(p, 3.0f));
    y = 0.0f;
    pad_mouse_look_apply(&c, 1047, 0, 1.0f, 0, -1.0f, 1.0f, &y, &p);
    check("1047 counts at 0.003 = 180 degrees in one update", fabsf(y + 3.141f) < 0.001f);

    PadMouseEv ev[8];
    int bad = 0;
    int n = pad_mouse_script_parse("20:1047,0 30.5:0,-200*3; x:1,2, 40:5", ev, 8, &bad);
    check("script: 2 events, 2 bad tokens", n == 2 && bad == 2 && ev[0].t == 20.0 && ev[0].dx == 1047 && ev[0].left == 1 &&
                                             ev[1].t == 30.5 && ev[1].dy == -200 && ev[1].left == 3);
    int dx = 0, dy = 0;
    pad_mouse_script_due(ev, n, 19.9, &dx, &dy);
    check("script: nothing before T", dx == 0 && dy == 0);
    pad_mouse_script_due(ev, n, 31.0, &dx, &dy);
    check("script: both due at 31 s", dx == 1047 && dy == -200);
    dx = dy = 0;
    pad_mouse_script_due(ev, n, 31.1, &dx, &dy);
    pad_mouse_script_due(ev, n, 31.2, &dx, &dy);
    pad_mouse_script_due(ev, n, 31.3, &dx, &dy);
    check("script: *3 = three updates, one-shot once", dx == 0 && dy == -400);

    /* Accumulator (pad_mouse.c, no switches set: no script). */
    pad_mouse_add(3, -2);
    pad_mouse_add(4, 1);
    check("take sums the counts", pad_mouse_take(1.0, &dx, &dy) == 1 && dx == 7 && dy == -1);
    check("take clears them", pad_mouse_take(1.1, &dx, &dy) == 0 && dx == 0 && dy == 0);
    check("on by default (patch 2097)", pad_mouse_enabled() == 1 && near(pad_mouse_cfg()->sens, PAD_MOUSE_SENS_DEFAULT));

    /* Patch 2099: sensitivity = user x deg/count. */
    bad = 1;
    check("default = 2.5 x 0.022 deg = 0.055 deg/count", near(PAD_MOUSE_SENS_DEFAULT * 57.2957795f, 0.055f) &&
                                                          near(pad_mouse_rad_per_count(2.5f, 0.022f, &bad), 0.00095993f) && !bad);
    check("sensitivity 1.0 = m_yaw 0.022 deg = 0.000384 rad", near(pad_mouse_rad_per_count(1.0f, 0.022f, NULL), 0.00038397f));
    check("bad values fall back", near(pad_mouse_rad_per_count(-1.0f, NAN, &bad), PAD_MOUSE_SENS_DEFAULT) && bad == 1);
    check("0 falls back", near(pad_mouse_rad_per_count(0.0f, 0.022f, &bad), PAD_MOUSE_SENS_DEFAULT) && bad == 1);
    {   /* 360 degrees at sensitivity 2.5 = 6545 counts (20.8 cm at 800 dpi) */
        PadMouseCfg d = { PAD_MOUSE_SENS_DEFAULT, 1.0f, 0 };
        float yy = 0.0f, pp = 0.0f;
        pad_mouse_look_apply(&d, 6545, 0, 1.0f, 0, -1.0f, 1.0f, &yy, &pp);
        check("6545 counts = one turn at the default", fabsf(yy + 6.2832f) < 0.002f);
        /* the hand test's first update (-3665,-1057) at the default: 3.5 rad yaw (was 11 at 0.003) */
        yy = 0.0f; pp = 0.0f;
        pad_mouse_look_apply(&d, -3665, 0, 1.0f, 0, -1.0f, 1.0f, &yy, &pp);
        check("3665 counts = 3.52 rad at the default", fabsf(yy - 3.518f) < 0.002f);
        /* ADS: weapon factor x user ads factor */
        d.zoom_sens = 0.8f; yy = 0.0f;
        pad_mouse_look_apply(&d, 1000, 0, 0.26f, 1, -1.0f, 1.0f, &yy, &pp);
        check("ads: weapon 0.26 x user 0.8", near(yy, -1000.0f * PAD_MOUSE_SENS_DEFAULT * 0.26f * 0.8f));
    }

    /* kz3.ini [input] keys */
    {
        int ok = PAD_MOUSE_NINI == 3;
        const char* want[3][2] = {{"mouse_sensitivity", "KZ3_MOUSE_SENS"}, {"mouse_ads_sensitivity", "KZ3_MOUSE_ZOOM_SENS"},
                                  {"mouse_invert_y", "KZ3_MOUSE_INVERT"}};
        for (size_t i = 0; ok && i < 3; i++)
            ok = !strcmp(pad_mouse_ini_keys[i].key, want[i][0]) && !strcmp(pad_mouse_ini_keys[i].env, want[i][1]);
        check("ini keys: mouse_sensitivity, mouse_ads_sensitivity, mouse_invert_y", ok);
    }

    /* Patch 2099: frame-stamped accumulator (pure). */
    {
        PadLookAcc a;
        memset(&a, 0, sizeof a);
        long long dr = 0;
        pad_look_acc_add(&a, 500, 100, 10);       /* menu: no take yet */
        pad_look_acc_add(&a, 300, 0, 11);
        pad_look_acc_add(&a, 7, -3, 12);
        pad_look_acc_take(&a, 12, &dx, &dy, &dr);
        check("first take keeps only its own frame's counts", dx == 7 && dy == -3 && dr == 900);
        /* one take per frame at 30 Hz while the window adds at 144 Hz: every count once */
        long long sx = 0, sent = 0;
        for (unsigned f = 13; f < 113; f++) {
            for (int k = 0; k < 5; k++) { pad_look_acc_add(&a, 3, 1, f); sent += 3; }
            pad_look_acc_take(&a, f, &dx, &dy, &dr);
            sx += dx;
            if (dr) sent = -1;
        }
        check("100 frames x 5 adds: nothing dropped or doubled", sx == sent && sent == 1500);
        /* counts that arrive after this frame's take, in the next frame, are taken by the next update */
        pad_look_acc_add(&a, 11, 0, 112);
        pad_look_acc_add(&a, 4, 0, 113);
        pad_look_acc_take(&a, 113, &dx, &dy, &dr);
        check("counts after a take carry into the next update", dx == 15 && dr == 0);
        /* stall: no flip for seconds, the motion is applied once, in full */
        for (int k = 0; k < 400; k++) pad_look_acc_add(&a, 5, 0, 114);
        pad_look_acc_take(&a, 114, &dx, &dy, &dr);
        check("stalled frame: 2000 counts kept", dx == 2000 && dr == 0);
        pad_look_acc_take(&a, 114, &dx, &dy, &dr);
        check("a second take in the same frame gets nothing (no doubling)", dx == 0 && dy == 0);
        /* a skipped look update (3 frames) keeps its counts */
        pad_look_acc_add(&a, 9, 0, 116);
        pad_look_acc_take(&a, 117, &dx, &dy, &dr);
        check("up to 3 frames without a take keep their counts", dx == 9 && dr == 0);
        /* pause: no takes for 100 frames */
        pad_look_acc_add(&a, 1000, 0, 130);
        pad_look_acc_add(&a, 2, 0, 217);
        pad_look_acc_take(&a, 217, &dx, &dy, &dr);
        check("after a pause only the resume frame's counts", dx == 2 && dr == 1000);
        pad_look_acc_add(&a, 50, 0, 220);
        pad_look_acc_take(&a, 240, &dx, &dy, &dr);
        check("old counts with no add since are dropped at the take", dx == 0 && dr == 50);
        memset(&a, 0, sizeof a);
        a.taken = 1; a.take_frame = 0xFFFFFFFEu;
        pad_look_acc_add(&a, 6, 0, 0u);
        pad_look_acc_take(&a, 0u, &dx, &dy, &dr);
        check("frame counter wrap", dx == 6 && dr == 0);
    }
    /* the runtime accumulator uses the frame clock */
    s_frame = 500;
    pad_mouse_take(2.0, &dx, &dy);
    pad_mouse_add(10, 0);
    s_frame = 501;
    pad_mouse_add(1, 0);
    check("runtime: next frame's take gets both", pad_mouse_take(2.1, &dx, &dy) == 1 && dx == 11);
    pad_mouse_add(10, 0);
    s_frame = 600;
    pad_mouse_add(1, 0);
    check("runtime: after 99 idle frames only the newest frame", pad_mouse_take(2.2, &dx, &dy) == 1 && dx == 1);
    /* Patch 2108 (RSX pass 103): counts on the game's clock (arrival times, take up to a cut) and the feed's rate. */
    {
        static PadLookAcc a;
        long long dr = 0;
        memset(&a, 0, sizeof a);
        a.taken = 1; a.take_frame = 700;
        pad_look_acc_add_at(&a, 5, 1, 700, 1000000ull);
        pad_look_acc_add_at(&a, 7, 0, 700, 1000000ull + 40000ull);   /* within 1 ms: merges into the newest */
        pad_look_acc_add_at(&a, 3, 0, 700, 3000000ull);
        pad_look_acc_add_at(&a, 4, -2, 700, 9000000ull);
        check("2108: adds within 1 ms merge, later ones get their own entry", a.qn == 3 && a.q[0].dx == 12);
        pad_look_acc_take_until(&a, 701, 3000000ull, &dx, &dy, &dr);
        check("2108: a take hands out the counts up to the cut only", dx == 15 && dy == 1 && a.dx == 4 && a.qn == 1);
        pad_look_acc_add(&a, 2, 0, 701);                               /* no time: due at the next take */
        pad_look_acc_take_until(&a, 702, 5000000ull, &dx, &dy, &dr);
        check("2108: untimed counts go at once, later timed ones stay", dx == 2 && dy == 0 && a.dx == 4 && a.dy == -2);
        pad_look_acc_take_until(&a, 703, 9000000ull, &dx, &dy, &dr);
        check("2108: the kept counts come at the cut that passes them", dx == 4 && dy == -2 && a.qn == 0 && a.dx == 0);
        long long sent = 0;
        for (int k = 0; k < 800; k++) pad_look_acc_add_at(&a, 1, 0, 704, 10000000ull + (unsigned long long)k * 100000ull);
        pad_look_acc_take_until(&a, 704, ~0ull, &dx, &dy, &dr); sent += dx;
        check("2108: a full ring merges into the newest, nothing lost", sent == 800 && a.qn == 0);
        for (int k = 0; k < 10; k++) pad_look_acc_add_at(&a, 3, 0, 705, 50000000ull + (unsigned long long)k * 200000ull);
        pad_look_acc_take_until(&a, 720, 60000000ull, &dx, &dy, &dr);
        check("2108: counts kept over an idle stretch are dropped as before", dx == 0 && dr == 30 && a.qn == 0);
        /* constant speed: equal cut steps get equal counts whatever the take times */
        memset(&a, 0, sizeof a);
        a.taken = 1; a.take_frame = 800;
        int got[6], gi = 0, ok = 1;
        unsigned long long t = 0, cut = 5320000ull;                   /* one 1/15 s slot in timebase ticks */
        const unsigned long long takes[6] = {5320000ull * 2 + 10, 5320000ull * 3 + 4000000ull, 5320000ull * 4 + 100,
                                             5320000ull * 5 + 5000000ull, 5320000ull * 6 + 1, 5320000ull * 7 + 2600000ull};
        for (int k = 0; k < 6; k++) {
            for (; t < takes[k]; t += 79800ull) pad_look_acc_add_at(&a, 1, 0, 800, t);   /* 1 count per ms */
            pad_look_acc_take_until(&a, 800, cut, &dx, &dy, &dr);
            got[gi++] = dx;
            cut += 5320000ull;
        }
        for (int k = 1; k < 6; k++) ok &= got[k] >= 66 && got[k] <= 68;
        check("2108: steady hand, uneven take times -> 66-67 counts per slot", ok);
        check("2108: feed owes rate x elapsed after its start, capped at its length",
              pad_mouse_feed_owed(500.0, 2.0, 10.0, 1.0) == 0 && pad_mouse_feed_owed(500.0, 2.0, 10.0, 3.5) == 750 &&
              pad_mouse_feed_owed(-500.0, 2.0, 10.0, 30.0) == -5000);
    }
    printf("%s\n", fails ? "FAIL" : "all ok");
    return fails != 0;
}

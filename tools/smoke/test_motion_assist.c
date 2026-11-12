/* Host test for KZ3 patches 4030/4031 (network pass 16): libs/input/pad_assist.h + pad_assist_drive.h, the pure logic of the mouse-driven valve stick
 * and the opt-in motion-section autocomplete. No game data, no GPU: a fake guest memory and a small model of the Valve
 * entity update (hand speed limit, grab-gated valve angle, clamp), written from the RE notes s.14.2.
 * Negative controls: a stick below the dead zone / magnitude 0.5 is refused, a driver that never releases the grab makes
 * no net progress, a not-engaged combo leaves the guest memory byte for byte unchanged. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "pad_assist_drive.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { CTRL = 0x11000, ENT = 0x12000, RES = 0x13000, ARR = 0x14000 };
static uint8_t mem[0x18000];

static void put32(uint32_t ea, uint32_t v) { mem[ea] = (uint8_t)(v >> 24); mem[ea + 1] = (uint8_t)(v >> 16); mem[ea + 2] = (uint8_t)(v >> 8); mem[ea + 3] = (uint8_t)v; }
static void putf(uint32_t ea, float f) { pas_wf(mem, ea, f); }

/* Flooded Pyrrhus' Valve_CounterClockwise_AutoDisable (decoded with tools/core.py, the RE notes s.14.10) */
static void setup_valve(float angle_deg, float disable)
{
    memset(mem, 0, sizeof mem);
    put32(CTRL + 0x20, ENT);
    put32(ENT + 0xE4, RES);
    putf(ENT + 0x288, angle_deg * PAS_DEG2RAD);
    putf(ENT + 0x290, 0.0f);
    putf(RES + 0x18C, 0.0f); putf(RES + 0x190, 200.0f);
    putf(RES + 0x198, -35.0f); putf(RES + 0x19C, 35.0f);
    putf(RES + 0x1A0, 360.0f);
    putf(RES + 0x1A8, disable);
    putf(CTRL + 0x10C, 1.0f); putf(CTRL + 0x110, 1.0f);
}

/* The entity update as s.14.2 describes it: the hand moves toward the controller target at speed*dt, clamped to the
 * hand limits; the valve angle advances by the hand's change while a grab gain is below 0.5, clamped to Min/Max.
 * hand_sign = which hand direction raises the angle. */
static void entity_update(float dt, float hand_sign, int always_grab)
{
    const float tgt = pas_rf(mem, CTRL + 0x108), spd = pas_rf(mem, RES + 0x1A0) * PAS_DEG2RAD;
    float hand = pas_rf(mem, ENT + 0x290), ang = pas_rf(mem, ENT + 0x288), d, nh;
    const float lo = pas_rf(mem, RES + 0x198) * PAS_DEG2RAD, hi = pas_rf(mem, RES + 0x19C) * PAS_DEG2RAD;
    const float g = pas_rf(mem, CTRL + 0x10C);
    d = tgt - hand;
    if (d > spd * dt) d = spd * dt;
    if (d < -spd * dt) d = -spd * dt;
    nh = hand + d;
    if (nh < lo) nh = lo;
    if (nh > hi) nh = hi;
    if (g < 0.5f || always_grab) {
        ang += hand_sign * (nh - hand);
        if (ang < 0) ang = 0;
        if (ang > 200.0f * PAS_DEG2RAD) ang = 200.0f * PAS_DEG2RAD;
    }
    putf(ENT + 0x290, nh);
    putf(ENT + 0x288, ang);
}
static float vn(void) { return pas_rf(mem, ENT + 0x288) / (200.0f * PAS_DEG2RAD); }

static int run_valve(float start_deg, float disable, float hand_sign, int max_updates, int cheat_never_release, int* sweeps)
{
    PasState st;
    PasDrive dr;
    unsigned long long now = 10000;
    int i;
    memset(&st, 0, sizeof st);
    memset(&dr, 0, sizeof dr);
    dr.mode = PAS_HOLD;
    dr.combo.engaged = 1;
    setup_valve(start_deg, disable);
    for (i = 0; i < max_updates; i++) {
        now += 33;
        pas_valve_post(&st, &dr, mem, CTRL, now);
        if (cheat_never_release) { putf(CTRL + 0x10C, 0.0f); putf(CTRL + 0x110, 0.0f); }
        if (dr.rt.done) break;
        entity_update(1.0f / 30.0f, hand_sign, cheat_never_release);
    }
    if (sweeps) *sweeps = dr.rt.pushes;
    return dr.rt.done ? i : -1;
}

int main(void)
{
    int i, n;

    /* mode names */
    CHECK(pas_mode_by_name("off", 3) == PAS_OFF && pas_mode_by_name("HOLD", 4) == PAS_HOLD && pas_mode_by_name("always", 6) == -1 &&
          pas_mode_by_name("", 0) == -1, "mode names");

    /* combo: 0.5 s hold, restarts when broken, the release edge */
    {
        PasCombo c;
        memset(&c, 0, sizeof c);
        CHECK(pas_combo_step(&c, 1, 1000) == 0, "press");
        CHECK(pas_combo_step(&c, 1, 1499) == 0 && !c.engaged, "499 ms is too early");
        CHECK(pas_combo_step(&c, 1, 1500) == 1 && c.engaged, "500 ms engages");
        CHECK(pas_combo_step(&c, 1, 1900) == 0 && c.engaged, "stays engaged without a second edge");
        CHECK(pas_combo_step(&c, 0, 1950) == -1 && !c.engaged, "release edge");
        CHECK(pas_combo_step(&c, 0, 1960) == 0, "idle");
        CHECK(pas_combo_step(&c, 1, 2000) == 0 && pas_combo_step(&c, 1, 2400) == 0, "restarted hold");
        CHECK(pas_combo_step(&c, 0, 2450) == 0, "broken before 500 ms: never engaged");
        CHECK(pas_combo_step(&c, 1, 2460) == 0 && pas_combo_step(&c, 1, 2900) == 0 && !c.engaged, "the hold counts from the new press");
        CHECK(pas_combo_step(&c, 1, 2960) == 1, "engaged 500 ms after the new press");
    }

    /* mouse -> stick: direction, dead zone 0.29, magnitude 0.5 */
    {
        PasStick s = {0, 0};
        uint8_t bx, by;
        float ang;
        CHECK(!pas_stick_bytes(&s, &bx, &by) && bx == 128 && by == 128, "rest");
        pas_stick_add(&s, 10, 0);
        CHECK(!pas_stick_bytes(&s, &bx, &by) && bx == 128, "10 counts stay under the threshold");
        pas_stick_add(&s, 90, 0);
        CHECK(pas_stick_bytes(&s, &bx, &by) && bx == 255 && by == 128, "100 counts right = full right, got %u,%u", bx, by);
        CHECK(pas_game_stick(bx, by, &ang) && fabsf(ang - 1.5707963f) < 0.01f, "right = +90 deg, got %f", (double)ang);
        pas_stick_add(&s, -100, -100);
        CHECK(pas_stick_bytes(&s, &bx, &by) && by < 128, "up is a low byte");
        s.x = s.y = 0;
        pas_stick_add(&s, 0, -100);
        pas_stick_bytes(&s, &bx, &by);
        CHECK(pas_game_stick(bx, by, &ang) && fabsf(ang) < 0.01f, "up = 0 deg, got %f", (double)ang);
        s.x = s.y = 0;
        pas_stick_add(&s, 70, -70);
        pas_stick_bytes(&s, &bx, &by);
        CHECK(pas_game_stick(bx, by, &ang) && fabsf(ang - 0.7853982f) < 0.02f, "up-right = 45 deg (full magnitude diagonal), got %f", (double)ang);
        s.x = s.y = 0;
        pas_stick_add(&s, 100000, 100000);
        CHECK(sqrtf(s.x * s.x + s.y * s.y) <= 1.0001f, "magnitude stays clamped");
        /* the game's own tests, byte offsets from the centre (negative controls): x only */
        for (n = 0; n <= 127; n++) {
            const int acc = pas_game_stick((uint8_t)(128 + n), 128, NULL);
            if (n <= 36) CHECK(!acc, "offset %d is inside the 0.29 dead zone", n);          /* 36/127 = 0.283 */
            if (n >= 37 && n <= 81) CHECK(!acc, "offset %d clears the dead zone but not magnitude 0.5", n);
            if (n >= 82) CHECK(acc, "offset %d must be taken (>= 0.5 after the dead zone)", n);
        }
        CHECK(!pas_game_stick(128 + 30, 128, NULL), "a light push is refused (negative control)");
    }

    /* ratchet vs the entity model: both hand polarities, both directions of DisableValue */
    {
        int sw;
        const float start[] = { 180.0f, 20.0f, 100.0f };
        const float dis[] = { 0.0f, 1.0f, 0.0f };
        for (i = 0; i < 3; i++) {
            int r = run_valve(start[i], dis[i], 1.0f, 600, 0, &sw), r2 = run_valve(start[i], dis[i], -1.0f, 600, 0, NULL);
            CHECK(r > 0 && r < 300, "case %d hand+ : done after %d updates", i, r);
            CHECK(r2 > 0 && r2 < 400, "case %d hand- : done after %d updates (after one wrong-way flip)", i, r2);
            CHECK(vn() * 0 == 0, "n");
            if (i == 0) { printf("[test] Pyrrhus valve 180 -> 0: %d updates (%.1f s at 30/s), %d sweeps; flipped polarity %d updates\n", r, r / 30.0, sw, r2); }
        }
        CHECK(run_valve(180.0f, 0.0f, 1.0f, 600, 0, &sw) >= 2 * 3, "needs several sweeps, it is a ratchet (180 deg / 70 deg per push)");
        CHECK(sw >= 3, "sweeps %d", sw);
        /* negative control: the grab never released -> the valve follows the hand both ways, no net progress */
        CHECK(run_valve(180.0f, 0.0f, 1.0f, 900, 1, NULL) == -1, "without the release the model must not complete");
    }

    /* guest memory: not engaged -> byte for byte unchanged; bad pointers -> nothing written */
    {
        PasState st;
        PasDrive dr;
        uint8_t copy[sizeof mem];
        memset(&st, 0, sizeof st);
        memset(&dr, 0, sizeof dr);
        dr.mode = PAS_HOLD;                       /* mode on, combo NOT held */
        setup_valve(180.0f, 0.0f);
        memcpy(copy, mem, sizeof mem);
        CHECK(pas_valve_post(&st, &dr, mem, CTRL, 5000) == 0 && memcmp(copy, mem, sizeof mem) == 0, "not engaged writes nothing");
        CHECK(pas_valve_live(&st, 5100) && !pas_valve_live(&st, 5000 + PAS_SECTION_MS + 1), "section stamp lifetime");
        dr.mode = PAS_OFF; dr.combo.engaged = 1;  /* held but the setting is off */
        CHECK(pas_valve_post(&st, &dr, mem, CTRL, 5100) == 0 && memcmp(copy, mem, sizeof mem) == 0, "setting off writes nothing");
        dr.mode = PAS_HOLD;
        put32(CTRL + 0x20, 0); memcpy(copy, mem, sizeof mem);
        CHECK(pas_valve_post(&st, &dr, mem, CTRL, 5200) == 0 && memcmp(copy, mem, sizeof mem) == 0, "no entity writes nothing");
        put32(CTRL + 0x20, ENT); put32(ENT + 0xE4, 0xF1000000u); memcpy(copy, mem, sizeof mem);
        CHECK(pas_valve_post(&st, &dr, mem, CTRL, 5300) == 0 && memcmp(copy, mem, sizeof mem) == 0, "wild resource pointer writes nothing");
        put32(ENT + 0xE4, RES); putf(RES + 0x190, 0.0f); memcpy(copy, mem, sizeof mem);   /* Max <= Min */
        CHECK(pas_valve_post(&st, &dr, mem, CTRL, 5400) == 0 && memcmp(copy, mem, sizeof mem) == 0, "empty angle range writes nothing");
        putf(RES + 0x190, 200.0f); putf(RES + 0x19C, -35.0f); memcpy(copy, mem, sizeof mem);   /* hand limits empty */
        CHECK(pas_valve_post(&st, &dr, mem, CTRL, 5500) == 0 && memcmp(copy, mem, sizeof mem) == 0, "empty hand limits write nothing");
        setup_valve(180.0f, 0.0f);
        CHECK(pas_valve_post(&st, &dr, mem, CTRL, 5600) == 1, "engaged on a good valve drives it");
        CHECK(pas_rf(mem, CTRL + 0x10C) == 0.0f && pas_rf(mem, CTRL + 0x110) == 0.0f, "first push: grab held (gain 0.0)");
        CHECK(fabsf(pas_rf(mem, CTRL + 0x108) - 35.0f * PAS_DEG2RAD) < 1e-4f || fabsf(pas_rf(mem, CTRL + 0x108) + 35.0f * PAS_DEG2RAD) < 1e-4f,
              "target is a hand limit: %f", (double)pas_rf(mem, CTRL + 0x108));
    }

    /* charge: windows alternate, grab byte, gate bit */
    {
        PasState st;
        PasDrive dr;
        unsigned long long now = 20000;
        int seen_hi = 0, seen_lo = 0;
        memset(&st, 0, sizeof st);
        memset(&dr, 0, sizeof dr);
        dr.mode = PAS_HOLD; dr.combo.engaged = 1;
        memset(mem, 0, sizeof mem);
        put32(CTRL + 0x20, ENT); put32(ENT + 0xE4, RES);
        putf(RES + 0x190, -45.0f); putf(RES + 0x194, 45.0f); putf(RES + 0x198, 360.0f);
        put32(RES + 0x200, ARR); put32(RES + 0x204, 2); putf(RES + 0x20C, 0.0f);
        putf(ARR, 0.741765f); putf(ARR + 4, 0.829031f); putf(ARR + 8, -0.829031f); putf(ARR + 12, -0.741765f);
        put32(CTRL + 0x24, 8);
        CHECK(pas_charge_post(&st, &dr, mem, CTRL, now) == 0 && !pas_charge_live(&st, now), "bit 8 set: not placing, nothing");
        put32(CTRL + 0x24, 0);
        for (i = 0; i < 90; i++) {
            const float t = (now += 33, pas_charge_post(&st, &dr, mem, CTRL, now), pas_rf(mem, CTRL + 0x108));
            if (fabsf(t - 0.785398f) < 1e-3f) seen_hi = 1;
            if (fabsf(t + 0.785398f) < 1e-3f) seen_lo = 1;
            CHECK(mem[CTRL + 0x114] == 1, "grab byte");
        }
        CHECK(seen_hi && seen_lo, "both arm windows visited in 3 s (hi %d lo %d)", seen_hi, seen_lo);
        CHECK(pas_charge_live(&st, now), "charge section stamp");
        dr.combo.engaged = 0;
        { uint8_t c2[sizeof mem]; memcpy(c2, mem, sizeof mem); pas_charge_post(&st, &dr, mem, CTRL, now + 33); CHECK(!memcmp(c2, mem, sizeof mem), "released: nothing written"); }
    }

    if (fails) { printf("test_motion_assist: %d FAILED\n", fails); return 1; }
    printf("test_motion_assist: all passed\n");
    return 0;
}

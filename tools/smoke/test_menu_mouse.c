/* Host test for KZ3 patch 2098 (menu mouse, RSX pass 93): kz3_menu_hit_test, the click/wheel injection state machine,
 * the hover gate, the event mapping, the KZ3_MENU_MOUSE_SCRIPT parser and pad_cursor_decide's menu input.
 * Pure headers only (libs/input/pad_menu_mouse.h, pad_mouse.h). tools/smoke/test_menu_mouse.sh builds and runs it. */
#include "pad_menu_mouse.h"
#include "pad_mouse.h"
#include <stdio.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static KZ3MenuCand cand(int id, int l, int t, int r, int b, int depth)
{
    KZ3MenuCand c;
    c.id = id; c.l = l; c.t = t; c.r = r; c.b = b; c.depth = depth; c.n_clips = 0; c.enabled = 1; c.focusable = 1;
    return c;
}

static void test_hit(void)
{
    KZ3MenuCand c[6];
    /* a panel item (depth 1) and a list row inside it (depth 3): the row wins where both cover */
    c[0] = cand(10, 100, 100, 600, 400, 1);
    c[1] = cand(11, 120, 120, 500, 160, 3);
    CHECK(kz3_menu_hit_test(c, 2, 130, 130, 0) == 11, "innermost wins");
    CHECK(kz3_menu_hit_test(c, 2, 550, 300, 0) == 10, "outer item outside the row");
    CHECK(kz3_menu_hit_test(c, 2, 50, 50, 0) == -1, "no hit outside every item");
    CHECK(kz3_menu_hit_test(c, 0, 130, 130, 0) == -1, "no candidates");
    /* boundaries: l/t inclusive, r/b exclusive; pad widens */
    CHECK(kz3_menu_hit_test(c + 1, 1, 120, 120, 0) == 11, "left/top edge in");
    CHECK(kz3_menu_hit_test(c + 1, 1, 500, 130, 0) == -1, "right edge out");
    CHECK(kz3_menu_hit_test(c + 1, 1, 499.9f, 159.9f, 0) == 11, "last pixel in");
    CHECK(kz3_menu_hit_test(c + 1, 1, 130, 160, 0) == -1, "bottom edge out");
    CHECK(kz3_menu_hit_test(c + 1, 1, 118, 130, 2) == 11, "pad 2 px");
    /* scrolled out: a row whose rect lies outside its list's clip */
    c[2] = cand(12, 120, 420, 500, 460, 3);
    c[2].n_clips = 1; c[2].clip[0].l = 100; c[2].clip[0].t = 100; c[2].clip[0].r = 600; c[2].clip[0].b = 400;
    CHECK(kz3_menu_hit_test(c + 2, 1, 130, 430, 0) == -1, "row scrolled out of its clip");
    c[2].t = 380; c[2].b = 420;
    CHECK(kz3_menu_hit_test(c + 2, 1, 130, 390, 0) == 12, "visible part of a clipped row");
    CHECK(kz3_menu_hit_test(c + 2, 1, 130, 410, 0) == -1, "clipped part of a row");
    /* disabled / not focusable / empty rect */
    c[3] = cand(13, 0, 0, 50, 50, 1); c[3].enabled = 0;
    CHECK(kz3_menu_hit_test(c + 3, 1, 10, 10, 0) == -1, "disabled");
    c[3].enabled = 1; c[3].focusable = 0;
    CHECK(kz3_menu_hit_test(c + 3, 1, 10, 10, 0) == -1, "not focusable");
    c[3] = cand(13, 50, 50, 50, 80, 1);
    CHECK(kz3_menu_hit_test(c + 3, 1, 50, 60, 0) == -1, "zero width");
    /* overlap at equal depth: the later (drawn last) wins; a dialog on top of a page is the caller's subtree choice */
    c[4] = cand(14, 0, 0, 100, 100, 2);
    c[5] = cand(15, 50, 50, 150, 150, 2);
    CHECK(kz3_menu_hit_test(c + 4, 2, 75, 75, 0) == 15, "tie -> later index");
    CHECK(kz3_menu_hit_test(c + 4, 1, 75, 75, 0) == 14, "modal subtree only: the page item is not a candidate");
}

/* One pad packet built (and delivered or not), and optionally a menu frame starting after it. */
static unsigned pkt(PadMenuInject* s, int delivered, int frame)
{
    unsigned b = pmi_bits(s);
    pmi_delivered(s, delivered);
    if (frame) pmi_tick(s);
    return b;
}

static void test_inject(void)
{
    PadMenuInject s;
    unsigned b;
    int i, n;
    pmi_init(&s);
    CHECK(pmi_bits(&s) == 0 && pmi_idle(&s), "idle");
    CHECK(pmi_push(&s, PMM_CROSS), "push");
    CHECK(pmi_bits(&s) == PMM_CROSS && pmi_bits(&s) == PMM_CROSS, "press starts, idempotent within a packet");
    /* the pad thread polls much faster than the menu frames: the press stays through many packets */
    for (i = 0, n = 0; i < 10; i++) n += pkt(&s, 1, 0) == PMM_CROSS;
    CHECK(n == 10, "press held over 10 packets without a menu frame (%d)", n);
    CHECK(pkt(&s, 1, 1) == PMM_CROSS, "press still on in the packet before frame 1");
    CHECK(pkt(&s, 1, 1) == PMM_CROSS, "press on until frame 2 began");
    CHECK(pkt(&s, 1, 0) == 0, "release after two menu frames");
    pkt(&s, 1, 1); pkt(&s, 1, 1);
    CHECK(pmi_idle(&s), "idle after the release's two frames");
    /* frames without a delivered packet do not end the press */
    pmi_push(&s, PMM_DOWN);
    pkt(&s, 0, 1); pkt(&s, 0, 1); pkt(&s, 0, 1);
    CHECK(pmi_bits(&s) == PMM_DOWN, "undelivered packets keep the press over frames");
    pkt(&s, 1, 1);
    CHECK(pmi_bits(&s) == 0, "delivered once + two frames: released");
    pkt(&s, 1, 1); pkt(&s, 1, 1);
    /* two clicks = two edges */
    pmi_push(&s, PMM_CROSS); pmi_push(&s, PMM_CROSS);
    unsigned seq[10];
    for (i = 0; i < 10; i++) seq[i] = pkt(&s, 1, 1);
    CHECK(seq[0] == PMM_CROSS && seq[1] == PMM_CROSS && seq[2] == 0 && seq[3] == 0 && seq[4] == PMM_CROSS &&
          seq[5] == PMM_CROSS && seq[6] == 0 && seq[7] == 0 && pmi_idle(&s),
          "press, release, press, release: %x %x %x %x %x %x %x %x", seq[0], seq[1], seq[2], seq[3], seq[4], seq[5], seq[6], seq[7]);
    /* defer: a click that moved the focus waits one menu frame */
    pmi_push(&s, PMM_CROSS); pmi_defer(&s);
    CHECK(pkt(&s, 1, 0) == 0 && pkt(&s, 1, 0) == 0, "deferred until a menu frame");
    pmi_tick(&s);
    CHECK(pmi_bits(&s) == PMM_CROSS, "press after the frame");
    /* clear during a press: the release still comes, queued presses go */
    pmi_push(&s, PMM_DOWN);
    pmi_clear(&s);
    CHECK(pmi_bits(&s) == 0, "cleared press -> release");
    pkt(&s, 1, 1); pkt(&s, 1, 1);
    CHECK(pmi_idle(&s), "nothing queued after clear");
    /* queue cap */
    for (i = 0; i < PMI_QUEUE; i++) CHECK(pmi_push(&s, PMM_UP), "push %d", i);
    CHECK(!pmi_push(&s, PMM_UP), "queue full");
    CHECK(!pmi_push(&s, 0), "empty press refused");
    for (i = 0, b = 0; i < 8 * PMI_QUEUE; i++) { unsigned x = pmi_bits(&s); if (x && s.ticks == 0 && !s.sent) b++; pkt(&s, 1, 1); }
    CHECK(b == PMI_QUEUE && pmi_idle(&s), "every queued press started once (%u)", b);
}

static void test_hover_and_events(void)
{
    PadMenuHover h = {0, 0};
    pmh_sync(&h, 5);
    CHECK(!pmh_should_hover(&h, 5, 1, 0), "still pointer: no hover");
    CHECK(pmh_should_hover(&h, 6, 1, 0), "moved: hover");
    CHECK(!pmh_should_hover(&h, 6, 1, 0), "still again");
    CHECK(!pmh_should_hover(&h, 7, 1, 1), "moved while the pad is used: no hover");
    CHECK(!pmh_should_hover(&h, 7, 1, 0), "pad stopped, pointer still: no hover");
    CHECK(!pmh_should_hover(&h, 8, 0, 0), "moved onto a letterbox bar: no hover");
    PadMenuHover h2 = {0, 0};
    CHECK(!pmh_should_hover(&h2, 3, 1, 0), "first look never hovers");
    CHECK(pmm_event_bits(PMM_EV_LEFT, 0, 0, 0) == PMM_CROSS, "left on an item = CROSS");
    CHECK(pmm_event_bits(PMM_EV_LEFT, 0, -1, 0) == 0, "left on empty space = nothing");
    CHECK(pmm_event_bits(PMM_EV_RIGHT, 0, -1, 0) == PMM_CIRCLE, "right = CIRCLE anywhere");
    CHECK(pmm_event_bits(PMM_EV_WHEEL, 1, -1, 0) == PMM_UP && pmm_event_bits(PMM_EV_WHEEL, -1, -1, 0) == PMM_DOWN, "wheel list");
    CHECK(pmm_event_bits(PMM_EV_WHEEL, 1, -1, 1) == PMM_RIGHT && pmm_event_bits(PMM_EV_WHEEL, -1, -1, 1) == PMM_LEFT, "wheel value");
    CHECK((PMM_PAD_MENU_MASK & (PMM_UP | PMM_DOWN | PMM_CROSS | PMM_CIRCLE)) == (PMM_UP | PMM_DOWN | PMM_CROSS | PMM_CIRCLE) &&
          !(PMM_PAD_MENU_MASK & 0x0306u), "pad mask: d-pad and faces, not L3/R3/L2/R2");
}

static void test_script_and_cursor(void)
{
    PadMenuScriptEv ev[8];
    int bad = 0, n = pad_menu_script_parse("2:m:640,360 2.5:l; 3:w:-2 4:r x:l 5:q 6:w:0", ev, 8, &bad);
    CHECK(n == 4 && bad == 3, "script: %d events %d bad", n, bad);
    CHECK(n >= 4 && ev[0].kind == PMS_MOVE && ev[0].a == 640 && ev[0].b == 360 && ev[1].kind == PMS_LEFT && ev[1].t == 2.5 &&
          ev[2].kind == PMS_WHEEL && ev[2].a == -2 && ev[3].kind == PMS_RIGHT, "script fields");
    PadCursorState s = {0, 1, 0, 0, 0, 1, 1, 0};
    PadCursorDo d = pad_cursor_decide(&s, 1);
    CHECK(d.hide && d.clip, "gameplay: hidden and locked");
    s.menu = 1;
    d = pad_cursor_decide(&s, 1);
    CHECK(!d.hide && !d.clip, "menu: shown and free");
    s.mode = 2;
    d = pad_cursor_decide(&s, 0);
    CHECK(!d.hide && !d.clip, "menu in fullscreen: shown and free");
    s.menu = 0;
    d = pad_cursor_decide(&s, 0);
    CHECK(d.hide && d.clip, "menu gone: hidden and locked again");
}

int main(void)
{
    test_hit();
    test_inject();
    test_hover_and_events();
    test_script_and_cursor();
    printf("[test_menu_mouse] %s: %d/%d checks\n", fails ? "FAIL" : "PASS", checks - fails, checks);
    return fails ? 1 : 0;
}

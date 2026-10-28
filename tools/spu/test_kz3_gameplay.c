/* Host test for port/spu/kz3_gameplay.c and the kz3_gameplay.h hook macros (SPU pass 33). Run by
 * tools/spu/test_kz3_gameplay.sh; argv[1] = a scratch file stderr is redirected to between checks. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
typedef struct {
    uint64_t gpr[32];
    double fpr[32];
    uint64_t lr;
} ppu_context; /* stub: the macros use gpr[3]/[4] */
uint8_t* vm_base;
#include "kz3_gameplay.h"

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)
static void wr32(uint32_t ea, uint32_t v)
{
    vm_base[ea] = v >> 24;
    vm_base[ea + 1] = v >> 16;
    vm_base[ea + 2] = v >> 8;
    vm_base[ea + 3] = v;
}
static void wrf(uint32_t ea, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    wr32(ea, u);
}

/* ---- stub lifted bodies (what the game's instructions store) ---- */
static int n_ctor, n_view, n_move, n_def, n_pad, n_load;
#define PROFILE 0x20000u
#define CTRL 0x40000u
#define ENTITY 0x50000u
#define MOVE 0x60000u
#define VT 0x00070000u
#define OPD 0x00070400u
#define MAT 0x00070800u
static int n_vcall; static uint32_t vcall_ent, vcall_opd;
uint32_t kz3_trace_vcall_ctx(uint32_t entity, uint32_t opd, void* user)
{
    (void)user;
    n_vcall++; vcall_ent = entity; vcall_opd = opd;
    return opd == OPD ? MAT : 0;
}
static void func_004C2F08__lifted(ppu_context* ctx)
{
    n_ctor++;
    vm_base[(uint32_t)ctx->gpr[3] + KZ3_SP_ALL_UNLOCKED_OFF] = 0;
}
static void func_005A3350__lifted(ppu_context* ctx)
{
    n_view++;
    uint32_t t = (uint32_t)ctx->gpr[3];
    wrf(t + 0x9C, (float)ctx->fpr[1]);
    wrf(t + 0xA0, (float)ctx->fpr[2]);
}
static void func_006861B0__lifted(ppu_context* ctx)
{
    n_move++;
    uint32_t t = (uint32_t)ctx->gpr[3];
    wrf(t + 0xFC, (float)ctx->fpr[1]);
    wrf(t + 0xF8, (float)ctx->fpr[2]);
}
static void func_00964880__lifted(ppu_context* ctx)
{
    n_def++;
    vm_base[(uint32_t)ctx->gpr[3] + KZ3_SIXAXIS_OFF] = 1;
    vm_base[(uint32_t)ctx->gpr[3] + 0xB8] = 1;
}
static void func_009652DC__lifted(ppu_context* ctx)
{
    n_pad++;
    vm_base[(uint32_t)ctx->gpr[3] + KZ3_SIXAXIS_OFF] = 1;
    vm_base[(uint32_t)ctx->gpr[3] + 0xA2] = 7;
}
static void func_009703A0__lifted(ppu_context* ctx)
{
    n_load++;
    vm_base[(uint32_t)ctx->gpr[4] + KZ3_SIXAXIS_OFF] = 1;
}
void kz3_mouse_look_pre(void* ctx) { (void)ctx; }   /* rsx patch 2069 lives in port/kz3/main.cpp */
KZ3_HOOK_SP_UNLOCK(004C2F08)
KZ3_HOOK_TRACE_VIEW(005A3350)
KZ3_HOOK_TRACE_MOVE(006861B0)
KZ3_HOOK_SIXAXIS_DEFAULT(00964880)
KZ3_HOOK_SIXAXIS_PADTYPE(009652DC)
KZ3_HOOK_SIXAXIS_LOAD(009703A0)

/* ---- pass 43 test aids: a player list at MGR (u32 -> list object), two players: local P1 (entity ENT1), bot P2
 * (entity ENT2, +0x71 set), one enemy entity ENT3 not in the list. Stub bodies store what the game's code would. ---- */
#define MGR 0x80000u
#define LIST 0x80100u
#define PDATA 0x80200u
#define P1 0x80300u
#define P2 0x80400u
#define ENT1 0x81000u
#define ENT2 0x82000u
#define ENT3 0x83000u
#define DES1 0x8A000u  /* ENT1's Destructibility component: *(ent+0x104), health float +0x24 (RE pass 107) */
#define WPN1 0x84000u
#define WPN3 0x85000u
#define EJ1 0x86000u
#define EJ3 0x87000u
#define ST1 0x88000u
#define ST3 0x89000u
static int n_god, n_own, n_shot, n_str; static uint32_t str_r4;
static void func_005A7158__lifted(ppu_context* ctx) { n_god++; ctx->gpr[3] = 0; }
static void func_0061A480__lifted(ppu_context* ctx) { n_own++; ctx->gpr[3] = 0; }
static void func_0073D4B8__lifted(ppu_context* ctx)
{
    n_shot++;
    uint32_t st = ((uint32_t)vm_base[(uint32_t)ctx->gpr[3] + 0x24] << 24) | ((uint32_t)vm_base[(uint32_t)ctx->gpr[3] + 0x25] << 16) |
                  ((uint32_t)vm_base[(uint32_t)ctx->gpr[3] + 0x26] << 8) | vm_base[(uint32_t)ctx->gpr[3] + 0x27];
    wr32(st + 0x10, 29); /* clip 30 -> 29 */
}
static void func_00016700__lifted(ppu_context* ctx) { n_str++; str_r4 = (uint32_t)ctx->gpr[4]; }
KZ3_HOOK_GOD_MODE(005A7158, MGR, 0x580u)
KZ3_HOOK_INF_AMMO_OWNER(0061A480, MGR, 0x580u)
KZ3_HOOK_INF_MAG(0073D4B8, MGR)
KZ3_HOOK_DEBUG_MENU(00016700, 0x0089A164u, 0x00E6163Cu)
static uint32_t rdv(uint32_t ea)
{
    return ((uint32_t)vm_base[ea] << 24) | ((uint32_t)vm_base[ea + 1] << 16) | ((uint32_t)vm_base[ea + 2] << 8) | vm_base[ea + 3];
}
static void aids_world(void)
{
    wr32(MGR, LIST);
    wr32(LIST + 0x10, 2);
    wr32(LIST + 0x14, PDATA);
    wr32(PDATA, P1);
    wr32(PDATA + 4, P2);
    vm_base[P1 + 0x70] = 1; vm_base[P1 + 0x71] = 0; wr32(P1 + 0x68, ENT1);
    vm_base[P2 + 0x70] = 1; vm_base[P2 + 0x71] = 1; wr32(P2 + 0x68, ENT2);
    wrf(ENT1 + 0x580, 7.0f);   /* the Humanoid death timer: must not be read as health */
    wr32(ENT1 + 0x104, DES1); wrf(DES1 + 0x24, -5.0f);
    wr32(WPN1 + 0xF0, ENT1); wr32(WPN3 + 0xF0, ENT3);
    wr32(EJ1 + 0x1C, WPN1); wr32(EJ1 + 0x24, ST1);
    wr32(EJ3 + 0x1C, WPN3); wr32(EJ3 + 0x24, ST3);
    wr32(ST1 + 0x10, 30); wr32(ST1 + 0xC, 200);
    wr32(ST3 + 0x10, 30); wr32(ST3 + 0xC, 200);
}
static int test_aids(const char* errf, void (*cap_b)(void), int (*cap_e)(const char*, char*, size_t))
{
    (void)errf;
    ppu_context ctx;
    char line[256];
    int f0 = fails;
    aids_world();
    CHECK(kz3_is_local_player_entity(vm_base, MGR, ENT1) == 1, "local player entity");
    CHECK(kz3_is_local_player_entity(vm_base, MGR, ENT2) == 0, "bot player is not local");
    CHECK(kz3_is_local_player_entity(vm_base, MGR, ENT3) == 0, "enemy is not local");
    CHECK(kz3_is_local_player_entity(vm_base, MGR, 0) == 0, "null entity");
    /* all off: lifted answers pass through, no byte changed, no line */
    unsetenv("KZ3_GOD_MODE"); unsetenv("KZ3_INFINITE_AMMO"); unsetenv("KZ3_DEBUG_MENU");
    kz3_gameplay_reset();
    cap_b();
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = ENT1; func_005A7158(&ctx);
    CHECK(ctx.gpr[3] == 0 && n_god == 1, "god off: 0");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = ENT1; func_0061A480(&ctx);
    CHECK(ctx.gpr[3] == 0 && n_own == 1, "ammo off: 0");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = EJ1; func_0073D4B8(&ctx);
    CHECK(rdv(ST1 + 0x10) == 29 && n_shot == 1, "ammo off: clip drops");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[4] = 0xE61018u; ctx.lr = 0x0089A164u; func_00016700(&ctx);
    CHECK(str_r4 == 0xE61018u && n_str == 1, "debug off: false literal kept");
    CHECK(cap_e("[", line, sizeof line) == 0, "off: no lines (%s)", line);
    /* on */
    setenv("KZ3_GOD_MODE", "1", 1); setenv("KZ3_INFINITE_AMMO", "2", 1); setenv("KZ3_DEBUG_MENU", "1", 1);
    kz3_gameplay_reset();
    aids_world();
    cap_b();
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = ENT1; func_005A7158(&ctx);
    CHECK(ctx.gpr[3] == 1, "god on: local player 1");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = ENT3; func_005A7158(&ctx);
    CHECK(ctx.gpr[3] == 0, "god on: enemy 0");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = ENT2; func_005A7158(&ctx);
    CHECK(ctx.gpr[3] == 0, "god on: bot 0");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = ENT1; func_0061A480(&ctx);
    CHECK(ctx.gpr[3] == 1, "ammo on: local 1");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = ENT3; func_0061A480(&ctx);
    CHECK(ctx.gpr[3] == 0, "ammo on: enemy 0");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = EJ1; func_0073D4B8(&ctx);
    CHECK(rdv(ST1 + 0x10) == 30 && rdv(ST1 + 0xC) == 200, "ammo 2: player clip kept (%u)", rdv(ST1 + 0x10));
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = EJ3; func_0073D4B8(&ctx);
    CHECK(rdv(ST3 + 0x10) == 29, "ammo 2: enemy clip drops (%u)", rdv(ST3 + 0x10));
    memset(&ctx, 0, sizeof ctx); ctx.gpr[4] = 0xE61018u; ctx.lr = 0x0089A164u; func_00016700(&ctx);
    CHECK(str_r4 == 0xE6163Cu, "debug on: true literal at the cheat_enabled call (0x%X)", str_r4);
    memset(&ctx, 0, sizeof ctx); ctx.gpr[4] = 0xE61018u; ctx.lr = 0x0089A1D0u; func_00016700(&ctx);
    CHECK(str_r4 == 0xE61018u, "debug on: other callers untouched");
    int ng = cap_e("[god] t=", line, sizeof line);
    CHECK(ng == 1 && strstr(line, "health=-5.00") && strstr(line, "saves=1 "), "god line (%d: %s)", ng, line);
    cap_b(); cap_e("[", line, sizeof line);
    /* ammo 1: magazine drops, owner test still 1 */
    setenv("KZ3_INFINITE_AMMO", "1", 1);
    kz3_gameplay_reset();
    aids_world();
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = EJ1; func_0073D4B8(&ctx);
    CHECK(rdv(ST1 + 0x10) == 29, "ammo 1: magazine still drops");
    unsetenv("KZ3_GOD_MODE"); unsetenv("KZ3_INFINITE_AMMO"); unsetenv("KZ3_DEBUG_MENU");
    kz3_gameplay_reset();
    return fails - f0;
}

/* stderr capture */
static const char* errpath;
static int saved_err = -1;
static void cap_begin(void)
{
    fflush(stderr);
    saved_err = dup(2);
    int fd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(fd, 2);
    close(fd);
}
static int cap_end(const char* needle, char* line, size_t n) /* lines containing needle; line = the first */
{
    fflush(stderr);
    dup2(saved_err, 2);
    close(saved_err);
    FILE* f = fopen(errpath, "r");
    int cnt = 0;
    char buf[512];
    if (line) line[0] = 0;
    while (f && fgets(buf, sizeof buf, f))
        if (strstr(buf, needle)) {
            if (!cnt && line) snprintf(line, n, "%s", buf);
            cnt++;
        }
    if (f) fclose(f);
    return cnt;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: t ERRFILE\n");
        return 2;
    }
    errpath = argv[1];
    vm_base = calloc(1, 0x100000);
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    char line[512];

    /* 1. env parsing */
    unsetenv("KZ3_T");
    CHECK(kz3_env_on("KZ3_T") == 0, "unset");
    setenv("KZ3_T", "", 1);
    CHECK(kz3_env_on("KZ3_T") == 0, "empty");
    setenv("KZ3_T", "0", 1);
    CHECK(kz3_env_on("KZ3_T") == 0, "zero");
    setenv("KZ3_T", "no", 1);
    CHECK(kz3_env_on("KZ3_T") == 0, "no");
    setenv("KZ3_T", "1", 1);
    CHECK(kz3_env_on("KZ3_T") == 1, "one");
    setenv("KZ3_T", "2", 1);
    CHECK(kz3_env_on("KZ3_T") == 1, "two");

    /* 2. inert path: both switches off (the .sh runs us with them unset) */
    kz3_gameplay_reset();
    cap_begin();
    ctx.gpr[3] = PROFILE;
    vm_base[PROFILE + KZ3_SP_ALL_UNLOCKED_OFF] = 0x55;
    func_004C2F08(&ctx);
    CHECK(n_ctor == 1 && vm_base[PROFILE + KZ3_SP_ALL_UNLOCKED_OFF] == 0, "off: ctor ran, byte stays 0 (%u)",
          vm_base[PROFILE + KZ3_SP_ALL_UNLOCKED_OFF]);
    ctx.gpr[3] = CTRL;
    ctx.fpr[1] = 1.5;
    ctx.fpr[2] = -0.25;
    wr32(CTRL + 0x20, ENTITY);
    func_005A3350(&ctx);
    ctx.gpr[3] = MOVE;
    ctx.fpr[1] = 0.5;
    ctx.fpr[2] = 1.0;
    func_006861B0(&ctx);
    CHECK(n_view == 1 && n_move == 1, "off: lifted bodies ran");
    CHECK(kz3_trace_emit(100.0, stderr) == 0, "off: emit returns 0");
    int n = cap_end("[", line, sizeof line);
    CHECK(n == 0, "off: no log line, got %d: %s", n, line);
    CHECK(kz3_sp_unlock_enabled() == 0 && g_kz3_trace == 0, "off: cached flags 0/0 (%d/%d)", kz3_sp_unlock_enabled(),
          g_kz3_trace);

    /* 3. SP unlock on: byte written, logged once */
    setenv("KZ3_SP_UNLOCK", "1", 1);
    kz3_gameplay_reset();
    cap_begin();
    ctx.gpr[3] = PROFILE;
    func_004C2F08(&ctx);
    func_004C2F08(&ctx);
    n = cap_end("[sp-unlock]", line, sizeof line);
    CHECK(vm_base[PROFILE + KZ3_SP_ALL_UNLOCKED_OFF] == 1, "on: byte = 1");
    CHECK(n == 1 && strstr(line, "0x00020000") && strstr(line, "+0x11F18"), "on: one line with ea and offset (%d: %s)",
          n, line);
    CHECK(kz3_sp_unlock_apply(NULL, 0) == 0, "null profile");
    uint8_t buf[KZ3_SP_ALL_UNLOCKED_OFF + 2] = {0};
    CHECK(kz3_sp_unlock_apply(buf, 0) == 1 && buf[KZ3_SP_ALL_UNLOCKED_OFF] == 1 &&
              buf[KZ3_SP_ALL_UNLOCKED_OFF + 1] == 0,
          "apply writes one byte");
    setenv("KZ3_SP_UNLOCK", "0", 1);
    kz3_gameplay_reset();
    memset(buf, 0, sizeof buf);
    CHECK(kz3_sp_unlock_apply(buf, 0) == 0 && buf[KZ3_SP_ALL_UNLOCKED_OFF] == 0, "=0 is off");

    /* 4. Motion Sensor (no gyro) */
    kz3_gameplay_reset();
    CHECK(kz3_pad_has_gyro() == 0, "stub: no gyro");
    cap_begin();
    CHECK(kz3_sixaxis_filter(0) == 0, "filter 0");
    CHECK(kz3_sixaxis_filter(1) == 0 && kz3_sixaxis_filter(1) == 0, "filter 1 -> 0");
    ctx.gpr[3] = PROFILE;
    func_00964880(&ctx);
    CHECK(n_def == 1 && vm_base[PROFILE + KZ3_SIXAXIS_OFF] == 0 && vm_base[PROFILE + 0xB8] == 1,
          "defaults: +0xB6 0, +0xB8 kept (%u/%u)", vm_base[PROFILE + KZ3_SIXAXIS_OFF], vm_base[PROFILE + 0xB8]);
    ctx.gpr[3] = 0;
    ctx.gpr[4] = PROFILE;
    func_009703A0(&ctx);
    CHECK(n_load == 1 && vm_base[PROFILE + KZ3_SIXAXIS_OFF] == 0, "load: +0xB6 0");
    vm_base[PROFILE + KZ3_SIXAXIS_OFF] = 1;
    ctx.gpr[3] = PROFILE;
    func_009652DC(&ctx); /* the player's On stays On */
    CHECK(n_pad == 1 && vm_base[PROFILE + KZ3_SIXAXIS_OFF] == 1 && vm_base[PROFILE + 0xA2] == 7,
          "padtype: pre-call 1 restored, +0xA2 kept");
    vm_base[PROFILE + KZ3_SIXAXIS_OFF] = 0;
    func_009652DC(&ctx);
    CHECK(n_pad == 2 && vm_base[PROFILE + KZ3_SIXAXIS_OFF] == 0, "padtype: pre-call 0 restored over the body's 1");
    n = cap_end("[sixaxis]", line, sizeof line);
    CHECK(n == 1, "sixaxis logged once (%d)", n);
    uint8_t prof[0x100];
    memset(prof, 0xEE, sizeof prof);
    prof[KZ3_SIXAXIS_OFF] = 1;
    CHECK(kz3_sixaxis_fixup(prof) == 0 && prof[KZ3_SIXAXIS_OFF] == 0 && prof[KZ3_SIXAXIS_OFF - 1] == 0xEE &&
              prof[KZ3_SIXAXIS_OFF + 1] == 0xEE,
          "fixup touches one byte");
    CHECK(kz3_sixaxis_fixup(NULL) == 0, "fixup null");

    /* 5. trace limiter and format */
    setenv("KZ3_GAMEPLAY_TRACE", "1", 1);
    kz3_gameplay_reset();
    cap_begin();
    CHECK(kz3_trace_on() == 1, "trace on");
    char* out = NULL;
    size_t outn = 0;
    FILE* m = open_memstream(&out, &outn);
    kz3_trace_view(1.25f, -0.5f, (const float[]){10.f, 20.f, 30.f});
    kz3_trace_move(0.5f, -1.f);
    CHECK(kz3_trace_emit(50.0, m) == 1, "first emits");
    CHECK(kz3_trace_emit(50.5, m) == 0, "0.5 s: no");
    CHECK(kz3_trace_emit(50.999, m) == 0, "0.999 s: no");
    CHECK(kz3_trace_emit(51.0, m) == 1, "1.0 s: yes");
    CHECK(kz3_trace_emit(51.9, m) == 0, "1.9 s: no");
    CHECK(kz3_trace_emit(53.0, m) == 1, "3.0 s: yes");
    fclose(m);
    CHECK(strstr(out, "[trace] t=0.0 yaw=1.250 pitch=-0.500 pos=(10.00,20.00,30.00) move=(0.50,-1.00)\n") == out,
          "line 1 format: %s", out);
    CHECK(strstr(out, "[trace] t=1.0 ") && strstr(out, "[trace] t=3.0 ") && !strstr(out, "partial"),
          "lines 2-3, complete");
    int nl = 0;
    for (char* p = out; (p = strchr(p, '\n')); p++) nl++;
    CHECK(nl == 3, "3 lines (%d)", nl);
    free(out);
    /* the guest readers: hooks with the switch on write the first line at once (the first emit) and read the
     * object: yaw/pitch after the body's stores, position from the entity, move from +0xF8/+0xFC */
    kz3_gameplay_reset();
    wr32(ENTITY, KZ3_VT_SOLDIER_100); /* Soldier vtable: position straight from entity+0x2A0 */
    wrf(ENTITY + 0x2A0, 1.f);
    wrf(ENTITY + 0x2A4, 2.f);
    wrf(ENTITY + 0x2A8, 3.f);
    wrf(ENTITY + 0x30, 9.f); /* the round-33 (wrong) field: must not be read */
    ctx.gpr[3] = MOVE;
    ctx.fpr[1] = 0.75;
    ctx.fpr[2] = -0.25;
    func_006861B0(&ctx);
    ctx.gpr[3] = CTRL;
    ctx.fpr[1] = 2.5;
    ctx.fpr[2] = 0.125;
    func_005A3350(&ctx); /* rate-limited: no second line */
    n = cap_end("[trace] t=", line, sizeof line);
    CHECK(n == 1 && strstr(line, "move=(-0.25,0.75)") && strstr(line, "(partial)"),
          "move hook first: one partial line (%d: %s)", n, line);
    cap_begin();
    kz3_gameplay_reset();
    kz3_trace_move(-0.25f, 0.75f);
    func_005A3350(&ctx);
    n = cap_end("[trace] t=", line, sizeof line);
    CHECK(n == 1 && strstr(line, "yaw=2.500 pitch=0.125 pos=(1.00,2.00,3.00) move=(-0.25,0.75)") &&
              !strstr(line, "partial"),
          "view hook: full line (%s)", line);
    cap_begin();
    kz3_gameplay_reset();
    wr32(CTRL + 0x20, 0);
    func_005A3350(&ctx); /* null entity: no position */
    n = cap_end("[trace] t=", line, sizeof line);
    CHECK(n == 1 && strstr(line, "pos=(0.00,0.00,0.00)") && strstr(line, "partial"), "null entity: partial (%s)", line);
    CHECK(n_vcall == 0, "soldier vtable: vf136 not called (%d)", n_vcall);
    /* another entity class (a vehicle): vf136 through the vtable's slot 0x220 (an OPD), position at result+0x30 */
    cap_begin();
    kz3_gameplay_reset();
    wr32(CTRL + 0x20, ENTITY);
    wr32(ENTITY, VT);
    wr32(VT + KZ3_VF136_SLOT, OPD);
    wr32(OPD, 0x00BDF938);
    wrf(MAT + 0x30, -4.f);
    wrf(MAT + 0x34, 5.f);
    wrf(MAT + 0x38, 6.5f);
    kz3_trace_move(0.f, 0.f);
    func_005A3350(&ctx);
    n = cap_end("[trace] t=", line, sizeof line);
    CHECK(n == 1 && strstr(line, "pos=(-4.00,5.00,6.50)") && !strstr(line, "partial"), "vehicle: vf136 position (%s)", line);
    CHECK(n_vcall == 1 && vcall_ent == ENTITY && vcall_opd == OPD, "vf136 called once with entity and OPD (%d 0x%X 0x%X)",
          n_vcall, vcall_ent, vcall_opd);
    wr32(ENTITY, KZ3_VT_HUMANOID_114);
    kz3_gameplay_reset();
    kz3_trace_move(0.f, 0.f);
    cap_begin();
    func_005A3350(&ctx);
    n = cap_end("[trace] t=", line, sizeof line);
    CHECK(n == 1 && strstr(line, "pos=(1.00,2.00,3.00)") && n_vcall == 1, "1.14 Humanoid vtable: direct read (%s)", line);
    /* no vcall available (NULL): partial line */
    wr32(ENTITY, VT);
    kz3_gameplay_reset();
    cap_begin();
    kz3_trace_view_guest(vm_base, CTRL, NULL, NULL);
    n = cap_end("[trace] t=", line, sizeof line);
    CHECK(n == 1 && strstr(line, "partial") && n_vcall == 1, "no vcall: partial (%s)", line);

    test_aids(argv[1], cap_begin, cap_end);
    printf("%s: %d checks failed\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

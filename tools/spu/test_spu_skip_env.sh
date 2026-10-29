#!/usr/bin/env bash
# Host test (spu rounds 30-31): port/spu/kz3_spu_skip.c resolves KZ3_SPU_SKIP_OVL (ids or lifted image names) and
# KZ3_CULL_AB=skipNN / empty44 (KZ3 gfx job ids, by module EA for 1.00 or 1.14) into runtime image ids and modes,
# and kz3_spu_apply_skip_env() hands them to spu_overlay_set_skip (patch 1043) or spu_overlay_set_entry_hook
# (patch 1044). Self-contained: a stub name table, stub runtime entry points and a stub hook, no game data, no work/.
#   nix develop .#re --command tools/sandbox.sh -- tools/spu/test_spu_skip_env.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
cat > "$T/main.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "kz3_spu_skip.h"
const kz3_spu_image_name kz3_spu_image_names[] = {
    {"elf_00f2d300", 7, 0x00F3D300u},
    {"job_0112af00", 220, 0x0113AF00u},   /* job 44 (1.00) */
    {"job_011a8280", 250, 0x011B8280u},   /* job 51 (1.00) */
    {"job_01224c80", 252, 0x01234C80u},   /* job 51 (1.14 name) */
};
const unsigned kz3_spu_image_names_n = 4u;
static int got[16], ngot, hooked[16], nhook; static void* hook_user[16]; static void* hook_fn[16];
void spu_overlay_set_skip(int id, int on) { if (on && ngot < 16) got[ngot++] = id; }
void spu_overlay_set_entry_hook(int id, int (*fn)(struct spu_context*, int, void*), void* user)
{ if (nhook < 16) { hooked[nhook] = id; hook_fn[nhook] = (void*)fn; hook_user[nhook++] = user; } }
int kz3_cull_empty_entry(struct spu_context* ctx, int image_id, void* user) { (void)ctx; (void)image_id; (void)user; return 0; }
void kz3_ts_interp_install(void) {} /* kz3_ts_interp.c (KZ3_TS_INTERP=1, SPU pass 55): not under test here */
void kz3_stall_diag_install(void) {} /* kz3_stall_diag.c (KZ3_STALL_DIAG=1, SPU pass 63): not under test here */
void kz3_frame_trace_install(void) {} /* kz3_frame_trace.c (KZ3_FRAME_TRACE, SPU pass 66): not under test here */
void kz3_jobhook_install(void) {} /* kz3_jobhook.c (KZ3_JOB_CAPTURE / KZ3_NATIVE_JOB, SPU pass 120): tools/spu/test_r120_jobreplay.sh */
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
int main(void)
{
    int ids[8]; uint8_t modes[8]; unsigned n;
    n = kz3_spu_skip_resolve("job_0112af00, 250,7", NULL, ids, modes, 8);
    CHECK(n == 3 && ids[0] == 220 && ids[1] == 250 && ids[2] == 7, "names+ids: n=%u %d %d %d", n, ids[0], ids[1], ids[2]);
    CHECK(modes[0] == 1 && modes[1] == 1 && modes[2] == 1, "names+ids are skips");
    n = kz3_spu_skip_resolve(NULL, "skip44,skip51", ids, modes, 8);
    CHECK(n == 2 && ids[0] == 220 && ids[1] == 250, "cull 44/51: n=%u %d %d", n, ids[0], ids[1]);
    n = kz3_spu_skip_resolve("220", "skip44", ids, modes, 8);
    CHECK(n == 1 && ids[0] == 220, "dedup: n=%u", n);
    n = kz3_spu_skip_resolve("nosuch,0", "skip99,bogus,empty", ids, modes, 8);
    CHECK(n == 0, "unknown entries rejected: n=%u", n);
    /* 1.14 fallback: a table that only has the 1.14 name of job 51 */
    n = kz3_spu_skip_resolve(NULL, "skip51", ids, modes, 1);  /* max 1: first match (1.00 name) wins */
    CHECK(n == 1 && ids[0] == 250, "max_ids honoured: n=%u %d", n, ids[0]);
    /* pass 31: empty44 is a hook (mode 2); pass 32: empty51 too; empty43 has no layout and is rejected; with
       modes == NULL the empty entries are dropped; a second mention with another mode keeps the first */
    n = kz3_spu_skip_resolve(NULL, "empty44,empty51,empty43,skip51", ids, modes, 8);
    CHECK(n == 2 && ids[0] == 220 && modes[0] == KZ3_CULL_MODE_EMPTY && ids[1] == 250 && modes[1] == KZ3_CULL_MODE_EMPTY,
          "empty44 + empty51 (+ rejected empty43, repeated skip51): n=%u %d/%d %d/%d", n, ids[0], modes[0], ids[1], modes[1]);
    n = kz3_spu_skip_resolve(NULL, "empty44", ids, NULL, 8);
    CHECK(n == 0, "modes==NULL drops empty entries: n=%u", n);
    n = kz3_spu_skip_resolve(NULL, "skip44,empty44", ids, modes, 8);
    CHECK(n == 1 && modes[0] == KZ3_CULL_MODE_SKIP, "first mode wins: n=%u mode=%d", n, modes[0]);
    setenv("KZ3_SPU_SKIP_OVL", "job_011a8280", 1); setenv("KZ3_CULL_AB", "skip44", 1);
    kz3_spu_apply_skip_env();
    CHECK(ngot == 2 && got[0] == 250 && got[1] == 220 && nhook == 0, "apply: ngot=%d %d %d nhook=%d", ngot, got[0], got[1], nhook);
    unsetenv("KZ3_SPU_SKIP_OVL"); setenv("KZ3_CULL_AB", "empty44", 1); ngot = 0;
    kz3_spu_apply_skip_env();
    CHECK(ngot == 0 && nhook == 1 && hooked[0] == 220 && hook_fn[0] == (void*)kz3_cull_empty_entry && hook_user[0] == (void*)(intptr_t)44,
          "apply empty44: ngot=%d nhook=%d id=%d", ngot, nhook, hooked[0]);
    unsetenv("KZ3_SPU_SKIP_OVL"); setenv("KZ3_CULL_AB", "empty51", 1); ngot = 0; nhook = 0;
    kz3_spu_apply_skip_env();
    CHECK(ngot == 0 && nhook == 1 && hooked[0] == 250 && hook_fn[0] == (void*)kz3_cull_empty_entry && hook_user[0] == (void*)(intptr_t)51,
          "apply empty51: ngot=%d nhook=%d id=%d user=%d", ngot, nhook, hooked[0], (int)(intptr_t)hook_user[0]);
    unsetenv("KZ3_SPU_SKIP_OVL"); unsetenv("KZ3_CULL_AB"); ngot = 0; nhook = 0;
    kz3_spu_apply_skip_env();
    CHECK(ngot == 0 && nhook == 0, "no env: ngot=%d nhook=%d", ngot, nhook);
    printf("%s: %d check group(s), %d failure(s)\n", fails ? "FAIL" : "PASS", 13, fails);
    return fails ? 1 : 0;
}
C
gcc -std=gnu11 -Wall -Wextra -O1 -I "$ROOT/port/spu" "$T/main.c" "$ROOT/port/spu/kz3_spu_skip.c" -o "$T/t"
"$T/t" 2>&1

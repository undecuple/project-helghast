/* Patch 2083 host test: the LD_TEX_WRITER page stamp (libs/video/rsx_tex_writer.h). A mark made after an entry's
 * hash began must read as "after" even in the hash's own frame; a mark made before must not; the packed fields
 * round-trip; the sequence compare survives the 32-bit wrap. -DOLD_RULE evaluates patch 2081's rule (newest frame >
 * last-hash frame) on the same cases: it fails the same-frame case, the defect the review reported.
 * Usage: tools/smoke/test_tex_writer.sh [--old] (Linux, no game data). */
#include "rsx_tex_writer.h"
#include <stdio.h>

static int after(uint64_t v, uint32_t hash_frame, uint32_t hash_seq)
{
#ifdef OLD_RULE
    (void)hash_seq; return v != 0 && ld_ws_frame(v) > hash_frame;
#else
    (void)hash_frame; return ld_ws_after(v, hash_seq);
#endif
}

int main(void)
{
    int bad = 0;
    const uint64_t v = ld_ws_pack(0x123456u, 0xDEADBEEFu, 8u);
    if (ld_ws_frame(v) != 0x123456u || ld_ws_seq(v) != 0xDEADBEEFu || ld_ws_src(v) != 8u) { puts("pack"); bad++; }
    /* hash in frame 10 took sequence 100 */
    if (!after(ld_ws_pack(10, 101, 8), 10, 100)) { puts("same-frame mark after the hash read as untracked"); bad++; }
    if (after(ld_ws_pack(10, 99, 2), 10, 100))   { puts("same-frame mark before the hash read as after"); bad++; }
    if (!after(ld_ws_pack(11, 150, 1), 10, 100)) { puts("later-frame mark read as untracked"); bad++; }
    if (after(ld_ws_pack(9, 50, 1), 10, 100))    { puts("earlier mark read as after"); bad++; }
    if (after(0, 10, 100))                       { puts("never-marked page read as after"); bad++; }
    if (!after(ld_ws_pack(12, 5, 1), 12, 0xFFFFFFF0u)) { puts("sequence wrap"); bad++; }
    printf("[test_tex_writer] %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

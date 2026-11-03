/* Host test for the PSO key file header (patch 2141 (from the Killzone 2 port's 6047), libs/video/rsx_pso_keys_hdr.h): a file from another build's shader
 * translator is told apart from an unreadable one and from a good one. */
#include <stdio.h>
#include <string.h>
#include "rsx_pso_keys_hdr.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    const uint64_t id = 0x1234ABCD5678EF90ull;
    ld_pl_hdr h = ld_pl_hdr_make(400, 300, 2, id);
    CHECK(ld_pl_hdr_id(&h) == id);
    CHECK(ld_pl_hdr_check(&h, 400, 300, 2, id) == LD_PL_HDR_OK);
    CHECK(ld_pl_hdr_check(&h, 400, 300, 2, id ^ 1) == LD_PL_HDR_TRANSLATOR);           /* another build */
    CHECK(ld_pl_hdr_check(&h, 400, 300, 2, id ^ (1ull << 40)) == LD_PL_HDR_TRANSLATOR); /* high word counts */
    CHECK(ld_pl_hdr_check(&h, 400, 300, 2, 0) == LD_PL_HDR_TRANSLATOR);                /* a build without an id */
    CHECK(ld_pl_hdr_check(&h, 404, 300, 2, id) == LD_PL_HDR_LAYOUT);
    CHECK(ld_pl_hdr_check(&h, 400, 304, 2, id) == LD_PL_HDR_LAYOUT);
    CHECK(ld_pl_hdr_check(&h, 400, 300, 3, id) == LD_PL_HDR_LAYOUT);
    ld_pl_hdr v = h; v.magic ^= 1; CHECK(ld_pl_hdr_check(&v, 400, 300, 2, id) == LD_PL_HDR_LAYOUT);
    /* a version-2 file (0.1.5 and older: no id, 24-byte header) */
    v = h; v.version = 2; CHECK(ld_pl_hdr_check(&v, 400, 300, 2, id) == LD_PL_HDR_LAYOUT);
    CHECK(sizeof(ld_pl_hdr) == 32);
    /* the build's id round-trips through a written header, byte layout fixed: id_lo at 24, id_hi at 28 */
    {
        ld_pl_hdr b = ld_pl_hdr_make(400, 300, 2, RSX_TRANSLATOR_ID);
        unsigned char raw[sizeof b]; memcpy(raw, &b, sizeof b);
        ld_pl_hdr r; memcpy(&r, raw, sizeof r);
        CHECK(ld_pl_hdr_check(&r, 400, 300, 2, RSX_TRANSLATOR_ID) == LD_PL_HDR_OK);
        CHECK(raw[24] == (unsigned char)RSX_TRANSLATOR_ID && raw[28] == (unsigned char)(RSX_TRANSLATOR_ID >> 32));
#ifdef EXPECT_ID0
        CHECK(RSX_TRANSLATOR_ID == 0ull);                      /* a build that does not define the id */
        CHECK(ld_pl_hdr_check(&h, 400, 300, 2, RSX_TRANSLATOR_ID) == LD_PL_HDR_TRANSLATOR);
#else
        CHECK(RSX_TRANSLATOR_ID == 0x0123456789ABCDEFull);
#endif
    }
    if (fails) { printf("test_pso_keys_hdr: %d failure(s)\n", fails); return 1; }
    printf("test_pso_keys_hdr: ok\n");
    return 0;
}

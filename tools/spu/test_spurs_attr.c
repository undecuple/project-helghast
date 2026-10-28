/* KZ3 patch 1062 host test: the register-to-argument mapping of the HLE _cellSpursAttributeInitialize
 * (NID 0x95180230, libs/spurs/kz3_spurs_attr.h) with KZ3's own two calls (RE pass 104: Spurs creator
 * 0x8A214, li r6 at 0x8A2A0 / 0x8A368; same in 1.14 at 0x8BA90 / 0x8BB58). `--sdk` asserts the SDK
 * order (the default since SPU pass 60); without it the old mapping (KZ3_SPURS_ATTR_SDK=0: r4 = the
 * revision 2 read as nSpus) is asserted. Negative control: `--sdk` with KZ3_SPURS_ATTR_SDK=0 fails. */
#include <stdio.h>
#include <string.h>
#include "kz3_spurs_attr.h"
static int fails;
#define CHECK_EQ(what, a, e) do { long long _a = (long long)(a), _e = (long long)(e); \
    if (_a != _e) { fails++; fprintf(stderr, "FAIL: %s = %lld, expected %lld\n", what, _a, _e); } } while (0)
int main(int argc, char** argv)
{
    int sdk = argc > 1 && strcmp(argv[1], "--sdk") == 0;
    if (!sdk) {   /* the default mapping: KZ3 gets 2 and 2 */
        kz3_spurs_attr_args d = kz3_spurs_attr_map(2, 0x330000, 5, 0x80, 0x258, 0);
        CHECK_EQ("default HiPrio nSpus (r4)", d.nSpus, 2);
        CHECK_EQ("default spuPriority (r5)", d.spuPriority, 0x330000);
        d = kz3_spurs_attr_map(2, 0x330000, 1, 0x81, 0x320, 0);
        CHECK_EQ("default LoPrio nSpus (r4)", d.nSpus, 2);
        printf("test_spurs_attr (default mapping): %d failed\n", fails);
        return fails ? 1 : 0;
    }
    /* HiPrio: call 0x8A2A8: r4=2 (revision) r5=0x330000 (sdk) r6=5 r7=0x80 r8=0x258 r9=0 */
    kz3_spurs_attr_args h = kz3_spurs_attr_map(2, 0x330000, 5, 0x80, 0x258, 0);
    CHECK_EQ("HiPrio nSpus", h.nSpus, 5);
    CHECK_EQ("HiPrio spuPriority", h.spuPriority, 0x80);
    CHECK_EQ("HiPrio ppuPriority", h.ppuPriority, 0x258);
    CHECK_EQ("HiPrio exitIfNoWork", h.exitIfNoWork, 0);
    CHECK_EQ("HiPrio revision", h.revision, 2);
    CHECK_EQ("HiPrio sdkVersion", h.sdkVersion, 0x330000);
    /* LoPrio: call 0x8A370: r6=1 r7=0x81 r8=0x320 */
    kz3_spurs_attr_args l = kz3_spurs_attr_map(2, 0x330000, 1, 0x81, 0x320, 0);
    CHECK_EQ("LoPrio nSpus", l.nSpus, 1);
    CHECK_EQ("LoPrio spuPriority", l.spuPriority, 0x81);
    CHECK_EQ("LoPrio ppuPriority", l.ppuPriority, 0x320);
    /* The SDK clamps nothing here; the public cellSpursAttributeInitialize clamps 1..8 later. A 64-bit
     * register with junk in its upper half must not leak into the 32-bit arguments. */
    kz3_spurs_attr_args j = kz3_spurs_attr_map(0xFFFFFFFF00000002ull, 0x330000, 0xDEADBEEF00000005ull, 0x80, 0x258, 0x100);
    CHECK_EQ("upper-half junk nSpus", j.nSpus, 5);
    CHECK_EQ("upper-half junk exitIfNoWork", j.exitIfNoWork, 0);
    printf("test_spurs_attr: %d failed\n", fails);
    return fails ? 1 : 0;
}

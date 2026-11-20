// Host test for port/kz3/kz3_psarc_attrib.h: the RSX_ATTRIB=1 [psarc] line formats and totals. No game data.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "kz3_psarc_attrib.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("[FAIL] "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main()
{
    char b[600];
    kz3_psarc_attrib_open_line(b, sizeof b, 123456789012LL, "localcacheps3/x.core", 1048576ULL, 4242ULL, 0.25, 12.5, "inflate");
    CHECK(!strcmp(b, "[psarc] open qpc=123456789012 key=localcacheps3/x.core size=1048576 thread=4242 wait_ms=0.250 inflate_ms=12.500 role=inflate"),
          "open line: %s", b);
    kz3_psarc_attrib_open_line(b, sizeof b, 5, "a", 0, 1, 0, 0, "cached");
    CHECK(!strcmp(b, "[psarc] open qpc=5 key=a size=0 thread=1 wait_ms=0.000 inflate_ms=0.000 role=cached"), "cached line: %s", b);

    // ticks to ms at the usual 10 MHz QPC and at the host fallback 1 GHz
    CHECK(fabs(kz3_psarc_ticks_ms(10000000, 10000000) - 1000.0) < 1e-9, "10 MHz");
    CHECK(fabs(kz3_psarc_ticks_ms(1500000, 1000000000) - 1.5) < 1e-9, "1 GHz");
    CHECK(kz3_psarc_ticks_ms(5, 0) == 0.0, "zero frequency must not divide");

    kz3_psarc_attrib_stats s;
    kz3_psarc_attrib_exit_line(b, sizeof b, s, 10000000);
    CHECK(!strcmp(b, "[psarc] exit count=0 inflated=0 wait_ms total=0.000 max=0.000 inflate_ms total=0.000 max=0.000 qpc_hz=10000000"), "empty exit: %s", b);
    kz3_psarc_attrib_add(s, 1.0, 20.0, true);
    kz3_psarc_attrib_add(s, 4.0, 0.0, false);     // a cached open that waited on another thread's inflate
    kz3_psarc_attrib_add(s, 0.5, 35.5, true);
    kz3_psarc_attrib_exit_line(b, sizeof b, s, 10000000);
    CHECK(!strcmp(b, "[psarc] exit count=3 inflated=2 wait_ms total=5.500 max=4.000 inflate_ms total=55.500 max=35.500 qpc_hz=10000000"), "exit: %s", b);


    if (!fails) printf("[test_psarc_attrib] all passed\n");
    return fails ? 1 : 0;
}

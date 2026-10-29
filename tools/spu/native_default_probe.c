/* Spu pass 127 host probe: runs the gate constructor (kz3_native_jobs.c) under the environment it is started with and
 * prints "slot N native|lifted" for the Gfx.LightProbes rows and the count of other native slots. */
#include <stdio.h>
#include <string.h>
#include "kz3_native_jobs.h"
extern int (*g_kz3_native_gate[64])(spu_context*);
int main(void)
{
    int other = 0;
    for (int s = 0; s < 64; s++) {
        const kz3_native_entry* e = kz3_native_find(s);
        if (!e) continue;
        const int on = g_kz3_native_gate[s] != 0;
        if (!strcmp(e->job, "Gfx.LightProbes")) printf("slot %d %s\n", s, on ? "native" : "lifted");
        else other += on;
    }
    printf("other native %d\n", other);
    return 0;
}

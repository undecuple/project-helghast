/* kz3_native_toggle.h -- same-run A/B of native gates (SPU pass 133). Same-run toggle A/B design from the Killzone 2
 * port (SPU pass 22: KZ2_NATIVE_TOGGLE, port/kz2/spu/native_toggle.h, tools/spu/toggle_ab.py); the slot-list parser and
 * the phase rule are KZ2's, adapted to KZ3's gate table (re/spu/native_gates.tsv, kz3_native_jobs.c).
 *
 * KZ3_NATIVE_TOGGLE=<slots> flips those gate slots between the lifted code and the native kernel every
 * KZ3_NATIVE_TOGGLE_PERIOD seconds (default 2, 0.1 to 60) of ONE run, so a job's two versions meet the same scene,
 * load and heat. Opt-in diagnostic: unset = the gates are installed exactly as before (no extra code on the call path).
 * Needs KZ3_WAIT_ACCT (the phases are counted from KZ3_PROF's time zero, the base of KZ3_FRAME_TRACE, so
 * tools/spu/toggle_ab.py can put every traced job call into its phase).
 *
 * Phase k = floor(t / period), t = time since that base: even = lifted, odd = native. A toggled slot takes the
 * toggle's gate whatever its default or switch says. Slots: a comma list of numbers and ranges ("0,4,6", "4-6"). */
#ifndef KZ3_NATIVE_TOGGLE_H
#define KZ3_NATIVE_TOGGLE_H

#include <stdlib.h>

/* Marks the slots of `list` in mask[0..nslots); returns how many were marked, -1 on a malformed list (nothing kept
 * from it then: the caller treats that as "toggle off"). */
static inline int kz3_native_toggle_parse(const char* list, unsigned char* mask, int nslots)
{
    int n = 0;
    for (int i = 0; i < nslots; i++) mask[i] = 0;
    const char* p = list;
    while (p && *p) {
        char* e;
        const long a = strtol(p, &e, 10);
        if (e == p) goto bad;
        long b = a;
        p = e;
        if (*p == '-') {
            b = strtol(p + 1, &e, 10);
            if (e == p + 1) goto bad;
            p = e;
        }
        if (a < 0 || b < a || b >= nslots) goto bad;
        for (long s = a; s <= b; s++)
            if (!mask[s]) {
                mask[s] = 1;
                n++;
            }
        if (*p == ',')
            p++;
        else if (*p)
            goto bad;
    }
    return n;
bad:
    for (int i = 0; i < nslots; i++) mask[i] = 0;
    return -1;
}

/* Phase index of time t_ns since the base (period_ns > 0); odd = native. */
static inline long kz3_native_toggle_phase(unsigned long long t_ns, unsigned long long period_ns)
{
    return (long)(t_ns / period_ns);
}

/* KZ3_NATIVE_TOGGLE_PERIOD text -> period in ns; 2 s when unset or out of 0.1..60. */
static inline unsigned long long kz3_native_toggle_period_ns(const char* s)
{
    const double d = (s && *s) ? atof(s) : 2.0;
    return (unsigned long long)((d >= 0.1 && d <= 60.0 ? d : 2.0) * 1e9);
}

#endif

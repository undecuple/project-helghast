/* Killzone 3 port: line formats and totals for the RSX_ATTRIB=1 psarc attribution lines (stutter Task 0, pass 165 follow-up).
 * Pure C++ (no OS calls: the caller passes QPC ticks, the frequency and the thread id), so tools/smoke builds it on the host
 * (tools/smoke/test_psarc_attrib.sh). port/kz3/psarc_vfs.cpp reads the clock and the switch.
 *
 *   [psarc] open qpc=<ticks> key=<name> size=<bytes> thread=<id> wait_ms=<..> inflate_ms=<..> role=<inflate|cached>
 *     qpc        raw QueryPerformanceCounter at the END of the call (the same clock as scratch/present_qpc_<pid>.csv and the
 *                qpc= of the [pso-await] / [pump] gap lines; start of the call = qpc - (wait_ms + inflate_ms) in ticks)
 *     key        the archive entry's lowercased name
 *     size       the entry's inflated size in bytes
 *     thread     the calling thread's OS id
 *     wait_ms    time spent waiting for the archive lock (another thread was inflating)
 *     inflate_ms time the archive lock was held for this open: reading and inflating the blocks and writing the cache file
 *                (0 for role=cached)
 *     role       inflate = the entry was not in the cache and was inflated now; cached = already in the cache (logged at the
 *                first open of each key only, every call still counts in the totals)
 *   [psarc] exit count=<n> inflated=<n> wait_ms total=<..> max=<..> inflate_ms total=<..> max=<..> qpc_hz=<hz>
 */
#ifndef KZ3_PSARC_ATTRIB_H
#define KZ3_PSARC_ATTRIB_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct kz3_psarc_attrib_stats {
    unsigned long long count = 0, inflated = 0;
    double wait_total_ms = 0, wait_max_ms = 0, inflate_total_ms = 0, inflate_max_ms = 0;
};

inline double kz3_psarc_ticks_ms(long long ticks, long long hz) { return hz > 0 ? (double)ticks * 1000.0 / (double)hz : 0.0; }

/* One archive resolution: wait_ms for the lock, inflate_ms under it (0 for a cached file). */
inline void kz3_psarc_attrib_add(kz3_psarc_attrib_stats& s, double wait_ms, double inflate_ms, bool inflated)
{
    s.count++;
    if (inflated) s.inflated++;
    s.wait_total_ms += wait_ms;
    if (wait_ms > s.wait_max_ms) s.wait_max_ms = wait_ms;
    s.inflate_total_ms += inflate_ms;
    if (inflate_ms > s.inflate_max_ms) s.inflate_max_ms = inflate_ms;
}

inline int kz3_psarc_attrib_open_line(char* buf, size_t cap, long long qpc, const char* key, unsigned long long size,
                                      unsigned long long thread, double wait_ms, double inflate_ms, const char* role)
{
    return snprintf(buf, cap, "[psarc] open qpc=%lld key=%s size=%llu thread=%llu wait_ms=%.3f inflate_ms=%.3f role=%s",
                    qpc, key, size, thread, wait_ms, inflate_ms, role);
}

inline int kz3_psarc_attrib_exit_line(char* buf, size_t cap, const kz3_psarc_attrib_stats& s, long long hz)
{
    return snprintf(buf, cap, "[psarc] exit count=%llu inflated=%llu wait_ms total=%.3f max=%.3f inflate_ms total=%.3f max=%.3f qpc_hz=%lld",
                    s.count, s.inflated, s.wait_total_ms, s.wait_max_ms, s.inflate_total_ms, s.inflate_max_ms, hz);
}
#endif

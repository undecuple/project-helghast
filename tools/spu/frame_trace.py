#!/usr/bin/env python3
"""Per-frame timeline from a KZ3_FRAME_TRACE file (port/spu/kz3_frame_trace.c, runtime patch 1085; SPU pass 66).

    python3 tools/spu/frame_trace.py work/proton/RUN/app/frametrace.bin [--release-img 244] [--frames N]

Prints, averaged per frame (flip to flip): the walker's GCM park and what released it (the job call that ended last
before the release, normally job 36 Gfx.Post.Unlock = image 244), how many SPU threads ran a job during the park,
per-SPU-thread job / policy-module / kernel-sleep time, the job table (calls, wall per call, ms per frame), the
hand-off gaps (job end -> next job start on one SPU thread; last job end -> park release), and a heuristic critical
path walked back from each park release (the latest job that ended before the current one started, any SPU thread).
"""
import argparse
import collections
import struct
import sys

KIND = {0: "hle", 1: "sc", 2: "usleep", 3: "label", 4: "ksleep", 5: "pmrun", 6: "job", 7: "kgap", 8: "kskip",
        9: "park", 32: "flip", 33: "jobcall", 34: "ucmd-queue", 35: "ucmd-deliver"}


def load(path):
    with open(path, "rb") as f:
        b = f.read()
    if b[:8] != b"KZ3FTRC1":
        sys.exit("not a KZ3FTRC1 file")
    nn, ne = struct.unpack_from("<II", b, 8)
    off = 24
    names = []
    for i in range(nn):
        raw = b[off:off + 40].split(b"\0")[0].decode("latin-1")
        names.append(raw or "thr%d" % i)
        off += 40
    ev = []
    for i in range(ne):
        t0, t1, site, kind, thr = struct.unpack_from("<QQIBB", b, off + 24 * i)
        ev.append((t0, t1, site, kind, thr))
    return names, ev


def overlap(a0, a1, b0, b1):
    return max(0, min(a1, b1) - max(a0, b0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--release-img", type=int, default=244, help="image of the park-releasing job (job 36 = 244)")
    ap.add_argument("--top", type=int, default=15)
    a = ap.parse_args()
    names, ev = load(a.trace)
    ms = 1e6
    flips = sorted(e[1] for e in ev if e[3] == 32)
    parks = sorted((e[0], e[1], e[4]) for e in ev if e[3] == 9)
    jobs = sorted((e[0], e[1], e[2] & 0xFFFF, e[2] >> 16, e[4]) for e in ev if e[3] == 33)
    if len(flips) < 3:
        sys.exit("fewer than 3 flips in the trace")
    f0, f1 = flips[0], flips[-1]
    nf = len(flips) - 1
    fr = (f1 - f0) / nf / ms
    print("events %d, threads %d, frames %d (%.1f-%.1f s), %.2f ms per frame = %.2f flips/s" %
          (len(ev), len(names), nf, f0 / 1e9, f1 / 1e9, fr, 1000.0 / fr))

    def per_frame(ns):
        return ns / nf / ms

    # threads
    spu_thr = sorted({j[4] for j in jobs})
    by_thr = collections.defaultdict(lambda: collections.Counter())
    cnt_thr = collections.defaultdict(lambda: collections.Counter())
    for t0, t1, site, kind, thr in ev:
        if kind in (32, 33, 34, 35, 6, 7, 8):
            continue
        d = overlap(t0, t1, f0, f1)
        key = KIND.get(kind, str(kind))
        if kind in (2, 3):
            key += ":%X" % site
        by_thr[thr][key] += d
        cnt_thr[thr][key] += 1
    for t0, t1, img, jn, thr in jobs:
        by_thr[thr]["jobcall"] += overlap(t0, t1, f0, f1)
        cnt_thr[thr]["jobcall"] += 1
    print("\n## per thread, ms per frame (calls per frame)")
    for thr in sorted(by_thr, key=lambda t: names[t]):
        top = sorted(by_thr[thr].items(), key=lambda kv: -kv[1])[:6]
        if not top or per_frame(top[0][1]) < 0.5:
            continue
        print("%-28s %s" % (names[thr][:28], "  ".join("%s %.1f (%.0f)" % (k, per_frame(v), cnt_thr[thr][k] / nf)
                                                         for k, v in top if per_frame(v) >= 0.2)))

    # jobs
    print("\n## jobs (all SPU threads): image, calls per frame, us per call (mean / p90), SPU-ms per frame")
    js = collections.defaultdict(list)
    for t0, t1, img, jn, thr in jobs:
        if f0 <= t1 <= f1:
            js[img].append(t1 - t0)
    tot = sum(sum(v) for v in js.values())
    print("total job calls %.0f per frame, %.1f SPU-ms per frame over %d SPU threads" %
          (sum(len(v) for v in js.values()) / nf, per_frame(tot), len(spu_thr)))
    for img, v in sorted(js.items(), key=lambda kv: -sum(kv[1]))[:a.top]:
        v.sort()
        print("  img %3d  %6.1f/frame  %7.1f us  p90 %7.1f us  %6.2f ms/frame" %
              (img, len(v) / nf, sum(v) / len(v) / 1e3, v[int(len(v) * 0.9)] / 1e3, per_frame(sum(v))))

    # hand-off gaps on one SPU thread
    print("\n## hand-off: job end -> next job start on the same SPU thread (us)")
    gaps = []
    per = collections.defaultdict(list)
    for t0, t1, img, jn, thr in jobs:
        per[thr].append((t0, t1, img))
    for thr, v in per.items():
        v.sort()
        for (a0, a1, ai), (b0, b1, bi) in zip(v, v[1:]):
            if f0 <= a1 <= f1:
                gaps.append(b0 - a1)
    gaps.sort()
    if gaps:
        q = lambda p: gaps[min(len(gaps) - 1, int(len(gaps) * p))] / 1e3
        print("  n=%d  p10 %.1f  p50 %.1f  p90 %.1f  p99 %.1f  max %.1f; gaps < 20 us sum %.1f ms/frame, >= 20 us "
              "sum %.1f ms/frame" % (len(gaps), q(.1), q(.5), q(.9), q(.99), gaps[-1] / 1e3,
                                     per_frame(sum(g for g in gaps if g < 20000)),
                                     per_frame(sum(g for g in gaps if g >= 20000))))

    # parks
    print("\n## walker parks (GCM FIFO park until the SPU patches it)")
    rel = []
    conc = []
    for p0, p1, wthr in parks:
        if not (f0 <= p1 <= f1):
            continue
        ended = [j for j in jobs if j[1] <= p1 + 50000]  # 50 us slack: the walker notes after it saw the patch
        last = max(ended, key=lambda j: j[1]) if ended else None
        rj = [j for j in jobs if j[2] == a.release_img and p0 - 5e6 <= j[1] <= p1 + 50000]
        r = max(rj, key=lambda j: j[1]) if rj else None
        busy = sum(overlap(j[0], j[1], p0, p1) for j in jobs) / max(1, p1 - p0)
        conc.append(busy)
        rel.append((p0, p1, last, r))
    if rel:
        dur = sorted((p1 - p0) for p0, p1, _, _ in rel)
        print("  parks %d (%.2f per frame), ms per park mean %.2f p50 %.2f max %.2f = %.1f ms per frame" %
              (len(rel), len(rel) / nf, sum(dur) / len(dur) / ms, dur[len(dur) // 2] / ms, dur[-1] / ms,
               per_frame(sum(dur))))
        print("  SPU threads running a job during a park: mean %.2f of %d" % (sum(conc) / len(conc), len(spu_thr)))
        lat = [p1 - r[1] for p0, p1, _, r in rel if r]
        st = [r[0] - p0 for p0, p1, _, r in rel if r]
        dr = [r[1] - r[0] for p0, p1, _, r in rel if r]
        if lat:
            lat.sort()
            st.sort()
            print("  release job img %d found for %d/%d parks: its start after park start p50 %.2f ms (min %.2f max "
                  "%.2f), its wall p50 %.0f us, its end -> park release p50 %.0f us (max %.0f)" %
                  (a.release_img, len(lat), len(rel), st[len(st) // 2] / ms, st[0] / ms, st[-1] / ms,
                   sorted(dr)[len(dr) // 2] / 1e3, lat[len(lat) // 2] / 1e3, lat[-1] / 1e3))
        lastimg = collections.Counter(l[2] for _, _, l, _ in rel if l)
        print("  last job call to end before the release: %s" % ", ".join("img %d x%d" % kv
                                                                         for kv in lastimg.most_common(5)))

    # GCM user command (the frame's job-36 kick): walker queue -> PPU pump delivery -> release job start -> park end
    uq = sorted((e[1], e[2]) for e in ev if e[3] == 34)
    ud = sorted((e[1], e[2], e[4]) for e in ev if e[3] == 35)
    if uq and ud:
        print("\n## GCM user command hand-off (walker queue -> PPU pump runs the handler -> release job)")
        import bisect as _b
        dts = [d[0] for d in ud]
        lat, thr_c, to_job, park_after = [], collections.Counter(), [], []
        rstarts = sorted(j[0] for j in jobs if j[2] == a.release_img)
        pstarts = sorted(p[0] for p in parks)
        for tq, cmd in uq:
            i = _b.bisect_left(dts, tq)
            if i >= len(ud):
                continue
            lat.append(ud[i][0] - tq)
            thr_c[names[ud[i][2]]] += 1
            k = _b.bisect_left(rstarts, ud[i][0])
            if k < len(rstarts):
                to_job.append(rstarts[k] - ud[i][0])
            k = _b.bisect_left(pstarts, tq)
            if k < len(pstarts):
                park_after.append(pstarts[k] - tq)
        q = lambda v, p: sorted(v)[min(len(v) - 1, int(len(v) * p))] / 1e3 if v else float("nan")
        print("  queued %d (%.2f per frame), delivered %d; queue -> deliver us p10 %.0f p50 %.0f p90 %.0f max %.0f" %
              (len(uq), len(uq) / nf, len(ud), q(lat, .1), q(lat, .5), q(lat, .9), q(lat, 1)))
        print("  delivering threads: %s" % ", ".join("%s x%d" % kv for kv in thr_c.most_common(4)))
        print("  deliver -> next release job (img %d) start us p50 %.0f p90 %.0f; queue -> next park start us p50 %.0f"
              % (a.release_img, q(to_job, .5), q(to_job, .9), q(park_after, .5)))

    # critical path, heuristic: from the release job back through the latest-ending earlier job
    print("\n## heuristic critical path back from the release job (median over parks; each step = the job that ended "
          "last before the current one started, any SPU thread)")
    jobs_by_end = sorted(jobs, key=lambda j: j[1])
    ends = [j[1] for j in jobs_by_end]
    import bisect
    chains = []
    for p0, p1, _, r in rel:
        if not r:
            continue
        cur = r
        chain = [cur]
        for _ in range(12):
            i = bisect.bisect_right(ends, cur[0]) - 1
            if i < 0:
                break
            prev = jobs_by_end[i]
            chain.append(prev)
            cur = prev
        chains.append((p0, chain))
    if chains:
        prof = collections.defaultdict(list)
        for p0, ch in chains:
            for k, j in enumerate(ch):
                prof[k].append((j[2], (j[1] - j[0]) / 1e3, (ch[k - 1][0] - j[1]) / 1e3 if k else 0.0,
                                (j[0] - p0) / ms))
        for k in sorted(prof):
            v = prof[k]
            c = collections.Counter(x[0] for x in v).most_common(3)
            w = sorted(x[1] for x in v)
            g = sorted(x[2] for x in v)
            s = sorted(x[3] for x in v)
            print("  step -%d: img %s; wall p50 %.0f us; gap to next step p50 %.0f us; start vs park start p50 %.2f ms"
                  % (k, "/".join("%d(%d)" % kv for kv in c), w[len(w) // 2], g[len(g) // 2], s[len(s) // 2]))


if __name__ == "__main__":
    main()

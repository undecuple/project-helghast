#!/usr/bin/env python3
"""Boot metrics for regression checks (the maintainers' boot check). The boot UI animates, so screenshots alone cannot catch a
regression; these counters can.
    kz3_metrics.py LOG [--alive 0|1] [--out JSON] [--baseline JSON]
Exit 1 if a metric regressed against the baseline (rules below); the caller then keeps the old baseline."""
import argparse, bisect, json, re, sys

COUNTERS = {   # name: (regex counted over the log, rule)  rule: "max" = must not exceed baseline, "min" = must not drop
    "unresolved_imports": (r"\[hle\] unresolved NID", "max"),
    "crash_markers":      (r"DEADCA7|\[null-call\]|GUARDVIOLATION|unhandled exception|EXCEPTION_ACCESS", "max"),
    "shader_failures":    (r"compile failed|fallback draw|pipeline (?:state )?creation failed|PSO failed", "max"),
    "files_opened":       (r"^\[fs\] open '", "min"),     # [psarc] log lines are capped at 64; opens are not
}


# Files KZ3 asks for that are not on the disc (dev override, FIOS cache, optional savedata media): expected misses.
KNOWN_ABSENT = re.compile(r"Game_PS3\.ini|fios/cache\.idx|ICONS/SAVEDATA/(SND0\.AT3|ICON1\.PAM)|savegame/savedata\.dat|levels\.multi_player\.mp_(09|1[0-6])\.leveldescription_|MULTIPLAYER/DLC_BUY_NOW\.GUM", re.I)

# Gameplay counters (the gameplay test plan B2): counted from lines the runtime already prints, informational only
# (no regression rule; compare() ignores them, so an old baseline JSON stays valid).
GAMEPLAY = {
    "script_events":  r"^\[cellPad\] (?:script|PAD_SCRIPT) t=\S+ (?:press|sticks)",   # v2 "script t=", v1 "PAD_SCRIPT t="
    "script_analog":  r"^\[cellPad\] script t=\S+ sticks",                            # analog segments (patch 3006)
    "script_dropped": r"^\[cellPad\] PAD_SCRIPT: WARNING",                            # dropped or unparsable events
    "autosaves":      r"^\[cellSaveData\] UserAutoSave\(",                            # patch 3001 handler
    "autoloads":      r"^\[cellSaveData\] UserAutoLoad\(",
    "hardexit":       r"^\[HardExit\]",                                               # kz3_log.h (RE pass 64)
    "link_miss":      r"^\[link-miss\]",
}
# A level's lumps: `[fs] open '.../LocalCachePS3/lumps/levels.single_player.<dir>.<lump>'` (case varies on the disc).
# The first such open marks the level start; autosaves and .gum pages after it are "in level".
LEVEL_LUMP = re.compile(r"^\[fs\] open '[^']*?lumps/levels\.single_player\.([a-z0-9_]+)\.([^']+)'", re.I | re.M)
GUM_PAGE = re.compile(r"^\[fs\] open '[^']*?/GUM/([^']+\.gum)'", re.I | re.M)
PAUSE_PAGE = re.compile(r"campaignoptions\.gum|pause", re.I)   # the pause menu page (the gameplay test plan T7)
FLIP_LINE = re.compile(r"^\[flip\] (\d+) at (\d+) ms", re.M)
# Frame pacing (next step 9b3b, RSX pass 68): one line per 10 s window of per-frame present intervals (RSX_PACE, default on).
PACE_LINE = re.compile(r"^\[pace\] t=(\d+) n=(\d+) p50=([\d.]+) p99=([\d.]+) max=([\d.]+) sd=([\d.]+) low1=([\d.]+)", re.M)
LEVEL_MIN_MS = 20000   # lump opens before this run time are the boot scan, not the level (needs GCM_FLIPCOUNT=1)


def fps(flips, window=None):
    """Frame rate from the sampled `[flip] N at T ms` lines (GCM_FLIPCOUNT=1; cumulative count, not every flip is
    logged). Informational, no regression rule yet (the performance probe sets the bar):
      fps_avg      over the whole time the game was presenting (first to last logged flip),
      fps_last10s  over the 10 s before presenting ended (the screen the run ended on),
      fps_min1s    the slowest 1 s window while presenting (stutters, loads),
      fps_window   over `window` = (from_s, to_s) when given (--window A,B: in-level fps for the gameplay tests).
    The cumulative count is interpolated linearly between logged lines."""
    pts = sorted({ms: n for n, ms in flips}.items())          # (ms, count), one per timestamp
    pts = [(ms, n) for ms, n in pts if ms > 0] or pts         # drop the t=0 burst of setup flips
    if len(pts) < 2 or pts[-1][0] - pts[0][0] < 1000:
        return {"fps_avg": 0.0, "fps_last10s": 0.0, "fps_min1s": 0.0, **({"fps_window": 0.0} if window else {})}
    def count(t):
        for (t0, n0), (t1, n1) in zip(pts, pts[1:]):
            if t0 <= t <= t1:
                return n0 + (n1 - n0) * (t - t0) / max(t1 - t0, 1)
        return pts[-1][1] if t > pts[-1][0] else pts[0][1]
    t0, t1 = pts[0][0], pts[-1][0]
    rate = lambda a, b: (count(b) - count(a)) * 1000.0 / (b - a)
    wins = [rate(t, t + 1000) for t in range(t0, t1 - 999, 500)]
    r = {"fps_avg": round(rate(t0, t1), 1),
         "fps_last10s": round(rate(max(t0, t1 - 10000), t1), 1),
         "fps_min1s": round(min(wins), 1) if wins else 0.0}
    if window:
        a, b = max(t0, int(window[0] * 1000)), min(t1, int(window[1] * 1000))
        r["fps_window"] = round(rate(a, b), 1) if b - a >= 1000 else 0.0
    return r


def pace(lines, window=None):
    """Frame pacing from the `[pace] t=T n=N p50= p99= max= sd= low1=` lines (one per ~10 s window ending at T ms; times
    in ms). Over the windows that end inside `window` (end in [from + 10 s, to]), or all of them:
      fps_low1pct   frame-weighted mean of the windows' 1 % low fps (1000 / mean of the slowest 1 % of frame times),
      frametime_sd  pooled frame-time standard deviation in ms (each window's mean frame time = its length / N).
    Record only: lower frametime_sd = steadier. Empty dict when the log has no [pace] lines (older builds)."""
    ws = sorted((int(t), int(n), float(sd), float(lo)) for t, n, _, _, _, sd, lo in lines)
    sel, prev = [], None
    for t, n, sd, lo in ws:
        dur = t - prev if prev is not None else min(t, 10000)
        prev = t
        if n > 0 and dur > 0 and (not window or window[0] * 1000 + 10000 <= t <= window[1] * 1000):
            sel.append((n, dur / n, sd, lo))
    if not sel:
        return {}
    N = sum(n for n, *_ in sel)
    mean = sum(n * mu for n, mu, _, _ in sel) / N
    var = sum(n * (sd * sd + mu * mu) for n, mu, sd, _ in sel) / N - mean * mean
    return {"fps_low1pct": round(sum(n * lo for n, _, _, lo in sel) / N, 1), "frametime_sd": round(max(var, 0.0) ** 0.5, 2)}


def blocks(log, size=32 << 20):
    """The log in newline-aligned blocks: every pattern here matches within one line, so the counts equal a whole-file
    scan while memory stays at ~2 blocks (level logs reach 0.2-0.4 GB; reading them whole took ~3x their size)."""
    with open(log, encoding="latin-1") as f:
        rest = ""
        while True:
            b = f.read(size)
            if not b:
                if rest:
                    yield rest
                return
            b = rest + b
            cut = b.rfind("\n") + 1
            if cut == 0:
                rest = b
                continue
            yield b[:cut]
            rest = b[cut:]


def measure(log, alive, window=None):
    m = {k: 0 for k in COUNTERS}
    failed, flips, ovl, paces = set(), [], set(), []
    g = {k: 0 for k in GAMEPLAY}
    lumps, dirs, in_level = set(), [], False
    g["autosaves_in_level"] = g["gum_pages_in_level"] = g["pause_pages"] = g["level_start_ms"] = 0
    last_ms = 0
    for t in blocks(log):
        for k, (p, _) in COUNTERS.items():
            m[k] += len(re.findall(p, t, re.M))
        for k, p in GAMEPLAY.items():
            g[k] += len(re.findall(p, t, re.M))
        failed.update(re.findall(r"open FAIL(?:ED)?:? '?([^' (]+)", t))
        ovl.update(re.findall(r"resident ovl image (\d+)", t))
        # Level start = the first level lump open after LEVEL_MIN_MS of run time (by the last `[flip] N at T ms` line
        # before it: the game opens every lump of the level once at boot, 1-9 s into the run in the logs seen, and the
        # real load follows the menu at 60 s or later). What follows it (this block and the later ones) is in-level.
        fl = [(mm.start(), int(mm.group(2))) for mm in FLIP_LINE.finditer(t)]
        first = None
        if not in_level:
            for mm in LEVEL_LUMP.finditer(t):
                i = bisect.bisect([pos for pos, _ in fl], mm.start())
                if (fl[i - 1][1] if i else last_ms) >= LEVEL_MIN_MS:
                    first, in_level = mm, True
                    g["level_start_ms"] = fl[i - 1][1] if i else last_ms
                    break
        if fl:
            last_ms = max(last_ms, fl[-1][1])
        tail = t[first.start():] if first else (t if in_level else "")
        if tail:
            g["autosaves_in_level"] += len(re.findall(GAMEPLAY["autosaves"], tail, re.M))
            pages = GUM_PAGE.findall(tail)
            g["gum_pages_in_level"] += len(pages)
            g["pause_pages"] += sum(1 for p in pages if PAUSE_PAGE.search(p))
        for d, name in LEVEL_LUMP.findall(t):
            lumps.add(f"{d}.{name}".lower())
            if d.lower() not in dirs:
                dirs.append(d.lower())
        flips += [(int(a), int(b)) for a, b in FLIP_LINE.findall(t)]
        paces += PACE_LINE.findall(t)
    # Distinct failing paths outside the known-absent set (stdout/stderr interleave, so raw line counts are unreliable).
    # A path with a "[" in it was cut by another thread's "[tag]" line (seen 2026-10-07: ICONS/SAVEDATA garbled); skip it.
    m["unexpected_open_failures"] = len({f for f in failed if "[" not in f and not KNOWN_ABSENT.search(f)})
    m["flips"] = max((f for f, _ in flips), default=0)
    m["last_flip_ms"] = max((ms for _, ms in flips), default=0)   # when presenting stopped (gamma screen ~16-19 s)
    m.update(fps(flips, window))
    m["spu_job_modules"] = len(ovl)
    m["alive"] = alive
    # Gameplay (after the existing keys, so the table and JSON keep their old order):
    m.update(g)
    m["level_lumps"] = len(lumps)                 # distinct levels.single_player.* lumps opened
    m["levels_opened"] = len(dirs)                # distinct level directories (mission_0 also pulls stahl_arms_research lumps)
    m["level_dirs"] = ",".join(dirs)              # in order of first open
    m.update(pace(paces, window))                 # fps_low1pct, frametime_sd (only when the log has [pace] lines)
    return m


def row(name, m):
    """One Markdown table row per run (the gameplay test plan T9: one row per level)."""
    cols = ["alive", "flips", "fps_avg", "fps_window", "level_dirs", "level_lumps", "autosaves_in_level", "script_events",
            "crash_markers", "unresolved_imports", "hardexit", "link_miss"]
    return "| " + " | ".join([name] + [str(m.get(c, "-")) for c in cols]) + " |"


def compare(m, base):
    bad = []
    if base.get("alive") and not m["alive"]:
        bad.append("program no longer running at the end of the run")
    # The flip count before the gamma screen varies ~3x between identical builds (580..1,870), so only a collapse
    # counts; and presenting must not stop much earlier than in the baseline.
    if m["flips"] < 0.25 * base.get("flips", 0):
        bad.append(f"flips {m['flips']} < a quarter of baseline {base['flips']}")
    if m["last_flip_ms"] < 0.6 * base.get("last_flip_ms", 0):
        bad.append(f"presenting stopped at {m['last_flip_ms']} ms, baseline {base['last_flip_ms']} ms")
    if m["unexpected_open_failures"] > base.get("unexpected_open_failures", 1 << 30):
        bad.append(f"unexpected_open_failures {m['unexpected_open_failures']} > {base['unexpected_open_failures']}")
    if m["spu_job_modules"] < base.get("spu_job_modules", 0):
        bad.append(f"spu_job_modules {m['spu_job_modules']} < {base['spu_job_modules']}")
    for k, (_, rule) in COUNTERS.items():
        if k not in base:
            continue
        if rule == "max" and m[k] > base[k]:
            bad.append(f"{k} {m[k]} > baseline {base[k]}")
        # Timing-dependent: 10 % tolerance, 20 % for files_opened (how far a 60 s window gets into loading; quiet-machine
        # runs on the same code spread 418-479, 2026-10-09, so 10 % failed at random).
        tol = 0.8 if k == "files_opened" else 0.9
        if rule == "min" and m[k] < tol * base[k]:
            bad.append(f"{k} {m[k]} < baseline {base[k]}")
    return bad


DRIFT_KEYS = ("fps_avg", "flips")   # speed figures the regression rules let slide (flips only fail at a quarter)
DRIFT_PCT = 15


def drift(m, base, pct=DRIFT_PCT):
    """Speed drift against the previous PASS record: fps_avg or flips more than `pct` % below the
    baseline. A warning, not a failure: the boot UI's flip count varies ~3x between identical builds, so a drop here is
    a hint to look at the host (load average, a build during the run: the record's `host` block) and at the next check,
    not a verdict. Returns one line per drifted key."""
    out = []
    for k in DRIFT_KEYS:
        now, was = m.get(k), base.get(k)
        if not isinstance(now, (int, float)) or not isinstance(was, (int, float)) or was <= 0:
            continue
        change = 100.0 * (now - was) / was
        if change < -pct:
            out.append(f"{k} {now} is {-change:.0f} % below the last PASS ({was})")
    return out


def host_of(run_json):
    """The host conditions runprov recorded for the run (tools/runprov.py: config.host, outcome.host_end,
    outcome.build_lock_held) as one flat block for the check record; missing fields are None."""
    try:
        d = json.load(open(run_json))
    except (OSError, ValueError) as e:
        return {"error": f"{run_json}: {e}"}
    v = lambda sec, name: (d.get(sec, {}).get(name) or {}).get("value")   # noqa: E731
    start, end = v("config", "host") or {}, v("outcome", "host_end") or {}
    return {"load_avg_1m_start": start.get("load_avg_1m"), "load_avg_1m_end": end.get("load_avg_1m"),
            "build_lock_held": v("outcome", "build_lock_held"), "run_json": run_json}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log"); ap.add_argument("--alive", type=int, default=1)
    ap.add_argument("--out"); ap.add_argument("--baseline")
    ap.add_argument("--window", metavar="A,B", help="seconds of run time: adds fps_window over [A,B] (in-level fps)")
    ap.add_argument("--row", metavar="NAME", help="also print one Markdown table row named NAME (gameplay tests)")
    ap.add_argument("--warn-drift", action="store_true",
                    help=f"with --baseline: print a WARN line when fps_avg or flips fall more than {DRIFT_PCT} %% below it (never fails)")
    ap.add_argument("--run-json", metavar="RUN_JSON",
                    help="the run's run.json (tools/runprov.py): its load average and build-lock state go into the record as `host`")
    a = ap.parse_args()
    window = tuple(float(x) for x in a.window.split(",")) if a.window else None
    m = measure(a.log, bool(a.alive), window)
    if a.run_json:
        m["host"] = host_of(a.run_json)
    if a.out:
        json.dump(m, open(a.out, "w"), indent=1)
    base = json.load(open(a.baseline)) if a.baseline else {}
    bad = compare(m, base) if base else []
    print("metric".ljust(20), "now".rjust(8), "baseline".rjust(9))
    for k in m:
        if k == "host":
            continue
        print(k.ljust(20), str(m[k]).rjust(8), str(base.get(k, "-")).rjust(9))
    if "host" in m:
        h = m["host"]
        print(f"host: load {h.get('load_avg_1m_start')} -> {h.get('load_avg_1m_end')}, build lock held: {h.get('build_lock_held')}")
    if a.row:
        print(row(a.row, m))
    if a.warn_drift and base:
        for line in drift(m, base):
            print(f"WARN drift: {line}" + (" (a build held the build lock during this run)" if (m.get("host") or {}).get("build_lock_held") else ""))
    print("REGRESSION:\n  " + "\n  ".join(bad) if bad else ("PASS" if base else "PASS (no baseline yet)"))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Boot metrics for regression checks (the maintainers' boot check). The boot UI animates, so screenshots alone cannot catch a
regression; these counters can.
    kz3_metrics.py LOG [--alive 0|1] [--out JSON] [--baseline JSON]
Exit 1 if a metric regressed against the baseline (rules below); the caller then keeps the old baseline."""
import argparse, bisect, io, json, math, re, statistics, sys
from collections import defaultdict

COUNTERS = {   # name: (regex counted over the log, rule)  rule: "max" = must not exceed baseline, "min" = must not drop
    "unresolved_imports": (r"\[hle\] unresolved NID", "max"),
    "crash_markers":      (r"DEADCA7|\[null-call\]|GUARDVIOLATION|unhandled exception|EXCEPTION_ACCESS|"
                           r"SPU pool did not drain|undrained SPU workers", "max"),
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


# The opt-in benchmark contract is deliberately separate from the boot-check
# output above. In particular, different runtime reporters do not share an
# origin, so their native observations cannot be silently merged.
_BM_NUM = r"(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?"
_BM_VALUE = re.compile(rf"[+-]?{_BM_NUM}\Z")
_BM_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_.:@+-]*\Z")
_BM_KEY = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
_BM_EPS = 0.000001  # milliseconds, only for floating-point boundary comparisons
_BM_MAX_MS = 1_000_000_000_000
_BM_MAX_NATIVE_INT = (1 << 64) - 1
_BM_CRASH = re.compile(r"DEADCA7|\[null-call\]|GUARDVIOLATION|unhandled exception|EXCEPTION_ACCESS|"
                       r"\[CRASH\]|\[HardExit\]|\[fatal\]|segmentation fault|"
                       r"SPU pool did not drain|undrained SPU workers", re.I)
_BM_PAD = re.compile(r"\[cellPad\]\s*PAD_SCRIPT(?:_FILE)?\s*:\s*(?:WARN(?:ING)?|ERROR)\b", re.I)
_BM_EXIT = re.compile(r"\[exit code\s+(-?\d+)\]", re.I)
_BM_NATIVE_NUMBER = re.compile(rf"(?<![A-Za-z0-9_.]){_BM_NUM}")
_BM_FLIP = re.compile(r"^\[flip\]\s+(\d+)\s+at\s+(\d+)\s+ms(?:\s+host=\d+\s+ms)?\s*$")
_BM_PACE = re.compile(
    rf"^\[pace\]\s+t=(\d+)\s+n=(\d+)\s+p50=({_BM_NUM})\s+p99=({_BM_NUM})"
    rf"\s+max=({_BM_NUM})\s+sd=({_BM_NUM})\s+low1=({_BM_NUM})\s*$")
_BM_PERF = re.compile(
    rf"^\[perf\] t=({_BM_NUM})s wall=({_BM_NUM})ms flips=(\d+) fifo-flips=(\d+) presents=(\d+) draws=(\d+)"
    rf" \| walk n=(\d+) ({_BM_NUM})ms methods=(\d+)"
    rf" \| flush n=(\d+) \(present (\d+), ref (\d+), other (\d+)\) ({_BM_NUM})ms gpuwait ({_BM_NUM})ms"
    rf" \| present ({_BM_NUM})ms swap ({_BM_NUM})ms \| refpoll=(\d+)"
    rf" \| usleep gcm n=(\d+) ({_BM_NUM})ms other ({_BM_NUM})ms\s*$")
_BM_PERF_WALK = re.compile(
    rf"^\[perf-walk\] t=({_BM_NUM})s draw ({_BM_NUM})ms"
    rf" \(vtx {_BM_NUM}, pso {_BM_NUM} new \d+/{_BM_NUM}ms, const {_BM_NUM}, tex {_BM_NUM} new \d+/{_BM_NUM}ms\)"
    rf" clear ({_BM_NUM})"
    rf" \| submit ({_BM_NUM})ms chunks (\d+) \| gpuwait rb ({_BM_NUM})ms other ({_BM_NUM})ms"
    rf" \| wb nv0039 (\d+)/({_BM_NUM})ms sync (\d+)/({_BM_NUM})ms store ({_BM_NUM})ms"
    rf" \| m2mf {_BM_NUM}ms \| tex-hash {_BM_NUM}ms"
    rf" \| pso: pre {_BM_NUM} units {_BM_NUM} key {_BM_NUM} find {_BM_NUM} memo-hits \d+"
    r"(?: \| cbv copy \d+ reuse \d+)?\s*$")
_BM_PERF_THR = re.compile(rf"^\[perf-thr\] cpu=({_BM_NUM})ms threads=(\d+):(.*)$")
_BM_GPU_POLL = re.compile(
    rf"^\[perf-gpu\] t=(\d+)s lists (\d+) busy ({_BM_NUM}) ms of ({_BM_NUM})"
    rf" longest ({_BM_NUM}) missed (\d+)\s*$")
_BM_GPU_TIMESTAMP = re.compile(
    rf"^\[perf-gt\] t=(\d+)s frames (\d+) dropped (\d+) gpu ({_BM_NUM}) ms/frame"
    rf" \(copy MB/frame {_BM_NUM}\):"
    rf" draw {_BM_NUM} \({_BM_NUM}/frame\) clear {_BM_NUM} \({_BM_NUM}/frame\)"
    rf" copy {_BM_NUM} \({_BM_NUM}/frame\) tail {_BM_NUM} \({_BM_NUM}/frame\)"
    rf"(?: vbcopy-list {_BM_NUM} \({_BM_NUM}/frame\))?\s*$")
_BM_SPURS_CPU = re.compile(
    rf"^\[spurs-cpu\] (.+?) k(\d+) cpu=({_BM_NUM}) ms/s yields=({_BM_NUM})/s sleeps=({_BM_NUM})/s\s*$")
_BM_SPURS_READY = re.compile(
    r"^\[spurs-ready\] t=(\d+)s store=(\d+)/(\d+) swap=(\d+)/(\d+) cas=(\d+)/(\d+)"
    r" \(calls/byte changed\) per wid:.*$")
_BM_PARK_WAKE = re.compile(
    r"^\[cellGcmSys\] park idle wake: sleeps (\d+), park waits woken by the PUT (\d+),"
    r" released before (\d+), timed out (\d+); PUTs over a published park (\d+)\s*$")
_BM_WACCT_HEADER = re.compile(rf"^\[wacct\] ---- t=({_BM_NUM})s window=(\d+)ms flips=(\d+)\s*$")
_BM_WACCT_THREAD = re.compile(r"^\[wacct\] thr=(.+?) wait=(\d+)ms(.*?)\|(.*)$")
_BM_WACCT_SITE = re.compile(
    rf"\s*([A-Za-z_][A-Za-z0-9_-]*):"
    rf"((?:(?!\s+[A-Za-z_][A-Za-z0-9_-]*:)[^=|])+?)=({_BM_NUM})ms/(\d+)(?=\s|$)")
_BM_USERCMD = re.compile(
    rf"^\[usercmd-wait\] t=(\d+) n=(\d+) coll=(\d+) replaced=(\d+) wait_ms=({_BM_NUM}) max_ms=({_BM_NUM})"
    r" all_n=(\d+) all_coll=(\d+) all_replaced=(\d+)\s*$")


def _bm_number(text):
    if len(text) > 128 or not _BM_VALUE.fullmatch(text):
        raise ValueError("expected a finite number")
    value = int(text) if re.fullmatch(r"[+-]?\d+", text) else float(text)
    if abs(value) > 1e30 or not math.isfinite(value):
        raise ValueError("number is out of range")
    if isinstance(value, float) and value == 0 and any(c in "123456789" for c in re.split("[eE]", text)[0]):
        raise ValueError("number is too small to represent")
    return value


def _bm_native_int(text, *, signed=False, max_value=_BM_MAX_NATIVE_INT):
    digits = text[1:] if signed and text.startswith("-") else text
    if not digits.isdecimal() or len(digits) > 20:
        raise ValueError("native integer is invalid or out of range")
    value = int(text)
    if abs(value) > max_value:
        raise ValueError("native integer is out of range")
    return value


def _bm_native_number(text):
    return _bm_native_int(text) if text.isdecimal() else _bm_number(text)


def _bm_validate_native_numbers(text):
    for match in _BM_NATIVE_NUMBER.finditer(text):
        _bm_native_number(match.group())


def _bm_validate_site_name(name):
    numeric = re.fullmatch(r"(?:wid|ovl|label|sc)?(\d+)", name)
    if numeric:
        _bm_native_int(numeric[1])
    hexadecimal = re.fullmatch(r"(?:lr|nid)([0-9a-f]+)", name, re.I)
    if hexadecimal and len(hexadecimal[1]) > 16:
        raise ValueError("wait-accounting site identifier is out of range")


def _bm_fields(text):
    fields = {}
    for token in text.split():
        key, sep, value = token.partition("=")
        if not sep or not _BM_KEY.fullmatch(key) or not value or key in fields:
            raise ValueError("expected unique key=value fields")
        fields[key] = value
    return fields


def _bm_clock(text):
    if not _BM_NAME.fullmatch(text):
        raise ValueError("invalid clock or name")
    return text


def _bm_window(window):
    if not window or len(window) != 2:
        raise ValueError("benchmark --window needs exactly two numbers A,B")
    try:
        a, b = (float(x) for x in window)
    except (TypeError, ValueError, OverflowError) as e:
        raise ValueError("benchmark --window needs exactly two finite numbers A,B") from e
    if not (math.isfinite(a) and math.isfinite(b) and 0 <= a < b and b * 1000 <= _BM_MAX_MS):
        raise ValueError("benchmark --window must satisfy 0 <= A < B within the supported range")
    return a, b


def _bm_frame_empty():
    return {"source": None, "clock": None, "quality": "unavailable", "sample_count": None,
            "actual_start_ms": None, "actual_end_ms": None, "fps_mean": None, "fps_median": None,
            "frame_time_median_ms": None, "frame_time_p95_ms": None, "frame_time_p99_ms": None,
            "fps_low_1pct": None, "notes": []}


def _bm_failed(code, message, window=None):
    a, b = window if window is not None else (None, None)
    return {"schema": "kz3-benchmark-v1", "status": "fail",
            "window": {"start_s": a, "end_s": b, "clock": None}, "frames": _bm_frame_empty(),
            "cpu": None, "gpu": {"status": "unavailable", "clock": None, "method": None, "metrics": None,
                                 "alternatives": []},
            "walker": None, "draw": None, "submit": None, "fence": None, "writeback": None, "spu": None,
            "candidates": None, "native": {}, "unscoped_metrics": [],
            "failures": [{"code": code, "line": None, "message": message}]}


def _bm_frame_distribution(rows, source, clock, quality):
    """Rows are complete, positive frame intervals wholly inside the requested window."""
    times = sorted(row["duration_ms"] for row in rows)
    n = len(times)
    slow = times[-((n + 99) // 100):]
    r = _bm_frame_empty()
    r.update({"source": source, "clock": clock, "quality": quality, "sample_count": n,
              "actual_start_ms": rows[0]["start_ms"], "actual_end_ms": rows[-1]["end_ms"],
              "fps_mean": 1000.0 * n / math.fsum(times),
              "fps_median": statistics.median(1000.0 / t for t in times),
              "frame_time_median_ms": statistics.median(times),
              "frame_time_p95_ms": times[(95 * n + 99) // 100 - 1],
              "frame_time_p99_ms": times[(99 * n + 99) // 100 - 1],
              "fps_low_1pct": 1000.0 * len(slow) / math.fsum(slow)})
    return r


class BenchmarkV1:
    """Analyze one benchmark without assuming that independent log clocks align."""

    def __init__(self, alive, window):
        self.start_s, self.end_s = _bm_window(window)
        self.start, self.end = self.start_s * 1000, self.end_s * 1000
        self.failures = []
        self.frames, self.flips, self.paces, self.records = [], [], [], []
        self.native = {}
        self.last_perf = None
        self.last_perf_has_threads = False
        self.last_wacct = None
        if not alive:
            self.fail("not_alive", "program was not alive at the end of the measurement")

    def fail(self, code, message, line=None):
        self.failures.append({"code": code, "line": line, "message": message})

    def add_native(self, group, clock, kind, sample):
        group_data = self.native.setdefault(group, {"clock": clock})
        group_data.setdefault(kind, []).append(sample)

    def frame_line(self, text):
        fields = _bm_fields(text)
        clock = _bm_clock(fields.pop("clock", "benchmark"))
        if set(fields) != {"end_ms", "duration_ms"}:
            raise ValueError("frame needs end_ms and duration_ms")
        end, duration = (_bm_number(fields[k]) for k in ("end_ms", "duration_ms"))
        if not (0 < duration <= end <= _BM_MAX_MS) or duration < _BM_EPS:
            raise ValueError("invalid frame timestamp or duration")
        self.frames.append({"clock": clock, "start_ms": end - duration, "end_ms": end, "duration_ms": duration})

    def metric_line(self, text):
        fields = _bm_fields(text)
        required = {"category", "start_ms", "end_ms", "op"}
        if not required <= fields.keys():
            raise ValueError("metric needs category, start_ms, end_ms and op")
        category = fields.pop("category")
        if category not in {"cpu", "gpu", "walker", "draw", "submit", "fence", "writeback", "spu", "candidate"}:
            raise ValueError("unknown metric category")
        start, end = (_bm_number(fields.pop(k)) for k in ("start_ms", "end_ms"))
        if not (0 <= start < end <= _BM_MAX_MS):
            raise ValueError("invalid metric interval")
        op = fields.pop("op")
        if op not in {"sum", "max", "last"}:
            raise ValueError("metric op must be sum, max or last")
        clock = _bm_clock(fields.pop("clock", "benchmark"))
        name, method = fields.pop("name", None), fields.pop("method", None)
        if category in {"cpu", "candidate"}:
            if name is None or method is not None:
                raise ValueError("cpu and candidate metrics need a name, not a method")
            _bm_clock(name)
        elif category == "gpu":
            if name is not None or method not in {"timestamp", "fence_poll"}:
                raise ValueError("GPU metrics need method=timestamp or method=fence_poll")
        elif name is not None or method is not None:
            raise ValueError("unexpected name or method")
        if not fields:
            raise ValueError("metric has no numeric values")
        values = {key: _bm_number(value) for key, value in fields.items()}
        nonnegative = {"dropped", "dropped_samples", "missed", "missed_fences", "frames"} if category == "gpu" else set()
        if any(value < 0 for key, value in values.items()
               if key in nonnegative or key.endswith(("_ms", "_us", "_ns", "_count"))):
            raise ValueError("negative time or count")
        self.records.append({"category": category, "name": name, "method": method, "clock": clock,
                             "start_ms": start, "end_ms": end, "op": op, "metrics": values})

    def flip_line(self, line, line_no):
        m = _BM_FLIP.fullmatch(line)
        if not m:
            self.fail("malformed_flip", "malformed flip record", line_no)
            return
        count = _bm_native_int(m[1])
        ms = _bm_native_int(m[2], max_value=_BM_MAX_MS)
        if self.flips and (count <= self.flips[-1][1] or ms < self.flips[-1][0]):
            self.fail("malformed_flip", "flip counts or timestamps are not monotonic", line_no)
            return
        self.flips.append((ms, count))

    def pace_line(self, line, line_no):
        m = _BM_PACE.fullmatch(line)
        if not m:
            self.fail("malformed_pace", "malformed or incomplete pace window", line_no)
            return
        t, n = _bm_native_int(m[1], max_value=_BM_MAX_MS), _bm_native_int(m[2])
        p50, p99, maximum, sd, low = (_bm_native_number(m[i]) for i in range(3, 8))
        prev = self.paces[-1]["end_ms"] if self.paces else 0
        if not (t > prev and n > 0 and
                all(math.isfinite(x) for x in (p50, p99, maximum, sd, low)) and
                _BM_EPS <= p50 <= p99 <= maximum + 0.14 and maximum > 0 and sd >= 0 and low > 0):
            self.fail("malformed_pace", "invalid pace window values", line_no)
            return
        # A report can overshoot its 10 s boundary by at most its longest interval.
        if t - prev > 10000 + maximum + 2:
            self.fail("incomplete_pace", "pace reports have a missing interval", line_no)
        sample = {"start_ms": prev, "end_ms": t, "frames": n, "p50_ms": p50, "p99_ms": p99,
                  "max_ms": maximum, "sd_ms": sd, "low_1pct_fps": low}
        self.paces.append(sample)
        self.add_native("pace", "pace", "samples", sample)

    def native_line(self, line, line_no):
        if line.startswith("[perf]"):
            if line.startswith("[perf] RSX_PERF="):
                return
            self.last_perf, self.last_perf_has_threads = None, False
            m = _BM_PERF.fullmatch(line)
            if not m:
                self.fail("malformed_perf", "malformed RSX performance sample", line_no)
                return
            sample = {"end_s": _bm_native_number(m[1]), "wall_ms": _bm_native_number(m[2]),
                      "flips": _bm_native_int(m[3]), "fifo_flips": _bm_native_int(m[4]),
                      "presents": _bm_native_int(m[5]), "draws": _bm_native_int(m[6]),
                      "walker": {"runs": _bm_native_int(m[7]), "run_ms": _bm_native_number(m[8]),
                                 "methods": _bm_native_int(m[9])},
                      "submit": {"flushes": _bm_native_int(m[10]), "present_flushes": _bm_native_int(m[11]),
                                 "reference_flushes": _bm_native_int(m[12]), "other_flushes": _bm_native_int(m[13]),
                                 "flush_ms": _bm_native_number(m[14])},
                      "fence": {"cpu_wait_ms": _bm_native_number(m[15])},
                      "present_ms": _bm_native_number(m[16]), "swap_ms": _bm_native_number(m[17]),
                      "reference_polls": _bm_native_int(m[18]),
                      "wait": {"gcm_sleeps": _bm_native_int(m[19]), "gcm_ms": _bm_native_number(m[20]),
                               "other_ms": _bm_native_number(m[21])}}
            if sample["wall_ms"] <= 0:
                raise ValueError("invalid RSX performance sample duration")
            self.last_perf, self.last_perf_has_threads = sample, False
            self.add_native("rsx_perf", "rsx_perf", "samples", sample)
        elif line.startswith("[perf-walk]"):
            m = _BM_PERF_WALK.fullmatch(line)
            if not m:
                self.fail("malformed_perf_walk", "malformed walker performance sample", line_no)
                return
            _bm_validate_native_numbers(line)
            sample = {"end_s": _bm_native_number(m[1]), "draw_ms": _bm_native_number(m[2]),
                      "clear_ms": _bm_native_number(m[3]), "submit_ms": _bm_native_number(m[4]),
                      "submit_chunks": _bm_native_int(m[5]), "fence_readback_wait_ms": _bm_native_number(m[6]),
                      "fence_other_wait_ms": _bm_native_number(m[7]), "writeback_nv0039_count": _bm_native_int(m[8]),
                      "writeback_nv0039_ms": _bm_native_number(m[9]), "writeback_sync_count": _bm_native_int(m[10]),
                      "writeback_sync_ms": _bm_native_number(m[11]), "writeback_store_ms": _bm_native_number(m[12])}
            cbv = re.search(r"\|\s*cbv copy (\d+) reuse (\d+)(?:\s|$)", line)
            if "| cbv" in line and not cbv:
                self.fail("malformed_candidate", "malformed constant-buffer reuse counters", line_no)
            if cbv:
                sample["candidates"] = {"ld_cb_reuse": {"copy": _bm_native_int(cbv[1]),
                                                        "reuse": _bm_native_int(cbv[2])}}
            self.add_native("rsx_perf", "rsx_perf", "walker_split", sample)
        elif line.startswith("[perf-thr]"):
            m = _BM_PERF_THR.fullmatch(line)
            if not m:
                self.fail("malformed_perf_threads", "malformed host-thread CPU sample", line_no)
                return
            rest, threads = m[3], []
            while rest.strip():
                pair = re.match(rf"\s+(.+?)=({_BM_NUM})(?=\s|$)", rest)
                if not pair or not pair[1].strip():
                    self.fail("malformed_perf_threads", "malformed host-thread CPU entry", line_no)
                    return
                threads.append({"name": pair[1].strip(), "cpu_ms": _bm_native_number(pair[2])})
                rest = rest[pair.end():]
            paired = self.last_perf is not None and not self.last_perf_has_threads
            sample = {"end_s": self.last_perf["end_s"] if paired else None,
                      "wall_ms": self.last_perf["wall_ms"] if paired else None,
                      "time_association": "preceding perf line" if paired else "unavailable",
                      "sampled_process_cpu_ms": _bm_native_number(m[1]), "thread_count": _bm_native_int(m[2]),
                      "top_threads": threads,
                      "note": "a thread's first sample can include CPU time from before this period"}
            if paired:
                self.last_perf_has_threads = True
            self.add_native("rsx_perf", "rsx_perf", "thread_samples", sample)
        elif line.startswith("[perf-gpu]"):
            m = _BM_GPU_POLL.fullmatch(line)
            if not m:
                self.fail("malformed_gpu", "malformed GPU fence-poll sample", line_no)
                return
            missed = _bm_native_int(m[6])
            busy, wall, longest = (_bm_native_number(m[i]) for i in (3, 4, 5))
            sample = {"end_s": _bm_native_int(m[1]), "lists": _bm_native_int(m[2]),
                      "busy_ms": busy if not missed else None, "wall_ms": wall,
                      "longest_ms": longest if not missed else None, "missed": missed,
                      "status": "approximate" if not missed else "unavailable"}
            if sample["wall_ms"] <= 0 or missed:
                self.fail("incomplete_gpu_window", "GPU fence-poll sample is incomplete", line_no)
                sample["status"] = "unavailable"
                sample["busy_ms"] = sample["longest_ms"] = None
            self.add_native("gpu_monitor", "gpu_monitor", "fence_poll_samples", sample)
        elif line.startswith("[perf-gt]"):
            if line.startswith("[perf-gt] GPU timestamps on:"):
                return
            if re.search(r"\bfailed\b", line, re.I):
                self.fail("gpu_instrumentation_failure", "GPU timestamp instrumentation failed", line_no)
                return
            m = _BM_GPU_TIMESTAMP.fullmatch(line)
            if not m:
                self.fail("malformed_gpu", "malformed GPU timestamp sample", line_no)
                return
            _bm_validate_native_numbers(line)
            frames, dropped = _bm_native_int(m[2]), _bm_native_int(m[3])
            valid = frames > 0 and dropped == 0
            sample = {"end_s": _bm_native_int(m[1]), "frames": frames, "dropped": dropped,
                      "gpu_ms_per_frame": _bm_native_number(m[4]) if valid else None,
                      "status": "exact" if valid else "unavailable"}
            if not valid:
                self.fail("incomplete_gpu_window", "GPU timestamp sample has no frames or dropped queries", line_no)
            self.add_native("gpu_monitor", "gpu_monitor", "timestamp_samples", sample)
        elif line.startswith("[spurs-cpu]"):
            m = _BM_SPURS_CPU.fullmatch(line)
            if not m:
                self.fail("malformed_spu", "malformed SPURS CPU sample", line_no)
                return
            _bm_native_int(m[2])
            sample = {"thread": f"{m[1]} k{m[2]}", "cpu_ms_per_s": _bm_native_number(m[3]),
                      "yields_per_s": _bm_native_number(m[4]), "sleeps_per_s": _bm_native_number(m[5])}
            self.add_native("spurs_cpu", None, "samples", sample)
        elif line.startswith("[spurs-ready]"):
            m = _BM_SPURS_READY.fullmatch(line)
            if not m:
                self.fail("malformed_spu", "malformed SPURS ready-count sample", line_no)
                return
            _bm_validate_native_numbers(line)
            for workload in re.findall(r"\bw(\d+):", line):
                _bm_native_int(workload)
            sample = {"end_s": _bm_native_int(m[1]), "cumulative": True, "store_calls": _bm_native_int(m[2]),
                      "store_changed": _bm_native_int(m[3]), "swap_calls": _bm_native_int(m[4]),
                      "swap_changed": _bm_native_int(m[5]), "cas_calls": _bm_native_int(m[6]),
                      "cas_changed": _bm_native_int(m[7])}
            self.add_native("spurs_ready", "spurs_watch", "samples", sample)
        elif line.startswith("[wacct] ----"):
            self.last_wacct = None
            m = _BM_WACCT_HEADER.fullmatch(line)
            if not m:
                self.fail("malformed_wacct", "malformed wait-accounting window", line_no)
                return
            window_ms = _bm_native_int(m[2])
            if window_ms <= 0:
                self.fail("malformed_wacct", "malformed wait-accounting window", line_no)
                return
            self.last_wacct = {"end_s": _bm_native_number(m[1]), "nominal_window_ms": window_ms,
                               "flips": _bm_native_int(m[3]), "threads": []}
            self.add_native("wait_accounting", "wacct", "windows", self.last_wacct)
        elif line.startswith("[wacct] thr="):
            m = _BM_WACCT_THREAD.fullmatch(line)
            if not m or self.last_wacct is None:
                self.fail("malformed_wacct", "wait-accounting thread has no complete window header", line_no)
                return
            sample = {"name": m[1], "wait_ms": _bm_native_int(m[2]), "cpu_ms": None, "host_tid": None,
                      "logical_cpu_samples": None, "kinds": {}, "kernel_skips": None, "gaps": [], "sites": []}
            for token in m[3].split():
                kind = re.fullmatch(r"(hle|sc|usleep|label|ksleep|pmrun|job|kgap|park)=(\d+)/(\d+)", token)
                gap = re.fullmatch(rf"gap:wid(\d+)=({_BM_NUM})ms\*(\d+)", token)
                scalar = re.fullmatch(r"(cpums|tid|kskip)=(\d+)", token)
                cpus = re.fullmatch(r"cpus=((?:\d+:\d+,)*)", token)
                if kind and kind[1] not in sample["kinds"]:
                    sample["kinds"][kind[1]] = {"ms": _bm_native_int(kind[2]), "count": _bm_native_int(kind[3])}
                elif gap:
                    sample["gaps"].append({"workload": _bm_native_int(gap[1]), "mean_ms": _bm_native_number(gap[2]),
                                           "count": _bm_native_int(gap[3])})
                elif scalar:
                    field = {"cpums": "cpu_ms", "tid": "host_tid", "kskip": "kernel_skips"}[scalar[1]]
                    if sample[field] is not None:
                        self.fail("malformed_wacct", "duplicate wait-accounting field", line_no)
                        return
                    sample[field] = _bm_native_int(scalar[2])
                elif cpus and sample["logical_cpu_samples"] is None:
                    sample["logical_cpu_samples"] = [{"cpu": _bm_native_int(cpu), "samples": _bm_native_int(count)}
                                                     for cpu, count in re.findall(r"(\d+):(\d+),", cpus[1])]
                else:
                    self.fail("malformed_wacct", "malformed or duplicate wait-accounting field", line_no)
                    return
            rest = m[4]
            while rest.strip():
                site = _BM_WACCT_SITE.match(rest)
                if not site or site[2] != site[2].strip():
                    self.fail("malformed_wacct", "malformed wait-accounting site", line_no)
                    return
                _bm_validate_site_name(site[2])
                sample["sites"].append({"kind": site[1], "site": site[2], "ms": _bm_native_number(site[3]),
                                        "count": _bm_native_int(site[4])})
                rest = rest[site.end():]
            self.last_wacct["threads"].append(sample)
        elif line.startswith("[usercmd-wait]"):
            m = _BM_USERCMD.fullmatch(line)
            if not m:
                self.fail("malformed_usercmd", "malformed walker user-command wait sample", line_no)
                return
            sample = {"end_ms": _bm_native_int(m[1]), "commands": _bm_native_int(m[2]),
                      "collisions": _bm_native_int(m[3]), "replaced": _bm_native_int(m[4]),
                      "wait_ms": _bm_native_number(m[5]), "max_ms": _bm_native_number(m[6]),
                      "all_commands": _bm_native_int(m[7]), "all_collisions": _bm_native_int(m[8]),
                      "all_replaced": _bm_native_int(m[9])}
            self.add_native("usercmd_wait", "ps3_ms_now", "samples", sample)
        elif line.startswith("[wacct]") and not line.startswith("[wacct] on:"):
            self.fail("malformed_wacct", "malformed wait-accounting record", line_no)
        elif line.startswith("[cellGcmSys] park idle wake:"):
            m = _BM_PARK_WAKE.fullmatch(line)
            if not m:
                self.fail("malformed_candidate", "malformed park-wake counters", line_no)
                return
            sample = {"cumulative": True, "sleeps": _bm_native_int(m[1]), "put_wakes": _bm_native_int(m[2]),
                      "released_before_wait": _bm_native_int(m[3]), "timeouts": _bm_native_int(m[4]),
                      "overlapping_puts": _bm_native_int(m[5])}
            self.add_native("park_wake", None, "samples", sample)
        elif line.startswith("[park-idle]"):
            names = {
                "t_ms": "end_ms", "sleeps": "sleeps", "parks": "parks",
                "direct": "direct_signaled_releases", "pre_wait": "released_before_wait",
                "timeout": "timeouts", "spurious": "spurious_signals", "wait_failed": "wait_failures",
                "shutdown": "shutdowns", "overlaps": "overlapping_puts", "signaled": "wait_signals",
                "park_ns": "park_time_ns", "progress": "walker_progress",
                "wake_progress_ns": "wake_progress_ns", "wake_progress_max_ns": "wake_progress_max_ns",
            }
            try:
                fields = _bm_fields(line[len("[park-idle]"):])
                if set(fields) != set(names):
                    raise ValueError("missing or unexpected field")
                sample = {"cumulative": True, "clock": "GetTickCount64"}
                sample.update({name: _bm_native_int(fields[key]) for key, name in names.items()})
            except ValueError:
                self.fail("malformed_candidate", "malformed park-idle counters", line_no)
                return
            self.add_native("park_wake", None, "samples", sample)

    def feed(self, line, line_no):
        line = line.rstrip("\r\n")
        if _BM_CRASH.search(line):
            self.fail("crash_marker", "crash marker in log", line_no)
        if _BM_PAD.search(line):
            self.fail("pad_script_warning", "pad-script warning or error in log", line_no)
        exit_code = _BM_EXIT.search(line)
        if exit_code:
            try:
                status = _bm_native_int(exit_code[1], signed=True)
            except ValueError:
                self.fail("malformed_exit", "exit-status integer is out of range", line_no)
            else:
                if status != 0:
                    self.fail("nonzero_exit", "program exited with a nonzero status", line_no)
        elif line.lower().startswith("[exit code"):
            self.fail("malformed_exit", "malformed exit-status record", line_no)
        try:
            if line.startswith("[benchmark-frame]"):
                self.frame_line(line[len("[benchmark-frame]"):])
            elif line.startswith("[benchmark-metric]"):
                self.metric_line(line[len("[benchmark-metric]"):])
            elif line.startswith("[benchmark-"):
                raise ValueError("unknown benchmark record")
            elif line.startswith("[flip]"):
                self.flip_line(line, line_no)
            elif line.startswith("[pace]"):
                if not line[len("[pace]"):].lstrip().startswith("fps limit "):
                    self.pace_line(line, line_no)
            else:
                self.native_line(line, line_no)
        except (ValueError, OverflowError) as e:
            code = ("malformed_wacct" if line.startswith("[wacct]") else
                    "malformed_flip" if line.startswith("[flip]") else
                    "malformed_pace" if line.startswith("[pace]") else "malformed_record")
            self.fail(code, str(e), line_no)

    def raw_frames(self):
        clocks = {row["clock"] for row in self.frames}
        if len(clocks) != 1:
            self.fail("mixed_frame_clocks", "frame records have different clock origins")
            return _bm_frame_empty()
        clock = next(iter(clocks))
        relevant = [row for row in self.frames if row["end_ms"] > self.start + _BM_EPS and
                    row["start_ms"] < self.end - _BM_EPS]
        if not relevant or relevant[0]["start_ms"] > self.start + _BM_EPS or relevant[-1]["end_ms"] < self.end - _BM_EPS:
            self.fail("incomplete_frame_window", "frame records do not cover the requested window")
            return _bm_frame_empty()
        for previous, current in zip(relevant, relevant[1:]):
            if abs(current["start_ms"] - previous["end_ms"]) > _BM_EPS:
                self.fail("incomplete_frame_window", "frame intervals contain a gap, overlap or reorder")
                return _bm_frame_empty()
        whole = [row for row in relevant if row["start_ms"] >= self.start - _BM_EPS and
                 row["end_ms"] <= self.end + _BM_EPS]
        if not whole:
            self.fail("incomplete_frame_window", "window contains no complete frame interval")
            return _bm_frame_empty()
        r = _bm_frame_distribution(whole, "benchmark-frame", clock, "exact")
        if len(whole) != len(relevant):
            r["notes"].append("partial boundary frames are excluded")
        return r

    def flip_frames(self):
        if len(self.flips) < 2 or self.flips[0][0] > self.start or self.flips[-1][0] < self.end:
            return None, False
        pairs = [(a, b) for a, b in zip(self.flips, self.flips[1:])
                 if b[0] > self.start and a[0] < self.end]
        zero_at_boundary = any(a[0] == b[0] and self.start <= a[0] <= self.end
                               for a, b in zip(self.flips, self.flips[1:]))
        complete = bool(pairs) and not zero_at_boundary and all(b[1] - a[1] == 1 and b[0] > a[0] for a, b in pairs)
        if complete:
            whole = [{"start_ms": a[0], "end_ms": b[0], "duration_ms": b[0] - a[0]}
                     for a, b in pairs if a[0] >= self.start and b[0] <= self.end]
            if whole:
                r = _bm_frame_distribution(whole, "guest_flip", "flip", "approximate")
                r["notes"].append("guest flip timestamps have millisecond resolution, not present timestamps")
                if len(whole) != len(pairs):
                    r["notes"].append("partial boundary frames are excluded")
                return r, True
        # Sparse counts give a rate only between recorded points, not individual frame times.
        points = {}
        for t, count in self.flips:
            points[t] = count
        inside = sorted(t for t in points if self.start <= t <= self.end)
        if len(inside) < 2:
            return None, False
        a, b = inside[0], inside[-1]
        count = points[b] - points[a]
        if count <= 0 or a == b:
            return None, False
        r = _bm_frame_empty()
        r.update({"source": "sampled_guest_flip", "clock": "flip", "quality": "approximate",
                  "actual_start_ms": a, "actual_end_ms": b,
                  "fps_mean": count * 1000.0 / (b - a),
                  "notes": ["rate between recorded guest flips; frame distributions are unavailable"]})
        return r, False

    def pace_frames(self):
        selected = [p for p in self.paces if p["end_ms"] > self.start + _BM_EPS and
                    p["start_ms"] < self.end - _BM_EPS]
        if not selected or abs(selected[0]["start_ms"] - self.start) > _BM_EPS or \
                abs(selected[-1]["end_ms"] - self.end) > _BM_EPS:
            return None
        n = sum(p["frames"] for p in selected)
        r = _bm_frame_empty()
        r.update({"source": "present_pace_histogram", "clock": "pace", "quality": "approximate",
                  "sample_count": n, "actual_start_ms": self.start, "actual_end_ms": self.end,
                  "fps_mean": n * 1000.0 / (self.end - self.start)})
        if len(selected) == 1:
            p = selected[0]
            r.update({"frame_time_p99_ms": p["p99_ms"], "fps_low_1pct": p["low_1pct_fps"]})
            if p["frames"] % 2:
                r.update({"fps_median": 1000.0 / p["p50_ms"], "frame_time_median_ms": p["p50_ms"]})
            else:
                r["notes"].append("the upper middle frame of an even-sized histogram is not reported")
            r["notes"].append("available percentiles and 1% low use the runtime's histogram; p95 is not logged")
        else:
            r["notes"].append("per-window percentiles and 1% lows cannot be combined into a distribution")
        return r

    def frame_metrics(self):
        if self.frames:
            return self.raw_frames()
        flips, distribution = self.flip_frames()
        if distribution:
            return flips
        pace = self.pace_frames()
        if pace:
            return pace
        if flips:
            return flips
        self.fail("incomplete_frame_window", "no frame source completely covers the requested window")
        return _bm_frame_empty()

    def aggregate(self, clock):
        series, unscoped = defaultdict(list), []
        for record in self.records:
            if record["clock"] != clock:
                unscoped.append(record)
                continue
            for key, value in record["metrics"].items():
                series[(record["category"], record["name"], record["method"], key)].append((record, value))
        groups = {}
        for (category, name, method, key), values in series.items():
            values = [(r, v) for r, v in values if r["end_ms"] > self.start + _BM_EPS and
                      r["start_ms"] < self.end - _BM_EPS]
            if not values:
                continue
            values.sort(key=lambda pair: (pair[0]["start_ms"], pair[0]["end_ms"]))
            group = groups.setdefault((category, name, method),
                                      {"clock": clock, "metrics": {}, "aggregation": {},
                                       "sample_counts": {}, "complete": True})
            ops = {r["op"] for r, _ in values}
            valid, cursor = len(ops) == 1, self.start
            for record, _ in values:
                if abs(record["start_ms"] - cursor) > _BM_EPS:
                    valid = False
                cursor = record["end_ms"]
            if abs(cursor - self.end) > _BM_EPS:
                valid = False
            op = next(iter(ops)) if len(ops) == 1 else None
            group["aggregation"][key] = op
            group["sample_counts"][key] = len(values)
            if not valid:
                group["metrics"][key] = None
                group["complete"] = False
                label = ".".join(str(x) for x in (category, name, method, key) if x is not None)
                self.fail("incomplete_metric_window", f"{label} does not cover the window exactly once")
                continue
            nums = [value for _, value in values]
            if op == "sum":
                value = sum(nums) if all(isinstance(n, int) for n in nums) else math.fsum(nums)
            elif op == "max":
                value = max(nums)
            else:
                value = nums[-1]
            group["metrics"][key] = value
        return groups, unscoped

    def validate_native_gpu(self):
        gpu = self.native.get("gpu_monitor")
        if not gpu:
            return
        polls = defaultdict(list)
        for sample in gpu.get("fence_poll_samples", []):
            polls[sample["end_s"]].append(sample)
        for sample in gpu.get("timestamp_samples", []):
            if sample["status"] != "exact":
                continue
            end = sample["end_s"]
            covered = end >= 10 and end % 10 == 0 and all(
                len(polls[t]) == 1 and polls[t][0]["status"] == "approximate" for t in range(end - 9, end + 1))
            if not covered:
                sample["status"] = "unavailable"
                sample["gpu_ms_per_frame"] = None
                self.fail("incomplete_gpu_window", "GPU timestamp window lacks loss-free fence-poll coverage")

    def finish(self):
        frames = self.frame_metrics()
        self.validate_native_gpu()
        groups, unscoped = self.aggregate(frames["clock"])
        result = {"schema": "kz3-benchmark-v1", "status": None,
                  "window": {"start_s": self.start_s, "end_s": self.end_s, "clock": frames["clock"]},
                  "frames": frames, "cpu": None,
                  "gpu": {"status": "unavailable", "clock": None, "method": None, "metrics": None,
                          "alternatives": []},
                  "walker": None, "draw": None, "submit": None, "fence": None, "writeback": None, "spu": None,
                  "candidates": None, "native": self.native, "unscoped_metrics": unscoped,
                  "failures": self.failures}
        for (category, name, method), group in groups.items():
            if category == "cpu":
                cpu = result["cpu"]
                if cpu is None:
                    cpu = result["cpu"] = {"clock": frames["clock"], "process": None, "threads": {}}
                if name == "process":
                    cpu["process"] = group
                else:
                    cpu["threads"][name] = group
            elif category == "candidate":
                if result["candidates"] is None:
                    result["candidates"] = {}
                result["candidates"][name] = group
            elif category != "gpu":
                result[category] = group

        gpu_observations = []
        for method in ("timestamp", "fence_poll"):
            group = groups.get(("gpu", None, method))
            if not group:
                continue
            values = group["metrics"]
            loss_names = ("dropped", "dropped_samples") if method == "timestamp" else ("missed", "missed_fences")
            losses = [values[k] for k in loss_names if k in values]
            observed_losses = [
                record["metrics"][key]
                for record in self.records
                if record["category"] == "gpu" and record["method"] == method and
                record["clock"] == frames["clock"] and record["end_ms"] > self.start + _BM_EPS and
                record["start_ms"] < self.end - _BM_EPS
                for key in loss_names if key in record["metrics"]
            ]
            timing_keys = [k for k in ("gpu_ms", "gpu_time_ms", "gpu_ms_per_frame", "duration_ms")
                           if values.get(k) is not None]
            timing = bool(timing_keys) and all(
                (group["sample_counts"][k] == 1 and values.get("frames") is not None and values["frames"] > 0
                 if k == "gpu_ms_per_frame" else group["aggregation"][k] == "sum")
                for k in timing_keys)
            if (not group["complete"] or not losses or any(loss is None or loss != 0 for loss in losses) or
                    any(loss != 0 for loss in observed_losses) or not timing):
                self.fail("incomplete_gpu_window", f"{method} GPU timing lacks complete, loss-free samples")
                status = "unavailable"
            else:
                status = "exact" if method == "timestamp" else "approximate"
            gpu_observations.append({"status": status, "method": method, **group})
        usable_gpu = [observation for observation in gpu_observations if observation["status"] != "unavailable"]
        if usable_gpu:
            chosen = usable_gpu[0]
            result["gpu"] = {**chosen, "alternatives": [entry for entry in gpu_observations if entry is not chosen]}
        else:
            result["gpu"]["alternatives"] = gpu_observations
        result["status"] = "fail" if self.failures else "pass"
        return result


def measure_benchmark(log, alive, window):
    """Return the versioned strict benchmark result; '-' reads a log from stdin."""
    benchmark = BenchmarkV1(alive, window)
    lines = sys.stdin if log == "-" else (line for block in blocks(log) for line in io.StringIO(block))
    for line_no, line in enumerate(lines, 1):
        if not line.endswith(("\n", "\r")):
            benchmark.fail("truncated_log", "log ends with an unterminated record", line_no)
        benchmark.feed(line, line_no)
    try:
        return benchmark.finish()
    except (ValueError, OverflowError, ZeroDivisionError):
        result = _bm_failed("numeric_overflow", "benchmark arithmetic exceeded its supported range",
                            (benchmark.start_s, benchmark.end_s))
        result["failures"] = benchmark.failures + result["failures"]
        return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log"); ap.add_argument("--alive", type=int, default=1)
    ap.add_argument("--out"); ap.add_argument("--baseline")
    ap.add_argument("--window", metavar="A,B", help="seconds of run time: adds fps_window over [A,B] (in-level fps)")
    ap.add_argument("--benchmark-v1", action="store_true",
                    help="emit strict versioned benchmark JSON (requires --window; LOG may be - for stdin)")
    ap.add_argument("--row", metavar="NAME", help="also print one Markdown table row named NAME (gameplay tests)")
    ap.add_argument("--warn-drift", action="store_true",
                    help=f"with --baseline: print a WARN line when fps_avg or flips fall more than {DRIFT_PCT} %% below it (never fails)")
    ap.add_argument("--run-json", metavar="RUN_JSON",
                    help="the run's run.json (tools/runprov.py): its load average and build-lock state go into the record as `host`")
    a = ap.parse_args()
    if a.benchmark_v1:
        if a.baseline or a.row or a.warn_drift or a.run_json:
            ap.error("--benchmark-v1 cannot be combined with boot-baseline, row, drift or run-json options")
        if a.alive not in (0, 1):
            ap.error("--benchmark-v1 requires --alive 0 or 1")
        exit_code, window = 0, None
        try:
            window = _bm_window(a.window.split(",") if a.window else None)
        except ValueError as e:
            result, exit_code = _bm_failed("invalid_window", str(e)), 2
        else:
            try:
                result = measure_benchmark(a.log, bool(a.alive), window)
            except OSError:
                result, exit_code = _bm_failed("input_error", "cannot read input log", window), 1
        try:
            payload = json.dumps(result, indent=2, allow_nan=False)
        except (ValueError, OverflowError):
            failures = result.get("failures", [])
            result = _bm_failed("numeric_overflow", "benchmark result contains an out-of-range number", window)
            result["failures"] = failures + result["failures"]
            payload = json.dumps(result, indent=2, allow_nan=False)
        if a.out:
            with open(a.out, "w", encoding="utf-8") as f:
                f.write(payload + "\n")
        print(payload)
        sys.exit(exit_code or (1 if result["status"] == "fail" else 0))
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

#!/usr/bin/env python3
"""Run provenance: writes run.json (schema kz3.run/1) beside a Proton run's log so two runs can be checked for
comparability (a comparison tool reads it) instead of being compared by eye: two runs with different builds, a cold
shader cache on one side or an early stop are not comparable, and run.json says so.

    runprov.py start  RUN_DIR [--exe EXE] [--seconds N] [--display xvfb|real] [--proton DIR] [--root REPO]
    runprov.py finish RUN_DIR [--seconds N] [--root REPO]           (also the backfill for runs made before this tool)
    runprov.py show   RUN_DIR                                       (print run.json)

`start` runs after the exe's directory was copied to RUN_DIR/app and before the game starts: build identity (exe
sha256, git revision/dirty/branch, patch-set stamp, game version, toolchain), config (the env the runtime reads,
Proton, run seconds, display) and fixtures (savedata hash, pad script hash and event count, cache state).
`finish` runs after the run and fills `outcome` from the log: last flip, flips, early stop, the first stop signature,
plus tools/kz3_metrics.py's numbers (reused, not reparsed). Hooked into tools/proton-test.sh, so every run gets one.
Both phases sample the host (`config.host` at start, `outcome.host_end` at finish): the 1-minute load average and
whether the build lock (profile `build_lock`, tools/build-kz3.sh's flock) was held, by a non-blocking flock test;
`outcome.build_lock_held` is true when either sample saw it held (boot checks taken next to a
build drift in fps and flips; a comparison treats a held lock as fatal for perf/ab, disclosed for regression).

Every field is {"value": ..., "source": "how it was found"}; what could not be found is {"value": null,
"source": "unknown", "reason": "why"}. Nothing is guessed. The tool never fails a run: on any error it writes what it
has and appends to `errors`. Exit status is 0 unless the usage is wrong.

Game-specific knowledge (log signatures, env prefixes, cache paths) lives in tools/runprofile_<name>.py (--profile,
default kz3); this file is title-agnostic.
"""
import argparse
import datetime
import glob
import hashlib
import importlib
import json
import os
import re
import subprocess
import sys
import traceback

SCHEMA = "kz3.run/1"
TOOL_VERSION = 1
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def known(value, source):
    return {"value": value, "source": source}


def unknown(reason):
    return {"value": None, "source": "unknown", "reason": reason}


def is_known(field):
    return isinstance(field, dict) and field.get("source") != "unknown"


def val(field, default=None):
    return field.get("value", default) if isinstance(field, dict) else default


def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_file(path, chunk=1 << 22):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def sha256_tree(root):
    """(file count, sha256 over sorted "relpath sha256(content)" lines) of every file under root."""
    lines = []
    for d, _, files in os.walk(root):
        for n in files:
            p = os.path.join(d, n)
            if os.path.isfile(p):
                lines.append(f"{os.path.relpath(p, root)} {sha256_file(p)}")
    lines.sort()
    return len(lines), hashlib.sha256("\n".join(lines).encode()).hexdigest()


def run_git(root, *args):
    out = subprocess.run(["git", "-C", root, *args], capture_output=True, text=True, timeout=30)
    if out.returncode != 0:
        raise RuntimeError((out.stderr or out.stdout).strip() or f"git {' '.join(args)} failed")
    return out.stdout.strip()


class Prov:
    """One run.json in the making: `doc` is the document, every step is guarded so a failure becomes an error entry."""

    def __init__(self, profile, run_dir, root):
        self.p = profile
        self.run_dir = os.path.abspath(run_dir)
        self.root = os.path.abspath(root) if root else None
        self.path = os.path.join(self.run_dir, "run.json")
        self.doc = None

    # -- document -------------------------------------------------------------------------------------------------
    def load_or_new(self, phase):
        if os.path.exists(self.path):
            try:
                with open(self.path) as f:
                    self.doc = json.load(f)
                if self.doc.get("schema") != SCHEMA:
                    self.error(f"existing run.json has schema {self.doc.get('schema')!r}, expected {SCHEMA}; replaced")
                    self.doc = None
            except Exception as e:   # noqa: BLE001 - never fail the run
                self.doc = self.new_doc(phase)
                self.error(f"existing run.json unreadable ({e}); replaced")
        if self.doc is None:
            self.doc = self.new_doc(phase)
        self.doc.setdefault("errors", [])
        for k in ("build", "config", "fixtures", "outcome"):
            self.doc.setdefault(k, {})
        self.doc["run"]["phase"] = phase
        return self.doc

    def new_doc(self, phase):
        return {
            "schema": SCHEMA,
            "run": {"name": os.path.basename(self.run_dir), "dir": self.run_dir, "phase": phase,
                    "profile": self.p["name"], "runprov_version": TOOL_VERSION,
                    "started_at": None, "finished_at": None},
            "build": {}, "config": {}, "fixtures": {}, "outcome": {}, "errors": [],
        }

    def error(self, msg):
        self.doc.setdefault("errors", []).append(msg)

    def step(self, section, name, fn, reason_prefix=None):
        """Run fn() -> field; on an exception store an unknown field with the error and log it."""
        try:
            self.doc[section][name] = fn()
        except Exception as e:   # noqa: BLE001
            why = f"{reason_prefix or name}: {type(e).__name__}: {e}"
            self.doc[section][name] = unknown(why)
            self.error(f"{section}.{name}: {why}")

    def write(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.doc, f, indent=1, sort_keys=False)
            f.write("\n")
        os.replace(tmp, self.path)
        return self.path

    # -- paths -----------------------------------------------------------------------------------------------------
    def run_path(self, rel):
        return os.path.join(self.run_dir, rel)

    def root_path(self, rel):
        """A repo-relative path: this checkout's, else the main checkout's (KZ3_MAIN, for a second working tree)."""
        if not self.root:
            return None
        p = os.path.join(self.root, rel)
        if os.path.exists(p):
            return p
        for main in (os.environ.get("KZ3_MAIN"), self.main_checkout()):
            if main and os.path.exists(os.path.join(main, rel)):
                return os.path.join(main, rel)
        return p

    def main_checkout(self):
        """A second working tree's main checkout (the .kz3-main file tools/agents/lock.sh reads), else None."""
        try:
            with open(os.path.join(self.root, ".kz3-main")) as f:
                return f.read().strip() or None
        except OSError:
            return None

    # -- host ------------------------------------------------------------------------------------------------------
    def host_sample(self, when):
        """1-minute load average and the build lock's state now (flock -n test; nothing is created or blocked)."""
        lock_rel = self.p.get("build_lock")
        lock = self.root_path(lock_rel) if lock_rel and self.root else None
        held, lock_src = None, "no --root: the build lock could not be located"
        if lock:
            if not os.path.isfile(lock):
                held, lock_src = False, f"no {lock_rel} yet (no build has run): nothing holds it"
            else:
                import fcntl
                fd = os.open(lock, os.O_RDONLY)
                try:
                    try:
                        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        fcntl.flock(fd, fcntl.LOCK_UN)
                        held = False
                    except BlockingIOError:
                        held = True
                finally:
                    os.close(fd)
                lock_src = f"flock -n on {lock} at {when}"
        try:
            load = round(os.getloadavg()[0], 2)
            load_src = f"os.getloadavg()[0] at {when}"
        except OSError as e:
            load, load_src = None, f"getloadavg failed: {e}"
        return known({"load_avg_1m": load, "load_source": load_src, "build_lock_held": held,
                      "build_lock_source": lock_src, "sampled_at": now_iso()},
                     f"host sampled at {when} (load: {load_src}; lock: {lock_src})")

    # -- start phase -----------------------------------------------------------------------------------------------
    def start(self, exe, seconds, display, proton):
        d = self.doc
        d["run"]["started_at"] = now_iso()
        d["run"]["started_at_source"] = "runprov start (before the game started)"
        p = self.p

        def f_exe():
            staged = self.run_path(os.path.join("app", p["exe_name"]))
            cand = staged if os.path.isfile(staged) else exe
            if not cand or not os.path.isfile(cand):
                return unknown(f"no {p['exe_name']} in {self.run_path('app')}" + (f" and no exe at {exe}" if exe else ""))
            return known({"sha256": sha256_file(cand), "size": os.path.getsize(cand), "path": cand},
                         "sha256 of the staged copy the run executes" if cand == staged else "sha256 of --exe")
        self.step("build", "exe", f_exe)

        def f_git():
            if not self.root:
                return unknown("no --root (repo checkout) given")
            rev = run_git(self.root, "rev-parse", "HEAD")
            branch = run_git(self.root, "rev-parse", "--abbrev-ref", "HEAD")
            dirty = run_git(self.root, "status", "--porcelain", "--untracked-files=no") != ""
            return known({"rev": rev, "branch": branch, "dirty": dirty},
                         "git rev-parse HEAD / --abbrev-ref HEAD / status --porcelain (tracked files) in --root")
        self.step("build", "git", f_git)

        def f_sub():
            if not self.root:
                return unknown("no --root given")
            sub = os.path.join(self.root, "third_party", "ps3recomp")
            if not os.path.exists(os.path.join(sub, ".git")):
                return unknown(f"{sub} is not a checked-out submodule")
            return known(run_git(sub, "rev-parse", "HEAD"), "git rev-parse HEAD in third_party/ps3recomp")
        self.step("build", "ps3recomp_rev", f_sub)

        def f_patchset():
            sp = self.root_path(p["patchset_stamp"])
            if not sp or not os.path.isfile(sp):
                return unknown(f"no {p['patchset_stamp']} (written by the boot check after patching)")
            return known(open(sp).read().strip(), f"{p['patchset_stamp']} (sha256 of port/patches + submodule rev)")
        self.step("build", "patchset", f_patchset)

        def f_ver():
            env_v = os.environ.get(p["game_version_env"])
            if env_v:
                return known(env_v, f"${p['game_version_env']}")
            vp = self.root_path(p["game_version_file"])
            if vp and os.path.isfile(vp):
                return known(open(vp).read().strip(), f"{p['game_version_file']} (the SPU lift's version)")
            return known(p["game_version_default"], f"profile default (tools/spu/kz3ver.py DEFAULT); "
                                                    f"${p['game_version_env']} unset, no {p['game_version_file']}")
        self.step("build", "game_version_configured", f_ver)
        d["build"]["game_version"] = unknown("not read from the log yet (finish fills it from the TOC line)")

        def f_tc():
            cc = self.root_path(p["cmake_cache"])
            if not cc or not os.path.isfile(cc):
                return unknown(f"no {p['cmake_cache']} (tools/build-kz3.sh writes no toolchain stamp)")
            tc = {}
            for line in open(cc, errors="replace"):
                for key in ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "CMAKE_BUILD_TYPE", "CMAKE_TOOLCHAIN_FILE"):
                    if line.startswith(key + ":"):
                        tc[key] = line.split("=", 1)[1].strip()
            return known(tc, f"{p['cmake_cache']} (the last configure of the build dir, not necessarily this exe's)")
        self.step("build", "toolchain", f_tc)

        def f_env():
            env = {}
            for k, v in sorted(os.environ.items()):
                if k.startswith(p["env_prefixes"]):
                    env[k] = "<redacted>" if p["secret_re"].search(k) else v
            for k, v in p.get("env_defaults", {}).items():
                env.setdefault(k, v)
            return known(env, f"os.environ at start, prefixes {', '.join(p['env_prefixes'])}; "
                              "secret-looking names redacted; harness defaults added as in the profile")
        self.step("config", "env", f_env)

        def f_proton():
            if not proton:
                return unknown("no --proton given")
            vf = os.path.join(proton, "version")
            ver = open(vf).read().strip() if os.path.isfile(vf) else None
            real = os.path.realpath(proton)
            return known({"dir": proton, "realpath": real, "version": ver},
                         "the --proton dir, its `version` file" + ("" if ver else " (missing)"))
        self.step("config", "proton", f_proton)
        d["config"]["run_seconds"] = known(seconds, "--seconds") if seconds is not None else unknown("no --seconds")
        d["config"]["display"] = known(display, "--display") if display else unknown("no --display")
        self.step("config", "host", lambda: self.host_sample("start"))

        def f_save():
            sd = self.run_path(p["save_rel"])
            src = os.environ.get(p["save_source_env"])
            if not os.path.isdir(sd):
                return known({"files": 0, "sha256": None, "source_dir": src},
                             f"no {p['save_rel']} at start: first boot, no savedata")
            n, h = sha256_tree(sd)
            return known({"files": n, "sha256": h, "source_dir": src},
                         f"sha256 over every file under {p['save_rel']} at start; source_dir from "
                         f"${p['save_source_env']}" + ("" if src else " (unset: copied by hand or none)"))
        self.step("fixtures", "save", f_save)

        def f_pad():
            s = os.environ.get(p["pad_env"])
            if s is None:
                return known(None, f"${p['pad_env']} unset: no scripted input")
            # v1 "sec:mask,..." and v2 (patch 3006: ',', space, tab or newline separators, '#' comments)
            body = re.sub(r"#[^\n]*", "", s)
            events = [e for e in re.split(r"[,\s]+", body) if ":" in e]
            return known({"sha256": hashlib.sha256(s.encode()).hexdigest(), "events": len(events),
                          "truncated": len(events) > p["pad_max_events"], "max_events": p["pad_max_events"]},
                         f"${p['pad_env']} at start (events beyond {p['pad_max_events']} are dropped by the runtime)")
        self.step("fixtures", "pad", f_pad)
        self.step("fixtures", "cache_start", self.cache_snapshot)
        return d

    def cache_snapshot(self, before_epoch=None):
        """Shader/PSO cache state in the run dir. before_epoch (backfill): count only files older than it."""
        p = self.p
        dirs = sorted(glob.glob(self.run_path(p["cache_dir_glob"])))
        if not dirs:
            return known({"dir": None, "shader_files": 0, "pso_keys": 0, "pso_library_bytes": 0, "state": "cold"},
                         f"no {p['cache_dir_glob']} in the run dir at start" if before_epoch is None else
                         f"no {p['cache_dir_glob']} in the run dir after the run: it never wrote one (backfill)")
        cd = dirs[-1]
        files = glob.glob(os.path.join(cd, p["cache_file_glob"]))
        keys_p, lib_p = os.path.join(cd, p["pso_keys"]), os.path.join(cd, p["pso_library"])
        how = "files in the run dir at start"
        if before_epoch is not None:
            files = [f for f in files if os.path.getmtime(f) < before_epoch]
            how = "files in the run dir older than the log's start (backfill: inferred from mtimes, not a snapshot)"
        keys = 0
        if os.path.isfile(keys_p) and (before_epoch is None or os.path.getmtime(keys_p) < before_epoch):
            import struct
            with open(keys_p, "rb") as f:
                h = f.read(24)
            if len(h) == 24:
                magic, _ver, rec, _rs, _cv, _r = struct.unpack("<6I", h)
                if magic == p["pso_keys_magic"] and rec > 0:
                    keys = max(0, (os.path.getsize(keys_p) - 24) // rec)
        lib = os.path.getsize(lib_p) if os.path.isfile(lib_p) and (
            before_epoch is None or os.path.getmtime(lib_p) < before_epoch) else 0
        state = "cold" if not files and keys == 0 and lib == 0 else "warm"
        return known({"dir": os.path.relpath(cd, self.run_dir), "shader_files": len(files), "pso_keys": keys,
                      "pso_library_bytes": lib, "state": state}, how)

    # -- finish phase ----------------------------------------------------------------------------------------------
    # -- observed facts for frame comparisons ------------------------------------------------------------------------
    @staticmethod
    def levels_seen(log):
        """Level lumps in the log, first-seen order. loaded = its `.leveldescription` file was opened (a level load);
        mentioned = any file of it (the menu alone opens levels.single_player.stahl_arms_research ambient lumps)."""
        import re
        loaded, mentioned = [], []
        with open(log, "rb") as f:
            for m in re.finditer(rb"(levels\.[a-z_]+\.[A-Za-z0-9_]+)\.(leveldescription)?", f.read()):
                s = m.group(1).decode()
                if s not in mentioned:
                    mentioned.append(s)
                if m.group(2) and s not in loaded:
                    loaded.append(s)
        return known({"loaded": loaded, "mentioned": mentioned},
                     "lump names in the run's stdout log; loaded = `<lump>.leveldescription*` opened")

    @staticmethod
    def captures(log):
        """LD_CAPTURE_FLIP frames (patch 2085): frame number -> {hash, path, size} from the `[capture] frame=` lines."""
        import re
        out = {}
        pat = re.compile(r"\[capture\] frame=(\d+) .*? (\d+)x(\d+) nonblack=(-?\d+) hash=([0-9A-F]+) path=(\S+)")
        with open(log, errors="replace") as f:
            for line in f:
                m = pat.search(line)
                if m:
                    out[m.group(1)] = {"size": [int(m.group(2)), int(m.group(3))], "nonblack": int(m.group(4)),
                                       "hash": m.group(5), "path": m.group(6)}
        return known(out, "`[capture] frame=` lines (LD_CAPTURE_FLIP, patch 2085); empty = none requested/written")

    @staticmethod
    def gpu_id():
        """GPU model and driver version from the NVIDIA proc files (the host side; Proton reports the same GPU)."""
        info = {}
        try:
            with open("/proc/driver/nvidia/version") as f:
                info["driver"] = f.readline().strip()[:160]
        except OSError as e:
            info["driver_error"] = str(e)
        for p in sorted(glob.glob("/proc/driver/nvidia/gpus/*/information"))[:1]:
            with open(p) as f:
                for line in f:
                    if line.startswith(("Model:", "Bus Location:")):
                        k, v = line.split(":", 1)
                        info[k.strip().lower().replace(" ", "_")] = v.strip()
        if not info.get("model") and not info.get("driver"):
            return unknown("no /proc/driver/nvidia files readable here: " + info.get("driver_error", ""))
        return known(info, "/proc/driver/nvidia/version and gpus/*/information at finish")

    def finish(self, seconds=None):
        d = self.doc
        p = self.p
        d["run"]["finished_at"] = now_iso()
        # Host at the end of the run; build_lock_held = held at start or at the end (a build that started and ended
        # inside the run escapes both samples: the load average still shows it).
        self.step("outcome", "host_end", lambda: self.host_sample("finish"))
        samples = [(w, val(d[s].get(n))) for s, n, w in (("config", "host", "start"), ("outcome", "host_end", "finish"))
                   if is_known(d[s].get(n))]
        states = [(n, v.get("build_lock_held")) for n, v in samples if isinstance(v, dict) and v.get("build_lock_held") is not None]
        if "host" not in d["config"]:
            d["outcome"]["build_lock_held"] = unknown("no start sample (run made before host sampling, or a backfill): "
                                                     "the finish sample says nothing about the run itself")
        elif any(h is True for _, h in states):
            d["outcome"]["build_lock_held"] = known(True, "the build lock was held at " + ", ".join(n for n, h in states if h))
        elif states and all(h is False for _, h in states):
            d["outcome"]["build_lock_held"] = known(False, f"flock -n test free at {' and '.join(n for n, _ in states)}")
        else:
            d["outcome"]["build_lock_held"] = unknown("the build lock could not be tested (no --root to locate it)")
        log = self.run_path(p["log"])
        if not os.path.isfile(log):
            d["outcome"]["log"] = unknown(f"no {p['log']} in the run dir")
            return d
        scan = {}
        try:
            scan = self.scan_log(log)
            d["outcome"]["log"] = known({"path": log, "bytes": os.path.getsize(log), "lines": scan["lines"],
                                         "truncated_last_line": scan["truncated"]}, "the run's stdout log")
        except Exception as e:   # noqa: BLE001
            self.error(f"outcome.log: scan failed: {type(e).__name__}: {e}")
            d["outcome"]["log"] = unknown(f"scan failed: {e}")
        if not scan:
            return d
        # Game version as the log shows it; the configured one stays beside it.
        if scan.get("toc") is not None:
            ver = p["toc_versions"].get(scan["toc"])
            if ver:
                d["build"]["game_version"] = known(ver, f"log TOC 0x{scan['toc']:X} (tools/spu/kz3ver.py table)")
            else:
                d["build"]["game_version"] = unknown(f"log TOC 0x{scan['toc']:X} is not in the profile's table")
            cfg = val(d["build"].get("game_version_configured"))
            if ver and cfg and cfg != ver:
                self.error(f"game version: configured {cfg}, the log's TOC says {ver} (kept the log's)")
        else:
            d["build"]["game_version"] = (d["build"].get("game_version_configured")
                                          or unknown("no `[ppu] run: ... toc` line in the log and nothing configured"))
        # Exit code and liveness.
        if scan["exit_code"] is not None:
            d["outcome"]["exit_code"] = known(scan["exit_code"], "the harness's [exit code] line in the log")
            alive = False
        else:
            d["outcome"]["exit_code"] = known(None, "no [exit code] line: still running when the harness stopped it")
            alive = True
        # Run length: config, --seconds, the runner's own log (backfill), else the logger file's last timestamp.
        secs = d["config"].get("run_seconds")
        if not is_known(secs) and seconds is not None:
            secs = known(seconds, "--seconds given to finish")
        if not is_known(secs):
            secs = self.runner_seconds()
        d["config"]["run_seconds"] = secs
        self.step("outcome", "log_end_ms", self.logger_end)
        # Start time for runs made before runprov (backfill): the logger file's name, when there is one.
        if not d["run"].get("started_at"):
            t0 = self.logger_start_epoch()
            if t0 is not None:
                d["run"]["started_at"] = datetime.datetime.fromtimestamp(t0, datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
                d["run"]["started_at_source"] = "the logger file's name (backfill)"
        # Metrics (tools/kz3_metrics.py), reused; fps_window over the profiler window (env, else the log) so an A/B
        # (a paired comparison) reads it from run.json instead of reparsing the log.
        window = None
        s = (val(d["config"].get("env")) or {}).get(p["prof_env"]) if is_known(d["config"].get("env")) else None
        if s and "," in s:
            window = [int(x) for x in s.split(",")[:2]]
        elif scan.get("prof"):
            window = list(scan["prof"])

        def f_metrics():
            m = importlib.import_module(p["metrics_module"])
            r = m.measure(log, alive, window)
            r["window"] = window
            return known(r, f"tools/{p['metrics_module']}.py measure()" + (f" over window {window}" if window else ""))
        self.step("outcome", "metrics", f_metrics)
        self.step("outcome", "levels_seen", lambda: self.levels_seen(log))
        self.step("outcome", "captures", lambda: self.captures(log))
        self.step("outcome", "gpu", self.gpu_id)
        metrics = val(d["outcome"]["metrics"]) or {}
        flips, last = metrics.get("flips"), metrics.get("last_flip_ms")
        if flips is None:
            flips, last = scan["flips"], scan["last_flip_ms"]
        d["outcome"]["flips"] = known(flips, "max `[flip] N` in the log")
        d["outcome"]["last_flip_ms"] = known(last, "last `[flip] N at T ms` in the log")
        # Early stop: presenting ended well before the run did.
        end_ms, end_src = None, None
        if is_known(secs):
            end_ms, end_src = val(secs) * 1000, f"run_seconds ({secs['source']})"
        elif is_known(d["outcome"].get("log_end_ms")):
            end_ms, end_src = val(d["outcome"]["log_end_ms"]), "the logger file's last timestamp"
        if end_ms is None:
            d["outcome"]["stopped_early"] = unknown("run length unknown (no run_seconds and no logger file)")
        elif last is None or last == 0:
            d["outcome"]["stopped_early"] = known(True, "no flip in the log at all")
        else:
            margin = p["early_stop_margin_s"] * 1000
            d["outcome"]["stopped_early"] = known(
                last < end_ms - margin,
                f"last flip {last} ms vs run end {end_ms} ms ({end_src}) minus {p['early_stop_margin_s']} s margin")
        fs = scan["first_stop"]
        d["outcome"]["first_stop"] = (known(fs, "first log line matching a stop signature of the profile; "
                                                "approx_ms is the last flip time before it")
                                      if fs else known(None, "no line matches a stop signature of the profile"))
        d["outcome"]["prof_window"] = (known(scan["prof"], "the log's `[prof] on` line") if scan.get("prof")
                                       else known(None, "no `[prof] on` line in the log"))
        if scan.get("pad_events") is not None:
            d["outcome"]["pad_events_log"] = known(scan["pad_events"], "the runtime's PAD_SCRIPT event count line")
        # Cache state: a start snapshot wins; else the runtime's own start line; else mtimes (backfill).
        cs = d["fixtures"].get("cache_start")
        if not is_known(cs):
            t0 = self.logger_start_epoch()
            dated = None
            if t0 is not None:
                try:
                    dated = val(self.cache_snapshot(before_epoch=t0))
                except Exception as e:   # noqa: BLE001
                    self.error(f"fixtures.cache_start: dating the cache files failed: {e}")
            if scan.get("pso_start"):
                k, _lib_on, b = scan["pso_start"]
                files = dated["shader_files"] if dated else None
                d["fixtures"]["cache_start"] = known(
                    {"dir": dated["dir"] if dated else None, "shader_files": files, "pso_keys": k,
                     "pso_library_bytes": b, "state": "cold" if k == 0 and b == 0 and not files else "warm"},
                    "the runtime's `[pso-preload] start` line (keys on file, library bytes)"
                    + ("; shader files dated by mtime against the log's start (backfill)" if dated
                       else "; shader file count unknown"))
            elif dated is not None:
                d["fixtures"]["cache_start"] = known(dated, "backfill: cache files in the run dir dated by mtime "
                                                            "against the logger's start time, not a snapshot")
            else:
                d["fixtures"]["cache_start"] = unknown("no start snapshot, no `[pso-preload] start` line and no "
                                                       "logger file to date the cache files against")
        for name, why in (("save", "no start snapshot (run made before runprov); the dir now holds what the run wrote"),
                          ("pad", f"no start snapshot and no `{p['pad_env']}` count line in the log")):
            if not is_known(d["fixtures"].get(name)):
                if name == "pad" and scan.get("pad_events") is not None:
                    d["fixtures"]["pad"] = known({"sha256": None, "events": scan["pad_events"],
                                                  "truncated": scan["pad_events"] >= p["pad_max_events"],
                                                  "max_events": p["pad_max_events"]},
                                                 "the runtime's PAD_SCRIPT event count line (script text unknown)")
                else:
                    d["fixtures"][name] = unknown(why)
        for name in ("exe", "git", "patchset", "ps3recomp_rev", "toolchain"):
            if name not in d["build"]:
                d["build"][name] = unknown("no start phase (run made before runprov)")
        if "exe" in d["build"] and not is_known(d["build"]["exe"]):
            self.step("build", "exe", lambda: self.exe_after())
        for name in ("env", "proton", "display"):
            if name not in d["config"]:
                d["config"][name] = unknown("no start phase (run made before runprov)")
        return d

    def exe_after(self):
        staged = self.run_path(os.path.join("app", self.p["exe_name"]))
        if not os.path.isfile(staged):
            return unknown(f"no {self.p['exe_name']} in the run dir")
        return known({"sha256": sha256_file(staged), "size": os.path.getsize(staged), "path": staged},
                     "sha256 of the exe in the run dir after the run (backfill; the harness does not rewrite it)")

    def scan_log(self, log):
        p = self.p
        r = {"lines": 0, "truncated": False, "flips": 0, "last_flip_ms": 0, "first_stop": None, "exit_code": None,
             "toc": None, "prof": None, "pad_events": None, "pso_start": None}
        last_line = ""
        with open(log, encoding="latin-1") as f:
            for n, line in enumerate(f, 1):
                last_line = line
                r["lines"] = n
                m = p["flip_re"].match(line)
                if m:
                    r["flips"] = max(r["flips"], int(m.group(1)))
                    r["last_flip_ms"] = max(r["last_flip_ms"], int(m.group(2)))
                    continue
                if r["first_stop"] is None:
                    for kind, rx in p["stop_signatures"]:
                        if rx.search(line):
                            r["first_stop"] = {"kind": kind, "line": n, "approx_ms": r["last_flip_ms"],
                                               "text": line.rstrip("\n")[:200]}
                            break
                if r["toc"] is None:
                    m = p["toc_log_re"].match(line)
                    if m:
                        r["toc"] = int(m.group(1), 16)
                        continue
                if r["prof"] is None:
                    m = p["prof_log_re"].match(line)
                    if m:
                        r["prof"] = [int(m.group(1)), int(m.group(2))]
                        continue
                if r["pad_events"] is None:
                    m = p["pad_events_log_re"].match(line)
                    if m:
                        r["pad_events"] = int(m.group(1))
                        continue
                if r["pso_start"] is None:
                    m = p["pso_start_log_re"].match(line)
                    if m:
                        r["pso_start"] = (int(m.group(1)), m.group(2) == "yes", int(m.group(3)))
                        continue
                m = p["exit_re"].search(line)
                if m:
                    r["exit_code"] = int(m.group(1))
        r["truncated"] = bool(last_line) and not last_line.endswith("\n")
        return r

    def logger_file(self):
        fs = sorted(glob.glob(self.run_path(self.p["logger_glob"])))
        return fs[-1] if fs else None

    def logger_end(self):
        lf = self.logger_file()
        if not lf:
            return unknown(f"no {self.p['logger_glob']} (logger file) in the run dir")
        size = os.path.getsize(lf)
        with open(lf, "rb") as f:
            f.seek(max(0, size - 65536))
            tail = f.read().decode("latin-1").splitlines()
        for line in reversed(tail):
            m = self.p["logger_line_re"].match(line)
            if m:
                return known(int(m.group(1)), f"last timestamped line of {os.path.relpath(lf, self.run_dir)}")
        return unknown("no timestamped line at the end of the logger file")

    def logger_start_epoch(self):
        lf = self.logger_file()
        if not lf:
            return None
        m = self.p["logger_name_re"].search(os.path.basename(lf))
        if not m:
            return None
        t = datetime.datetime.strptime(m.group(1), "%Y%m%d-%H%M%S").replace(tzinfo=datetime.timezone.utc)
        return t.timestamp()

    def runner_seconds(self):
        p = self.p
        rl = self.root_path(p["runner_log"].format(name=os.path.basename(self.run_dir))) if self.root else None
        if not rl or not os.path.isfile(rl):
            return unknown("no --seconds, and no runner log " + (rl or p["runner_log"]) + " to read it from")
        for line in open(rl, errors="replace"):
            m = p["runner_seconds_re"].search(line)
            if m:
                return known(int(m.group(1)), f"runner log {os.path.relpath(rl, self.root)}: '{m.group(0)}'")
        return unknown(f"runner log {os.path.relpath(rl, self.root)} has no run-length line")


def load_profile(name):
    return importlib.import_module(f"runprofile_{name}").PROFILE


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for c in ("start", "finish", "show"):
        s = sub.add_parser(c)
        s.add_argument("run_dir")
        s.add_argument("--profile", default="kz3")
        s.add_argument("--root", help="repo checkout the run was made from (git, patch stamp, lift version)")
        s.add_argument("--seconds", type=int)
        if c == "start":
            s.add_argument("--exe")
            s.add_argument("--display", choices=["xvfb", "real"])
            s.add_argument("--proton")
    a = ap.parse_args(argv)
    if a.cmd == "show":
        print(open(os.path.join(a.run_dir, "run.json")).read())
        return 0
    try:
        prov = Prov(load_profile(a.profile), a.run_dir, a.root)
        os.makedirs(prov.run_dir, exist_ok=True)
        prov.load_or_new(a.cmd)
        try:
            if a.cmd == "start":
                prov.start(a.exe, a.seconds, a.display, a.proton)
            else:
                prov.finish(a.seconds)
        except Exception:   # noqa: BLE001
            prov.error(f"{a.cmd}: " + traceback.format_exc().strip().splitlines()[-1])
        out = prov.write()
        n = len(prov.doc.get("errors", []))
        print(f"[runprov] {a.cmd}: {out}" + (f" ({n} error(s), see run.json)" if n else ""))
    except Exception as e:   # noqa: BLE001 - a provenance failure must never fail the run
        print(f"[runprov] {a.cmd} failed without writing run.json: {type(e).__name__}: {e}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

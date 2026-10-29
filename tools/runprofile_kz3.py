"""KZ3-specific knowledge for tools/runprov.py (run provenance) and run comparisons (comparability): the log
signatures, env prefixes, cache and fixture paths of a kz3.exe run under tools/proton-test.sh. The two tools stay
game-agnostic and read everything title-specific from PROFILE (the design brief in docs/notes-to-review/diag-tool.md
asks for exactly this split: core / profile).

Every entry below was read off the repo or an existing run dir (2026-10-11); the comments say where.
"""
import re
import os


def _kz3ver_default():
    """tools/spu/kz3ver.py's `DEFAULT = "..."` line (read, not imported: an import would exit on a bad KZ3_EBOOT_VER)."""
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "spu", "kz3ver.py")
    m = re.search(r'^DEFAULT = "(100|114)"', open(p, encoding="utf-8").read(), re.M) if os.path.isfile(p) else None
    return m.group(1) if m else "114"


PROFILE = {
    "name": "kz3",
    # Layout of a run dir (tools/proton-test.sh): the exe's directory is copied to app/, the game writes its log,
    # caches and savedata relative to app/ (its VFS root).
    "exe_name": "kz3.exe",
    "log": "app/app_stdout.txt",
    # The logger (patch 2030) also writes app/logs/Killzone3-PC-YYYYMMDD-HHMMSS.log whose first column is the process
    # time in ms: the last line's timestamp is when the log ended (the stdout log has no timestamps).
    "logger_glob": "app/logs/Killzone3-PC-[0-9]*.log",
    "logger_name_re": re.compile(r"kz3-(\d{8}-\d{6})\.log$"),       # UTC, from the `[log] started` line
    "logger_line_re": re.compile(r"^\s*(\d+)\s+\d+\s+"),
    # Savedata: cellSaveData's root relative to the exe dir (tools/spu/run.sh --save-from copies it into the stage).
    "save_rel": "app/gamedata",
    "save_source_env": "KZ3_RUN_SAVE_FROM",                        # exported by tools/spu/run.sh --save-from
    # Shader disk cache (libs/video/rsx_live_draw.c: "scratch\\rsx_shader_cache_v%u" under the exe dir) and the
    # PSO preload files of patch 2031 inside it. pso_keys.bin: 24-byte header {magic "PSPL", version, rec_size,
    # rs_size, cache_version, reserved} then rec_size-byte records (repeated headers are skipped by the runtime).
    "cache_dir_glob": "app/scratch/rsx_shader_cache_v*",
    "cache_file_glob": "[VP]_*.bin",
    "pso_keys": "pso_keys.bin",
    "pso_keys_magic": 0x4C505350,
    "pso_library": "pso_library.bin",
    # Env the runtime reads (prefixes); names that look like secrets are redacted.
    "env_prefixes": ("KZ3_", "LD_", "RSX_", "PAD_", "GCM_", "SPU_"),
    "secret_re": re.compile(r"(token|secret|password|passwd|webhook|apikey|api_key|credential)", re.I),
    # tools/proton-inner.sh sets these when unset (the logger writes to stdout, no error box): part of every run.
    "env_defaults": {"KZ3_LOG_STDOUT": "1", "KZ3_NO_ERROR_BOX": "1"},
    # Scripted input (libs/input/cellPad.c, patch 3006 PAD_SCRIPT v2): up to 1,024 events (PAD_SCRIPT_MAX); was 32 before 3006.
    "pad_env": "PAD_SCRIPT",
    "pad_max_events": 1024,
    # Sampling profiler window (patch 1033): KZ3_PROF=FROM,TO seconds; the log echoes it.
    "prof_env": "KZ3_PROF",
    "prof_log_re": re.compile(r"^\[prof\] on: (\d+)\.\.(\d+) s"),
    # Game version from the log: `[ppu] run: code 0x..., toc 0x...` (tools/spu/kz3ver.py: TOC per EBOOT version).
    "toc_log_re": re.compile(r"^\[ppu\] run: code 0x[0-9A-Fa-f]+, toc 0x([0-9A-Fa-f]+)"),
    "toc_versions": {0x132C960: "100", 0x139FE40: "114"},
    "game_version_file": "work/spu/eboot_ver",                     # the SPU lift's version (tools/spu/run.sh)
    "game_version_env": "KZ3_EBOOT_VER",
    "game_version_default": _kz3ver_default(),                     # tools/spu/kz3ver.py DEFAULT (114 since step 6)
    "patchset_stamp": "work/ps3recomp-patched/.kz3-patchset",      # the boot check writes it
    # The build lock (tools/build-kz3.sh: flock on the main checkout's work/locks/build.lock): a run made while a
    # build held it shared the machine with up to four cc1plus: fps drift in boot checks.
    "build_lock": "work/locks/build.lock",
    "cmake_cache": "build/kz3-win/CMakeCache.txt",                 # toolchain, when the build dir is there
    # Log lines parsed after the run.
    "flip_re": re.compile(r"^\[flip\] (\d+) at (\d+) ms"),
    "exit_re": re.compile(r"\[exit code (-?\d+)\]"),
    "pad_events_log_re": re.compile(r"^\[cellPad\] PAD_SCRIPT: (\d+) event"),
    "pso_start_log_re": re.compile(r"^\[pso-preload\] start: (\d+) keys on file, library (\w+) \((\d+) bytes on disk\)"),
    # Stop signatures, in the order they are reported; the first line matching any of them is `first_stop`.
    "stop_signatures": (
        ("spu-job-chain-stall", re.compile(r"^\[fiber\] S=0x[0-9A-Fa-f]+ waiting")),
        ("gcm-park-not-patched", re.compile(r"park 0x[0-9A-Fa-f]+ not patched")),
        ("crash", re.compile(r"\[CRASH\]|DEADCA7|unhandled exception|EXCEPTION_ACCESS|GUARDVIOLATION")),
        ("unresolved-call", re.compile(r"\[hle\] unresolved NID|\[null-call\]")),
    ),
    # Presenting that stops more than this many seconds before the run's end counts as an early stop
    # (the sampled `[flip]` lines come every 10 flips, loads pause presenting for a few seconds).
    "early_stop_margin_s": 30,
    # The runner's own stdout (tools/spu/run.sh keeps it): run length for backfills of runs made before runprov.
    "runner_log": "work/spu/run-{name}.log",
    "runner_seconds_re": re.compile(r"still running after (\d+)s"),
    # Metrics module (tools/kz3_metrics.py): measure(log, alive) -> dict; reused, not reparsed.
    "metrics_module": "kz3_metrics",
}

"""Wizard test fixture (never shipped): stands in for kz3-setup/release/firstrun.py in the payload that
tools/release/wizard_test.sh compiles, so the setup wizard's build page can be screenshotted without game data.

It takes firstrun.py's command line, writes the same --status-file lines (LOG, then PROGRESS ..., then RESULT) with
firstrun.py's step names and weights, a few seconds per step, and exits. KZ3_WIZARD_DEMO_FAIL=1 ends with
RESULT ok=0 at the compile step (the build-error dialog). KZ3_WIZARD_DEMO_SECS = seconds per step (default 3).
--stop-file as in firstrun.py: once the file appears, RESULT ok=0 step=cancelled (the Stop button).
"""
import argparse
import os
import sys
import time

STEPS = [("check", 1), ("load", 4), ("inventory", 1), ("ppu-lift", 7), ("spu-lift", 22),
         ("configure", 1), ("compile", 63), ("stamp", 1)]


def main(argv=None):
    ap = argparse.ArgumentParser()
    for opt in ("--root", "--dump", "--update", "--eboot", "--toolchain", "--log-dir", "--log-name",
                "--status-file", "--stop-file", "--shaders"):
        ap.add_argument(opt, default="")
    a, _ = ap.parse_known_args(argv)
    log_path = os.path.join(a.log_dir or ".", a.log_name or "demo.log")
    head = f"LOG path={log_path}"
    secs = float(os.environ.get("KZ3_WIZARD_DEMO_SECS", "3"))
    fail = os.environ.get("KZ3_WIZARD_DEMO_FAIL") == "1"
    t0 = time.monotonic()

    def status(line):
        with open(log_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        if a.status_file:
            tmp = a.status_file + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(head + "\n" + line + "\n")
            os.replace(tmp, a.status_file)

    total_w = sum(w for _, w in STEPS)
    done_w = 0
    for i, (name, w) in enumerate(STEPS):
        n = 4 if name == "compile" else 1
        for k in range(n):
            pct = 100.0 * (done_w + w * k / n) / total_w
            extra = f"done={k * 1100} total=4400" if name == "compile" else "done=0 total=1"
            status(f"PROGRESS step={i + 1}/{len(STEPS)} name={name} {extra} "
                   f"elapsed={int(time.monotonic() - t0)} pct={pct:.1f}")
            end = time.monotonic() + (secs / n if name != "compile" else secs)
            while time.monotonic() < end:
                if a.stop_file and os.path.exists(a.stop_file):
                    status(f"RESULT ok=0 step=cancelled elapsed={int(time.monotonic() - t0)}")
                    return 130
                time.sleep(0.2)
        if fail and name == "compile":
            status(f"RESULT ok=0 step=compile elapsed={int(time.monotonic() - t0)}")
            return 1
        done_w += w
    status(f"RESULT ok=1 step=- elapsed={int(time.monotonic() - t0)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

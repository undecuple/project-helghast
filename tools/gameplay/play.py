#!/usr/bin/env python3
"""Bounded log-gated PAD_FILE play and marker-relative captures inside the Proton sandbox."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
SCHEMA = "kz3.play/1"
BUTTONS = dict(SELECT=1, L3=2, R3=4, START=8, UP=16, RIGHT=32, DOWN=64, LEFT=128,
               L2=256, R2=512, L1=1024, R1=2048, TRIANGLE=4096, CIRCLE=8192,
               CROSS=16384, SQUARE=32768)
FLIP = re.compile(r"^\[flip\] (\d+) at (\d+) ms")
FATAL = re.compile(r"\[CRASH\]|DEADCA7|\[null-call\]|\[hle\] unresolved NID|\[exit code")
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}\Z")


class PlayError(Exception):
    pass


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def positive(value, label, limit=1200):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not 0 < value <= limit:
        raise PlayError(f"{label} must be positive and at most {limit}")
    return value


def mask(value):
    if isinstance(value, str):
        if value.upper() in BUTTONS:
            return BUTTONS[value.upper()]
        try:
            value = int(value, 0)
        except ValueError as exc:
            raise PlayError(f"unknown button: {value}") from exc
    if type(value) is not int or not 1 <= value <= 65535:
        raise PlayError("key mask must be a nonzero 16-bit integer or button name")
    return value


def load_scenario(path):
    try:
        doc = json.loads(Path(path).read_text())
    except (OSError, ValueError) as exc:
        raise PlayError(f"cannot read scenario: {exc}") from exc
    if not isinstance(doc, dict) or set(doc) - {"schema", "name", "steps"} or doc.get("schema") != SCHEMA:
        raise PlayError(f"scenario requires schema {SCHEMA}, name and steps")
    if not isinstance(doc.get("name"), str) or not NAME.fullmatch(doc["name"]):
        raise PlayError("scenario name must be a short filename-safe identifier")
    steps = doc.get("steps")
    if not isinstance(steps, list) or not 1 <= len(steps) <= 1024:
        raise PlayError("scenario needs 1..1024 steps")
    allowed = {"wait": {"text"}, "key": {"button", "reports"}, "flips": {"count"}, "capture": set()}
    seen, gated = set(), False
    for step in steps:
        if not isinstance(step, dict) or step.get("op") not in allowed:
            raise PlayError("step op must be wait, key, flips or capture")
        ident = step.get("id")
        if not isinstance(ident, str) or not NAME.fullmatch(ident) or ident in seen:
            raise PlayError("step IDs must be unique short filename-safe identifiers")
        seen.add(ident)
        if set(step) - ({"id", "op", "timeout"} | allowed[step["op"]]):
            raise PlayError(f"unknown fields in step {ident}")
        positive(step.get("timeout", 30), f"timeout in {ident}")
        if step["op"] == "wait":
            if not isinstance(step.get("text"), str) or not 1 <= len(step["text"]) <= 4096 or "\n" in step["text"]:
                raise PlayError("wait text must be a nonempty single-line literal")
            gated = True
        elif step["op"] == "key":
            mask(step.get("button"))
            n = step.get("reports", 4)
            if type(n) is not int or not 1 <= n <= 1000:
                raise PlayError("reports must be an integer in 1..1000")
        elif step["op"] == "flips":
            n = step.get("count")
            if type(n) is not int or not 1 <= n <= 100000:
                raise PlayError("flip count must be an integer in 1..100000")
        elif not gated:
            raise PlayError("capture requires a preceding log-state wait")
    return doc


def inside(path, parent):
    return path.resolve().is_relative_to(parent.resolve())


class LogTail:
    """Consume whole lines, preserving later lines in the same read and partial writes."""
    def __init__(self, path):
        self.path, self.offset, self.buffer, self.identity = Path(path), 0, b"", None

    def line(self):
        if b"\n" not in self.buffer:
            try:
                with self.path.open("rb") as stream:
                    st = os.fstat(stream.fileno())
                    ident = (st.st_dev, st.st_ino)
                    if self.identity is not None and (ident != self.identity or st.st_size < self.offset):
                        raise PlayError("run log was replaced or truncated")
                    self.identity = ident
                    stream.seek(self.offset)
                    data = stream.read(65536)
                    self.offset += len(data)
                    self.buffer += data
            except FileNotFoundError:
                return None
        if len(self.buffer) > 1048576 and b"\n" not in self.buffer:
            raise PlayError("run log contains an oversized incomplete line")
        if b"\n" not in self.buffer:
            return None
        line, self.buffer = self.buffer.split(b"\n", 1)
        return line.decode("latin1").rstrip("\r")


def capture_display(path, timeout):
    if not os.environ.get("DISPLAY"):
        raise PlayError("capture needs the runner's sandbox DISPLAY")
    subprocess.run(["import", "-window", "root", str(path)], timeout=timeout, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


class Driver:
    def __init__(self, scenario, run, timeout, capture=capture_display):
        self.path, self.run = Path(scenario), Path(run)
        self.doc = load_scenario(self.path)
        self.tail = LogTail(self.run / "app/app_stdout.txt")
        self.input = self.run / "app/play-input.txt"
        self.start = time.monotonic()
        self.deadline = self.start + positive(timeout, "run timeout")
        self.capture_fn, self.flip, self.last_gate = capture, None, None
        self.report = dict(schema=SCHEMA, name=self.doc["name"], scenario_sha256=digest(self.path),
                           controller_sha256=digest(__file__), status="running", steps=[], captures=[],
                           capture_semantics="marker-relative display snapshot; exact guest frame not established")
        self.trace = None

    def event(self, kind, **fields):
        record = {"event": kind, "elapsed_s": round(time.monotonic() - self.start, 6), **fields}
        if self.trace:
            self.trace.write(json.dumps(record) + "\n")
            self.trace.flush()
        return record

    def observe(self, line):
        if FATAL.search(line):
            raise PlayError(f"run stopped or reported a fatal marker: {line[:250]}")
        match = FLIP.match(line)
        if match:
            current = dict(count=int(match[1]), ms=int(match[2]))
            if self.flip and (current["count"] < self.flip["count"] or current["ms"] < self.flip["ms"]):
                raise PlayError("flip counters moved backwards")
            self.flip = current

    def drain(self, end=None):
        while (line := self.tail.line()) is not None:
            if time.monotonic() >= min(end or self.deadline, self.deadline):
                raise PlayError("deadline exceeded while draining log")
            self.observe(line)

    def until(self, predicate, end, what):
        while time.monotonic() < min(end, self.deadline):
            line = self.tail.line()
            if line is not None:
                self.observe(line)
                if predicate(line):
                    return line
            else:
                if predicate(None):
                    return None
                time.sleep(0.02)
        raise PlayError(f"timeout: {what}")

    def channel_idle(self, end, require_file=False):
        # Do not consume post-ack state markers while waiting for the runtime's truncate.
        while time.monotonic() < min(end, self.deadline):
            exists = self.input.exists()
            if (exists and self.input.stat().st_size == 0) or (not exists and not require_file):
                return
            time.sleep(0.02)
        raise PlayError("timeout: input channel admission/idle")

    def step(self, step):
        ident, op = step["id"], step["op"]
        end = min(self.deadline, time.monotonic() + step.get("timeout", 30))
        self.event("step-start", id=ident, op=op)
        result = dict(id=ident, op=op)
        if op == "wait":
            line = self.until(lambda line: line is not None and step["text"] in line, end, ident)
            self.last_gate = dict(id=ident, line=line, observed_flip=self.flip)
            result["gate"] = self.last_gate
        elif op == "key":
            self.drain(end)  # an old acknowledgement cannot satisfy a new command
            self.channel_idle(end)
            command = f"0x{mask(step['button']):04X} {step.get('reports', 4)}\n"
            temp = self.input.with_suffix(".new")
            temp.write_text(command)
            os.replace(temp, self.input)
            self.event("input-request", id=ident, command=command.strip())
            ack = f"[cellPad] PAD_FILE press 0x{mask(step['button']):04X}"
            line = self.until(lambda line: line is not None and line == ack, end, f"acknowledgement for {ident}")
            # The runtime logs admission before truncating the channel. Do not overwrite a later queued key
            # in that interval; acknowledgement alone is insufficient evidence that the file is reusable.
            self.channel_idle(end, require_file=True)
            result.update(command=command.strip(), acknowledgement=line, observed_flip=self.flip)
        elif op == "flips":
            self.drain(end)  # settling starts at the current observed counter, not an older buffered marker
            if self.flip is None:
                self.until(lambda _: self.flip is not None, end, "first flip")
            before = dict(self.flip)
            target = before["count"] + step["count"]
            self.until(lambda _: self.flip is not None and self.flip["count"] >= target, end, ident)
            result.update(before=before, after=dict(self.flip), requested=step["count"])
        else:
            self.drain(end)
            if self.flip is None:
                raise PlayError("capture requires an observed guest flip")
            before = self.flip
            folder = self.run / "app/captures"
            folder.mkdir(parents=True, exist_ok=True)
            dest, temp = folder / f"play-{ident}.png", folder / f"play-{ident}.part.png"
            self.capture_fn(temp, max(0.01, end - time.monotonic()))
            if time.monotonic() >= end or temp.read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
                raise PlayError(f"capture {ident} failed or exceeded its deadline")
            os.replace(temp, dest)
            self.drain(end)
            result.update(path=str(dest.relative_to(self.run)), sha256=digest(dest), gate=self.last_gate,
                          observed_flip_before=before, observed_flip_after=self.flip, exact_frame=False)
            self.report["captures"].append(result.copy())
        result.update(status="complete", elapsed_s=round(time.monotonic() - self.start, 6))
        self.report["steps"].append(result)
        self.event("step-complete", **result)

    def run_steps(self):
        self.run.mkdir(parents=True, exist_ok=True)
        output = self.run / "play.json"
        if output.exists() or (self.run / "play.jsonl").exists():
            raise PlayError("refusing to overwrite an existing play report; use a fresh run name")
        with (self.run / "play.jsonl").open("x") as self.trace:
            try:
                if self.input.exists() and self.input.stat().st_size:
                    raise PlayError("input channel contains a stale command")
                self.event("start", scenario_sha256=self.report["scenario_sha256"])
                for step in self.doc["steps"]:
                    if time.monotonic() >= self.deadline:
                        raise PlayError("run deadline exceeded")
                    self.step(step)
                self.report["status"] = "complete"
            except (PlayError, OSError, subprocess.SubprocessError) as exc:
                self.report.update(status="failed", error=str(exc))
            finally:
                self.event("finish", status=self.report["status"])
                self.report["trace_sha256"] = digest(self.run / "play.jsonl")
                self.report["elapsed_s"] = round(time.monotonic() - self.start, 6)
                temp = output.with_suffix(".new")
                temp.write_text(json.dumps(self.report, indent=2) + "\n")
                os.replace(temp, output)
        return 0 if self.report["status"] == "complete" else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="command", required=True)
    validate = sub.add_parser("validate")
    validate.add_argument("scenario", type=Path)
    drive = sub.add_parser("drive")
    drive.add_argument("scenario", type=Path)
    drive.add_argument("run", type=Path)
    drive.add_argument("timeout", type=float)
    args = ap.parse_args(argv)
    try:
        if not inside(args.scenario, ROOT):
            raise PlayError("scenario must be inside this checkout")
        doc = load_scenario(args.scenario)
        if args.command == "validate":
            print(f"play: valid {doc['name']} ({len(doc['steps'])} steps)")
            return 0
        if not inside(args.run, ROOT / "work/proton") or args.run.resolve() == (ROOT / "work/proton").resolve():
            raise PlayError("run must be a directory under this checkout's work/proton")
        def cancelled(*_):
            raise PlayError("runner stopped the play controller")
        signal.signal(signal.SIGTERM, cancelled)
        driver = Driver(args.scenario, args.run, args.timeout)
        rc = driver.run_steps()
        print(f"play: {driver.report['status']}: {driver.report.get('error', doc['name'])}")
        return rc
    except (PlayError, OSError) as exc:
        print(f"play: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())

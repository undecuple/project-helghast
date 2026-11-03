#!/usr/bin/env python3
"""installer/run.cmd (the portable zip's entry point), static check: no cmd on this host, so the script is read, not run.
  - no line inside a parenthesised block echoes %PY% or any path variable (a ")" in the folder name ended the block early);
    the script has no parenthesised block at all;
  - pause is conditional: only after a failure or a double-click start, never with KZ3_NO_PAUSE;
  - the interpreter and the log name are still there, and the file keeps CRLF line ends.
A real cmd.exe check (folder "A (x)" under Windows, started from a console: returns without waiting) is pending."""
import os
import re
import sys

P = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "installer", "run.cmd")
FAILS = []


def check(ok, msg):
    print(("ok   " if ok else "FAIL ") + msg)
    if not ok:
        FAILS.append(msg)


def main():
    raw = open(P, "rb").read()
    lines = [ln for ln in raw.decode("utf-8").splitlines() if not re.match(r"\s*rem\b", ln, re.I)]
    check(b"\r\n" in raw and b"\n" not in raw.replace(b"\r\n", b""), "CRLF line ends")
    depth, inblock_echo = 0, []
    for ln in lines:
        code = re.sub(r'"[^"]*"', "", ln)             # quoted text does not open or close blocks
        if depth and re.search(r"%[A-Za-z~:]", ln):
            inblock_echo.append(ln.strip())
        depth += code.count("(") - code.count(")")
        depth = max(depth, 0)
        if re.search(r"\)\s*$|^\s*\(|\bdo\s*\(|else\s*\(|\bif\b.*\($", code):
            inblock_echo.append("block: " + ln.strip())
    check(not any(x.startswith("block:") for x in inblock_echo), "no parenthesised block (a ')' in %PY% cannot end one)")
    check(not [x for x in inblock_echo if not x.startswith("block:")], "no variable expanded inside a block")
    body = "\n".join(lines)
    check(bool(re.search(r"^pause\s*$", body, re.M)), "pause is still there")
    check("KZ3_NO_PAUSE" in body and "cmdcmdline" in body and 'not "%RC%"=="0"' in body,
          "pause only after a failure or a double-click start; KZ3_NO_PAUSE skips it")
    pause_at = body.index("\npause")
    pre = body[:pause_at]
    check(pre.rfind(":wait") > pre.rfind("exit /b"), "pause is reached only through :wait")
    check("firstrun.py" in body and "--log-name Project-Helghast-firstrun.log" in body, "interpreter call and log name kept")
    print("test_run_cmd: " + ("PASS" if not FAILS else "FAIL (%d)" % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

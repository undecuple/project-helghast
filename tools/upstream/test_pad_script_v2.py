#!/usr/bin/env python3
"""Focused test for ps3recomp patch 3006 (PAD_SCRIPT v2: per-event holds, analog segments, file form, raised limit).

Extracts the real parser and poll code (PAD_SCRIPT_* defines, PadScriptEvent/PadScript, pad_script_skip/blank/
number/axis/parse/poll/read_file) from libs/input/cellPad.c of the tree under test (default work/ps3recomp-patched;
OLD_TREE / NEW_TREE override) and drives it with a fake clock and fake polls. Checks: the old "sec:mask" format
(latched at the event time, held exactly 40 polls), digital holds in seconds, analog segments (values -1..1 to
0..255, triggers, the latest segment wins, centre afterwards, default 1 s), 47 events kept (v1 kept 32), the drop
counter past PAD_SCRIPT_MAX, comments and newlines through a script file, unparsable tokens skipped, and the log
rate limit (20 lines per wall second, then one summary line). Upstream v0.13.0 has none of this (no parser to
extract). Negative control: the tree under test with PAD_SCRIPT_MAX set back to 32 (the 47-event case then fails).
Needs a C compiler (cc). Prints one PASS/FAIL line."""
import os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import _extract as X

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
%s
static int fails = 0;
#define CHECK(name, cond) do { if (cond) printf("ok   %%s\n", name); else { fails++; printf("FAIL %%s (line %%d)\n", name, __LINE__); } } while (0)
static PadScript S;
static unsigned mask; static unsigned char ax[6]; static int axs;
static int poll(double now) { mask = 0; axs = 0; return pad_script_poll(&S, now, &mask, ax, &axs); }
static void fresh(const char* text) { memset(&S, 0, sizeof S); pad_script_parse(&S, text); }
int main(void) {
    int i, n;
    /* A: v1 format, latched, exactly 40 polls, two events, trailing separators tolerated */
    fresh("10:0x4000,25:0x0008, ");
    CHECK("v1 parse count", S.n == 2 && S.bad == 0 && S.ev[0].mask == 0x4000 && S.ev[1].mask == 8);
    poll(9.99); CHECK("v1 not before time", mask == 0);
    n = 0; for (i = 0; i < 40; i++) { poll(10.0 + i * 0.01); if (mask == 0x4000) n++; }
    CHECK("v1 fires for 40 polls", n == 40);
    poll(10.5); CHECK("v1 released after 40 polls", mask == 0);
    poll(30.0); CHECK("v1 latched late (loading screen)", mask == 0x0008);
    /* B: digital hold in seconds outlives 40 polls */
    fresh("5:0x0100/3");
    n = 0; for (i = 0; i < 300; i++) { poll(5.0 + i * 0.01); if (mask == 0x0100) n++; }
    CHECK("hold 3 s: 300 polls over 3 s all pressed", n == 300);
    poll(8.01); CHECK("hold 3 s: released after", mask == 0);
    /* C: analog segments */
    fresh("20:0,-1,0,0/10 40:0,0,1,0,0,1/2 50:0.5,0,0,0 60:1,0,0,0/5,62:-1,0,0,0/1");
    CHECK("analog parse count", S.n == 5 && S.bad == 0 && S.ev[0].analog && !S.ev[0].mask);
    poll(19.9); CHECK("analog not before", !axs);
    poll(20.0); CHECK("left stick up", axs && ax[0] == 128 && ax[1] == 0 && ax[2] == 128 && ax[3] == 128 && ax[4] == 0 && ax[5] == 0 && mask == 0);
    poll(30.0); CHECK("still at 10 s", axs);
    poll(30.01); CHECK("over after 10 s", !axs);
    poll(40.0); CHECK("right stick right + R2 pressure and bit", axs && ax[2] == 255 && ax[5] == 255 && ax[4] == 0 && mask == 0x0200);
    poll(50.0); CHECK("half deflection = 192, default 1 s", axs && ax[0] == 192);
    poll(51.5); CHECK("default hold over", !axs);
    /* latched: the holds count from the poll that fired them (60.5 and 62.5), not from the scripted second */
    poll(60.5); CHECK("first overlap segment", axs && ax[0] == 255);
    poll(62.5); CHECK("latest segment wins", axs && ax[0] == 0);
    poll(63.6); CHECK("earlier segment still active", axs && ax[0] == 255);
    poll(65.6); CHECK("both over", !axs);
    /* D: more than 32 events (pad-r12 has 47) */
    { static char big[8192]; int k, p = 0; big[0] = 0;
      for (k = 0; k < 47; k++) p += snprintf(big + p, sizeof big - p, "%%d:0x4000,", 100 + k);
      fresh(big); CHECK("47 events kept", S.n == 47 && S.dropped == 0);
      n = 0; for (k = 0; k < 47; k++) n += poll(100.0 + k); CHECK("47 events fire", n == 47); }
    /* E: past PAD_SCRIPT_MAX counted as dropped */
    { char* big = (char*)malloc((PAD_SCRIPT_MAX + 5) * 16); int k, p = 0;
      for (k = 0; k < PAD_SCRIPT_MAX + 5; k++) p += sprintf(big + p, "%%d:0x4000,", k);
      fresh(big); free(big);
      CHECK("limit reached, 5 dropped", S.n == PAD_SCRIPT_MAX && S.dropped == 5); }
    /* F: a file with comments, blank lines, tabs */
    { char* txt = pad_script_read_file("%s");
      CHECK("file read", txt != NULL);
      if (txt) { fresh(txt); free(txt);
        CHECK("file: 3 events, comments skipped", S.n == 3 && S.bad == 0 && S.ev[0].t == 10 && S.ev[0].mask == 0x4000
              && S.ev[1].mask == 0x8000 && S.ev[1].hold_s == 1.0 && S.ev[2].analog && S.ev[2].ax[1] == 0 && S.ev[2].hold_s == 10); } }
    /* G: bad tokens skipped and counted, the rest parsed */
    fresh("10:0x4000,garbage,20:0x0008 30:1,2,3 40:0x0010/-1 50:0x0020");
    CHECK("bad tokens counted, good ones kept", S.n == 3 && S.bad == 3 && S.ev[2].mask == 0x20);
    /* H: log rate limit: 25 events at t=1 print 20 lines, the 26th at t=2 prints the summary */
    { char big[1024]; int k, p = 0; for (k = 0; k < 25; k++) p += sprintf(big + p, "1:0x4000,"); sprintf(big + p, "2:0x0008");
      fresh(big); printf("--rate--\n"); poll(1.0); poll(2.0); printf("--end--\n"); }
    printf(fails ? "FAILED %%d\n" : "ALL-OK\n", fails);
    return 0;
}
'''

SCRIPT_FILE = "# menu\n10:0x4000\t# CROSS\n\n   20:0x8000/1   # SQUARE held 1 s\n# walk\n430:0,-1,0,0/10\n"
PARTS = [r"static const char\* pad_script_skip\(const char\* p\)", r"static const char\* pad_script_blank\(const char\* p\)",
         r"static int pad_script_number\(const char\* p, const char\*\* end, double\* out\)",
         r"static unsigned char pad_script_axis\(double v, int trigger\)",
         r"static int pad_script_parse\(PadScript\* s, const char\* text\)",
         r"static int pad_script_poll\(PadScript\* s, double now, unsigned\* mask, unsigned char ax\[6\], int\* ax_set\)",
         r"static char\* pad_script_read_file\(const char\* path\)"]


def run(src, script_path):
    """Returns (last line of the harness, its stdout)."""
    code = []
    for d in ("PAD_SCRIPT_MAX", "PAD_SCRIPT_HOLD_POLLS", "PAD_SCRIPT_LOG_PER_SEC"):
        m = re.search(rf"^#define {d}\b.*$", src, re.M)
        if not m:
            return "no parser (#define %s missing)" % d, ""
        code.append(m.group(0))
    for ty in ("PadScriptEvent", "PadScript"):
        m = re.search(r"typedef struct \{[^}]*\} " + ty + ";", src)
        if not m:
            return f"no parser (typedef {ty} missing)", ""
        code.append(m.group(0))
    for sig in PARTS:
        f = X.function(src, sig)
        if not f:
            return f"no parser ({sig[:40]}... missing)", ""
        code.append(f)
    with tempfile.TemporaryDirectory() as d:
        open(f"{d}/t.c", "w").write(HARNESS % ("\n".join(code), script_path.replace("\\", "\\\\")))
        r = subprocess.run(["cc", "-O0", "-w", f"{d}/t.c", "-o", f"{d}/t", "-lm"], capture_output=True, text=True)
        if r.returncode:
            return "compile error: " + r.stderr[:600], ""
        out = subprocess.run([f"{d}/t"], capture_output=True, text=True).stdout
    lines = out.strip().splitlines()
    # the rate-limit case: 20 "script t=1.0s press" lines, then one summary at the second change
    rate = out.split("--rate--\n")[1].split("--end--")[0].splitlines() if "--rate--" in out else []
    presses = sum(1 for l in rate if l.startswith("[cellPad] script t=1.0s press 0x4000 for 40 polls"))
    summary = sum(1 for l in rate if l.startswith("[cellPad] script t=1s: 5 more event(s) fired"))
    if lines and lines[-1] == "ALL-OK" and not (presses == 20 and summary == 1):
        lines[-1] = f"FAILED rate limit (presses {presses}, summary {summary})"
    return (lines[-1] if lines else "no output"), out


old_t, new_t = X.trees()
rd = lambda t: open(os.path.join(t, "libs/input/cellPad.c")).read()
src_new = rd(new_t)
with tempfile.NamedTemporaryFile("w", suffix=".pad", delete=False) as f:
    f.write(SCRIPT_FILE); script_path = f.name
try:
    r_old, _ = run(rd(old_t), script_path)
    r_new, out_new = run(src_new, script_path)
    limit = "#define PAD_SCRIPT_MAX         1024"
    assert limit in src_new, "PAD_SCRIPT_MAX define not found in the tree under test"
    assert "WARNING %d event(s) dropped over the limit" in src_new, "drop warning missing from cellPadGetData"
    r_ctl, _ = run(src_new.replace(limit, "#define PAD_SCRIPT_MAX 32"), script_path)
finally:
    os.unlink(script_path)
if r_new != "ALL-OK":
    print("\n".join(l for l in out_new.splitlines() if l.startswith("FAIL") or l.startswith("compile")) or out_new[:800])
for label, r in (("upstream", r_old), ("tree under test", r_new), ("negative control", r_ctl)):
    print(f"{label:16s} PAD_SCRIPT v2 harness -> {r!r}   (want 'ALL-OK')")
X.verdict("3006 PAD_SCRIPT v2", r_old != "ALL-OK", r_new == "ALL-OK", r_ctl != "ALL-OK")

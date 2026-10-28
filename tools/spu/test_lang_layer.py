#!/usr/bin/env python3
"""Host test for patch 9933 (KZ3 layer over upstream PR #333, PS3_LANG): KZ3_LANG is an alias of PS3_LANG (PS3_LANG
wins; "auto" = unset), and Japanese / Korean / Chinese S / T set the font library's Han preference (jp / kr / sc / tc).
Extracts sysutil_language and its helpers from the patched cellSysutil.c (NEW_TREE=dir, default
work/ps3recomp-patched), compiles them with a stub kz3_font_set_han_pref, and runs one process per environment (the
language is cached per process). SKIP without the patched tree; needs cc.
  nix develop --command tools/sandbox.sh -- python3 tools/spu/test_lang_layer.py"""
import os, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "upstream")); import _extract as X

tree = X.trees()[1]
c_path, h_path = (os.path.join(tree, "libs/system", f) for f in ("cellSysutil.c", "cellSysutil.h"))
if not os.path.isfile(c_path):
    print("SKIP: no patched toolkit"); sys.exit(0)
src, hdr = open(c_path).read(), open(h_path).read()
fns = [X.function(src, r"static int sysutil_ieq\(") , X.function(src, r"static void sysutil_locale_code\("),
       X.function(src, r"static void sysutil_host_language\("), X.function(src, r"static s32 sysutil_language\(void\)")]
if not all(fns) or "KZ3_LANG" not in fns[3]:
    print("FAIL: sysutil_language without patch 9933 (or PR #333) in", c_path); sys.exit(1)
defs = "\n".join(re.findall(r"^#define CELL_SYSUTIL_LANG_\w+\s+\d+.*$", hdr, re.M))
code = f"""#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
typedef int s32;
{defs}
static const char* g_han = "-";
void kz3_font_set_han_pref(const char* lang) {{ g_han = lang; }}
{chr(10).join(fns)}
int main(void) {{ s32 l = sysutil_language(); fprintf(stderr, "RESULT %d %s\\n", (int)l, g_han); return 0; }}
"""
CASES = [  # env, want lang, want han
    ({}, 1, "-"),                                         # nothing set, LANG=C host: English US
    ({"KZ3_LANG": "ja"}, 0, "jp"),
    ({"KZ3_LANG": "9"}, 9, "kr"),
    ({"KZ3_LANG": "zh-s"}, 11, "sc"),
    ({"PS3_LANG": "zh-t"}, 10, "tc"),
    ({"PS3_LANG": "fr", "KZ3_LANG": "ja"}, 2, "-"),       # PS3_LANG wins
    ({"KZ3_LANG": "auto"}, 1, "-"),                       # auto = unset -> host (C) -> English US
    ({"KZ3_LANG": "auto", "LANG": "de_DE.UTF-8"}, 4, "-"),
    ({"KZ3_LANG": "klingon"}, 1, "-"),                    # unknown: English
]
fail = 0
with tempfile.TemporaryDirectory() as d:
    open(f"{d}/t.c", "w").write(code)
    r = subprocess.run(["cc", "-O0", "-w", f"{d}/t.c", "-o", f"{d}/t"], capture_output=True, text=True)
    if r.returncode:
        print("FAIL: build\n" + r.stderr[-800:]); sys.exit(1)
    for env, want_l, want_h in CASES:
        e = {k: v for k, v in os.environ.items() if k not in ("PS3_LANG", "KZ3_LANG", "LANGUAGE", "LC_ALL", "LC_MESSAGES")}
        e["LANG"] = "C"; e.update(env)
        out = subprocess.run([f"{d}/t"], env=e, capture_output=True, text=True).stderr
        m = re.search(r"RESULT (-?\d+) (\S+)", out)
        got = (int(m.group(1)), m.group(2)) if m else None
        ok = got == (want_l, want_h); fail += not ok
        print(f"{'PASS' if ok else 'FAIL'} {env or '(none)'} -> {got} (want {(want_l, want_h)})")
print(f"lang layer: {'PASS' if not fail else f'FAIL ({fail})'}")
sys.exit(1 if fail else 0)

#!/usr/bin/env python3
"""Host test (release pass 92): the launcher writes display.ini and online.ini with the defaults when they are missing
and never touches an existing file; display.ini's values equal the initialisers in port/kz3/main.cpp (the game's
defaults) and its keys are the ones the game's writer emits (port/patches). No game data."""
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import launch as la  # noqa: E402

bad = 0


def check(ok, msg):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + msg)


def parse(text):
    return {m.group(1): m.group(2).strip() for m in re.finditer(r"^([a-z_0-9]+) = (.*)$", text, re.M)}


vals = parse(la.DISPLAY_INI_TEXT)
src = open(os.path.join(REPO, "port", "kz3", "main.cpp"), encoding="utf-8", errors="replace").read()
patches = "".join(open(os.path.join(REPO, "port", "patches", f), encoding="utf-8", errors="replace").read()
                  for f in sorted(os.listdir(os.path.join(REPO, "port", "patches"))) if f.startswith(("2095", "2103", "2107", "2109", "2113", "2121", "2126")))

m = re.search(r"s_dm_saved\[4\] = \{ RSX_DM_(\w+), RSX_DM_(\w+), RSX_DM_(\w+), RSX_DM_(\w+) \}", src)
check(bool(m), "main.cpp has the s_dm_saved initialiser")
if m:
    mode, scal, filt, vs = (x.lower() for x in m.groups())
    check(vals["mode"] == mode, f"mode {vals['mode']} = main.cpp {mode}")
    check(vals["scaling"] == scal, f"scaling {vals['scaling']} = main.cpp {scal}")
    check(vals["filter"] == filt, f"filter {vals['filter']} = main.cpp {filt}")
    check(vals["vsync"] == ("on" if vs == "vsync_on" else "off"), f"vsync {vals['vsync']} = main.cpp {vs}")
m = re.search(r"static int s_mb_saved = (\d+), s_fps_saved = RSX_DM_FPS_(\d+), s_bloom_saved = (\d+);", src)
check(bool(m), "main.cpp has the motion blur / fps / bloom initialiser")
if m:
    check(vals["motion_blur"] == m.group(1), f"motion_blur {vals['motion_blur']} = main.cpp {m.group(1)}")
    check(vals["fps_limit"] == m.group(2), f"fps_limit {vals['fps_limit']} = main.cpp {m.group(2)}")
    check(vals["bloom"] == m.group(3), f"bloom {vals['bloom']} = main.cpp {m.group(3)}")
check(re.search(r"static int s_fov_saved;", src) is not None and vals["fov"] == "default", "fov default = 0 (static, zero) = 'default'")
check(re.search(r"static int s_h169_saved = 1;", src) is not None and vals["hud_169"] == "on", "hud_169 default 1 = on")
check(re.search(r"static int s_unf_saved = RSX_UNF_KEEP;", src) is not None and vals["unfocused"] == "keep", "unfocused default KEEP = keep")
check(re.search(r"static int s_maxbl_saved = -1;.*missing = borderless", src) is not None and vals["maximize"] == "borderless",
      "maximize missing = borderless")
for k in vals:   # every key is one the game's writer or reader names
    check(re.search(r'"%s = |\b%s\b' % (k, k), patches) is not None, f"key {k} appears in the display patches")
check(len(vals) == 11, "eleven display keys")

# online.ini: only comments, and its keys are the ones kz3_net.h documents
net = open(os.path.join(REPO, "port", "net", "kz3_net.h"), encoding="utf-8").read()
doc = re.search(r"Top-level keys: (.*?)\. A \"\[server", net, re.S).group(1)
names = set(re.findall(r"[a-z_]+", doc.replace("(the default server)", "").replace("(which section to use)", "")))
keys = set(re.findall(r"^; ([a-z_]+) = ", la.ONLINE_INI_TEXT, re.M))
check(keys <= names | {"server"} and {"host", "ports", "region", "online_id", "ticket_file", "server"} == keys, f"online keys {sorted(keys)}")
check(not [l for l in la.ONLINE_INI_TEXT.splitlines() if l.strip() and not l.startswith(";")], "online.ini has comment lines only")

with tempfile.TemporaryDirectory() as tmp:
    d = os.path.join(tmp, "data", "sub")
    check(la.ensure_default_inis(d) == [], "writes into a missing folder without a problem")
    for n, t in (("display.ini", la.DISPLAY_INI_TEXT), ("online.ini", la.ONLINE_INI_TEXT)):
        with open(os.path.join(d, n), "rb") as f:
            b = f.read()
        check(b == t.replace("\n", "\r\n").encode("ascii"), f"{n} written whole, CRLF")
    open(os.path.join(d, "display.ini"), "w").write("[display]\nmode = fullscreen\n")
    open(os.path.join(d, "online.ini"), "w").close()
    la.ensure_default_inis(d)
    check(open(os.path.join(d, "display.ini")).read() == "[display]\nmode = fullscreen\n", "existing display.ini kept")
    check(os.path.getsize(os.path.join(d, "online.ini")) == 0, "existing empty online.ini kept")
    os.mkdir(os.path.join(tmp, "x"))
    os.mkdir(os.path.join(tmp, "x", "display.ini"))
    check(la.ensure_default_ini(os.path.join(tmp, "x"), "display.ini", "a") == "" and os.path.isdir(os.path.join(tmp, "x", "display.ini")),
          "a folder of that name is left alone")
    open(os.path.join(tmp, "f"), "w").close()
    r = la.ensure_default_inis(os.path.join(tmp, "f", "no"))
    check(len(r) == 2 and all(x.startswith("could not write") for x in r), "an unwritable folder is reported, not raised")

print("test_default_ini:", "FAIL" if bad else "PASS")
sys.exit(1 if bad else 0)

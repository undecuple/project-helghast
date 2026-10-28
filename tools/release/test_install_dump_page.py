#!/usr/bin/env python3
"""Host test (release pass 57): installer/kz3.iss' dump and update pages. Static checks (no ISCC, no game data):
a pick of PS3_GAME or PS3_GAME\\USRDIR is resolved to the folder that holds PS3_GAME (and USRDIR of the update to the
update folder), the TITLE_ID is read from PARAM.SFO and decides (US ok, EU ok with the untested-region note, other ids
refused by name), the update must carry the disc's id and APP_VER 01.14, the [Files] update folder uses the disc's id,
and the Inno pitfalls (no '#' line start, no '}' inside a { } comment) hold. Also runs the SFO reader's algorithm,
transcribed to Python, against firstrun_check.read_sfo on a synthetic PARAM.SFO."""
import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ISS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "installer", "kz3.iss")
if not os.path.isfile(ISS):
    print("SKIP: installer/kz3.iss not in this tree")
    sys.exit(0)
t = open(ISS, encoding="utf-8").read()
code = t[t.find("[Code]"):]
bad = 0


def check(ok, msg):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + msg)


def func(name):
    m = re.search(r"(?:function|procedure) " + name + r"\b.*?\nend;", code, re.S)
    return m.group(0) if m else ""


f = func("ResolveDump")
check("'PS3_GAME\\PARAM.SFO'" in f and "EndsWithName(D, 'USRDIR')" in f and "EndsWithName(D, 'PS3_GAME')" in f and
      "ExtractFileDir" in f, "ResolveDump walks up from PS3_GAME and PS3_GAME\\USRDIR")
f = func("ResolveUpdate")
check("'USRDIR'" in f and "PARAM.SFO" in f and "ExtractFileDir" in f, "ResolveUpdate takes USRDIR to the update folder")
f = func("SfoField")
check("LoadStringFromFile" in f and "SfoInt(S, 8, 4)" in f and "SfoInt(S, 12, 4)" in f and "SfoInt(S, 16, 4)" in f and
      "20 + 16 * I" in f and "SfoInt(S, E, 2)" in f and "SfoInt(S, E + 4, 4)" in f and "SfoInt(S, E + 12, 4)" in f,
      "SfoField reads the header and the 16-byte entries like read_sfo")
f = func("DumpProblem")
check("not Killzone 3 (US BCUS98234 or EU BCES01007)" in f and "'This is ' + Id" in f and "RegionOf(Id) = ''" in f,
      "DumpProblem refuses other ids naming the id found")
f = func("RegionOf")
check("'BCUS98234'" in f and "'BCES01007'" in f, "RegionOf knows exactly the US and EU ids")
f = func("UpdateIdProblem")
check("Id <> DiscId" in f and "'01.14'" in f and "'APP_VER'" in f and "'TITLE_ID'" in f,
      "UpdateIdProblem: same TITLE_ID as the disc, APP_VER 01.14")
m = re.search(r"function NextButtonClick.*?\nend;", code, re.S)
nb = m.group(0) if m else ""
check("DumpPage.Values[0] := ResolveDump(DumpPage.Values[0])" in nb and "UpdatePage.Values[0] := ResolveUpdate(UpdatePage.Values[0])" in nb
      and "EU disc (BCES01007): untested region" in nb and "UpdateIdProblem(" in nb,
      "NextButtonClick resolves both pages in place, notes EU, checks the update's id")
m = re.search(r"function PrepareToInstall.*?\nend;", code, re.S)
pt = m.group(0) if m else ""
check("ResolveDump(DumpPage.Values[0])" in pt and "ResolveUpdate(UpdatePage.Values[0])" in pt and "UpdateIdProblem(" in pt
      and "DumpProblem(" in pt, "silent /DUMP= /UPDATE= resolve and check the same way")
check(re.search(r'Source: "\{code:UpdateSrc\}\\\*"; DestDir: "\{app\}\\game\\dev_hdd0\\game\\\{code:DiscTitleId\}"', t),
      "[Files] puts the update under the disc's title id")
check("DiscTitleId('')" in func("BuildUpdate"), "BuildUpdate uses the disc's title id")
braced = [c for c in re.findall(r"\{[^}]*\}", code) if "{" in c[1:] and not c.startswith("{#")]
check(not braced, "no '{' inside a { } comment in [Code]")
check(not [l for l in code.splitlines() if re.match(r"\s*#\d", l)], "no [Code] line starts with a #NN constant")

# the reader's algorithm in Python against firstrun_check.read_sfo
sys.path.insert(0, HERE)
try:
    import firstrun_check as fc
except ImportError:
    fc = None


def sfo_field(d, key):
    if len(d) < 20 or d[:4] != b"\0PSF":
        return ""
    def i(p, n):
        return int.from_bytes(d[p:p + n], "little") if p >= 0 and p + n <= len(d) else 0
    def s(p, mx):
        out = b""
        for k in range(p, min(len(d), p + mx)):
            if d[k] == 0:
                break
            out += d[k:k + 1]
        return out.decode()
    kt, dt, n = i(8, 4), i(12, 4), min(i(16, 4), 200)
    for k in range(n):
        e = 20 + 16 * k
        if s(kt + i(e, 2), 32) == key:
            return s(dt + i(e + 12, 4), i(e + 4, 4))
    return ""


def make_sfo(fields):
    keys, data, ents = b"", b"", b""
    for k, v in fields.items():
        ents += struct.pack("<HHIII", len(keys), 0x204, len(v) + 1, 16, len(data))
        keys += k.encode() + b"\0"
        data += v.encode() + b"\0" + b"\0" * (15 - len(v))
    kt = 20 + 16 * len(fields)
    return struct.pack("<4sIIII", b"\0PSF", 0x101, kt, kt + len(keys), len(fields)) + ents + keys + data


if fc:
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "PARAM.SFO")
        for tid in ("BCUS98234", "BCES01007", "BCES00227"):
            open(p, "wb").write(make_sfo({"APP_VER": "01.14", "TITLE_ID": tid, "VERSION": "01.01"}))
            check(sfo_field(open(p, "rb").read(), "TITLE_ID") == tid == fc.read_sfo(p)["TITLE_ID"] and
                  sfo_field(open(p, "rb").read(), "APP_VER") == "01.14", "SFO reader algorithm agrees with read_sfo: " + tid)
sys.exit(1 if bad else 0)

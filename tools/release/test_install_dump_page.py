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
check("not Killzone 3 (US BCUS98234, EU BCES01007, Asia BCAS25008 or JP BCJS30066)" in f and "'This is ' + Id" in f and "RegionOf(Id) = ''" in f,
      "DumpProblem refuses other ids naming the id found")
f = func("RegionOf")
check(all(("'%s'" % i) in f for i in ("BCUS98234", "BCES01007", "BCAS25008", "BCJS30066")) and f.count("Result := '") == 5,
      "RegionOf knows exactly the four ids")
f = func("UpdateIdProblem")
check("'01.14'" in f and "'APP_VER'" in f and "'TITLE_ID'" in f,
      "UpdateIdProblem: a known region's TITLE_ID, APP_VER 01.14")
check("Id <> DiscId" not in f and "RegionOf(Id) = ''" in f, "UpdateIdProblem refuses only unknown ids, not a mixed pair")
f = func("UpdateMixNote")
check("another region (" in f and "than your disc (" in f and "same game data" in f and "our tests" in f,
      "UpdateMixNote words the mixed pair as a tested note")
m = re.search(r"function NextButtonClick.*?\nend;", code, re.S)
nb = m.group(0) if m else ""
check("DumpPage.Values[0] := ResolveDump(DumpPage.Values[0])" in nb and "UpdatePage.Values[0] := ResolveUpdate(UpdatePage.Values[0])" in nb
      and "EU disc (BCES01007): untested region" in nb and "UpdateIdProblem(" in nb and "Asian disc (BCAS25008): untested region" in nb
      and "Japanese disc (BCJS30066): untested region" in nb and "UpdateMixNote(" in nb,
      "NextButtonClick resolves both pages in place, notes EU, checks the update's id")
m = re.search(r"function PrepareToInstall.*?\nend;", code, re.S)
pt = m.group(0) if m else ""
check("ResolveDump(DumpPage.Values[0])" in pt and "ResolveUpdate(UpdatePage.Values[0])" in pt and "UpdateIdProblem(" in pt
      and "DumpProblem(" in pt, "silent /DUMP= /UPDATE= resolve and check the same way")
check(re.search(r'Source: "\{code:UpdateSrc\}\\\*"; DestDir: "\{app\}\\game\\dev_hdd0\\game\\\{code:DiscTitleId\}"', t),
      "[Files] puts the update under the disc's title id")
check("DiscTitleId('')" in func("BuildUpdate"), "BuildUpdate uses the disc's title id")
# a decrypted ISO as the dump (pass 58): light page check (name + file), the verdict is firstrun_check's
f = func("IsIsoPick")
check("'.iso'" in f and "FileExists(Path)" in f and "not DirExists(Path)" in f and "CompareText(ExtractFileExt" in f,
      "IsIsoPick: an existing file named *.iso")
check("LoadStringFromFile" not in f and "TFileStream" not in f, "IsIsoPick does not read the 10 GB file")
check("IsIsoPick(D) then Exit" in func("ResolveDump"), "ResolveDump leaves an ISO pick alone")
check("IsIsoPick(Dir) then Exit" in func("DumpProblem") and "was not found" in func("DumpProblem"),
      "DumpProblem accepts an ISO file, names a missing .iso")
check("IsIsoPick(Dump) then Exit" in func("DiscIdOf"), "DiscIdOf is empty for an ISO (the id is read inside by firstrun)")
check("(DiscId <> '')" in func("UpdateMixNote"),
      "the mixed-pair note is given only when the disc's id is known")
check("CopyGameData and not IsIsoPick(DumpPage.Values[0])" in func("CopyDumpFolder"), "an ISO is not copied by [Files]")
check(re.search(r'Source: "\{code:DumpSrc\}\\\*";[^\n]*Check: CopyDumpFolder', t), "[Files] dump copy uses CopyDumpFolder")
check("not IsIsoPick(DumpPage.Values[0])" in func("BuildDump"), "BuildDump passes the ISO itself to firstrun.py")
check("SfoField(AddBackslash(UpdatePage.Values[0])" in func("DiscTitleId"), "DiscTitleId falls back to the update's id for an ISO")
f = func("DumpIsoClick")
check("GetOpenFileName(" in f and "*.iso" in f, "Browse for ISO opens a file dialog filtered to *.iso")
check("DumpIsoBtn.OnClick := @DumpIsoClick" in code and "DumpInfo.Top := DumpIsoBtn.Top" in code, "ISO button on the dump page")
check("IsIsoPick(DumpPage.Values[0]) then begin" in nb and "less than 14 GB free" in nb, "dump page warns about disk space for an ISO")
check("/DUMP=<dump folder or .iso>" in t, "silent /DUMP= documents .iso")
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


f = func("WritePaths")
check(all(("SetIniString('paths', '%s'" % k) in f for k in ("dump", "update", "eboot", "font_dir")) and
      "kz3-install.ini" in f and "BuildDump" in f and "BuildUpdate" in f and "BuildEboot" in f and "BuildFontArg" in f,
      "WritePaths writes the wizard folders to kz3-install.ini [paths]")
f = func("CurStepChanged")
check("ssPostInstall then WritePaths" in f and "buildnow" not in f.split("WritePaths")[0].splitlines()[-1],
      "WritePaths runs at ssPostInstall whether or not buildnow is ticked")
check("[INI]" not in t or "Section: \"paths\"" not in t, "no [INI] entry for [paths] (the setup log would show it)")
check("FontDirArg := BuildFontArg" in func("RunBuild"), "RunBuild and WritePaths share BuildFontArg")
if fc:
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "PARAM.SFO")
        for tid in ("BCUS98234", "BCES01007", "BCAS25008", "BCJS30066", "BCES00227"):
            open(p, "wb").write(make_sfo({"APP_VER": "01.14", "TITLE_ID": tid, "VERSION": "01.01"}))
            check(sfo_field(open(p, "rb").read(), "TITLE_ID") == tid == fc.read_sfo(p)["TITLE_ID"] and
                  sfo_field(open(p, "rb").read(), "APP_VER") == "01.14", "SFO reader algorithm agrees with read_sfo: " + tid)

# FAT32 split archive (public issue 4): data_ps3.psarc.66600, .66601, ... accepted like the single file
f = func("ArchiveProblem")
check("'data_ps3.psarc'" in f and "66600" in f and "IntToStr(N)" in f and "FileExists(Usr + 'data_ps3.psarc.' + IntToStr(N))" in f
      and "66601 to 66640" in f and "data_ps3.psarc.66600 is missing" in f and "data_ps3.psarc is missing" in f,
      "ArchiveProblem takes the file or consecutive parts from .66600 and names the missing file")
check("ArchiveProblem(AddBackslash(Dir) + 'PS3_GAME\\USRDIR\\')" in func("DumpProblem"),
      "DumpProblem asks ArchiveProblem instead of looking for data_ps3.psarc only")
check("FileExists(AddBackslash(Dir) + 'PS3_GAME\\USRDIR\\data_ps3.psarc')" not in func("DumpProblem"),
      "no single-file-only archive check is left in DumpProblem")


def archive_problem(usr):
    """ArchiveProblem transcribed: -> the name of the missing file, '' when the archive is there."""
    ex = lambda n: os.path.isfile(os.path.join(usr, "data_ps3.psarc" + n))
    if ex(""):
        return ""
    n = 66600
    while ex("." + str(n)):
        n += 1
    if n > 66600:
        return "data_ps3.psarc." + str(n) if any(ex("." + str(k)) for k in range(n + 1, n + 41)) else ""
    if any(ex("." + str(k)) for k in range(66601, 66641)):
        return "data_ps3.psarc.66600"
    return "data_ps3.psarc"


if fc:
    with tempfile.TemporaryDirectory() as tmp:
        cases = {"single": [""], "all": [".66600", ".66601", ".66602"], "gap": [".66600", ".66601", ".66603"],
                 "no first": [".66601", ".66602"], "first only": [".66600"], "none": [], "other": [".bak"]}
        for name, sufs in cases.items():
            usr = os.path.join(tmp, name.replace(" ", "_"))
            os.makedirs(usr)
            for s in sufs:
                open(os.path.join(usr, "data_ps3.psarc" + s), "wb").write(b"x")
            parts, missing = fc.split_parts(os.path.join(usr, "data_ps3.psarc"))
            check(archive_problem(usr) == missing, "ArchiveProblem and firstrun_check.split_parts agree: %s (%r)" % (name, missing))
sys.exit(1 if bad else 0)

#!/usr/bin/env python3
"""Host test (release pass 52): installer/kz3.iss keeps the install folder safe for the uninstaller, which removes
the whole {app}. Static checks: AppendDefaultDirName=yes, the rule that appends AppDirName on leaving wpSelectDir,
the refusal of an existing non-empty folder without unins000.exe, and the same check in PrepareToInstall (silent
/DIR= never reaches NextButtonClick). Needs no game data and no ISCC."""
import os
import re
import sys

ISS = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "installer", "kz3.iss")
t = open(ISS, encoding="utf-8").read()
bad = 0


def check(ok, msg):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + msg)


check(re.search(r"^AppendDefaultDirName=yes\s*$", t, re.M), "AppendDefaultDirName=yes")
check("(CurPageID = wpSelectDir) and (not WizardSilent)" in t and "WizardForm.DirEdit.Text := AddBackslash(D) + '{#AppDirName}'" in t,
      "NextButtonClick appends AppDirName on the dir page")
check("CompareText(ExtractFileName(D), '{#AppDirName}')" in t, "last folder name compared case-insensitively")
check(re.search(r"DirExists\(D\) and DirHasFiles\(D\) and \(not FileExists\(AddBackslash\(D\) \+ 'unins000\.exe'\)\)", t),
      "non-empty foreign folder refused (unins000.exe = ours)")
check("IsProtectedFolder" in t and "{userdocs}" in t and "{win}" in t, "system and user folders refused")
# A '}' inside a { } Pascal comment ends it early (ISCC: "'BEGIN' expected"); the code section's brace comments hold none
code = t[t.find("[Code]"):]
braced = [c for c in re.findall(r"\{[^}]*\}", code) if "{" in c[1:] and not c.startswith("{#")]
check(not braced, "no '{' inside a { } comment in [Code]: " + "; ".join(b[:40] for b in braced[:3]))
# ISPP reads any line that starts with '#' as a preprocessor directive ("Unknown preprocessor directive"), so no
# [Code] line may start with a #13#10-style character constant
hashlines = [l.strip()[:40] for l in code.splitlines() if re.match(r"\s*#\d", l)]
check(not hashlines, "no [Code] line starts with a #NN character constant: " + "; ".join(hashlines[:3]))
m = re.search(r"function PrepareToInstall.*?DefaultEboot;", t, re.S)
check(m and "InstallDirProblem(ExpandConstant('{app}'))" in m.group(0) and
      m.group(0).index("InstallDirProblem") < m.group(0).index("if not WizardSilent"),
      "PrepareToInstall checks the folder before the silent-only branch")
# OneDrive (playtester's log, 2026-10-28): the install folder inside OneDrive is refused, silent installs too
m = re.search(r"function InOneDrive.*?\nend;", t, re.S)
check(m and all(v in m.group(0) for v in ("'OneDrive'", "'OneDriveConsumer'", "'OneDriveCommercial'")) and
      "GetEnv(" in m.group(0) and "Lowercase(" in m.group(0) and "Pos(B + '\\', D) = 1" in m.group(0),
      "InOneDrive reads the three OneDrive env vars, case-insensitive whole-component prefix match")
m = re.search(r"function InstallDirProblem.*?\nend;", t, re.S)
check(m and "else if InOneDrive(D) then" in m.group(0) and "inside OneDrive" in m.group(0) and
      "outside OneDrive" in m.group(0) and "%LOCALAPPDATA%" in m.group(0),
      "InstallDirProblem refuses OneDrive with the plain message (also reached by PrepareToInstall for /DIR=)")
sys.exit(1 if bad else 0)

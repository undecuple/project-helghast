#!/usr/bin/env python3
"""Host test (release pass 63, public issue 8): installer/kz3.iss' EBOOT page and failure box. Static checks (no
ISCC, no game data):
- the EBOOT page tests the picked file's version from firstrun_check's own table (eboot_sha256.json, shipped as a
  dontcopy file; no size or hash written into the script) and refuses a known EBOOT of another version than the
  release's, naming the update's USRDIR folder; an unknown EBOOT passes as before;
- Recommended does not skip the page, DefaultEboot replaces, and a silent install refuses a refused EBOOT;
- the build's failure box shows firstrun.py's REASON lines before the log path, and a check-step failure says the
  next start asks again;
- the table parser (EbootVersionOf + JsonNumber + JsonField), transcribed to Python, reads the shipped table the
  way firstrun_check.EBOOT_ELF_SIZES does, with a negative control (unknown size -> no version)."""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ISS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "installer", "kz3.iss")
if not os.path.isfile(ISS):
    print("SKIP: installer/kz3.iss not in this tree")
    sys.exit(0)
import firstrun_check as fc  # noqa: E402

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


table = json.load(open(fc.HASHES_FILE, encoding="utf-8"))
# 1. the table is reused, not copied
check(re.search(r'Source: "\{#Payload\}\\kz3-setup\\release\\eboot_sha256\.json"; Flags: dontcopy', t) is not None,
      "eboot_sha256.json ships as a dontcopy file for the wizard")
check("ExtractTemporaryFile('eboot_sha256.json')" in func("EbootTable"), "EbootTable extracts the shipped table")
nums = {str(v["size"]) for k, v in table.items() if isinstance(v, dict)} | \
       {v["sha256"] for k, v in table.items() if isinstance(v, dict) and v.get("sha256")}
check(not [n for n in nums if n in t], "no known EBOOT size or SHA-256 written into kz3.iss")
check({str(v["size"]): k for k, v in table.items() if isinstance(v, dict)} ==
      {str(s): v for s, v in fc.EBOOT_ELF_SIZES.items()}, "table sizes = firstrun_check.EBOOT_ELF_SIZES")

# 2. the page refuses, the skip/default/silent paths use the same test
ep = func("EbootProblem")
check("V <> '{#EbootTarget}'" in ep and "This is the disc''s 1.00 EBOOT." in ep and
      "the decrypted form of ' +\n      'EBOOT.BIN in the update''s USRDIR folder" in ep and
      "AddBackslash(UpdatePage.Values[0]) + 'USRDIR'" in ep and "dev_hdd0\\game\\BCUS98234\\USRDIR" in ep,
      "EbootProblem: 1.00 refused, names the update's USRDIR and the usual folder")
check("Result := '';" in ep and "if (V <> '') and" in ep, "EbootProblem: an unknown EBOOT passes")
check('#define EbootTarget "1.14"' in t, "the release's EBOOT target is 1.14")
nb = func("NextButtonClick")
check("Problem := EbootProblem(EbootPage.Values[0]);" in nb and "MsgBox(Problem, mbError, MB_OK);" in nb,
      "Next on the EBOOT page refuses with the reason")
check("(EbootProblem(EbootPage.Values[0]) = '')" in func("ShouldSkipPage"),
      "Recommended shows the page again for a recorded refused EBOOT")
check("(EbootProblem(EbootPage.Values[0]) <> '')" in func("DefaultEboot"), "DefaultEboot replaces a refused EBOOT")
check("EbootProblem(EbootPage.Values[0]) <> ''" in func("PrepareToInstall"), "a silent install stops on it")
eb = t[t.find("EbootPage := CreateInputFilePage"):]
eb = eb[:eb.find("EbootPage.Add(")]
check("1.14 update''s USRDIR folder" in eb and "Not the disc''s PS3_GAME\\USRDIR\\EBOOT" in eb,
      "page text: the 1.14 update's USRDIR, not the disc's")

# 3. the failure box carries the reason
rb = func("RunBuild")
check("Pos('REASON ', Lines[I]) = 1" in rb and "BuildFailStep := Field(L, 'step');" in rb,
      "RunBuild reads the REASON lines after RESULT")
sb = func("ShowBuildError")
check("Why + 'The log is at:'" in sb and "BuildFailStep = 'check'" in sb and "asks again for the" in sb,
      "ShowBuildError: reason first, then the log; a check failure says the next start asks again")
check(not [l for l in code.splitlines() if re.match(r"\s*#\d", l)], "no [Code] line starts with a #NN constant")


# 4. EbootVersionOf's parse, transcribed
def pos(sub, s):
    return s.find(sub) + 1


def json_number(text, key):
    p = pos('"' + key + '": ', text)
    if not p:
        return ""
    i, r = p + len(key) + 4, ""
    while i <= len(text) and text[i - 1].isdigit():
        r += text[i - 1]
        i += 1
    return r


def version_of(text, size):
    r = text
    p = pos('": {', r)
    while p:
        k = r[:p - 1]
        v = k[k.rfind('"') + 1:]
        s = r[p - 1:]
        e = pos("}", s)
        if e:
            s = s[:e]
        if json_number(s, "size") and int(json_number(s, "size")) == size:
            return v
        r = r[p - 1 + 4:]
        p = pos('": {', r)
    return ""


raw = open(fc.HASHES_FILE, encoding="utf-8").read()
for s, v in fc.EBOOT_ELF_SIZES.items():
    check(version_of(raw, s) == v, f"parse: size {s} -> {v}")
check(version_of(raw, 64) == "" and version_of(raw, 20_745_953) == "", "control: an unknown size has no version")
check(version_of(json.dumps(table, indent=2), 20_745_952) == "1.00", "parse holds for indent 2 as well")

print("test_install_eboot_page:", "PASS" if not bad else f"FAIL ({bad})")
sys.exit(1 if bad else 0)

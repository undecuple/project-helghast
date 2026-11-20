#!/usr/bin/env python3
"""Host test (release, installer simplification pass 1): installer/kz3.iss' "Your game files" page. Static checks (no
ISCC, no game data): there is no "Setup type" page (TypePage), the page has the RPCS3 and Customise checkboxes,
Customise alone decides IsAdvanced, the RPCS3 folder page is skipped unless the RPCS3 box is ticked, the three game
pages are skipped once RPCS3 gave all three paths, silent installs skip the new pages, the wizard runs
rpcs3_import.py from the temp folder (files listed in [Files]), and every mode the script runs exists in
rpcs3_import.py's command line. Exit 0 = PASS."""
import os
import re
import sys

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


check("TypePage" not in t and "Setup type'," not in code, "no Setup type page left")
check("SourcePage.Add('Use my RPCS3 install" in code and "SourcePage.Add('Customise" in code, "the page has the RPCS3 and Customise boxes")
check("CreateInputOptionPage(wpLicense, 'Your game files'" in code and ", False, False);" in func("InitializeWizard"),
      "the page follows the licence and its boxes are not exclusive")
check("SourcePage.Values[1]" in func("IsAdvanced"), "Customise decides IsAdvanced")
sk = func("ShouldSkipPage")
check("PageID = RpcsPage.ID) then Result := not SourcePage.Values[0]" in sk, "RPCS3 folder page only with the RPCS3 box")
check("Rpcs3Filled and SourcePage.Values[0]" in sk and "PageID = DumpPage.ID" in sk and "PageID = EbootPage.ID" in sk,
      "the three game pages are skipped once RPCS3 gave the paths")
check("(PageID = SourcePage.ID) or (PageID = RpcsPage.ID)" in sk, "silent installs skip the new pages")
check("not IsAdvanced" in sk and "wpSelectTasks" in sk, "without Customise the tasks, fonts and Advanced pages are skipped")
nb = func("NextButtonClick")
check("CurPageID = RpcsPage.ID" in nb and "ResolveRpcs3" in nb, "Next on the RPCS3 page resolves it")
rv = func("ResolveRpcs3")
check("'--detect'" in rv and "'--resolve --rpcs3" in rv and "Rpcs3Filled := True" in rv, "resolve runs --detect and --resolve")
check("Rpcs3Filled := False" in rv, "every resolve starts unfilled")
check("SourcePage.Values[0] := False" in rv, "a failed lookup can fall back to picking the folders")
check("UseRpcs3" in func("RegisterPreviousData") and "UseRpcs3" in func("InitializeWizard"), "the RPCS3 answer is remembered")
for n in ("rpcs3_import", "rpcs3_locate", "rpcs3_text", "firstrun_check", "iso9660"):
    check(re.search(r'Source: "\{#Payload\}\\kz3-setup\\release\\%s\.py"; DestDir: "\{tmp\}\\vcheck"; Flags: dontcopy' % n, t) is not None,
          f"{n}.py is extracted for the wizard")
ri = open(os.path.join(HERE, "rpcs3_import.py"), encoding="utf-8").read()
for opt in ("--detect", "--resolve", "--eboot-kind", "--identify", "--out", "--rpcs3"):
    check(f'"{opt}"' in ri, f"rpcs3_import.py has {opt}")
check("-I " in code and "rpcs3_import.py" in code, "the helper runs in isolated mode (python -I)")
for bad_word in ("decrypt it with", "File >", "Install Packages"):
    check(bad_word not in code, f"wizard text does not say {bad_word!r}")
print("test_install_rpcs3_page:", "FAIL (%d)" % bad if bad else "PASS")
sys.exit(1 if bad else 0)

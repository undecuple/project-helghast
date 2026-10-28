#!/usr/bin/env python3
"""Host test: the files tools/release/names.py cannot reach repeat its names exactly (release pass 19).

Checks the kz3.iss #define block, the launcher's C and resource strings, run.cmd's log name, release.yml's setup
file name, the public files' links and credit line, and that no file a player or visitor reads still carries the
name before the rename outside the places that must keep it (the data-folder move, the Killzone3-PC- log prefix).
Needs no game data; runs from the repo (public/ present) or the exported public tree (public/ is the root there:
the public-only checks then SKIP the installer files that are not exported).
"""
import fnmatch
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import names  # noqa: E402

ROOT = HERE.parents[1]
PUB = ROOT / "public" if (ROOT / "public").is_dir() else ROOT
FAILS = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILS.append(what)


def read(p):
    try:
        return p.read_text(encoding="utf-8")
    except OSError:
        return None


def defines(text):
    return dict(re.findall(r'^#define (\w+) "([^"]*)"', text, re.M))


def release_yml(yml, iss):
    """release.yml as GitHub runs it (the release dry run): every published name, path and argument against names.py,
    kz3.iss and the repo."""
    z = names.zip_name("*")
    for n in (f"{names.SETUP_BASE}.exe", f"{names.SETUP_BASE}.exe.sha256", z, z + ".sha256"):
        check(f"installer/Output/{n}" in yml, f"release.yml uploads installer/Output/{n}")
        check(f" {n}" in yml.split("gh release create", 1)[-1], f"release.yml attaches {n} to the release")
    check("--clobber" not in yml and "--force" not in yml, "release.yml never overwrites (no --clobber/--force)")
    check("SHA256SUMS" in yml.split("gh release create", 1)[-1], "release.yml attaches SHA256SUMS")
    check("--prerelease" in yml and '--title "$GITHUB_REF_NAME"' in yml and "--verify-tag" in yml,
          "release.yml: pre-release while major is 0, title = tag, verify-tag")
    check("workflow_dispatch" in yml and "github.event_name == 'push'" in yml, "release.yml: dispatch never publishes")
    check("inputs.version" not in yml, "release.yml: version only from the tag / names.VERSION")
    check(fnmatch.fnmatch(names.zip_name("0.1.0"), z), "release.yml's zip glob matches names.zip_name()")
    check(f"name: {names.FILE_BASE}-${{{{ steps.ver.outputs.version }}}}" in yml, "release.yml artifact name")
    # every published-looking name in the workflow comes from FILE_BASE (catches a half-done rename)
    for tok in set(re.findall(r"[A-Z][a-z]+-[A-Z][a-z]+-[\w.*<>${}\- ]*?(?:setup|portable|\$\{\{)", yml)):
        check(tok.startswith(names.FILE_BASE + "-"), f"release.yml name {tok!r} starts with FILE_BASE")
    check(f'--launcher "build/launcher/{names.LAUNCHER_EXE}"' in yml, "release.yml passes the launcher's name")
    check("--out installer/Output" in yml and not re.search(r"^OutputDir=", iss, re.M),
          "release.yml's zip goes where ISCC writes (kz3.iss has no OutputDir: installer\\Output)")
    for script in sorted(set(re.findall(r"\btools/[\w/.-]+\.(?:py|sh)\b", yml))):
        check((ROOT / script).is_file(), f"release.yml calls {script}, which exists")
    for d in sorted(set(re.findall(r'"/D(\w+)=', yml))):
        check(f"#ifndef {d}\n" in iss, f"release.yml's ISCC /D{d} is a kz3.iss #ifndef default")
    m = re.search(r'INNO_VERSION: "([\d.]+)"', yml)
    check(m is not None and f"tested with {m.group(1)}" in iss, "release.yml pins the Inno version kz3.iss names")
    check("installer_art.py --fetch-fonts" in yml and '"..\\work\\release\\fonts"' in iss,
          "fonts fetched where kz3.iss's FontDir default reads them")
    check("trees/llvm-mingw/bin" in yml and '"llvm-mingw"' in (read(HERE / "toolchain.json") or ""),
          "release.yml's launcher compiler is toolchain.json's llvm-mingw")
    check("core.autocrlf false" in yml.split("uses: actions/checkout", 1)[0], "release.yml keeps LF before the checkout")


def main():
    iss = read(ROOT / "installer" / "kz3.iss")
    if iss is None:
        print("SKIP installer files (not in this tree)")
    else:
        d = defines(iss)
        want = {"AppName": names.PRODUCT, "AppDirName": names.DATA_FOLDER, "ProjectURL": names.REPO_URL,
                "SiteURL": names.SITE_URL, "SiteText": names.SITE_TEXT, "LauncherExe": names.LAUNCHER_EXE,
                "SetupBase": names.SETUP_BASE,
                "OldDataDirName": names.OLD_DATA_FOLDERS[0] if names.OLD_DATA_FOLDERS else ""}
        for k, v in want.items():
            check(d.get(k) == v, f"kz3.iss #define {k} = {v!r} (is {d.get(k)!r})")
        check("{08141EE8-C3FD-416B-9F09-5977CFF2F6A3}" in iss, "kz3.iss keeps the AppId (upgrades)")
        lit = [i + 1 for i, ln in enumerate(iss.splitlines())
               if names.PRODUCT in ln and not ln.lstrip().startswith((";", "#define", "{", "//"))]
        check(not lit, f"kz3.iss texts use {{#AppName}}, not the literal name (lines {lit[:5]})")
        check(d.get("AppDescription") == names.DESCRIPTION, "kz3.iss #define AppDescription = DESCRIPTION")
        check("AppComments={#AppDescription}\n" in iss and "WelcomeLabel2={#AppDescription}," in iss,
              "kz3.iss AppComments and the welcome text use AppDescription")
        c = read(ROOT / "installer" / "launcher" / "launcher.c")
        check(f'L"{names.PRODUCT}"' in c and names.ISSUES_URL in c, "launcher.c title and issues link")
        rc = read(ROOT / "installer" / "launcher" / "launcher.rc")
        for key, v in (("ProductName", names.PRODUCT), ("OriginalFilename", names.LAUNCHER_EXE),
                       ("Comments", names.SITE_URL)):
            check(f'VALUE "{key}", "{v}"' in rc, f"launcher.rc {key} = {v}")
        check(names.FIRSTRUN_LOG in read(ROOT / "installer" / "run.cmd"), "run.cmd log name")
        yml = read(PUB / ".github" / "workflows" / "release.yml")
        if yml is None and PUB == ROOT:
            print("SKIP release.yml (not in this tree)")
        else:
            check(yml is not None, "release.yml present")
        if yml is not None:
            release_yml(yml, iss)
    for rel, needles in (("README.md", (names.REPO_URL, f"# {names.PRODUCT}", names.DESCRIPTION, names.CREDIT)),
                         ("NOTICE.md", (names.REPO_URL, names.CREDIT)),
                         ("CITATION.cff", (f'title: "{names.PRODUCT}"', names.REPO_URL, names.DESCRIPTION)),
                         ("site/index.html", (f'<p class="desc">{names.DESCRIPTION}</p>',)),
                         (".recomp.json", (f'"project": "{names.PRODUCT}"', names.SITE_URL)),
                         ("docs/BUILDING.md", (f"{names.REPO_URL}\n", f"cd {names.REPO_SLUG}\n")),
                         (".github/ISSUE_TEMPLATE/config.yml", (names.REPO_URL,))):
        text = read(PUB / rel)
        for n in needles:
            check(text is not None and n in text, f"public {rel} has {n.strip()!r}")
    # the old name may stay only where it must (the move from the old data folder; the runtime's log prefix)
    allowed = {"bug_report.yml": "Killzone3-PC-crash"}
    for old in names.OLD_DATA_FOLDERS:
        pat = re.compile(re.escape(old) + "|" + re.escape(old.replace(" ", "").replace("3PC", "3-pc").lower())
                         + r"(?!-crash)", re.I)   # Killzone3-PC-crash-*: the runtime's log prefix, kept
        for p in sorted(PUB.rglob("*")):
            if PUB == ROOT and p.relative_to(ROOT).parts[0] in ("work", "build", "third_party", ".git"):
                continue   # the public tree's own build/run output and the submodule are not public pages
            if p.is_file() and p.suffix in (".md", ".cff", ".json", ".yml", ".html", ".css"):
                t = read(p) or ""
                hits = [m.group(0) for m in pat.finditer(t)]
                check(not hits, f"no {old!r} left in public/{p.relative_to(PUB)} {hits[:2]}")
        legacy = [m.group(0) for m in re.finditer(r"Killzone3-PC-\w+", read(PUB / ".github/ISSUE_TEMPLATE/bug_report.yml") or "")]
        check(all(h.startswith(allowed["bug_report.yml"]) for h in legacy), "bug_report.yml keeps only the runtime's log prefix")
    check(names.DESCRIPTION.startswith(names.TAG + " ") and "//" in names.TAG, "DESCRIPTION carries the TAG")
    art = read(HERE / "installer_art.py")
    check(art is None or "TAG_WIDE = names.TAG.upper()" in art, "installer_art.py takes the banner tag from names.TAG")
    print("test_names:", "FAIL" if FAILS else "PASS")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

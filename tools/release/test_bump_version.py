#!/usr/bin/env python3
"""Host test for bump_version.py: runs on a temp copy of the files it edits, never the real tree.

Covers a bump to the next patch version (every mirror, the markers, the CHANGELOG stub, test_version_agree passing
on the copy), idempotency, refusing a downgrade and bad syntax, and --check writing nothing. No game data needed.
Run: python3 tools/release/test_bump_version.py
"""
import contextlib
import io
import os
import pathlib
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REAL = pathlib.Path(HERE).parents[1]
sys.path.insert(0, HERE)
import bump_version  # noqa: E402
import names  # noqa: E402

COPY = ("public", "installer/kz3.iss", "installer/launcher/launcher.rc", "port/kz3/CMakeLists.txt", "port/kz3/app.rc",
        "tools/features.py", "docs/features.tsv", "docs/requirements.md", "docs/checkpoints/README.md")
PUBLIC_DOCS = ("docs/BUILDING.md", "docs/ROADMAP.md", "docs/GETTING-FILES.md", "docs/FEATURES.md", "docs/RELEASE-CRITERIA.md", "docs/screenshots", "site")
bad = 0


def check(ok, what):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + what)


def copy_one(src, out):
    out.parent.mkdir(parents=True, exist_ok=True)
    if src.is_dir():
        shutil.copytree(src, out, ignore=shutil.ignore_patterns(".git", "__pycache__"))
    else:
        shutil.copy2(src, out)


def make_copy(dst):
    """Copy only the files the test needs: the COPY list (in the public export, public/X is X at the root, pass 65: no
    copy of the whole tree, which another process creating and deleting files in it could break mid-copy)."""
    export = not (REAL / "public").is_dir()
    for rel in COPY:
        if export and rel == "public":
            for p in REAL.iterdir():   # the public files at the root: top-level files, the docs it edits and site/
                if p.is_file():
                    shutil.copy2(p, dst / p.name)
            for f in PUBLIC_DOCS:
                if (REAL / f).exists():
                    copy_one(REAL / f, dst / f)
            continue
        if export and rel.startswith("public/"):
            rel_src = rel[len("public/"):]
        else:
            rel_src = rel
        if export and not (REAL / rel_src).exists():
            continue   # the export lacks some internal files
        copy_one(REAL / rel_src, dst / (rel_src if export else rel))
    (dst / "tools/release").mkdir(parents=True, exist_ok=True)
    for p in sorted((REAL / "tools/release").iterdir()):
        if p.is_file() and p.suffix in (".py", ".json"):
            try:
                shutil.copy2(p, dst / "tools/release" / p.name)
            except FileNotFoundError:
                pass


def snapshot(root):
    return {str(p.relative_to(root)): p.read_bytes() for p in root.rglob("*") if p.is_file() and "__pycache__" not in p.parts}


def run(root, *args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = bump_version.main(["--root", str(root), *args])
    return rc, out.getvalue(), err.getvalue()


def next_patch(v):
    a, b, c = v.split(".")
    return f"{a}.{b}.{int(c) + 1}"


def main():
    cur = names.VERSION
    new = next_patch(cur)
    nums = ",".join((new.split(".") + ["0"])[:4])
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        make_copy(root)
        base = snapshot(root)

        rc, out, _ = run(root, cur, "--date", "2030-01-02")
        check(rc == 0 and snapshot(root) == base, f"current version {cur}: nothing changes (rc {rc})")
        rc, out, _ = run(root, new, "--check", "--date", "2030-01-02")
        check(rc == 0 and snapshot(root) == base and "would change" in out, "--check writes nothing, lists the changes")
        for v, why in (("0.0.1", "downgrade"), ("1.2", "bad syntax"), ("v0.1.2", "bad syntax with v"), ("x", "words")):
            rc, _, err = run(root, v)
            check(rc != 0 and snapshot(root) == base, f"{why} {v!r} refused, nothing written")
        rc, _, _ = run(root, new, "--date", "30-01-02")
        check(rc != 0 and snapshot(root) == base, "bad --date refused")

        t0_changelog = bump_version.at(root, "public/CHANGELOG.md").read_text(encoding="utf-8")
        rc, out, err = run(root, new, "--date", "2030-01-02")
        check(rc == 0, f"bump to {new} succeeds (rc {rc}) {err[-300:]}")
        t = lambda rel: bump_version.at(root, rel).read_text(encoding="utf-8")
        check(f'VERSION = "{new}"' in t("tools/release/names.py"), "names.py")
        rc_t = t("installer/launcher/launcher.rc")
        check(f"FILEVERSION {nums}" in rc_t and f"PRODUCTVERSION {nums}" in rc_t, "launcher.rc numbers")
        check(f'"FileVersion", "{new}"' in rc_t and f'"ProductVersion", "{new}"' in rc_t, "launcher.rc strings")
        check(f"\nversion: {new}\n" in t("public/CITATION.cff"), "CITATION.cff version")
        check(f"VERSION {new}\n" in t("port/kz3/CMakeLists.txt"), "CMakeLists project VERSION")
        check(f'#define AppVersion "{new}"' in t("installer/kz3.iss"), "kz3.iss AppVersion")
        readme = t("public/README.md")
        check(f"Version <!-- version -->{new}<!-- /version -->" in readme
              and f"releases/tag/v{new})<!-- /version -->" in readme, "README markers")
        check(f"<!-- version -->v{new}<!-- /version --> pre-release" in t("public/docs/BUILDING.md"), "BUILDING marker")
        check(f"latest <!-- version -->v{new}<!-- /version -->" in t("public/docs/ROADMAP.md"), "ROADMAP marker")
        cl = t("public/CHANGELOG.md")
        if "## [Unreleased]" in t0_changelog:   # the unreleased entries become the release's
            check(f"## [{new}] - 2030-01-02 (pre-release)\n\n" in cl and "## [Unreleased]" not in cl
                  and cl.count(f"## [{new}]") == 1, "CHANGELOG [Unreleased] becomes the new version")
        else:
            stub = f"## [{new}] - 2030-01-02 (pre-release)\n\n### Added\n\n## [{cur}]"
            check(stub in cl and cl.count(f"## [{new}]") == 1, "CHANGELOG stub above the newest entry")
        check(new in t("public/site/index.html"), "site regenerated")
        r = os.popen(f"{sys.executable} {root}/tools/release/test_version_agree.py").read()
        check(r.strip().endswith("PASS"), "test_version_agree passes on the copy")

        card = bump_version.at(root, "public/.recomp.json")
        good = card.read_text(encoding="utf-8")
        card.write_text(good.replace('"in-progress"', '"released"'), encoding="utf-8")
        rc, _, err = run(root, new)
        check(rc != 0 and "recomp_card" in err, "a card that disagrees stops the bump nonzero")
        card.write_text(good, encoding="utf-8")

        after = snapshot(root)
        rc, out, _ = run(root, new, "--date", "2031-05-05")
        check(rc == 0 and snapshot(root) == after, "second run with the same version changes nothing")
        rc, _, _ = run(root, cur)
        check(rc != 0, "going back to the old version is refused")
    check(names.VERSION == cur, "the real tree was not touched")
    print("PASS" if not bad else f"FAIL ({bad})")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

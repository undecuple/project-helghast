#!/usr/bin/env python3
"""Host test for recomp_card.py: PASS on the real public/.recomp.json, a negative control per rule (temp copies),
--write fixes status and dead links, both layouts (public/ and export root). No game data needed.
Run: python3 tools/release/test_recomp_card.py
"""
import contextlib
import copy
import io
import json
import os
import pathlib
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REAL = pathlib.Path(HERE).parents[1]
sys.path.insert(0, HERE)
import recomp_card  # noqa: E402

bad = 0


def check(ok, what):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + what)


def run(root, *args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = recomp_card.main(["--root", str(root), *args])
    return rc, out.getvalue(), err.getvalue()


def make(base, card, public=True, readme="> **Status: partly playable.** x\n"):
    r = pathlib.Path(base)
    p = r / "public" if public else r
    (p / "docs").mkdir(parents=True, exist_ok=True)
    (p / ".recomp.json").write_text(card if isinstance(card, str) else json.dumps(card), encoding="utf-8")
    (p / "README.md").write_text(readme, encoding="utf-8")
    for f in ("RELEASE-CRITERIA.md", "FEATURES.md", "BUILDING.md"):
        (p / "docs" / f).write_text("x\n", encoding="utf-8")
    (r / "docs").mkdir(exist_ok=True)
    (r / "docs" / "requirements.md").write_text("x\n", encoding="utf-8")
    return r


good = json.loads(recomp_card.card_path(REAL).read_text(encoding="utf-8"))  # public/ here, the root in the export
rc, out, err = run(REAL)
check(rc == 0 and "OK" in out, f"the real card passes ({err.strip()[:100]})")

GH = "https://github.com/undecuple/project-helghast"


def mut(fn):
    c = copy.deepcopy(good)
    fn(c)
    return c


cases = [
    ("unknown field", mut(lambda c: c.update(foo=1)), "foo"),
    ("version field", mut(lambda c: c.update(version="0.1.4")), "/releases"),
    ("wikidata", mut(lambda c: c.update(wikidata="Q0")), "wikidata"),
    ("http maintainer link", mut(lambda c: c["maintainers"][0].update(link="http://x")), "maintainer"),
    ("http link url", mut(lambda c: c["links"].append({"label": "A", "url": "http://x"})), "https://"),
    ("status value", mut(lambda c: c.update(status="done")), "not one of"),
    ("released pre-release", mut(lambda c: c.update(status="released")), "pre-release"),
    ("status vs README", mut(lambda c: c.update(status="playable")), "disagrees"),
    ("missing repo file", mut(lambda c: c["links"].append({"label": "Gone", "url": GH + "/blob/main/docs/NOPE.md"})), "NOPE.md"),
    ("serial", mut(lambda c: c["original"].update(serial="BCES01007")), "original.serial"),
    ("revision", mut(lambda c: c["original"].update(revision="1.00")), "original.revision"),
    ("original field", mut(lambda c: c["original"].update(md5="x")), "original.md5"),
]
for name, card, needle in cases:
    with tempfile.TemporaryDirectory() as d:
        rc, out, err = run(make(d, card))
        check(rc == 1 and needle in err, f"negative: {name} -> exit 1, names '{needle}'")
        if name == "unknown field":
            check(not run(make(d + "/x", mut(lambda c: c.update(**{"x-note": 1}))))[2], "x- fields allowed")

with tempfile.TemporaryDirectory() as d:
    rc, out, err = run(make(d, "{not json"))
    check(rc == 1 and "does not parse" in err, "negative: unparsable card")
with tempfile.TemporaryDirectory() as d:
    rc, out, err = run(make(d, good, readme="no status\n"))
    check(rc == 1 and "README" in err, "negative: README without a status line")
with tempfile.TemporaryDirectory() as d:
    rc, out, err = run(make(d, good, public=False))
    check(rc == 0, f"export-root layout passes ({err.strip()[:100]})")
with tempfile.TemporaryDirectory() as d:
    c = mut(lambda c: (c.update(status="exploring"), c["links"].append({"label": "Gone", "url": GH + "/blob/main/docs/NOPE.md"})))
    r = make(d, c)
    check(run(r)[0] == 1, "write: broken card fails the check first")
    rc, out, err = run(r, "--write")
    fixedc = json.loads((r / "public/.recomp.json").read_text(encoding="utf-8"))
    check(rc == 0 and fixedc["status"] == "in-progress" and all("NOPE" not in l["url"] for l in fixedc["links"]),
          "write: status from README, dead link dropped, then OK")
    check(run(r)[0] == 0, "write: second check-only run passes")
    before = (r / "public/.recomp.json").read_text(encoding="utf-8")
    run(r)
    check((r / "public/.recomp.json").read_text(encoding="utf-8") == before, "check-only never writes")

print("FAILED" if bad else "PASS")
sys.exit(1 if bad else 0)

#!/usr/bin/env python3
"""Host test: release_check.py on fixture release JSONs and small fake assets (no network, no game data).
Run: python3 tools/release/test_release_check.py
"""
import copy
import hashlib
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import names  # noqa: E402
import release_check as rc  # noqa: E402

V = names.VERSION
TAG = "v" + V
GOOD = {"tag_name": TAG, "name": TAG, "draft": False, "prerelease": V.startswith("0."),
        "body": f"Project Helghast {V}. SHA256SUMS.",
        "assets": [{"name": n} for n in rc.expected_assets(V)]}
fails = []


def expect(label, errs, needle=None):
    ok = (not errs) if needle is None else any(needle in e for e in errs)
    if not ok:
        fails.append(f"{label}: got {errs}")


def variant(**kw):
    r = copy.deepcopy(GOOD)
    r.update(kw)
    return r


expect("good", rc.check_release(GOOD, TAG))
expect("missing asset", rc.check_release(variant(assets=GOOD["assets"][:-1]), TAG), "missing asset " + rc.SUMS)
expect("extra asset", rc.check_release(variant(assets=GOOD["assets"] + [{"name": "kz3.exe"}]), TAG),
       "unexpected asset kz3.exe")
expect("wrong tag", rc.check_release(variant(tag_name="v9.9.9"), "v9.9.9"), "is not v" + V)
expect("tag_name differs", rc.check_release(variant(tag_name="v9.9.9"), TAG), "tag_name")
expect("draft", rc.check_release(variant(draft=True), TAG), "draft")
expect("not prerelease", rc.check_release(variant(prerelease=False), TAG), "prerelease")
one = dict(tag_name="v1.0.0", name="v1.0.0", body="1.0.0", assets=[{"name": n} for n in rc.expected_assets("1.0.0")])
expect("1.x pre-release flagged", rc.check_release(variant(prerelease=True, **one), "v1.0.0", "1.0.0"), "needs False")
expect("1.x good", rc.check_release(variant(prerelease=False, **one), "v1.0.0", "1.0.0"))
expect("title", rc.check_release(variant(name="Release"), TAG), "title")
expect("body", rc.check_release(variant(body="nothing"), TAG), "body")


def write_assets(d, tamper=None):
    setup, zipn = f"{names.SETUP_BASE}.exe", names.zip_name(V)
    sums = []
    for n, data in ((setup, b"fake setup"), (zipn, b"fake zip")):
        with open(os.path.join(d, n), "wb") as f:
            f.write(data)
        h = hashlib.sha256(data).hexdigest()
        sums.append(f"{h}  {n}\n")
        own = "0" * 64 if tamper == n + ".sha256" else h
        with open(os.path.join(d, n + ".sha256"), "w") as f:
            f.write(f"{own}  {n}\n")
    if tamper == "sums":
        sums[0] = "1" * 64 + f"  {setup}\n"
    with open(os.path.join(d, rc.SUMS), "w") as f:
        f.writelines(sums)
    if tamper == "file":
        with open(os.path.join(d, zipn), "wb") as f:
            f.write(b"changed")


for label, tamper, needle in (("hashes good", None, None), ("file changed", "file", "file is"),
                              ("SHA256SUMS wrong", "sums", "SHA256SUMS entry"),
                              (".sha256 wrong", names.zip_name(V) + ".sha256", "disagree")):
    with tempfile.TemporaryDirectory() as d:
        write_assets(d, tamper)
        expect(label, rc.check_hashes(d), needle)
with tempfile.TemporaryDirectory() as d:
    expect("assets not downloaded", rc.check_hashes(d), "not downloaded")

with tempfile.TemporaryDirectory() as d:
    os.makedirs(os.path.join(d, "site"))
    for p, t in (("README.md", f"[x](https://e.invalid/releases/tag/{TAG})"), ("site/index.html", "no link")):
        with open(os.path.join(d, p), "w") as f:
            f.write(t)
    errs = rc.check_links(d, TAG)
    expect("link missing in site", errs, "site")
    expect("README link ok", [e for e in errs if "README" in e])

# the command line: a good release file exits 0, a draft exits 1
with tempfile.TemporaryDirectory() as d:
    for name, rel in (("good.json", GOOD), ("draft.json", variant(draft=True))):
        with open(os.path.join(d, name), "w") as f:
            json.dump(rel, f)
    if rc.main(["--tag", TAG, "--release", os.path.join(d, "good.json")]) != 0:
        fails.append("cli good release did not exit 0")
    if rc.main(["--tag", TAG, "--release", os.path.join(d, "draft.json")]) != 1:
        fails.append("cli draft release did not exit 1")

if fails:
    print("test_release_check: FAIL\n  " + "\n  ".join(fails))
    sys.exit(1)
print("test_release_check: PASS")

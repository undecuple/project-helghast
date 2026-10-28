#!/usr/bin/env python3
"""Host test for shipping the shared pipeline template (release pass 33): assemble_payload.py includes
installer/pso-template.bin when it passes tools/smoke/pso_template.py check, refuses to package one that fails, and
builds exactly as before with none; launch.py sets LD_PSO_TEMPLATE / LD_PSO_TEMPLATE_OUT. Synthetic files only, no game
data. Exit 0 = PASS.

    python3 tools/release/test_template_ship.py
"""
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools", "smoke"))
import assemble_payload as ap  # noqa: E402
import launch as la  # noqa: E402
import pso_template as pt  # noqa: E402
import test_assemble_payload as ta  # noqa: E402

FAILS = []


def check(cond, what, detail=""):
    print(("ok   " if cond else "FAIL ") + what + ("" if cond else f"  {detail}"[:600]))
    if not cond:
        FAILS.append(what)


def good_template(path):
    pt.write(path, 0, [bytes([i + 1]) * pt.REC.size for i in range(3)])


def assemble(repo, out, *extra):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        try:
            rc = ap.main(["--repo", repo, "--out", out, "--version", "0.1.0", *extra], out=buf)
        except SystemExit as e:
            rc = e.code if isinstance(e.code, int) else 2
    return rc, buf.getvalue()


def main():
    tmp = tempfile.mkdtemp(prefix="kz3-tplship-")
    try:
        repo = os.path.join(tmp, "repo")
        ta.fake_repo(repo)
        tool = os.path.join(repo, "tools", "smoke", "pso_template.py")
        shutil.copyfile(os.path.join(REPO, "tools", "smoke", "pso_template.py"), tool)
        tpl = os.path.join(repo, "installer", "pso-template.bin")

        # 1. no template: the payload has no template file, builds as before
        out0 = os.path.join(tmp, "p0")
        rc, txt = assemble(repo, out0)
        check(rc == 0 and ap.TEMPLATE_DEST not in ta.listing(out0), "no template: builds, nothing added", txt)

        # 2. a good template ships at the payload root and is listed in the manifest
        os.makedirs(os.path.dirname(tpl), exist_ok=True)
        good_template(tpl)
        out1 = os.path.join(tmp, "p1")
        rc, txt = assemble(repo, out1)
        with open(os.path.join(out1, "manifest.json"), encoding="utf-8") as f:
            man = json.load(f)
        check(rc == 0 and ap.TEMPLATE_DEST in ta.listing(out1)
              and any(e["path"] == ap.TEMPLATE_DEST for e in man["files"]), "good template ships and is in the manifest", txt)
        with open(os.path.join(out1, ap.TEMPLATE_DEST), "rb") as a, open(tpl, "rb") as b:
            check(a.read() == b.read(), "shipped bytes equal the input")

        # 3. bad templates refused, nothing written: wrong magic, truncated, empty
        with open(tpl, "rb") as f:
            whole = f.read()
        for name, data in (("magic", b"NOPE" + bytes(200)), ("truncated", whole[:40]), ("empty", b"")):
            with open(tpl, "wb") as f:
                f.write(data)
            out2 = os.path.join(tmp, "p2" + name)
            rc, txt = assemble(repo, out2)
            check(rc == 1 and "payload refused" in txt and not os.path.exists(out2), f"bad template ({name}) refused", txt)

        # 3b. one rule for both packagers: a template over MAX_BINARY (2 MiB) passes assemble and pack_zip.verify;
        # one with bytes after the last record is refused; one over the format cap is refused by the shared scrub
        import pack_zip as pz
        n = (ap.MAX_BINARY // pt.REC.size) + 10
        pt.write(tpl, 0, [(i % 250 + 1).to_bytes(1, "little") * pt.REC.size for i in range(n)])
        check(os.path.getsize(tpl) > ap.MAX_BINARY, "3b test template is over MAX_BINARY")
        out5 = os.path.join(tmp, "p5")
        rc, txt = assemble(repo, out5)
        check(rc == 0 and ap.TEMPLATE_DEST in ta.listing(out5), "template over 2 MiB passes assemble", txt)
        probs = pz.verify(out5)[0] if rc == 0 else ["not assembled"]
        check(not any(ap.TEMPLATE_DEST in p for p in probs), "template over 2 MiB passes pack_zip.verify", probs)
        check(ap.scrub_ship(tpl, ap.TEMPLATE_DEST) == [] and ap.scrub_ship(tpl, "x.bin") != [],
              "scrub_ship exempts only the template")
        with open(tpl, "ab") as f:
            f.write(b"\0" * 7)
        out6 = os.path.join(tmp, "p6")
        rc, txt = assemble(repo, out6)
        check(rc == 1 and "after the last record" in txt and not os.path.exists(out6), "template with appended bytes refused", txt)
        good_template(tpl)

        # 4. --template naming a missing file is refused; the check tool missing is refused too
        os.remove(tpl)
        rc, txt = assemble(repo, os.path.join(tmp, "p3"), "--template", os.path.join(tmp, "none.bin"))
        check(rc == 1 and "MISSING" in txt, "--template with a missing file refused", txt)
        good_template(tpl)
        os.remove(tool)
        rc, txt = assemble(repo, os.path.join(tmp, "p4"))
        check(rc == 1 and "cannot check" in txt, "template without the check tool refused", txt)

        # 5. launcher variables
        root, data = os.path.join(tmp, "inst"), os.path.join(tmp, "data")
        check(la.template_env(root, data, {}) == {}, "no shipped template: no variables")
        os.makedirs(root)
        with open(os.path.join(root, la.TEMPLATE_SHIPPED), "wb") as f:
            f.write(b"x")
        e = la.template_env(root, data, {})
        check(e == {"LD_PSO_TEMPLATE": os.path.join(root, la.TEMPLATE_SHIPPED)}, "shipped template sets LD_PSO_TEMPLATE", e)
        e = la.template_env(root, data, {}, export=True)
        check(e.get("LD_PSO_TEMPLATE_OUT") == os.path.join(data, la.TEMPLATE_EXPORT), "export sets LD_PSO_TEMPLATE_OUT", e)
        e = la.template_env(root, data, {"LD_PSO_TEMPLATE": "mine", "LD_PSO_TEMPLATE_OUT": "o"}, export=True)
        check(e == {}, "the player's own variables win", e)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

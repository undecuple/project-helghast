#!/usr/bin/env python3
"""Host test of the public workflows (public/.github/workflows/*.yml), text based (no PyYAML needed).

Checks, per workflow file:
  1. every `uses:` of a third-party action is pinned to a full 40-hex commit SHA and carries a version comment
     (`# v1.2.3`); local actions (./...) are exempt;
  2. a top-level `permissions:` block exists, and it grants no write scope (jobs ask for their own);
  3. every job-level write scope is justified: `contents: write` only in a job that publishes (`gh release create`)
     and builds nothing (no upload-artifact, no ISCC/cmake/make step); `pages`/`id-token`/`security-events`/`issues`
     write only where the file's job needs them (named in WRITE_OK below).
Run: python3 tools/release/test_workflows.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# private tree: public/.github/workflows; the public tree has the contents of public/ at its root
WF = os.environ.get("KZ3_WORKFLOWS_DIR") or next(
    (d for d in (os.path.join(ROOT, "public", ".github", "workflows"), os.path.join(ROOT, ".github", "workflows"))
     if os.path.isdir(d)), "")
# (file, job) -> write scopes that job may hold
WRITE_OK = {
    ("release.yml", "publish"): {"contents"},
    ("pages.yml", "deploy"): {"pages", "id-token"},
    ("codeql.yml", "analyze"): {"security-events"},
    ("issue-labels.yml", "*"): {"issues"},     # workflow-level: the one job only writes issues
}
USES = re.compile(r"^\s*(?:-\s+)?uses:\s*(\S+)(.*)$")
PIN = re.compile(r"^[\w.-]+/[\w./-]+@[0-9a-f]{40}$")


def split_jobs(text):
    """-> (header_text, {job: text}) by the 2-space-indented keys under `jobs:`."""
    m = re.search(r"^jobs:\s*$", text, re.M)
    if not m:
        return text, {}
    head, body = text[:m.start()], text[m.end():]
    jobs, cur, name = {}, [], None
    for line in body.splitlines():
        k = re.match(r"^  ([A-Za-z0-9_-]+):\s*(#.*)?$", line)
        if k:
            if name:
                jobs[name] = "\n".join(cur)
            name, cur = k.group(1), []
        elif name:
            cur.append(line)
    if name:
        jobs[name] = "\n".join(cur)
    return head, jobs


def writes(block):
    """write scopes named in the first `permissions:` block of `block`."""
    m = re.search(r"^(\s*)permissions:\s*(\{\})?\s*(#.*)?$", block, re.M)
    if not m:
        return None, set()
    ind = len(m.group(1))
    out = set()
    for line in block[m.end():].splitlines():
        if line.strip() == "" or line.lstrip().startswith("#"):
            continue
        if len(line) - len(line.lstrip()) <= ind:
            break
        s = re.match(r"^\s+([a-z-]+):\s*write\b", line)
        if s:
            out.add(s.group(1))
    return True, out


def check(fn, text):
    errs = []
    for n, line in enumerate(text.splitlines(), 1):
        m = USES.match(line)
        if not m or m.group(1).startswith("./"):
            continue
        if not PIN.match(m.group(1)):
            errs.append(f"{fn}:{n}: {m.group(1)} is not pinned to a full commit SHA")
        elif not re.search(r"#\s*v\d", m.group(2)):
            errs.append(f"{fn}:{n}: {m.group(1)} has no version comment (# vX.Y.Z)")
    head, jobs = split_jobs(text)
    has, top_w = writes(head)
    if not has:
        errs.append(f"{fn}: no top-level permissions: block")
    allowed_top = WRITE_OK.get((fn, "*"), set())
    if top_w - allowed_top:
        errs.append(f"{fn}: top-level write scope {sorted(top_w - allowed_top)} (jobs must ask for their own)")
    for j, body in jobs.items():
        _, w = writes(body)
        extra = w - WRITE_OK.get((fn, j), set()) - allowed_top
        if extra:
            errs.append(f"{fn}: job {j} holds write scope {sorted(extra)} that is not listed as needed")
        if "contents" in w:
            if "gh release create" not in body:
                errs.append(f"{fn}: job {j} has contents: write but publishes nothing")
            if re.search(r"upload-artifact|ISCC|cmake|\bmake\b", body):
                errs.append(f"{fn}: job {j} has contents: write and also builds (split the publishing step out)")
    return errs


def selftest():
    bad = ("permissions:\n  contents: read\njobs:\n  a:\n    permissions:\n      contents: write\n    steps:\n"
           "      - uses: actions/checkout@v4\n      - uses: x/y@" + "a" * 40 + "\n")
    got = check("t.yml", bad)
    assert any("not pinned" in e for e in got) and any("no version comment" in e for e in got), got
    assert any("holds write scope" in e for e in got), got
    good = ("permissions:\n  contents: read\njobs:\n  a:\n    steps:\n      - uses: x/y@" + "b" * 40 + "  # v1.2.3\n")
    assert check("t.yml", good) == [], check("t.yml", good)


def main():
    selftest()
    if not WF:
        print("SKIP: no .github/workflows in this tree")
        return 0
    files = sorted(f for f in os.listdir(WF) if f.endswith(".yml"))
    errs = []
    for f in files:
        with open(os.path.join(WF, f), encoding="utf-8") as fh:
            errs += check(f, fh.read())
    if errs:
        print("FAIL")
        print("\n".join(errs))
        return 1
    print(f"ok: {len(files)} workflows, actions pinned with version comments, write scopes narrow")
    return 0


if __name__ == "__main__":
    sys.exit(main())

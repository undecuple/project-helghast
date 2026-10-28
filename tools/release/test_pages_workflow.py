#!/usr/bin/env python3
"""Static check of public/.github/workflows/pages.yml (text only, no YAML library): the Discussions board build keeps its
promises: least permissions, the wanted triggers, a SHA-pinned action set, the PAGES_ENABLED gate, no pull_request_target,
and the token reaching only the feed step, never printed."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PATH = "public/.github/workflows/pages.yml"
fails = []


def check(ok, msg):
    if not ok:
        fails.append(msg)
        print("FAIL", msg)


def main():
    f = ROOT / PATH
    if not f.exists():
        f = ROOT / ".github/workflows/pages.yml"       # the public tree layout
    if not f.exists():
        print("SKIP test_pages_workflow: no pages.yml")
        return 0
    t = f.read_text(encoding="utf-8")
    code = "\n".join(re.sub(r"\s#.*$", "", l) for l in t.splitlines() if not l.lstrip().startswith("#"))
    check("pull_request_target" not in code and "pull_request:" not in code, "no pull_request(_target) trigger")
    check(re.search(r"^  discussion:\n    types: \[created, edited, deleted, category_changed\]", code, re.M), "discussion triggers")
    check(re.search(r"^  discussion_comment:\n    types: \[created, deleted\]", code, re.M), "discussion_comment triggers")
    check(re.search(r"^  schedule:\n    - cron: \"[^\"]+\"", code, re.M), "daily schedule fallback")
    check("workflow_dispatch:" in code and re.search(r"^permissions: \{\}", code, re.M), "dispatch and empty top-level permissions")
    build = code.split("\n  build:\n", 1)[1].split("\n  deploy:\n", 1)[0]
    perms = re.search(r"permissions:\n((?:      .*\n)+)", build + "\n")
    got = dict(l.strip().split(": ", 1) for l in perms.group(1).splitlines()) if perms else {}
    check(got == {"contents": "read", "pages": "read", "discussions": "read"}, f"build permissions are read only: {got}")
    check("PAGES_ENABLED == 'true'" in build and "PAGES_ENABLED == 'true'" in code.split("\n  deploy:\n", 1)[1], "PAGES_ENABLED gate on both jobs")
    uses = re.findall(r"uses:\s*(\S+)", code)
    check(uses and all(re.fullmatch(r"[\w./-]+@[0-9a-f]{40}", u) for u in uses), f"all actions SHA-pinned: {uses}")
    check(re.search(r"^concurrency:\n  group: pages\n  cancel-in-progress: true", code, re.M), "concurrency collapses bursts")
    tok = [i for i, l in enumerate(code.splitlines()) if "GITHUB_TOKEN" in l or "secrets." in l]
    lines = code.splitlines()
    feed_step = next((i for i, l in enumerate(lines) if "Discussions board snapshot" in l), -1)
    check(len(tok) == 1 and feed_step >= 0 and 0 < tok[0] - feed_step < 4, "the token appears once, in the feed step's env")
    check(not re.search(r"(echo|printf|cat|set -x).*(TOKEN|secrets)", code), "token never echoed")
    render = code.split("Render the page with the snapshot", 1)[1].split("uses:", 1)[0]
    check("TOKEN" not in render and "secrets" not in render and "website.py --feed feed.json --out" in render, "website.py runs without the token")
    check("set -x" not in code and "ACTIONS_STEP_DEBUG" not in code, "no shell tracing")
    if fails:
        print(f"FAIL test_pages_workflow ({len(fails)})")
        return 1
    print("PASS test_pages_workflow (permissions, triggers, SHA pins, gate, token scope)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

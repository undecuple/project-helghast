#!/usr/bin/env python3
"""Checks that public/.recomp.json (the recomp.fyi project card, schema v1) is current and well formed.

  python3 tools/release/recomp_card.py [--root R] [--write]

Reads public/.recomp.json (or .recomp.json at the root of the public export; --root defaults to the repo or export
root, found the way release_notes.py finds its files). Rules, one output line per problem, exit 1 if any:
  - it parses, and uses only schema v1 fields (private extensions start with `x-`); no version field (link /releases);
  - wikidata matches ^Q[1-9][0-9]*$; every maintainers[].link and links[].url starts with https://;
  - status is a schema value and agrees with the README status line and, for a 0.x version, the release criteria
    (a pre-release is never "released" or "complete"; "playable" needs the README to say playable, not partly);
  - links into this repo (github.com/OWNER/REPO/blob|tree/main/PATH) name a file or folder that the public export holds;
  - original.serial / original.revision equal what the setup accepts: firstrun_check.TITLE_ID and the update version
    firstrun_check.UPDATE_APP_VER (read from there, not copied).
--write fixes only what follows from one source and nothing else: status from the README status line, and it drops
repo links whose file is gone. The default is check-only. Local tool: no network, no git.
"""
import argparse
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import firstrun_check  # noqa: E402
import names  # noqa: E402

TOP = {"$schema", "game", "wikidata", "system", "type", "original", "project", "status", "targets", "toolchain",
       "approach", "maintainers", "links", "help", "tags", "notes"}
ORIGINAL = {"region", "revision", "serial", "sha1"}
STATUS = ("exploring", "in-progress", "playable", "released", "complete", "paused", "unknown")
TYPES = ("recomp", "port", "decomp")
REPO_LINK = re.compile(r"^https://github\.com/" + re.escape(names.OWNER) + "/" + re.escape(names.REPO_SLUG)
                       + r"/(?:blob|tree)/main/([^#?]+)")
# Files the export takes from docs/ (not public/docs/): tools/export-public.sh copies them next to the public docs.
FROM_DOCS = ("docs/requirements.md",)


def at(root, rel):
    """The file for a repo path: the public export puts public/ at its root."""
    if rel.startswith("public/") and not (root / "public").is_dir():
        return root / rel[len("public/"):]
    return root / rel


def card_path(root):
    return at(root, "public/.recomp.json")


def readme_status(root):
    """The status the README states: 'playable', 'partly playable' or None."""
    p = at(root, "public/README.md")
    m = re.search(r"\*\*Status: ([^*.]+?)\.?\*\*", p.read_text(encoding="utf-8")) if p.is_file() else None
    return m.group(1).strip().lower() if m else None


def expected_status(readme):
    """The card status that follows from the README status line."""
    if readme is None:
        return None
    return "in-progress" if "partly" in readme or "not " in readme else "playable"


def repo_target_exists(root, rel):
    rel = rel.rstrip("/")
    if at(root, "public/" + rel).exists():
        return True
    return rel in FROM_DOCS and (root / rel).exists()


def check(root, card):
    """Problem lines for a parsed card."""
    bad = []
    if not isinstance(card, dict):
        return ["card is not a JSON object"]
    for k in card:
        if k not in TOP and not k.startswith("x-"):
            bad.append(f"field '{k}' is not in schema v1 (private fields start with x-)"
                       + (": link /releases instead of a version" if "version" in k.lower() else ""))
    w = card.get("wikidata")
    if w is not None and not (isinstance(w, str) and re.fullmatch(r"Q[1-9][0-9]*", w)):
        bad.append(f"wikidata {w!r} does not match ^Q[1-9][0-9]*$")
    t = card.get("type")
    for v in ([t] if isinstance(t, str) else t or []):
        if v not in TYPES:
            bad.append(f"type {v!r} is not one of {', '.join(TYPES)}")
    for m in card.get("maintainers", []) or []:
        if not isinstance(m, dict) or not m.get("name"):
            bad.append("a maintainer has no name")
            continue
        if "link" in m and not str(m["link"]).startswith("https://"):
            bad.append(f"maintainer {m['name']}: link {m['link']!r} is not https://")
    links = card.get("links", []) or []
    for l in links:
        u = l.get("url", "") if isinstance(l, dict) else ""
        if not str(u).startswith("https://"):
            bad.append(f"link {l.get('label') if isinstance(l, dict) else l!r}: url {u!r} is not https://")
            continue
        m = REPO_LINK.match(u)
        if m and not repo_target_exists(root, m.group(1)):
            bad.append(f"link {l.get('label')}: {m.group(1)} is not in the public export")
    orig = card.get("original")
    for o in ([orig] if isinstance(orig, dict) else orig or []):
        for k in o:
            if k not in ORIGINAL:
                bad.append(f"original.{k} is not in schema v1")
        if "serial" in o and o["serial"] != firstrun_check.TITLE_ID:
            bad.append(f"original.serial {o['serial']!r} is not the base disc the setup accepts "
                       f"({firstrun_check.TITLE_ID})")
        want = firstrun_check.UPDATE_APP_VER.lstrip("0")
        if "revision" in o and not str(o["revision"]).startswith(want):
            bad.append(f"original.revision {o['revision']!r} does not name the update the setup accepts ({want})")
    st = card.get("status")
    if st is not None and st not in STATUS:
        bad.append(f"status {st!r} is not one of {', '.join(STATUS)}")
    elif st is not None:
        if names.VERSION.startswith("0.") and st in ("released", "complete"):
            bad.append(f"status {st!r} for the pre-release {names.VERSION} (RELEASE-CRITERIA: published as a pre-release while 0.x)")
        rd = readme_status(root)
        exp = expected_status(rd)
        if rd is None:
            bad.append("README has no '**Status: ...**' line to compare the status with")
        elif st != exp and not (exp == "playable" and st in ("released", "complete")):
            bad.append(f"status {st!r} disagrees with the README status line ('{rd}', expects {exp!r})")
    crit = at(root, "public/docs/RELEASE-CRITERIA.md")
    if not crit.is_file():
        bad.append("docs/RELEASE-CRITERIA.md is missing")
    return bad


def fixed(root, card):
    """The card with the single-source fields rewritten (status; dead repo links dropped)."""
    out = dict(card)
    exp = expected_status(readme_status(root))
    if exp:
        out["status"] = exp
    out["links"] = [l for l in card.get("links", []) or []
                    if not (REPO_LINK.match(l.get("url", "")) and not repo_target_exists(root, REPO_LINK.match(l["url"]).group(1)))]
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=str(ROOT))
    ap.add_argument("--write", action="store_true", help="fix status and dead repo links in place")
    a = ap.parse_args(argv)
    root = pathlib.Path(a.root)
    p = card_path(root)
    if not p.is_file():
        print(f"recomp_card: {p} not found", file=sys.stderr)
        return 1
    try:
        card = json.loads(p.read_text(encoding="utf-8"))
    except ValueError as e:
        print(f"recomp_card: {p.name} does not parse: {e}", file=sys.stderr)
        return 1
    if a.write and isinstance(card, dict):
        new = fixed(root, card)
        if new != card:
            p.write_text(json.dumps(new, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
            print("recomp_card: rewrote status/links in " + p.name)
            card = new
    bad = check(root, card)
    for b in bad:
        print("recomp_card: " + b, file=sys.stderr)
    if not bad:
        print("recomp_card: OK")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

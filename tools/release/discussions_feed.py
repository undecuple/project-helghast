#!/usr/bin/env python3
"""Latest GitHub Discussions of the project -> a small JSON snapshot the project site renders (website.py --feed).

    GITHUB_TOKEN=... python3 tools/release/discussions_feed.py OUT.json    # the Pages build step (Actions token)

Rules: the token comes from GITHUB_TOKEN only and is never printed or written; only allowlisted categories are kept;
the snapshot holds title (length-capped), category, author login, created date, reply count and the discussion URL,
never a message body, an avatar or any other remote image. With no token, no network or an unexpected answer the
script writes nothing, says so on stderr and exits 0, so the site build never fails because of it.
`clean_items()` is the one validator: the writer and website.py both use it, so a hand-edited snapshot cannot put
anything else on the page.
"""
import json
import os
import pathlib
import re
import sys
import urllib.error
import urllib.request

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import names  # noqa: E402

CATEGORIES = ("Announcements", "Show and tell")
TITLE_MAX = 90
MAX_ITEMS = 8
API = "https://api.github.com/graphql"
QUERY = ("query($o:String!,$r:String!){repository(owner:$o,name:$r){discussions(first:30,"
         "orderBy:{field:CREATED_AT,direction:DESC}){nodes{title url createdAt category{name} "
         "author{login} comments{totalCount}}}}}")
URL_RE = re.compile(r"^" + re.escape(names.REPO_URL) + r"/discussions/\d+$")
LOGIN_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9-]{0,38}$")
DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}")


def clean_items(items):
    """Validated copies of the items: allowlisted category, github.com discussion URL of this repo, plain login,
    ISO date, int reply count, title capped; anything else is dropped. Titles stay raw text (the page escapes them)."""
    out = []
    for it in items if isinstance(items, list) else []:
        if not isinstance(it, dict):
            continue
        title, cat, url = it.get("title"), it.get("category"), it.get("url")
        login, date, n = it.get("author"), it.get("created"), it.get("replies")
        if cat not in CATEGORIES or not isinstance(url, str) or not URL_RE.match(url):
            continue
        if not isinstance(title, str) or not isinstance(login, str) or not LOGIN_RE.match(login):
            continue
        if not isinstance(date, str) or not DATE_RE.match(date) or not isinstance(n, int) or isinstance(n, bool):
            continue
        title = " ".join(title.split())
        if not title:
            continue
        if len(title) > TITLE_MAX:
            title = title[:TITLE_MAX - 1].rstrip() + "…"
        out.append({"title": title, "category": cat, "author": login, "created": date[:10],
                    "replies": max(0, n), "url": url})
    return out[:MAX_ITEMS]


def from_graphql(data):
    nodes = (((data or {}).get("data") or {}).get("repository") or {}).get("discussions", {}).get("nodes") or []
    items = []
    for n in nodes:
        if not isinstance(n, dict):
            continue
        items.append({"title": n.get("title"), "category": (n.get("category") or {}).get("name"),
                      "author": (n.get("author") or {}).get("login"), "created": n.get("createdAt"),
                      "replies": (n.get("comments") or {}).get("totalCount"), "url": n.get("url")})
    return clean_items(items)


def load(path):
    """Items of a snapshot file, validated; [] when the file is missing or unreadable."""
    try:
        return clean_items(json.loads(pathlib.Path(path).read_text(encoding="utf-8")).get("items"))
    except (OSError, ValueError, AttributeError):
        return []


def fetch(token, opener=urllib.request.urlopen):
    body = json.dumps({"query": QUERY, "variables": {"o": names.OWNER, "r": names.REPO_SLUG}}).encode()
    req = urllib.request.Request(API, data=body, headers={
        "Authorization": "Bearer " + token, "Content-Type": "application/json", "User-Agent": "project-site-feed"})
    with opener(req, timeout=20) as r:
        return from_graphql(json.loads(r.read().decode("utf-8")))


def main(argv=None, environ=None, opener=urllib.request.urlopen):
    argv = sys.argv[1:] if argv is None else argv
    environ = os.environ if environ is None else environ
    if len(argv) != 1:
        print("usage: discussions_feed.py OUT.json", file=sys.stderr)
        return 2
    token = environ.get("GITHUB_TOKEN", "")
    if not token:
        print("discussions_feed: no GITHUB_TOKEN, no snapshot written (the site shows the static link)", file=sys.stderr)
        return 0
    try:
        items = fetch(token, opener)
    except (OSError, ValueError, urllib.error.URLError) as e:
        print(f"discussions_feed: not available ({type(e).__name__}), no snapshot written", file=sys.stderr)
        return 0
    pathlib.Path(argv[0]).write_text(json.dumps({"items": items}, indent=1) + "\n", encoding="utf-8")
    print(f"discussions_feed: {len(items)} discussions kept")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Host test for tools/release/website.py on a small fixture (no game data, no work/): the page is generated, --check
passes on a fresh page and fails on a stale page, a missing screenshot checkpoint, a missing key feature row, a missing
requirements section and a missing style.css; Markdown in the data is escaped and links become GitHub URLs; the
headline follows the "Full campaign" row; the lint refuses scripts, event handlers and outside resources other than
Google Fonts. Exit 0 = PASS.

    python3 tools/release/test_website.py
"""
import contextlib
import io
import json
import os
import pathlib
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import discussions_feed  # noqa: E402
import website  # noqa: E402

HEAD = "kind\tarea\tfeature\tdetail\tstatus\tevidence\tnote\n"
REQ = """# Requirements

## What you need from your own copy of the game

| Item | Required | Details |
|---|---|---|
| **Disc** | yes | dump it (D4) |
| **Update** | yes | 1.14 |

## Hardware to play (provisional)

| | Minimum (provisional) | Recommended (provisional) | Basis |
|---|---|---|---|
| **CPU** | 6 cores | 8 cores | threads |

## Other
"""


def fixture(root, playable="not yet", play=None, drop_key=None, drop_shot=None, req=REQ, drop_copy=None):
    rows = [("ps3", "Campaign", website.PLAYABLE_ROW, "9 levels", playable, "row note", "")]
    for i, k in enumerate(website.KEY_ROWS):
        if k != drop_key:
            rows.append(("ps3", "Campaign" if i % 2 else "Graphics", k, "PS3 does **it** `x`",
                         play if (play and k == website.PLAY_ROW) else "partial",
                         "cp:" + website.SHOTS[0][0], "see [notes](docs/completed/enhancements.md) <script>x</script>"))
    rows.append(("port", "Available now", "Deadlock breaker", "", "available", "on (2 s)", "`KZ3_X=0`"))
    rows.append(("port", "Planned", "Mouse aim", "", "planned", "on", ""))
    (root / "docs/checkpoints").mkdir(parents=True)
    (root / "docs/features.tsv").write_text("# fixture\n" + HEAD + "".join("\t".join(r) + "\n" for r in rows))
    cps = "".join(f"| 2026-10-0{i + 1} 00:00:00 | [{n}](2026100{i + 1}-000000Z_{n}.png) | abc | x | y |\n"
                  for i, (n, *_) in enumerate(website.SHOTS) if n != drop_shot)
    (root / "docs/checkpoints/README.md").write_text("# cps\n\n| a | b | c | d | e |\n|---|---|---|---|---|\n" + cps)
    (root / website.SHOTS_DIR).mkdir(parents=True)
    for i, (n, *_) in enumerate(website.SHOTS):   # the public copies (a missing one: drop_copy)
        if n != drop_copy:
            (root / website.SHOTS_DIR / f"2026100{i + 1}-000000Z_{n}.png").write_bytes(b"png")
    if req is not None:
        (root / "docs/requirements.md").write_text(req)
    (root / "public/site").mkdir(parents=True)
    (root / "public/site/style.css").write_text("body{}\n")


def run(*args):
    err = io.StringIO()
    with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
        rc = website.main(list(args))
    return rc, err.getvalue()


def main():
    fails = []

    def expect(cond, what):
        if not cond:
            fails.append(what)

    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t) / "ok"
        fixture(root)
        expect(run("--root", str(root), "--check")[0] == 1, "check passes with no page")
        expect(run("--root", str(root))[0] == 0, "generate fails")
        rc, err = run("--root", str(root), "--check")
        expect(rc == 0, f"check fails on a fresh page: {err}")
        page = (root / website.PAGE).read_text()
        expect("Partly playable" in page, "headline not 'Partly playable' (in-level play partial, campaign not yet)")
        expect("<script>" not in page and "&lt;script&gt;" in page, "note text not escaped")
        expect(f'href="{website.REPO}/blob/main/docs/completed/enhancements.md"' in page, "relative link not a GitHub URL")
        expect("<code>x</code>" in page and "<strong>it</strong>" in page, "inline Markdown not rendered")
        expect('src="../docs/screenshots/' in page and "docs/checkpoints" not in page.split("-->", 1)[1],
               "screenshot not linked from docs/screenshots, or the page names docs/checkpoints")
        expect("(D4)" not in page and "<td>6 cores</td>" in page and "Basis" not in page, "requirements table wrong")
        expect(page.count("<details>") == 3, "one <details> per area plus the port-features list expected")
        expect('<aside class="side">' in page and page.count('<figure class="tile">') == len(website.SHOTS)
               and page.count('class="news shot">') == len(website.SHOTS)
               and page.count('class="sticker new">New<') == 2, "sidebar, one gallery tile and one news entry per screenshot, one New sticker each")
        expect('<header id="top" class="box">' in page and page.count('<span class="emblem"') == 1
               and page.count('class="promo">') == 5
               and 'id="legal" class="legal"' in page and page.count('<a class="i-') == 7,
               "logo block with one emblem, nav of seven tabs, promo boxes, legal print in the footer")
        top = page.split('<div class="page wrap">', 1)[0]
        expect(website.esc(website.WHAT) in top and "Partly playable" in top and "your own copy of the game" in top
               and top.count('<a class="btn') == 2 and 'class="cta-box"' in top and 'class="learn" href="#shot-1"' in page
               and 'id="shot-1"' in page and page.count('class="promo pic"') == 1,
               "intro above the column (what, status, own copy, two buttons), CTA box, banner link, picture promo")
        expect(f'class="i-comm" href="{website.DISC}"' in page and 'id="community"' in page
               and "Join the discussion" in page and 'class="feed"' not in page and 'class="news ann"' not in page
               and page.count(f'href="{website.DISC_QA}"') == 2 and page.count(f'href="{website.DISC_ANN}"') == 2,
               "Community tab, static community box, Q&A links in install and requirements, Discuss links on the two static entries")
        expect(page.count('<li class="row"><span class="thumb tb-part"') == len(website.KEY_ROWS)
               and page.count('<span class="sticker wip">WIP<') == 1
               and page.count('<i class="on"></i>') == 10, "key rows (all partial here), the hero's WIP sticker; meters lit 9 (6 of 7 partial) + 1 (1 not yet)")
        expect(not website.lint(page), f"lint on the generated page: {website.lint(page)}")
        (root / website.PAGE).write_text(page + "x")
        expect(run("--root", str(root), "--check")[0] == 1, "stale page passes")

        root = pathlib.Path(t) / "playable"
        fixture(root, playable="works")
        run("--root", str(root))
        expect("<strong>Playable, with known bugs</strong>" in (root / website.PAGE).read_text(), "headline ignores the campaign row (frame rate partial)")
        root = pathlib.Path(t) / "notplayable"
        fixture(root, play="not yet")
        run("--root", str(root))
        expect("<strong>Not playable yet</strong>" in (root / website.PAGE).read_text(), "headline ignores the in-level play row")

        for name, kw in (("noshot", {"drop_shot": website.SHOTS[1][0]}), ("nocopy", {"drop_copy": website.SHOTS[2][0]}),
                         ("nokey", {"drop_key": website.KEY_ROWS[2]}),
                         ("noreq", {"req": None}), ("badreq", {"req": REQ.replace("## Hardware to play", "## Hardware")})):
            root = pathlib.Path(t) / name
            fixture(root, **kw)
            expect(run("--root", str(root))[0] == 1, f"{name}: generate passes")
        root = pathlib.Path(t) / "nocss"
        fixture(root)
        run("--root", str(root))
        (root / "public/site/style.css").unlink()
        expect(run("--root", str(root), "--check")[0] == 1, "missing style.css passes")

    # Discussions feed: fixture snapshot with a <script> title, a non-allowlisted category, a foreign URL, a bad login
    # and a long title; the page escapes, filters and caps; no token / unreadable snapshot = static page, rc 0.
    U = website.REPO + "/discussions/"
    snap = {"items": [
        {"title": "Release <script>alert(1)</script> notes", "category": "Announcements", "author": website.names.OWNER,
         "created": "2026-10-16T10:00:00Z", "replies": 3, "url": U + "1"},
        {"title": "Secret chat", "category": "Q&A", "author": "bob", "created": "2026-10-15", "replies": 1, "url": U + "2"},
        {"title": "My run", "category": "Show and tell", "author": "al<b>", "created": "2026-10-15", "replies": 0, "url": U + "3"},
        {"title": "Evil link", "category": "Show and tell", "author": "eve", "created": "2026-10-15", "replies": 0,
         "url": "https://example.com/discussions/4"},
        {"title": "Nice " + "x" * 300, "category": "Show and tell", "author": "carol", "created": "2026-10-14", "replies": 1,
         "url": U + "5", "body": "SECRETBODY", "avatar": "https://example.com/a.png"},
        {"title": "Community post", "category": "Announcements", "author": "dave", "created": "2026-10-13", "replies": 2, "url": U + "6"},
    ]}
    with tempfile.TemporaryDirectory() as t:
        root = pathlib.Path(t) / "feed"
        fixture(root)
        sp = pathlib.Path(t) / "snap.json"
        sp.write_text(json.dumps(snap))
        out = pathlib.Path(t) / "out" / "index.html"
        expect(run("--root", str(root), "--feed", str(sp))[0] == 1, "--feed without --out passes")
        rc, err = run("--root", str(root), "--feed", str(sp), "--out", str(out))
        expect(rc == 0, f"feed render fails: {err}")
        pg = out.read_text()
        expect("<script>" not in pg and "Release &lt;script&gt;alert(1)&lt;/script&gt; notes" in pg, "feed title not escaped")
        expect("Secret chat" not in pg and "al&lt;b&gt;" not in pg and "example.com" not in pg
               and "SECRETBODY" not in pg and "Evil link" not in pg, "category/login/URL filter or body/avatar leak")
        expect(pg.count('class="news ann"') == 1 and f'href="{U}1"' in pg and "3 comments" in pg,
               "only the owner's Announcements become news entries (with count and Discuss link)")
        expect(f'href="{U}5"' in pg and ("x" * 100) not in pg and "…" in pg and "carol · 2026-10-14 · 1 reply" in pg,
               "box row missing, title not capped, or reply wording wrong")
        expect("dave · 2026-10-13 · 2 replies" in pg and "Join the discussion" in pg, "community announcement row / join link")
        expect(not website.lint(pg), f"lint on the feed page: {website.lint(pg)}")
        expect(run("--root", str(root), "--feed", str(pathlib.Path(t) / "missing.json"), "--out", str(out))[0] == 0
               and 'class="feed"' not in out.read_text() and "Join the discussion" in out.read_text(), "missing snapshot not static")
        sp.write_text("not json")
        expect(run("--root", str(root), "--feed", str(sp), "--out", str(out))[0] == 0
               and 'class="feed"' not in out.read_text(), "broken snapshot not static")
        # the fetcher: no token -> nothing written, rc 0; network failure -> same; GraphQL answer -> filtered snapshot
        wr = pathlib.Path(t) / "w.json"
        errb = io.StringIO()
        with contextlib.redirect_stderr(errb), contextlib.redirect_stdout(io.StringIO()):
            rc1 = discussions_feed.main([str(wr)], environ={})

            def boom(*a, **k):
                raise OSError("offline")
            rc2 = discussions_feed.main([str(wr)], environ={"GITHUB_TOKEN": "tok-SECRET"}, opener=boom)
        expect(rc1 == 0 and rc2 == 0 and not wr.exists(), "fetcher writes without token / network")
        expect("tok-SECRET" not in errb.getvalue(), "token printed")

        class Resp:
            def __enter__(self): return self
            def __exit__(self, *a): return False
            def read(self):
                nodes = [{"title": i["title"], "url": i["url"], "createdAt": i["created"], "category": {"name": i["category"]},
                          "author": {"login": i["author"]}, "comments": {"totalCount": i["replies"]}} for i in snap["items"]]
                return json.dumps({"data": {"repository": {"discussions": {"nodes": nodes}}}}).encode()
        sent = {}

        def ok(req, timeout=0):
            sent["auth"] = req.get_header("Authorization")
            return Resp()
        with contextlib.redirect_stdout(io.StringIO()):
            rc3 = discussions_feed.main([str(wr)], environ={"GITHUB_TOKEN": "tok-SECRET"}, opener=ok)
        saved = wr.read_text()
        expect(rc3 == 0 and sent.get("auth") == "Bearer tok-SECRET" and "tok-SECRET" not in saved and "SECRETBODY" not in saved
               and len(json.loads(saved)["items"]) == 3 and "Secret chat" not in saved, "fetcher snapshot wrong or leaks the token")

    # Sixth pass: the generated changelog and to-do boxes, the button strip, the dividers.
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        expect(website.changelog(root) == [], "changelog of a tree without the checkpoint log should be empty")
        (root / "docs/checkpoints").mkdir(parents=True)
        log = "".join(f"| 2026-10-{day:02d} 10:00:00 | [n{day}](x.png) | abc | {what} | how |\n" for day, what in (
            (1, "Title screen: first frame"), (2, "Options > Wireless Controller page: mapping changed; more"),
            (3, "Update 1.14: all nine chapters load"), (4, "Secret thing, see D56 and /home/x"),
            (5, "Update 1.14: all nine chapters load"), (6, "A " + "very long line " * 12), (7, "Brightness page (two values)")))
        (root / "docs/checkpoints/README.md").write_text("# cps\n\n| a | b | c | d | e |\n|---|---|---|---|---|\n" + log + "not a row\n")
        got = website.changelog(root, 10)
        texts = [t for _, t in got]
        expect(got[0] == ("2026-10-07", "Brightness page"), f"newest first, cut before '(': {got[:1]}")
        expect("Options > Wireless Controller page" in texts, "a long lead-in before ': ' should stand alone")
        expect("Update 1.14: all nine chapters load" in texts and texts.count("Update 1.14: all nine chapters load") == 1,
               "a short lead-in keeps its text and duplicates collapse")
        expect(not any("D56" in t or "/home" in t for t in texts), "internal wording must be skipped")
        expect(all(len(t) <= 75 for t in texts) and any(t.endswith("...") for t in texts), "long lines must be capped")
        expect(len(website.changelog(root, 2)) == 2, "n caps the entries")
    rows = [{"kind": "ps3", "feature": f"F{i}", "status": s} for i, s in
            enumerate(["works", "partial", "not yet", "not yet", "unverified", "not yet", "partial"])]
    rows.append({"kind": "port", "feature": "P", "status": "planned"})
    expect(website.todo(rows, 4) == [("F2", "not yet"), ("F3", "not yet"), ("F5", "not yet"), ("F1", "partial")],
           f"to-do order wrong: {website.todo(rows, 4)}")
    expect(all(f != "P" for f, _ in website.todo(rows, 20)), "port rows must not appear in the to-do list")
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        fixture(root)
        probs = []
        page = website.render(root, probs)
        expect("class=\"clog\"" in page and "class=\"todo\"" in page and page.count("class=\"divider\"") == 3,
               "page misses the changelog, to-do or dividers")
        # Eighth pass: HUD label bars (date readout // title), comms-log and objectives boxes, no scrapbook classes.
        expect(page.count('class="hudbar"') == page.count("<figure class=\"tile\">") >= 1 and 'class="promo term"' in page
               and 'class="promo obj"' in page and 'class="box"' in page, "page misses the HUD label bars, comms log or objectives")
        expect(not any(x in page for x in ('class="stamp"', "paper", "Caveat", "tape")), "scrapbook leftovers in the page")
        for _cls, _t, _b, href, _title in website.BUTTONS:
            expect(f'href="{href}"' in page, f"button link {href} missing")
        expect(len(website.BUTTONS) == 8 and page.count("class=\"b88 ") == 8, "eight buttons expected")
        for want in ("https://discord.gg/CRpzGWZFcu", "https://recomp.fyi", "https://www.reddit.com/user/RelationshipNo4440",
                     "https://github.com/sp00nznet/ps3recomp"):
            expect(want in page, f"requested button {want} missing")
        expect(not website.lint(page) and not probs, f"sixth-pass page has problems: {probs} {website.lint(page)}")

    for bad in ('<script src="a.js"></script>', '<img src="x.png" onerror="x()">', '<a href="javascript:x()">a</a>',
                '<img src="https://example.com/a.png">', '<link rel="stylesheet" href="https://cdn.example/x.css">',
                '<link href="https://fonts.googleapis.com.evil.example/x">', '<img src="file:///C:/a.png">'):
        expect(website.lint(bad), f"lint misses {bad}")
    for good in ('<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=X">',
                 '<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>', '<img src="../docs/a.png">',
                 f'<a href="{website.REPO}/issues">issues</a>'):
        expect(not website.lint(good), f"lint refuses {good}")

    if fails:
        print("FAIL test_website:\n  - " + "\n  - ".join(fails))
        return 1
    print("PASS test_website (generate, check, 6 negative controls, 7 lint refusals, Discussions feed + fallback, changelog, to-do, buttons)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

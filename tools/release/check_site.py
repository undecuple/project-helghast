#!/usr/bin/env python3
"""Checks of the generated project site (public/site/: index.html, plain.html, style.css, plain.css, site.js).

Run on the staged layout, which is the repo layout: site/ next to docs/screenshots/ (public/site, public/docs).
  1. both pages are well-formed HTML (every tag closed in order, ids unique);
  2. every #fragment resolves on its page; every relative link and image resolves to a file in the layout, with the
     fragment of an other-page link (index.html#x) present there;
  3. nothing loads from outside: no external script or stylesheet except the Google Fonts stylesheet (index only), no
     @import or external url() in the CSS, no inline script, no inline event handler;
  4. plain.html and index.html agree: the same sections in the same order with the same headings, and the same set of
     links inside them (the two pages come from one generator; this keeps a hand edit from drifting);
  4b. every class used in index.html has a selector in style.css (NO_RULE_OK lists the JS-only exceptions);
  5. site.js: no network, storage or eval calls, no outside URL, balanced brackets, and `node --check` when node is on
     PATH (SKIP otherwise, said on stdout).

    python3 tools/release/check_site.py [SITE_DIR]      # default: public/site (or site/ in the public repo)
"""
import html.parser
import pathlib
import re
import shutil
import subprocess
import sys

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}
# Classes the markup may use without a style.css rule: hooks that are deliberately unstyled (their element is styled by
# tag or by its parent: .mi-l inside #menu, .promo/.news boxes) or that only site.js / a reader of the markup uses.
NO_RULE_OK = {"board", "disc", "mi-l", "o-not-yet", "obj", "term", "wip"}
FONT_CSS = re.compile(r"https://fonts\.googleapis\.com(/|$)")


class Page(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.stack, self.problems, self.ids, self.refs, self.scripts, self.links = [], [], [], [], [], []
        self.classes = set()     # every class used in the markup
        self.secs = []           # [(id, heading text)] of top-level section.sec
        self.sec_hrefs = {}      # section id -> set of hrefs
        self._sec = None
        self._h2 = None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        self.classes.update((a.get("class") or "").split())
        if "id" in a:
            self.ids.append(a["id"])
        for k in a:
            if k.startswith("on"):
                self.problems.append(f"inline event handler {k} on <{tag}>")
        for k in ("href", "src"):
            if k in a and a[k] is not None:
                self.refs.append((tag, k, a[k]))
                if self._sec and tag == "a" and k == "href":
                    self.sec_hrefs.setdefault(self._sec, set()).add(a[k])
        if tag == "link":
            self.links.append(a)
        if tag == "script":
            self.scripts.append(a)
        if tag == "section" and "sec" in (a.get("class") or "").split():
            self._sec = a.get("id")
            self.secs.append([self._sec, ""])
        if tag == "h2" and self._sec and a.get("id", "").startswith("h-"):
            self._h2 = []
        if tag not in VOID:
            self.stack.append(tag)

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in VOID and self.stack and self.stack[-1] == tag:
            self.stack.pop()

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        if not self.stack or self.stack[-1] != tag:
            self.problems.append(f"</{tag}> closes {self.stack[-1] if self.stack else 'nothing'}")
            if tag in self.stack:
                while self.stack and self.stack.pop() != tag:
                    pass
            return
        self.stack.pop()
        if tag == "h2" and self._h2 is not None:
            self.secs[-1][1] = "".join(self._h2).strip()
            self._h2 = None
        if tag == "section" and self._sec and "section" not in self.stack:
            self._sec = None

    def handle_data(self, data):
        if self._h2 is not None:
            self._h2.append(data)

    def close(self):
        super().close()
        if self.stack:
            self.problems.append(f"unclosed tags at the end: {self.stack[-3:]}")
        dup = {i for i in self.ids if self.ids.count(i) > 1}
        if dup:
            self.problems.append(f"duplicate ids: {sorted(dup)[:5]}")


def parse(path):
    p = Page()
    p.feed(path.read_text(encoding="utf-8"))
    p.close()
    return p


def is_external(u):
    return bool(re.match(r"[a-z][a-z0-9+.-]*:|//", u, re.I))


def check_page(path, pages, problems, allow_fonts):
    name = path.name
    pg = pages[name]
    problems += [f"{name}: {x}" for x in pg.problems]
    ids = set(pg.ids)
    for tag, k, u in pg.refs:
        if u.startswith("#"):
            if len(u) > 1 and u[1:] not in ids:
                problems.append(f"{name}: #{u[1:]} has no target")
        elif is_external(u):
            if tag in ("link", "script", "img", "iframe", "source") and not (allow_fonts and FONT_CSS.match(u)) \
                    and not re.match(r"https://fonts\.gstatic\.com", u):
                problems.append(f"{name}: <{tag}> loads {u}")
        else:
            target, _, frag = u.partition("#")
            f = (path.parent / target).resolve() if target else path
            if not f.is_file():
                problems.append(f"{name}: {u} does not resolve to a file")
            elif frag and f.suffix == ".html":
                other = pages.get(f.name) or parse(f)
                if frag not in other.ids:
                    problems.append(f"{name}: {u} has no target in {f.name}")
    for s in pg.scripts:
        src = s.get("src")
        if not src or is_external(src):
            problems.append(f"{name}: script that is not a same-origin file")
    for ln in pg.links:
        if "stylesheet" in (ln.get("rel") or "") and is_external(ln.get("href", "")) and not (allow_fonts and FONT_CSS.match(ln["href"])):
            problems.append(f"{name}: external stylesheet {ln.get('href')}")


def check_css(path, problems):
    t = re.sub(r"/\*.*?\*/", "", path.read_text(encoding="utf-8"), flags=re.S)
    if re.search(r"@import", t):
        problems.append(f"{path.name}: @import")
    for m in re.finditer(r"url\(\s*['\"]?([^)'\"]+)", t):
        if not m.group(1).startswith("data:"):
            problems.append(f"{path.name}: url({m.group(1)[:40]}) (only data: URLs)")
    if path.name == "style.css":
        for need in ("prefers-reduced-motion", ":focus-visible", "prefers-color-scheme: light", "max-width: 860px"):
            if need not in t:
                problems.append(f"style.css: lacks {need}")


JS_BAD = re.compile(r"\b(fetch|XMLHttpRequest|sendBeacon|WebSocket|EventSource|eval|localStorage|sessionStorage|indexedDB|importScripts)\b"
                    r"|document\.(write|cookie)|new Function|https?://|<script", re.I)


def strip_js(t):
    """Source without comments and string/template contents (for the checks below; regex literals are left as they are)."""
    out, i, n = [], 0, len(t)
    while i < n:
        c = t[i]
        if t.startswith("/*", i):
            j = t.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif t.startswith("//", i) and (i == 0 or t[i - 1] != ":"):
            j = t.find("\n", i)
            i = n if j < 0 else j
        elif c in "'\"`":
            j = i + 1
            while j < n and t[j] != c:
                j += 2 if t[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def check_js(path, problems, notes):
    raw = path.read_text(encoding="utf-8")
    bare = strip_js(raw)
    nocomment = re.sub(r"(?<![:\w])//[^\n]*", "", re.sub(r"/\*.*?\*/", "", raw, flags=re.S))   # strings stay: a URL in one counts
    m = JS_BAD.search(bare) or re.search(r"https?://|<script", nocomment)
    if m:
        problems.append(f"site.js: forbidden {m.group(0)!r}")
    pairs, stack = {")": "(", "]": "[", "}": "{"}, []
    for ch in re.sub(r"/\^?[^/\n]*\$?/[gimsuy]*(?=[.,;)])", "", bare):   # drop simple regex literals
        if ch in "([{":
            stack.append(ch)
        elif ch in ")]}":
            if not stack or stack.pop() != pairs[ch]:
                problems.append("site.js: brackets do not balance")
                break
    else:
        if stack:
            problems.append("site.js: brackets do not balance (unclosed)")
    node = shutil.which("node")
    if node:
        r = subprocess.run([node, "--check", str(path)], capture_output=True, text=True)
        if r.returncode:
            problems.append("site.js: node --check: " + r.stderr.strip().splitlines()[-1][:150])
        else:
            notes.append("node --check site.js: OK")
    else:
        notes.append("SKIP node --check site.js (node is not on PATH; bracket-balance and forbidden-call checks ran)")


def check(site):
    """-> (problems, notes) for the site directory (index.html, plain.html, style.css, plain.css, site.js)."""
    site = pathlib.Path(site).resolve()
    problems, notes = [], []
    for f in ("index.html", "plain.html", "style.css", "plain.css", "site.js"):
        if not (site / f).is_file():
            problems.append(f"{f} missing")
    if problems:
        return problems, notes
    pages = {f: parse(site / f) for f in ("index.html", "plain.html")}
    check_page(site / "index.html", pages, problems, True)
    check_page(site / "plain.html", pages, problems, False)
    for f in ("style.css", "plain.css"):
        check_css(site / f, problems)
    check_js(site / "site.js", problems, notes)
    a, b = pages["index.html"], pages["plain.html"]
    css = re.sub(r"/\*.*?\*/", "", (site / "style.css").read_text(encoding="utf-8"), flags=re.S)
    have = set(re.findall(r"\.([A-Za-z_][\w-]*)", css))
    for c in sorted(a.classes - have - NO_RULE_OK):
        problems.append(f"class {c!r} is used in index.html but style.css has no selector for it")
    if a.secs != b.secs:
        problems.append(f"sections differ: index {[s[0] for s in a.secs]} vs plain {[s[0] for s in b.secs]} (or their headings)")
    if not a.secs:
        problems.append("no sections")
    for sid, _h in a.secs:
        if a.sec_hrefs.get(sid, set()) != b.sec_hrefs.get(sid, set()):
            d = a.sec_hrefs.get(sid, set()) ^ b.sec_hrefs.get(sid, set())
            problems.append(f"links in section {sid} differ between the pages: {sorted(d)[:3]}")
    if "plain.html" not in {u for _t, _k, u in a.refs}:
        problems.append("index.html does not link plain.html")
    if "index.html" not in {u.split("#")[0] for _t, _k, u in b.refs}:
        problems.append("plain.html does not link back to index.html")
    nav = {u[1:] for t, k, u in a.refs if u.startswith("#") and len(u) > 1}
    for sid, _h in a.secs:
        if sid not in nav:
            problems.append(f"section {sid} has no link to it on the index page")
    shots = [u for t, k, u in a.refs if t == "a" and k == "href" and "/screenshots/" in u]
    if len(shots) != len([1 for t, k, u in a.refs if t == "img" and "/screenshots/" in u]) or not shots:
        problems.append("every screenshot thumbnail must be a link to its image")
    return problems, notes


def main(argv):
    site = pathlib.Path(argv[1]) if len(argv) > 1 else None
    if site is None:
        root = pathlib.Path(__file__).resolve().parents[2]
        site = root / "public/site" if (root / "public/site").is_dir() else root / "site"
    problems, notes = check(site)
    for n in notes:
        print(n)
    if problems:
        print("check_site: problems:")
        for p in problems:
            print("  - " + p)
        return 1
    print(f"check_site: OK ({site})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

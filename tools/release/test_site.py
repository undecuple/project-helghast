#!/usr/bin/env python3
"""Host test for tools/release/check_site.py: the committed site passes, and each rule fails on a planted fault
(unclosed tag, dead anchor, missing image, external script, inline script and handler, external stylesheet, a section
or a link present on one page only, a missing link between the pages, a network call or unbalanced bracket in site.js,
an @import). Needs public/site (SKIP without it). Exit 0 = PASS.

    python3 tools/release/test_site.py
"""
import pathlib
import re
import shutil
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_site  # noqa: E402

ROOT = HERE.parents[1]
SITE = ROOT / "public/site" if (ROOT / "public/site").is_dir() else ROOT / "site"
fails = []


def expect(cond, what):
    if not cond:
        fails.append(what)


def staged(tmp):
    """A copy of the site in a temp layout with an empty file for every screenshot it names."""
    site = tmp / "site"
    shutil.copytree(SITE, site)
    for m in re.finditer(r'(?:src|href)="\.\./docs/screenshots/([^"]+)"', (site / "index.html").read_text()):
        (tmp / "docs/screenshots").mkdir(parents=True, exist_ok=True)
        (tmp / "docs/screenshots" / m.group(1)).write_bytes(b"x")
    return site


def mutate(name, f, text_edit, want):
    with tempfile.TemporaryDirectory() as d:
        site = staged(pathlib.Path(d))
        t = (site / f).read_text()
        new = text_edit(t)
        expect(new != t, f"{name}: the planted fault changed nothing")
        (site / f).write_text(new)
        probs, _ = check_site.check(site)
        expect(any(want in p for p in probs), f"{name}: not caught (wanted {want!r}); got {probs[:3]}")


def main():
    if not (SITE / "index.html").is_file():
        print("SKIP test_site (no public/site)")
        return 0
    with tempfile.TemporaryDirectory() as d:
        probs, notes = check_site.check(staged(pathlib.Path(d)))
        expect(not probs, f"the committed site has problems: {probs[:4]}")
    first_shot = lambda t: re.search(r'href="(\.\./docs/screenshots/[^"]+)"', t).group(1)  # noqa: E731
    mutate("unclosed tag", "index.html", lambda t: t.replace("</figcaption>", "", 1), "closes")
    mutate("dead anchor", "index.html", lambda t: t.replace('href="#status"', 'href="#nowhere"', 1), "#nowhere has no target")
    mutate("missing image", "index.html", lambda t: t.replace(first_shot(t), "../docs/screenshots/gone.png", 2), "does not resolve")
    mutate("external script", "index.html", lambda t: t.replace('src="site.js"', 'src="https://cdn.example/a.js"'), "script")
    mutate("inline script", "plain.html", lambda t: t.replace("</head>", "<script>x()</script></head>", 1), "script")
    mutate("inline handler", "index.html", lambda t: t.replace("<main ", '<main onclick="x()" ', 1), "inline event handler")
    mutate("external stylesheet", "plain.html", lambda t: t.replace("</head>", '<link rel="stylesheet" href="https://cdn.example/x.css"></head>', 1), "loads https://cdn.example")
    mutate("fonts on the plain page", "plain.html", lambda t: t.replace("</head>", '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=X"></head>', 1), "loads https://fonts")
    mutate("section only on index", "index.html", lambda t: t.replace('<section id="about"', '<section id="extra" class="sec" aria-labelledby="h-extra"><h2 id="h-extra">Extra</h2></section>\n<section id="about"', 1), "sections differ")
    mutate("link only on plain", "plain.html", lambda t: t.replace('<section id="legal" class="sec"', '<section id="legal" class="sec"><a href="https://example.org/">x</a>', 1), "links in section legal differ")
    mutate("no link to plain", "index.html", lambda t: t.replace('href="plain.html"', 'href="#top"'), "does not link plain.html")
    mutate("no link back", "plain.html", lambda t: t.replace('href="index.html"', 'href="#top"'), "does not link back")
    mutate("thumbnail not a link", "index.html", lambda t: re.sub(r'<a class="shot" href="[^"]+"[^>]*>(<img[^>]*>)</a>', r"\1", t, count=1), "thumbnail")
    mutate("network call", "site.js", lambda t: t.replace("var root =", "fetch('x'); var root =", 1), "forbidden")
    mutate("outside url", "site.js", lambda t: t.replace("var root =", "var u = 'https://x.example'; var root =", 1), "forbidden")
    mutate("unbalanced bracket", "site.js", lambda t: t.replace("})();", "}();"), "brackets")
    mutate("css import", "style.css", lambda t: '@import url("https://x.example/a.css");\n' + t, "@import")
    mutate("css external url", "style.css", lambda t: t + "\nbody{background:url(https://x.example/a.png)}\n", "url(")
    mutate("no reduced motion", "style.css", lambda t: t.replace("prefers-reduced-motion", "prefers-x"), "reduced-motion")
    if fails:
        print("FAIL test_site:\n  - " + "\n  - ".join(fails))
        return 1
    print("PASS test_site (committed site clean, 19 planted faults caught; " + "; ".join(notes) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())

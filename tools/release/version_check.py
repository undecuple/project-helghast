#!/usr/bin/env python3
"""One-shot, opt-in check for a newer release of the port. Standard library only,
Python 3.8+, Windows and Linux.

    version_check.py --current X.Y.Z [--url URL] [--timeout S] [--json] [--result-file FILE] [--log FILE]

It makes one anonymous HTTPS GET of the repository's release list (DEFAULT_URL, newest 10; drafts skipped, pre-releases count) with only a User-Agent and an
Accept header: no cookies, no token, nothing about the player, the machine or the game. It runs only when the player
asked for it (the installer's "Check now" button, the portable zip's question, or the launcher when the player ticked
"Check for new versions"); nothing is downloaded here: a newer release is reported with its page, and the download
starts only from the player's click on that page (or a later, separate download step).

Output, one line (also written to --result-file, which the Inno wizard reads):
  VERSION status=newer|current|offline|error current=<x> latest=<tag or -> url=<release page or -> setup=<asset url or ->
Offline or blocked = status=offline, one line in --log (appended), no error dialog anywhere.
Exit 0 = up to date, 1 = a newer release exists, 2 = usage, 3 = offline or an unusable answer.

--url accepts https:// (any host), and http:// only on the loopback (the host test's local server) or file:// (a
saved answer); the tests never touch the network.
"""
import argparse
import datetime
import json
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import names  # noqa: E402  (product name, file names, links: tools/release/names.py)

DEFAULT_URL = names.API_RELEASES
USER_AGENT = names.USER_AGENT
LOOPBACK = {"127.0.0.1", "localhost", "::1"}


def parse_version(s):
    """'v1.2.3' / '1.2' / '0.0.0-dev' -> (numbers tuple, is_release). A pre-release suffix sorts below the release."""
    s = (s or "").strip().lstrip("vV")
    m = re.match(r"^(\d+(?:\.\d+)*)(.*)$", s)
    if not m:
        return None
    nums = tuple(int(x) for x in m.group(1).split("."))
    nums = nums + (0,) * (4 - len(nums)) if len(nums) < 4 else nums
    return nums, m.group(2).strip() == ""


def newer(latest, current):
    lv, cv = parse_version(latest), parse_version(current)
    if lv is None:
        return False
    if cv is None:
        return True
    if lv[0] != cv[0]:
        return lv[0] > cv[0]
    return lv[1] and not cv[1]          # same numbers: a release beats a -dev/-rc of the same numbers


def allowed_url(url):
    u = urllib.parse.urlparse(url)
    if u.scheme == "https" and u.hostname:
        return True
    if u.scheme == "http" and u.hostname in LOOPBACK:
        return True
    return u.scheme == "file"


def fetch(url, timeout):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/vnd.github+json"})
    # A plain opener: no cookie jar, no auth handler, proxies from the system settings only.
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read(1 << 20).decode("utf-8"))


def pick_setup(assets):
    """The installer asset (…setup….exe) and its checksum file, if the release has them."""
    exe = next((a for a in assets if re.search(r"setup.*\.exe$", a.get("name", ""), re.I)), None)
    if not exe:
        return None, None
    sha = next((a for a in assets if a.get("name", "").lower() in
                (exe["name"].lower() + ".sha256", exe["name"].lower().rsplit(".", 1)[0] + ".sha256")), None)
    return exe.get("browser_download_url"), (sha or {}).get("browser_download_url")


def pick_release(answer):
    """The newest published release of a /releases list by version (pre-releases included, drafts skipped).
    A single release object is accepted as a list of one."""
    items = [answer] if isinstance(answer, dict) else answer if isinstance(answer, list) else []
    best, key = None, None
    for r in items:
        if not isinstance(r, dict) or r.get("draft"):
            continue
        v = parse_version(r.get("tag_name"))
        if v is not None and (key is None or v > key):
            best, key = r, v
    return best


def check(current, url=DEFAULT_URL, timeout=10.0):
    """-> dict(status, current, latest, url, setup, sha256_url, detail)."""
    res = {"status": "error", "current": current, "latest": None, "url": None, "setup": None, "sha256_url": None,
           "detail": ""}
    try:
        j = fetch(url, timeout)
    except urllib.error.HTTPError as e:
        res["status"], res["detail"] = "offline", f"HTTP {e.code}"
        return res
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        res["status"], res["detail"] = "offline", str(getattr(e, "reason", e))
        return res
    except ValueError as e:
        res["detail"] = f"not JSON ({e})"
        return res
    j = pick_release(j)
    if j is None:
        res["detail"] = "no usable published release in the answer"
        return res
    tag = j["tag_name"]
    res["latest"], res["url"] = tag, j.get("html_url")
    res["setup"], res["sha256_url"] = pick_setup(j.get("assets") or [])
    res["status"] = "newer" if newer(tag, current) else "current"
    return res


def line(r):
    return (f"VERSION status={r['status']} current={r['current']} latest={r['latest'] or '-'} "
            f"url={r['url'] or '-'} setup={r['setup'] or '-'}")


def main(argv=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--current", required=True)
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--result-file", default="")
    ap.add_argument("--log", default="")
    a = ap.parse_args(argv)
    if not allowed_url(a.url):
        print(f"refusing --url {a.url}: https, loopback http or file only", file=sys.stderr)
        return 2
    r = check(a.current, a.url, a.timeout)
    text = line(r)
    print(json.dumps(r, indent=1) if a.json else text, file=out)
    if a.result_file:
        with open(a.result_file, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    if a.log:
        stamp = datetime.datetime.now().isoformat(timespec="seconds")
        detail = f" ({r['detail']})" if r["detail"] else ""
        with open(a.log, "a", encoding="utf-8") as f:
            f.write(f"# {stamp} version check: {text}{detail}\n")
    return {"current": 0, "newer": 1}.get(r["status"], 3)


if __name__ == "__main__":
    sys.exit(main())

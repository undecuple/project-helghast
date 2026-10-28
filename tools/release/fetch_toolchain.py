#!/usr/bin/env python3
"""Fetch the pinned release toolchain (tools/release/toolchain.json) into a local cache: download, check SHA-256,
unpack. Standard library only, Python 3.8+. The maintainer runs it outside the sandbox (it needs the network);
assemble_payload.py --toolchain CACHE then copies the trees into the payload.

    fetch_toolchain.py [--manifest FILE] [--cache DIR] [--only NAME[,NAME]] [--fill]
    fetch_toolchain.py --check [--strict] [--manifest FILE]

  --cache   default work/release/toolchain-cache (downloads/<name>/, trees/<name>/, fetched.json)
  --fill    entries whose sha256 is TO-FILL are fetched and their hash written into the manifest (trust on first
            fetch: compare it with the release page's published hash where one exists; CMake's SHA-256 list is
            checked automatically through the entry's sha256_list). Without --fill a TO-FILL entry is refused.
  --check   validate the manifest only (no network); --strict also refuses TO-FILL hashes and TO-CONFIRM pins
            (the release build's gate)

A filled hash that does not match the download is refused and the download deleted (if the version was changed
on purpose, set its sha256 back to TO-FILL and use --fill). Zip members with absolute paths, "..", drive letters or
symlink modes are refused. Exit 0 = ok, 1 = a check failed, 2 = usage.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MANIFEST = os.path.join(HERE, "toolchain.json")
CACHE = os.path.join(REPO, "work", "release", "toolchain-cache")
TO_FILL, TO_CONFIRM = "TO-FILL", "TO-CONFIRM"
HEX64 = re.compile(r"[0-9a-f]{64}")
NAME_RX = re.compile(r"[a-z0-9][a-z0-9.-]*")
MAX_DOWNLOAD = 1 << 30            # 1 GiB: llvm-mingw's Windows zip is ~200 MB (ASSUMPTION)
REQUIRED = ("name", "group", "version", "url", "sha256", "archive", "license", "release_page")
REPO_REQUIRED = ("name", "group", "version", "repo_path", "sha256", "archive", "license", "release_page")
FORMAT_FIELDS = ("url", "sha256_list", "notice")


# ---------------------------------------------------------------------------------------------------------- manifest
def load(path=MANIFEST):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def repo_file(rel):
    """Path of a file shipped in the repository, in the private layout (public/<rel>) or the export layout (<rel>)."""
    for c in (os.path.join(REPO, "public", rel), os.path.join(REPO, rel)):
        if os.path.isfile(c):
            return c
    return os.path.join(REPO, "public", rel)


def expand(entry):
    """The entry with '{version}', '{llvm}' (any of its own string fields) filled into url, sha256_list, notice."""
    e = dict(entry)
    fields = {k: v for k, v in entry.items() if isinstance(v, str)}
    for k in FORMAT_FIELDS:
        if k in e:
            e[k] = e[k].format(**fields)
    return e


def safe_rel(p):
    """True for a relative, forward-slash path without '..', drive letters or empty parts."""
    if not p or p.startswith(("/", "\\")) or "\\" in p or ":" in p:
        return False
    return all(part not in ("", ".", "..") for part in p.split("/"))


def validate(m, schemes=("https",), strict=False):
    """-> list of problems (empty = ok). strict: TO-FILL hashes and TO-CONFIRM pins are problems too."""
    bad = []
    if m.get("format") != 1:
        bad.append("format: expected 1")
    comps = m.get("components")
    if not isinstance(comps, list) or not comps:
        return bad + ["components: missing or empty"]
    names, groups_lic, groups_main = set(), set(), set()
    for i, raw in enumerate(comps):
        where = f"components[{i}]"
        miss = [k for k in (REPO_REQUIRED if raw.get("kind") == "repo" else REQUIRED) if not isinstance(raw.get(k), str) or not raw.get(k)]
        if miss:
            bad.append(f"{where}: missing {', '.join(miss)}")
            continue
        try:
            e = expand(raw)
        except (KeyError, IndexError, ValueError) as ex:
            bad.append(f"{where} {raw['name']}: bad placeholder {ex}")
            continue
        n = e["name"]
        where = f"{n}"
        if not NAME_RX.fullmatch(n):
            bad.append(f"{where}: name must be lower-case letters, digits, '.', '-'")
        if n in names:
            bad.append(f"{where}: duplicate name")
        names.add(n)
        for k in ("url", "sha256_list", "release_page"):
            if k in e and e[k].split(":", 1)[0] not in schemes:
                bad.append(f"{where}: {k} must use {'/'.join(schemes)}")
        if e.get("kind") == "repo":
            if e["archive"] != "file" or not safe_rel(e["repo_path"]):
                bad.append(f"{where}: kind repo needs archive file and a plain relative repo_path")
        if e["sha256"] == TO_FILL:
            if strict:
                bad.append(f"{where}: sha256 is TO-FILL (run fetch_toolchain.py --fill)")
        elif not HEX64.fullmatch(e["sha256"]):
            bad.append(f"{where}: sha256 must be 64 lower-case hex digits or TO-FILL")
        if e.get("pin") not in (None, "", TO_CONFIRM, "confirmed"):
            bad.append(f"{where}: pin must be TO-CONFIRM or confirmed")
        if strict and e.get("pin") == TO_CONFIRM:
            bad.append(f"{where}: pin is TO-CONFIRM (check the release page, then set 'confirmed')")
        if e.get("stage_only"):
            # fetched and hash-checked like the others but never copied into the payload's toolchain/ (SDL3.dll is
            # staged by stage_sdl3.py into runtime/); no dest, group or licence-globs bookkeeping
            if e["archive"] != "zip":
                bad.append(f"{where}: stage_only needs archive zip")
        elif e["archive"] == "zip":
            groups_main.add(e["group"])
            if not safe_rel(e.get("dest", "")):
                bad.append(f"{where}: dest must be a plain relative folder")
            for k in ("license_globs", "prune"):
                if not isinstance(e.get(k, []), list):
                    bad.append(f"{where}: {k} must be a list")
            if e.get("license_globs"):
                groups_lic.add(e["group"])
            if not e.get("notice"):
                bad.append(f"{where}: notice (the NOTICE.md line) missing")
        elif e["archive"] == "file":
            if not e.get("license_file") or not safe_rel(e["license_file"]) or "/" in e["license_file"]:
                bad.append(f"{where}: archive 'file' needs license_file (a plain file name)")
            groups_lic.add(e["group"])
        else:
            bad.append(f"{where}: archive must be zip or file")
    for g in sorted(groups_main - groups_lic):
        bad.append(f"group {g}: no licence source (license_globs or a licence-file entry)")
    for g in sorted(groups_lic - groups_main):
        bad.append(f"group {g}: licence entries but nothing shipped")
    dests = [c.get("dest") for c in comps if c.get("archive") == "zip" and not c.get("stage_only")]
    if len(dests) != len(set(dests)):
        bad.append("two zip entries share a dest folder")
    return bad


def fill_hash(path, name, digest):
    """Write `digest` into entry `name`'s TO-FILL sha256, keeping the file's layout (text edit, then re-parse)."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r'"name":\s*"%s"' % re.escape(name), text)
    if not m:
        raise ValueError(f"{name}: not found in {path}")
    rest = text[m.end():]
    nxt = re.search(r'"name":\s*"', rest)
    block = rest[:nxt.start()] if nxt else rest
    h = re.search(r'"sha256":\s*"%s"' % TO_FILL, block)
    if not h:
        raise ValueError(f"{name}: no TO-FILL sha256 to fill")
    pos = m.end() + h.start()
    new = text[:pos] + f'"sha256": "{digest}"' + text[pos + (h.end() - h.start()):]
    data = json.loads(new)
    got = [c for c in data["components"] if c.get("name") == name][0]["sha256"]
    if got != digest:
        raise ValueError(f"{name}: fill landed in the wrong place")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(new)
    os.replace(tmp, path)


# ---------------------------------------------------------------------------------------------------------- download
def open_url(url):
    req = urllib.request.Request(url, headers={"User-Agent": "kz3-release-fetch/1"})
    return urllib.request.urlopen(req, timeout=120)    # noqa: S310 (scheme checked by validate)


def download(url, dest, opener=open_url):
    """Stream url to dest (via dest.part), return its SHA-256. Refuses more than MAX_DOWNLOAD bytes."""
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    part, h, n = dest + ".part", hashlib.sha256(), 0
    try:
        with opener(url) as r, open(part, "wb") as f:
            for chunk in iter(lambda: r.read(1 << 20), b""):
                n += len(chunk)
                if n > MAX_DOWNLOAD:
                    raise ValueError(f"{url}: more than {MAX_DOWNLOAD:,} bytes")
                h.update(chunk)
                f.write(chunk)
        os.replace(part, dest)
    finally:
        if os.path.exists(part):
            os.remove(part)
    return h.hexdigest()


def hash_from_list(text, filename):
    """The hash for `filename` in a '<hex>  <name>' list (CMake's cmake-X-SHA-256.txt), else None."""
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip("*") == filename and HEX64.fullmatch(parts[0].lower()):
            return parts[0].lower()
    return None


# ---------------------------------------------------------------------------------------------------------- unpack
def unpack_zip(path, out, strip_top):
    """Extract into out (which must not exist). Refuses unsafe member names and symlinks; strip_top drops the one
    top-level folder every member shares (refused when there is not exactly one)."""
    with zipfile.ZipFile(path) as z:
        infos = [i for i in z.infolist() if not i.is_dir()]
        names = [i.filename for i in infos]
        for i in infos:
            if not safe_rel(i.filename.rstrip("/")):
                raise ValueError(f"unsafe zip member: {i.filename!r}")
            if stat.S_ISLNK(i.external_attr >> 16):
                raise ValueError(f"symlink in zip: {i.filename!r}")
        if strip_top:
            tops = {n.split("/", 1)[0] for n in names}
            if len(tops) != 1 or any("/" not in n for n in names):
                raise ValueError(f"strip_top: expected one top folder, found {sorted(tops)[:5]}")
        part = out + ".partial"
        if os.path.lexists(part):
            shutil.rmtree(part)
        for i in infos:
            rel = i.filename.split("/", 1)[1] if strip_top else i.filename
            d = os.path.join(part, *rel.split("/"))
            os.makedirs(os.path.dirname(d), exist_ok=True)
            with z.open(i) as src, open(d, "wb") as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
        os.makedirs(part, exist_ok=True)
    if os.path.lexists(out):
        shutil.rmtree(out)
    os.replace(part, out)
    return len(infos)


# ---------------------------------------------------------------------------------------------------------- fetch
def fetched_path(cache):
    return os.path.join(cache, "fetched.json")


def read_fetched(cache):
    try:
        with open(fetched_path(cache), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def write_fetched(cache, data):
    os.makedirs(cache, exist_ok=True)
    tmp = fetched_path(cache) + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=1, sort_keys=True)
        f.write("\n")
    os.replace(tmp, fetched_path(cache))


def drop(e, path):
    """Delete a refused download; never a file that lives in the repository."""
    if e.get("kind") != "repo":
        os.remove(path)


def fetch_one(raw, cache, fill, manifest_path, opener=open_url, out=sys.stdout):
    """Download, verify, unpack one entry. -> True on success (problems are printed)."""
    e = expand(raw)
    name = e["name"]
    if e.get("kind") == "repo":     # a file that ships in the repository: no network, still hash-checked
        url = "repo:" + e["repo_path"]
        dl = repo_file(e["repo_path"])
        fname = os.path.basename(dl)
    else:
        url = e["url"]
        fname = url.rstrip("/").rsplit("/", 1)[-1]
        dl = os.path.join(cache, "downloads", name, fname)
    try:
        if e.get("kind") == "repo":
            with open(dl, "rb") as f:
                got = hashlib.sha256(f.read()).hexdigest()
        else:
            got = download(url, dl, opener)
    except (OSError, ValueError) as ex:
        print(f"FAIL {name}: download {url}: {ex} (check the version on {e['release_page']})", file=out)
        return False
    want = e["sha256"]
    if e.get("sha256_list"):
        try:
            lst = os.path.join(cache, "downloads", name, "sha256-list.txt")
            download(e["sha256_list"], lst, opener)
            with open(lst, encoding="utf-8", errors="replace") as f:
                published = hash_from_list(f.read(), fname)
        except (OSError, ValueError) as ex:
            published = None
            print(f"FAIL {name}: published hash list {e['sha256_list']}: {ex}", file=out)
        if published != got:
            print(f"FAIL {name}: SHA-256 {got} is not the published one ({published})", file=out)
            drop(e, dl)
            return False
    if want == TO_FILL:
        if not fill:
            print(f"FAIL {name}: sha256 is TO-FILL; re-run with --fill to record {got}", file=out)
            drop(e, dl)
            return False
        fill_hash(manifest_path, name, got)
        print(f"FILL {name}: sha256 {got} written to {os.path.basename(manifest_path)}", file=out)
    elif got != want:
        print(f"FAIL {name}: SHA-256 mismatch: manifest {want}, download {got}; refused and deleted (a deliberate "
              "version change: set sha256 to TO-FILL and use --fill)", file=out)
        drop(e, dl)
        return False
    tree = os.path.join(cache, "trees", name)
    try:
        if e["archive"] == "zip":
            n = unpack_zip(dl, tree, bool(e.get("strip_top")))
        else:
            if os.path.lexists(tree):
                shutil.rmtree(tree)
            os.makedirs(tree)
            shutil.copyfile(dl, os.path.join(tree, e["license_file"]))
            n = 1
    except (OSError, ValueError, zipfile.BadZipFile) as ex:
        print(f"FAIL {name}: unpack: {ex}", file=out)
        return False
    data = read_fetched(cache)
    data[name] = {"sha256": got, "url": url, "version": e["version"], "files": n}
    write_fetched(cache, data)
    print(f"ok   {name} {e['version']}: {n} file(s) in trees/{name}", file=out)
    return True


def main(argv=None, out=sys.stdout, opener=open_url, schemes=("https",)):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--manifest", default=MANIFEST)
    ap.add_argument("--cache", default=CACHE)
    ap.add_argument("--only", default="")
    ap.add_argument("--fill", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--strict", action="store_true")
    a = ap.parse_args(argv)
    try:
        m = load(a.manifest)
    except (OSError, ValueError) as ex:
        print(f"FAIL manifest {a.manifest}: {ex}", file=out)
        return 1
    problems = validate(m, schemes, strict=a.strict)
    for p in problems:
        print(f"FAIL {p}", file=out)
    comps = m.get("components", [])
    if a.check:
        unfilled = sum(1 for c in comps if c.get("sha256") == TO_FILL)
        unconfirmed = sum(1 for c in comps if c.get("pin") == TO_CONFIRM)
        print(f"manifest: {len(comps)} entries, {unfilled} TO-FILL hash(es), {unconfirmed} TO-CONFIRM pin(s); "
              f"{'ok' if not problems else f'{len(problems)} problem(s)'}", file=out)
        return 1 if problems else 0
    if problems:
        return 1
    only = {x for x in a.only.split(",") if x}
    unknown = only - {c["name"] for c in comps}
    if unknown:
        print(f"--only: unknown {sorted(unknown)}", file=out)
        return 2
    ok = True
    for c in comps:
        if not only or c["name"] in only:
            ok = fetch_one(c, os.path.abspath(a.cache), a.fill, a.manifest, opener, out) and ok
    print(f"fetch: {'ok' if ok else 'FAILED'} (cache {a.cache})", file=out)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

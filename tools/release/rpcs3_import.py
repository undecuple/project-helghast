#!/usr/bin/env python3
"""RPCS3 import for the first-run wizard: offer the player's installed Killzone 3 disc
game and 1.14 update as the default folders, guide the decryption of the update's EBOOT.BIN with RPCS3's own tool, and
optionally copy the saves. Locating is rpcs3_locate.py (vendored, shared with the other PS3 ports); this file adds no
parsing of RPCS3's files.

Rules: no keys, no decryption code, nothing downloaded; RPCS3's folders are only read (the one write is the save copy,
into the port's own save root, refused if it lies inside an RPCS3 folder); RPCS3's shader and PPU caches are never read.
All player-facing text is in rpcs3_text.py. Errors come back as values, never raised, so the old prompts stay as they were.

    rpcs3_import.py [--rpcs3 DIR] [--json]     print what was found (paths as found)

The wizard (installer/kz3.iss, "Use my RPCS3 install") runs the modes below and reads key=value lines from --out
(UTF-8 with BOM so Inno reads them; written whole or not at all). Ported from the Killzone 2 port's
tools/release/rpcs3_import.py (same author, MIT): the --out file, --eboot-kind, --identify and the resolve shape.
  --detect [--rpcs3 DIR]        status=ok|not_found, root=, source=
  --resolve --rpcs3 DIR         status=ok|problems|not_found, dump=, update=, eboot=, title=, problem=code|text, help=text
  --eboot-kind PATH [--sha256]  status=encrypted|ok, help=; --sha256 adds size= and sha256=
                                --identify [--title ID] [--target 1.14] adds the first-run check's E1/H1 verdict on the
                                file: version=, level=OK|WARN|FAIL, code=, text= (the disc's 1.00 EBOOT is a FAIL)
Any unexpected error writes status=error to --out.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rpcs3_text as T  # noqa: E402

TITLE_ID = "BCUS98234"
TITLE_IDS = ("BCUS98234", "BCES01007", "BCAS25008", "BCJS30066")    # US; EU, Asia, JP are accepted as untested regions (firstrun_check.py)
UPDATE_VER = "01.14"
SAVE_USER = "00000001"       # ASSUMPTION: the port's single local user; RPCS3 uses 00000001 for its first user too


def _locate():
    import rpcs3_locate     # Python 3.11+: older Pythons fall back to the plain prompts
    return rpcs3_locate


def discover(hint=None, locate=None):
    """-> dict: ok, root, disc (folder or ""), update (folder or ""), update_problem (text or ""), saves (list).
    ok False = no RPCS3 folder: the caller keeps its prompts."""
    out = {"ok": False, "title": "", "root": "", "disc": "", "disc_iso": False, "disc_problem": "", "update": "", "update_problem": "", "saves": []}
    try:
        L = locate or _locate()
        res = L.locate(hint or None, {i: UPDATE_VER for i in TITLE_IDS})
    except Exception:       # an import or read failure must never stop the wizard
        return out
    if not res.get("ok"):
        return out
    # the release RPCS3 has a disc or update for (US first); none found = the US entry, which carries the problems
    tid = next((i for i in TITLE_IDS if ((res["titles"].get(i) or {}).get("disc") or {}).get("ok")
                or ((res["titles"].get(i) or {}).get("update") or {}).get("ok")), TITLE_ID)
    t = res["titles"].get(tid, {})
    out.update(ok=True, root=res["root"], title=tid)
    d = t.get("disc") or {}
    if d.get("ok") and d.get("is_iso"):
        # an ISO entry goes through the same ISO checks as an ISO the player picks by hand (decrypted, readable)
        bad = _iso_problem(d["disc_dir"])
        if bad:
            out["disc_problem"] = T.ISO_REFUSED.format(path=d["disc_dir"], why=bad)
        else:
            out.update(disc=d["disc_dir"], disc_iso=True)
    elif d.get("ok") and d.get("has_ps3_game"):
        out["disc"] = d["disc_dir"]
    u = t.get("update") or {}
    if u.get("ok"):
        out["update"] = u["path"]
    elif u.get("code") == "wrong_version":
        out["update_problem"] = T.WRONG_UPDATE.format(have=u.get("app_ver"), want=UPDATE_VER)
    for uid, items in ((t.get("saves") or {}).get("users") or {}).items():
        out["saves"] += items
    return out


def _iso_problem(path):
    """"" when the ISO passes firstrun_check.check_dump_iso, else the first failure's text."""
    try:
        import firstrun_check as fc
        r = fc.Report()
        fc.check_dump_iso(r, path, fc.MIN_PSARC_BYTES)
    except Exception as e:      # never stop the wizard
        return f"it could not be read ({e})"
    bad = [c["message"] for c in r.checks if c["level"] == "FAIL"]
    return bad[0] if bad else ""


def describe(found):
    """Lines to show for a discover() result."""
    if not found["ok"]:
        return []
    ls = [T.FOUND.format(root=found["root"])]
    if found.get("disc_problem"):
        ls.append(found["disc_problem"])
    elif found["disc"]:
        ls.append((T.FOUND_DISC_ISO if found.get("disc_iso") else T.FOUND_DISC).format(path=found["disc"]))
    else:
        ls.append(T.NO_DISC)
    if found["update"]:
        ls.append(T.FOUND_UPDATE.format(ver=UPDATE_VER, path=found["update"]))
    else:
        ls.append(found["update_problem"] or T.NO_UPDATE)
    return ls


def encrypted_eboot(update_dir):
    """The update's EBOOT.BIN path when it is a SELF (SCE header) and no EBOOT.elf lies beside it, else ""."""
    b = os.path.join(update_dir, "USRDIR", "EBOOT.BIN")
    e = os.path.join(update_dir, "USRDIR", "EBOOT.elf")
    return b if _locate().eboot_kind(b).get("kind") == "self" and not os.path.isfile(e) else ""


def eboot_guidance(update_dir):
    """Guidance when the update's EBOOT.BIN is still encrypted and no EBOOT.elf lies beside it, else ""."""
    b = encrypted_eboot(update_dir)
    if not b:
        return ""
    return T.ENCRYPTED + "\n" + T.STEPS.format(path=b)


def save_dest(save_root, name):
    return os.path.join(save_root, "gamedata", "dev_hdd0", "home", SAVE_USER, "savedata", name)


def import_saves(found, save_root, ask_yes, say, locate=None):
    """Optional step. ask_yes(question, default) -> bool. Copies each save folder; asks before replacing one.
    Returns (copied, kept) counts. Writes only below save_root."""
    saves = found.get("saves") or []
    if not saves:
        say(T.SAVES_NONE)
        return 0, 0
    L = locate or _locate()
    say(T.SAVES_FOUND.format(n=len(saves)))
    for s in saves:
        say(T.SAVES_ITEM.format(name=s["name"], user=s["user"]))
    if not ask_yes(T.SAVES_ASK, False):
        return 0, 0
    copied = kept = 0
    for s in saves:
        dst = save_dest(save_root, s["name"])
        r = L.copy_save(s["path"], dst, overwrite=False, dry_run=False)
        if not r["ok"] and r.get("code") == "exists":
            if not ask_yes(T.SAVES_EXISTS.format(name=s["name"]), False):
                say(T.SAVES_SKIPPED.format(name=s["name"]))
                kept += 1
                continue
            r = L.copy_save(s["path"], dst, overwrite=True, dry_run=False)
        if r["ok"]:
            say(T.SAVES_COPIED.format(name=s["name"], n=r["copied"]))
            copied += 1
        else:
            say(T.SAVES_FAILED.format(name=s["name"], why=r.get("text", "")))
    say(T.SAVES_DONE.format(copied=copied, kept=kept))
    return copied, kept


# ------------------------------------------------------------------------------------------------ wizard modes
def identify_pairs(path, title="", target=UPDATE_VER[1:]):
    """The EBOOT verdict: firstrun_check.check_eboot_elf (E1: size table, the disc's 1.00 refused) and
    check_eboot_hash (H1: the shared EBOOT id table) on the file alone. -> [("version", v), ("level", worst),
    ("code", c), ("text", t)]; level OK and no text when nothing is wrong. Adapted from the Killzone 2 port's
    rpcs3_import.identify_pairs."""
    import firstrun_check as fc
    r = fc.Report()
    if title in fc.TITLE_IDS:
        r.facts["disc"] = {"TITLE_ID": title}
    fc.check_eboot_elf(r, path, target)
    fc.check_eboot_hash(r, path, fc.HASHES_FILE, target)
    rank = {"OK": 0, "WARN": 1, "FAIL": 2}
    worst = max(r.checks, key=lambda c: rank[c["level"]], default={"level": "OK", "code": "", "message": ""})
    out = [("version", (r.facts.get("eboot_elf") or {}).get("version") or ""), ("level", worst["level"])]
    if worst["level"] != "OK":
        out += [("code", worst["code"]), ("text", worst["message"])]
    return out


def update_eboot(update_dir):
    """<update>/USRDIR/EBOOT.elf when it is a real ELF (not a SELF saved under that name), else ""."""
    e = os.path.join(update_dir, "USRDIR", "EBOOT.elf")
    return e if os.path.isfile(e) and _locate().eboot_kind(e).get("kind") == "elf" else ""


def resolve(hint):
    """What the wizard needs from the player's RPCS3: -> dict status (ok / problems / not_found), dump, update, eboot,
    title, problems [(code, text)], help [text]. status ok = all three paths found and the EBOOT is not refused."""
    f = discover(hint)
    res = {"status": "not_found", "root": f["root"], "dump": "", "update": "", "eboot": "", "title": f["title"],
           "problems": [], "help": []}
    if not f["ok"]:
        return res
    res.update(dump=f["disc"], update=f["update"])
    P = res["problems"]
    if not f["disc"]:
        P.append(("missing_disc", f["disc_problem"] or T.NO_DISC.strip()))
        res["help"].append(T.HELP_DISC)
    if not f["update"]:
        P.append(("wrong_version" if f["update_problem"] else "missing_update", f["update_problem"] or T.NO_UPDATE.strip()))
        res["help"].append(T.HELP_VERSION if f["update_problem"] else T.HELP_UPDATE)
    else:
        enc = bool(encrypted_eboot(f["update"]))
        e = "" if enc else update_eboot(f["update"])
        if not e:
            P.append(("encrypted_eboot" if enc else "no_decrypted_eboot", ""))
            res["help"].append(T.HELP_ENCRYPTED if enc else T.HELP_NO_ELF)
        else:
            v = dict(identify_pairs(e, f["title"]))
            if v.get("level") == "FAIL":
                P.append(("eboot_" + (v.get("code") or "E1").lower(), v.get("text", "")))
            else:
                res["eboot"] = e
    res["status"] = "problems" if P else "ok"
    return res


def _write(path, pairs):
    """The result file, whole or not at all (written beside it, renamed over it as the last act): the wizard polls for it."""
    tmp = path + ".part"
    with open(tmp, "w", encoding="utf-8-sig", newline="\r\n") as f:
        for k, v in pairs:
            f.write(f"{k}={str(v).replace(chr(13), ' ').replace(chr(10), ' ')}\n")
    os.replace(tmp, path)


def wizard_mode(a):
    """The key=value modes (--detect, --resolve, --eboot-kind); returns the exit code."""
    if a.eboot_kind is not None:
        enc = _locate().eboot_kind(a.eboot_kind).get("kind") == "self"
        pairs = [("status", "encrypted" if enc else "ok")] + ([("help", T.HELP_ENCRYPTED)] if enc else [])
        if (a.sha256 or a.identify) and not enc:
            h = _locate().sha256(a.eboot_kind)
            if h.get("ok"):
                pairs += [("size", h["size"]), ("sha256", h["sha256"])]
        if a.identify and not enc:
            pairs += identify_pairs(a.eboot_kind, a.title, a.target)
        rc = 1 if enc else 0
    elif a.detect:
        c = [x for x in _locate().find_rpcs3(a.rpcs3 or None) if x.get("root") and not x.get("code")]
        pairs = [("status", "ok"), ("root", c[0]["root"]), ("source", c[0]["source"])] if c else [("status", "not_found")]
        rc = 0 if c else 1
    else:
        r = resolve(a.rpcs3)
        pairs = [("status", r["status"])] + [(k, r[k]) for k in ("dump", "update", "eboot", "title")]
        pairs += [("problem", f"{c}|{t}") for c, t in r["problems"]] + [("help", h) for h in r["help"]]
        rc = 0 if r["status"] == "ok" else 1
    if a.out:
        _write(a.out, pairs)
    else:
        print("\n".join(f"{k}={v}" for k, v in pairs))
    return rc


def main(argv=None):
    """_main, but an unexpected error still leaves status=error in --out (the wizard waits for the file)."""
    try:
        return _main(argv)
    except Exception as e:  # noqa: BLE001
        argv = sys.argv[1:] if argv is None else list(argv)
        out = next((argv[i + 1] for i, x in enumerate(argv) if x == "--out" and i + 1 < len(argv)), "")
        if out:
            try:
                _write(out, [("status", "error"), ("error", type(e).__name__)])
            except OSError:
                pass
        import traceback
        traceback.print_exc()
        return 3


def _main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--rpcs3")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--detect", action="store_true")
    ap.add_argument("--resolve", action="store_true")
    ap.add_argument("--eboot-kind", metavar="PATH")
    ap.add_argument("--sha256", action="store_true", help="with --eboot-kind: also size= and sha256=")
    ap.add_argument("--identify", action="store_true", help="with --eboot-kind: also the E1/H1 verdict")
    ap.add_argument("--title", default="", help="with --identify: the disc's title id (for the region)")
    ap.add_argument("--target", default="1.14", help="with --identify: the EBOOT version the release is built for")
    ap.add_argument("--out", default="")
    a = ap.parse_args(argv)
    if a.detect or a.resolve or a.eboot_kind is not None:
        return wizard_mode(a)
    f = discover(a.rpcs3)
    if a.json:
        json.dump(f, sys.stdout, indent=2)
        print()
    else:
        print("\n".join(describe(f)) or "no RPCS3 folder found")
    return 0 if f["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""RPCS3 import for the first-run wizard: offer the player's installed Killzone 3 disc
game and 1.14 update as the default folders, guide the decryption of the update's EBOOT.BIN with RPCS3's own tool, and
optionally copy the saves. Locating is rpcs3_locate.py (vendored, shared with the other PS3 ports); this file adds no
parsing of RPCS3's files.

Rules: no keys, no decryption code, nothing downloaded; RPCS3's folders are only read (the one write is the save copy,
into the port's own save root, refused if it lies inside an RPCS3 folder); RPCS3's shader and PPU caches are never read.
All player-facing text is in rpcs3_text.py. Errors come back as values, never raised, so the old prompts stay as they were.

    rpcs3_import.py [--rpcs3 DIR] [--json]     print what was found (paths as found)
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rpcs3_text as T  # noqa: E402

TITLE_ID = "BCUS98234"
UPDATE_VER = "01.14"
SAVE_USER = "00000001"       # ASSUMPTION: the port's single local user; RPCS3 uses 00000001 for its first user too


def _locate():
    import rpcs3_locate     # Python 3.11+: older Pythons fall back to the plain prompts
    return rpcs3_locate


def discover(hint=None, locate=None):
    """-> dict: ok, root, disc (folder or ""), update (folder or ""), update_problem (text or ""), saves (list).
    ok False = no RPCS3 folder: the caller keeps its prompts."""
    out = {"ok": False, "root": "", "disc": "", "update": "", "update_problem": "", "saves": []}
    try:
        L = locate or _locate()
        res = L.locate(hint or None, {TITLE_ID: UPDATE_VER})
    except Exception:       # an import or read failure must never stop the wizard
        return out
    if not res.get("ok"):
        return out
    t = res["titles"].get(TITLE_ID, {})
    out.update(ok=True, root=res["root"])
    d = t.get("disc") or {}
    if d.get("ok") and d.get("has_ps3_game"):
        out["disc"] = d["disc_dir"]
    u = t.get("update") or {}
    if u.get("ok"):
        out["update"] = u["path"]
    elif u.get("code") == "wrong_version":
        out["update_problem"] = T.WRONG_UPDATE.format(have=u.get("app_ver"), want=UPDATE_VER)
    for uid, items in ((t.get("saves") or {}).get("users") or {}).items():
        out["saves"] += items
    return out


def describe(found):
    """Lines to show for a discover() result."""
    if not found["ok"]:
        return []
    ls = [T.FOUND.format(root=found["root"])]
    ls.append(T.FOUND_DISC.format(path=found["disc"]) if found["disc"] else T.NO_DISC)
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


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--rpcs3")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    f = discover(a.rpcs3)
    if a.json:
        json.dump(f, sys.stdout, indent=2)
        print()
    else:
        print("\n".join(describe(f)) or "no RPCS3 folder found")
    return 0 if f["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

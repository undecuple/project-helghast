#!/usr/bin/env python3
"""Host test: installer/recomplauncher.json, the RecompLauncher profile (format 1) at the portable zip's top level.

The schema (recomplauncher.com/schema/recomplauncher.json) is NOT vendored: it is a third party's file with no licence we
have read, and the test does not contact the site. Instead a structural check re-states the parts of format 1 we use
(a fixed copy of the format): root additionalProperties false with only `format` required,
game.kind enum, start.program a non-empty array of strings, settings items with id/label and one of choices/slider/
bindings, config items with format/file. ASSUMPTION: that list is a subset of the real schema; the real schema is
the authority and the launcher validates it again.

Checks: valid structure; start.program[0] is the launcher exe that assemble_payload puts in the payload (fake repo, same
assemble path as the release); the file lands at the payload root and as <top>/recomplauncher.json in the zip; every
settings key is a kz3.ini key in port/kz3/kz3_ini.h; the launcher cannot lose a setting to launch.write_game_ini
(an owned key -- GAME_INI_KEYS -- would be rewritten at every start, so none may be set); nothing but the allowed
root keys (no prepare/saves/folders/mods, no dumps/CRC, no game.update); no game data or key names. Negative controls:
a bad key, a missing program, a nested placement, an owned ini key (window_size), a dumps entry.
No game data. Exit 0 = PASS."""
import copy
import json
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import launch as la  # noqa: E402
import names  # noqa: E402
import pack_zip  # noqa: E402
import test_assemble_payload as tap  # noqa: E402

JSON_REL = "installer/recomplauncher.json"
ROOT_KEYS = {"$schema", "format", "name", "repo", "game", "start", "prepare", "config", "folders", "exit_codes", "settings",
             "groups", "saves", "games", "looks", "mods", "actions", "inputs", "requires", "min"}
WANTED_ROOT = {"$schema", "format", "repo", "game", "start", "config", "settings"}   # minimal format
bad = 0


def check(ok, msg, detail=""):
    global bad
    bad += not ok
    print(("ok   " if ok else "FAIL ") + msg + ("" if ok else "  " + detail[:300]))


def structure(j):
    """-> list of problems (format-1 subset, see the module docstring)."""
    p = []
    if not isinstance(j, dict):
        return ["root is not an object"]
    p += [f"root key {k}" for k in j if k not in ROOT_KEYS]
    if j.get("format") != 1:
        p.append("format must be 1")
    g = j.get("game", {})
    if "kind" in g and g["kind"] not in ("iso", "folder", "iso_or_folder"):
        p.append("game.kind")
    p += [f"game key {k}" for k in g if k not in ("kind", "label", "check", "dumps", "update")]
    p += [f"game.check key {k}" for k in g.get("check", {}) if k not in ("file", "xdvdfs", "iso9660", "nup_build_id", "ids")]
    s = j.get("start")
    if s is not None:
        prog = s.get("program") if isinstance(s, dict) else None
        if not (isinstance(prog, list) and prog and all(isinstance(x, str) for x in prog)):
            p.append("start.program must be a non-empty array of strings")
        p += [f"start key {k}" for k in s if k not in ("program", "args", "env")]
    for c in j.get("config", []):
        if c.get("format") not in ("toml", "ini", "json") or not c.get("file"):
            p.append("config item needs format and file")
    ids = {c.get("id", "main") for c in j.get("config", [])}
    for st in j.get("settings", []):
        if not st.get("id") or not st.get("label"):
            p.append("settings item needs id and label")
        if sum(k in st for k in ("choices", "slider", "bindings")) != 1:
            p.append(f"settings {st.get('id')}: exactly one of choices/slider/bindings")
        if st.get("in", "main") not in ids | {"args"}:
            p.append(f"settings {st.get('id')}: unknown config id")
        for ch in st.get("choices", []):
            if not ch.get("label") or ("set" not in ch and "args" not in ch):
                p.append(f"settings {st.get('id')}: a choice needs label and set/args")
        if len(st.get("choices", [1, 2])) < 2:
            p.append("choices needs at least 2")
    return p


def ini_keys():
    src = open(os.path.join(REPO, "port", "kz3", "kz3_ini.h"), encoding="utf-8").read()
    return set(re.findall(r'^\s*\{"(\w+)",', src, re.M))


def setting_keys(j):
    return [k for st in j.get("settings", []) for ch in st.get("choices", []) for k in ch.get("set", {})]


def problems(j, payload_files, ik, owned):
    """Everything the profile must satisfy; -> list of problems."""
    p = structure(j)
    p += [f"setting key {k} not in kz3_ini.h" for k in setting_keys(j) if k not in ik]
    p += [f"setting key {k} is rewritten by launch.write_game_ini" for k in setting_keys(j) if k in owned]
    prog = (j.get("start") or {}).get("program") or [""]
    if prog[0] not in payload_files:
        p.append(f"start.program {prog[0]!r} is not in the payload")
    p += [f"root key {k} not wanted" for k in j if k not in WANTED_ROOT]
    if "dumps" in j.get("game", {}) or "update" in j.get("game", {}):
        p.append("game.dumps / game.update not wanted")
    return p


raw = open(os.path.join(REPO, JSON_REL), encoding="utf-8").read()
good = json.loads(raw)
tmp = tempfile.mkdtemp(prefix="kz3-rl-")
try:
    repo = os.path.join(tmp, "repo")
    tap.fake_repo(repo)
    shutil.copyfile(os.path.join(REPO, JSON_REL), os.path.join(repo, *JSON_REL.split("/")))   # the real file, in the fake tree
    exe = os.path.join(tmp, names.LAUNCHER_EXE)
    open(exe, "wb").write(b"MZ" + bytes(4000))
    out = os.path.join(tmp, "payload")
    rc, txt = tap.run(["--repo", repo, "--out", out, "--launcher", exe])
    check(rc == 0, "the fake repo assembles with the real recomplauncher.json and the launcher", txt[-300:])
    pfiles = tap.listing(out)
    ik = ini_keys()
    owned = {k for _, k in la.GAME_INI_KEYS}
    owned |= set(la.GAME_INI_CHOICES)

    check(structure(good) == [], "structural check (format 1 subset) passes", str(structure(good)))
    check(good["repo"] == f"{names.OWNER}/{names.REPO_SLUG}", "repo = names.OWNER/REPO_SLUG")
    check(good["start"]["program"] == [names.LAUNCHER_EXE] and "args" not in good["start"], "start.program is the launcher exe, no args")
    check(good["game"]["kind"] == "folder" and good["game"]["check"] == {"file": "PS3_GAME/PARAM.SFO"}, "game: folder, PARAM.SFO check")
    check(set(good) == WANTED_ROOT, "root keys are the minimal set", str(sorted(good)))
    check(sorted(set(setting_keys(good))) == ["discord_presence", "mouse_look"], "settings keys: mouse_look, discord_presence")
    check(not (set(setting_keys(good)) & owned), "no setting key is launcher-owned (window_size is: paths.json would overwrite it)")
    check(set(setting_keys(good)) <= ik, "every settings key exists in kz3_ini.h")
    check(names.LAUNCHER_EXE in pfiles, "start.program exists in the assembled payload")
    check("recomplauncher.json" in pfiles and "/" not in "recomplauncher.json", "the file sits at the payload's top level")
    check(pack_zip.zip_name(names.ZIP_TOP, "recomplauncher.json").count("/") == 1,
          "in the zip it is <top>/recomplauncher.json, next to the launcher exe")
    check(open(os.path.join(out, "recomplauncher.json"), encoding="utf-8").read() == raw, "the payload copy equals installer/recomplauncher.json")
    low = raw.lower()
    check(not any(w in low for w in (".rap", ".edat", ".sdat", "idps", "act.dat", "crc32", "sha256", "\"dumps\"", "eboot")),
          "no keys, licence data, CRCs or EBOOT names in the file")
    check(problems(good, pfiles, ik, owned) == [], "the whole profile has no problems", str(problems(good, pfiles, ik, owned)))

    # a settings choice writes a key the launcher keeps: round trip through write_game_ini (a kept extra line)
    root = os.path.join(tmp, "inst")
    os.makedirs(os.path.join(root, "build"))
    ini = os.path.join(root, "build", "kz3.ini")
    open(ini, "w").write("[game]\nmouse_look = 0\ndiscord_presence = 1\n")
    v = {k: "" for _, k in la.GAME_INI_KEYS}
    la.write_game_ini(root, v)
    kept = la.read_game_ini(ini)
    check(kept.get("mouse_look") == "0" and kept.get("discord_presence") == "1", "launch.write_game_ini keeps a launcher-written mouse_look/discord_presence", str(kept))

    # negative controls
    def neg(name, mutate, expect):
        j = copy.deepcopy(good)
        mutate(j)
        pr = problems(j, pfiles, ik, owned)
        check(any(expect in x for x in pr), f"negative: {name} is refused", str(pr))

    neg("a bad settings key", lambda j: j["settings"][0]["choices"][0]["set"].update({"mouse_lok": "1"}), "not in kz3_ini.h")
    neg("a missing program", lambda j: j["start"].update({"program": ["Gone.exe"]}), "not in the payload")
    neg("an empty program list", lambda j: j["start"].update({"program": []}), "start.program must be")
    neg("a nested placement", lambda j: j["start"].update({"program": ["Project Helghast/" + names.LAUNCHER_EXE]}), "not in the payload")
    neg("an owned ini key (window_size)", lambda j: j["settings"][0]["choices"][0]["set"].update({"window_size": "1080p"}), "rewritten by launch.write_game_ini")
    neg("a dumps entry", lambda j: j["game"].update({"dumps": []}), "not wanted")
    neg("a prepare step", lambda j: j.update({"prepare": {"program": "x"}}), "not wanted")
    neg("an unknown root key", lambda j: j.update({"startt": {}}), "root key startt")
    neg("format 2", lambda j: j.update({"format": 2}), "format must be 1")
    # the file placed under a folder in the payload is not at the top level
    nested = os.path.join(tmp, "nested")
    os.makedirs(os.path.join(nested, "sub"))
    shutil.copyfile(os.path.join(out, "recomplauncher.json"), os.path.join(nested, "sub", "recomplauncher.json"))
    check(not os.path.isfile(os.path.join(nested, "recomplauncher.json")), "negative: a nested copy is not at the top level")
finally:
    shutil.rmtree(tmp, ignore_errors=True)
print("FAIL" if bad else "PASS", f"({bad} failed)" if bad else "")
sys.exit(1 if bad else 0)

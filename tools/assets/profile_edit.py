#!/usr/bin/env python3
"""Read and edit a Killzone 3 PROFILE.DAT copy (RE pass 66, gameplay plan B5).

PROFILE.DAT is the raw PlayerProfile object, runtime bytes [0x30, 0x11F18): 73,448 bytes, big-endian, no checksum
(validator 0x4D1FF0 checks only the size and the first word; PARAM.PFD is not checked by the HLE). File offset =
runtime offset - 0x30. Layout: the RE notes "PROFILE.DAT layout" and "Checkpoint save chain".

  profile_edit.py show SRC [--nodes]
  profile_edit.py edit SRC --out work/.../PROFILE.DAT [--unlock-through T] [--sixaxis 0|1] [--clear-continue]
  profile_edit.py stage SRC --template RUN --out work/.../RUNDIR
      copies RUN/app/gamedata (a port run's savedata tree, e.g. work/proton/coord-firstboot) to RUNDIR/app/gamedata
      and puts SRC's PROFILE.DAT in place of the template's, so `tools/level-run.sh --save-from RUNDIR` boots with it
      (e.g. a real save decrypted by pfd_decrypt.py; the US savedata directory name is kept whatever SRC's region).

SRC is a PROFILE.DAT or a run directory (work/proton/<name>): the file is searched below it. Writes go only to a
path with a `work` directory component and never to SRC itself. Nothing here needs game data: the layout is numbers.
"""
import argparse
import csv
import shutil
import os
import struct
import sys
from pathlib import Path

SIZE = 0x11EE8                 # bytes copied by the saver (0x4C8DA4) and checked by the validator (0x4D1FF0)
VERSION_WORD = 0x127           # first word of every real save seen; ASSUMPTION: class version from 0x8F380
RT2FILE = -0x30                # file offset = runtime offset + RT2FILE
# Continue/checkpoint record (runtime offsets in comments; writer 0x4D2170, reader CanContinueGame 0x4CE6F0)
CONT_LEVEL = 0x119D8 + RT2FILE   # s32 level id, -1 = none
CONT_DIFF = 0x119DC + RT2FILE    # s8 from [0x13A6D7C]+0x1E0, -1 = none; NOT the difficulty (5 on a Trooper and a Recruit save, pass 97)
CONT_B1 = 0x119DD + RT2FILE      # u8 from [0x13A6D7C]+0x1B54 (not decoded)
CONT_SECTION = 0x119DE + RT2FILE # u8: 1 = "start of level" save (level advance), 0 = in-level checkpoint
CONT_DONE = 0x119DF + RT2FILE    # u8: must be 0 for Continue; 1 after the campaign's last level
CONT_E0 = 0x119E0 + RT2FILE      # u8: set to 1 with CONT_DONE (campaign finished flag, ASSUMPTION)
CONT_SIZE = 0x11A10 + RT2FILE    # u32 = 0x84 when a record is present (loader 0x4CE5A0 checks it)
CONT_REC = 0x11A14 + RT2FILE     # 0x84-byte record built by 0x4CC448
CONT_REC_LEN = 0x84
SIXAXIS = 0xB6 + RT2FILE         # CanUseSixaxisMotion (1.14 "Motion Sensor Function" option; default 1)
SLOT0 = 0xF44 + RT2FILE          # level slot h at SLOT0 + h*SLOT_STRIDE (h = LevelHistoryID)
SLOT_STRIDE = 0x904
MODE_STRIDE = 0x480              # 0 campaign, 1 coop
NODE_STRIDE = 0x24               # node record: +0 u32 difficulty bits, +4 least deaths, +0x10 best time
NSLOTS = 20
NNODES = 32
DIFF_NAMES = ["Recruit", "Trooper", "Veteran", "Elite"]
LEVELS_TSV = Path(__file__).resolve().parents[2] / "re" / "data" / "campaign_levels.tsv"


def load_levels(path=LEVELS_TSV):
    """Chapter-1 rows of campaign_levels.tsv: index0 -> (name, level_history_id, start_node_new_game)."""
    out = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f, delimiter="\t"):
            try:
                if int(row["chapter"]) != 1:
                    continue  # chapter 2 (Corinth River) reuses index0 = 0
                out[int(row["index0"])] = (row["name_id"], int(row["level_history_id"]),
                                           int(row["start_node_new_game(node[1])"]))
            except (KeyError, ValueError):
                continue
    return out


def find_profile(src):
    p = Path(src)
    if p.is_file():
        return p
    if p.is_dir():
        hits = sorted(q for q in p.rglob("PROFILE.DAT") if q.is_file())
        if len(hits) == 1:
            return hits[0]
        raise SystemExit(f"{src}: {len(hits)} PROFILE.DAT below it (need exactly one)")
    raise SystemExit(f"{src}: not a file or directory")


def load(src):
    p = find_profile(src)
    data = bytearray(p.read_bytes())
    if len(data) != SIZE:
        raise SystemExit(f"{p}: size {len(data)} != {SIZE} (not a PROFILE.DAT; the game would reject it too)")
    ver = struct.unpack_from(">I", data, 0)[0]
    if ver != VERSION_WORD:
        print(f"warning: first word {ver:#x} != {VERSION_WORD:#x} (unknown profile version)", file=sys.stderr)
    return p, data


def node_off(h, node, mode=0):
    return SLOT0 + h * SLOT_STRIDE + mode * MODE_STRIDE + (node - 1) * NODE_STRIDE


def continue_info(data):
    lvl, = struct.unpack_from(">i", data, CONT_LEVEL)
    diff, = struct.unpack_from(">b", data, CONT_DIFF)
    rec_size, = struct.unpack_from(">I", data, CONT_SIZE)
    return {
        "level_id": lvl, "diff": diff, "b1": data[CONT_B1], "section": data[CONT_SECTION],
        "done": data[CONT_DONE], "e0": data[CONT_E0], "rec_size": rec_size,
        "rec": bytes(data[CONT_REC:CONT_REC + CONT_REC_LEN]),
        "can_continue": lvl != -1 and diff != -1 and data[CONT_DONE] == 0,
    }


def show(data, nodes=False):
    ci = continue_info(data)
    print(f"version word  {struct.unpack_from('>I', data, 0)[0]:#x}")
    print(f"sixaxis byte  file {SIXAXIS:#x} = {data[SIXAXIS]} (1 = Motion Sensor on)")
    print(f"continue      level_id={ci['level_id']} diff={ci['diff']} b1={ci['b1']} section={ci['section']} "
          f"done={ci['done']} e0={ci['e0']} rec_size={ci['rec_size']:#x} can_continue={ci['can_continue']}")
    if ci["rec_size"] == CONT_REC_LEN:
        print(f"  record[{CONT_REC:#x}] " + ci["rec"][:32].hex() + " ...")
    levels = {v[1]: (k, v[0]) for k, v in load_levels().items()}
    for h in range(NSLOTS):
        done = []
        for mode in range(2):
            for node in range(1, NNODES + 1):
                bits, = struct.unpack_from(">I", data, node_off(h, node, mode))
                if bits:
                    done.append((mode, node, bits))
        if done or nodes:
            name = levels.get(h, ("?", "?"))[1]
            txt = " ".join(f"{'coop' if m else 'sp'}:n{n}={b:#x}" for m, n, b in done) or "-"
            print(f"slot {h:2d} {name:<14} {txt}")


def edit(data, unlock_through=None, sixaxis=None, clear_continue=False):
    changes = []
    if unlock_through is not None:
        levels = load_levels()
        if unlock_through not in levels:
            raise SystemExit(f"--unlock-through {unlock_through}: not a chapter-1 index in {LEVELS_TSV.name}")
        # "Exact edit" (the RE notes): completing the first node of every level before T opens the lobby and
        # the Select Mission entries up to T; T = 0 marks mission_0's own first node to open the lobby.
        for j in ([0] if unlock_through == 0 else range(unlock_through)):
            name, h, first = levels[j]
            off = node_off(h, first)
            struct.pack_into(">I", data, off, struct.unpack_from(">I", data, off)[0] | 1)
            changes.append(f"slot {h} ({name}) node {first} bit0 at file {off:#x}")
    if sixaxis is not None:
        data[SIXAXIS] = sixaxis
        changes.append(f"sixaxis byte {SIXAXIS:#x} = {sixaxis}")
    if clear_continue:
        struct.pack_into(">i", data, CONT_LEVEL, -1)
        struct.pack_into(">b", data, CONT_DIFF, -1)
        data[CONT_B1] = data[CONT_SECTION] = data[CONT_DONE] = data[CONT_E0] = 0
        struct.pack_into(">I", data, CONT_SIZE, 0)
        data[CONT_REC:CONT_REC + CONT_REC_LEN] = bytes(CONT_REC_LEN)
        changes.append("continue record cleared")
    return changes


def check_out(out, src):
    o = Path(out).resolve()
    # The repo's own work/ (KZ3_WORK_DIR for tests), not any path with a "work" part: CI's temp dir is
    # under the runner's work/_temp, so the old part check let "outside" writes through there (2026-10-11).
    root = Path(os.environ.get("KZ3_WORK_DIR") or Path(__file__).resolve().parents[2] / "work").resolve()
    if not o.is_relative_to(root):
        raise SystemExit(f"{out}: output must be under {root} (never the game's or a run's original)")
    if o == Path(src).resolve():
        raise SystemExit(f"{out}: refusing to write the source in place")
    return o


def stage(src, data, template, out):
    tsrc = Path(template) / "app" / "gamedata"
    tprof = sorted(tsrc.rglob("PROFILE.DAT")) if tsrc.is_dir() else []
    if len(tprof) != 1:
        raise SystemExit(f"{template}: need exactly one app/gamedata/**/PROFILE.DAT, found {len(tprof)}")
    o = check_out(out, src)
    if o.exists():
        raise SystemExit(f"{out}: exists; pick a new run dir")
    shutil.copytree(tsrc, o / "app" / "gamedata")
    dst = o / "app" / "gamedata" / tprof[0].relative_to(tsrc)
    dst.write_bytes(bytes(data))
    print(f"staged {src} -> {dst}")
    print(f"use: tools/level-run.sh NAME --save-from {o}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("show", help="print the continue record, sixaxis byte and non-empty level slots")
    s.add_argument("src")
    s.add_argument("--nodes", action="store_true", help="list every slot, even empty ones")
    e = sub.add_parser("edit", help="write an edited copy")
    e.add_argument("src")
    e.add_argument("--out", required=True)
    e.add_argument("--unlock-through", type=int, metavar="T", help="chapter-1 index 0..8 (campaign_levels.tsv)")
    e.add_argument("--sixaxis", type=int, choices=[0, 1], help="Motion Sensor byte (file 0x86)")
    e.add_argument("--clear-continue", action="store_true")
    g = sub.add_parser("stage", help="make a --save-from run dir: template savedata tree + this PROFILE.DAT")
    g.add_argument("src")
    g.add_argument("--template", required=True, help="run dir with app/gamedata (port-written savedata)")
    g.add_argument("--out", required=True, help="new run dir under work/ (must not exist)")
    a = ap.parse_args(argv)
    src, data = load(a.src)
    if a.cmd == "show":
        print(f"file          {src}")
        show(data, a.nodes)
        return 0
    if a.cmd == "stage":
        return stage(src, data, a.template, a.out)
    out = check_out(a.out, src)
    changes = edit(data, a.unlock_through, a.sixaxis, a.clear_continue)
    if not changes:
        raise SystemExit("edit: nothing to change (give --unlock-through, --sixaxis or --clear-continue)")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(data))
    for c in changes:
        print("edit:", c)
    print(f"wrote {out} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

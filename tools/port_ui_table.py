#!/usr/bin/env python3
"""Port UI translation table: port/text/port_ui.tsv (the RE notes 173.5).

  port_ui_table.py                 check the committed table (host test; no game data)
  port_ui_table.py build SRC_DIR   rebuild it from SRC_DIR/port-ui-keys.tsv, port-ui-en.txt and
                                   port-ui-<col>.txt (one line per key, in the keys file's order)

Columns follow the game's language ids (1 en, 4 es, 20 es_la, 21 pt_br, 3 fr, 5 de, 6 it, 7 nl, 8 pt,
10 zh_tw, 11 ko, 12 ru, 13 pl, 14 da, 15 fi, 16 no, 17 sv, 18 ja, 22 tr). A cell may be empty (the
lookup falls back to en) or `@game:<InGameID>` (the game's own word). The check refuses tabs and raw
newlines in cells, placeholder sets ({0}..{2}) that differ from en, characters the game font lacks
for Latin-script columns (curly quotes, dashes, NBSP, ellipsis), and reports cells over max_chars.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = ROOT / "port" / "text" / "port_ui.tsv"
COLS = ["en", "es", "es_la", "pt_br", "fr", "de", "it", "nl", "pt", "zh_tw",
        "ko", "ru", "pl", "da", "fi", "no", "sv", "ja", "tr"]
LANG_ID = {"en": 1, "es": 4, "es_la": 20, "pt_br": 21, "fr": 3, "de": 5, "it": 6, "nl": 7, "pt": 8,
           "zh_tw": 10, "ko": 11, "ru": 12, "pl": 13, "da": 14, "fi": 15, "no": 16, "sv": 17,
           "ja": 18, "tr": 22}
# Not in the Fishmonger font at all (173.4); the build turns them into ASCII.
NORM = {"“": '"', "”": '"', "„": '"', "‘": "'", "’": "'",
        "–": "-", "—": "-", "…": "...", " ": " ", " ": " "}
# Pixel budget (task 2026-11-02, tester report: ru labels overlapped their values).
# ASSUMPTION: no game data was mounted, so these are not measured from the Fishmonger/cellFont advances.
# They come from one 1341x720 screenshot: the select label column is ~275 px and a 20-char Cyrillic
# label just touches the value, i.e. ~13.75 px per Cyrillic capital (the game upper-cases labels);
# max_chars 28 is read as 28 average Latin capitals = 275 px, so ~9.8 px per Latin char. CJK cells are
# full-width (~20 px, ASSUMPTION: cellFont outlines at the same height). Re-measure from the font
# (the RE notes s.22: FishmongerCR 'A' 536 / em 727) when data is mounted.
LATIN_PX = 275 / 28
CHAR_PX = {"ru": 13.75, "ja": 20.0, "ko": 20.0, "zh_tw": 20.0}
LABEL_MAXC = 28          # width-checked (failing) rows: the select label column
PENDING = Path(__file__).resolve().parent / "port_ui_width_pending.txt"
PH = re.compile(r"\{\d\}")


def build(src):
    src = Path(src)
    keys = [l.split("\t") for l in (src / "port-ui-keys.tsv").read_text(encoding="utf-8").splitlines()[1:]]
    cols = {}
    for c in COLS:
        lines = (src / f"port-ui-{c}.txt").read_text(encoding="utf-8").splitlines()
        if len(lines) != len(keys):
            sys.exit(f"{c}: {len(lines)} lines, keys file has {len(keys)}")
        cols[c] = lines
    out = ["key\tmax_chars\t" + "\t".join(COLS)]
    for i, k in enumerate(keys):
        cells = []
        for c in COLS:
            s = cols[c][i]
            for a, b in NORM.items():
                s = s.replace(a, b)
            cells.append(s)
        out.append(f"{k[1]}\t{k[3]}\t" + "\t".join(cells))
    TABLE.parent.mkdir(parents=True, exist_ok=True)
    TABLE.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"wrote {TABLE.relative_to(ROOT)}: {len(keys)} keys x {len(COLS)} languages")


def check():
    errs, long_, wide, wide_warn = [], [], [], []
    pending = set()
    if PENDING.exists():
        pending = {l.split("#")[0].strip() for l in PENDING.read_text(encoding="utf-8").splitlines()
                   if l.split("#")[0].strip()}
    hit = set()
    rows = TABLE.read_text(encoding="utf-8").splitlines()
    head = rows[0].split("\t")
    if head != ["key", "max_chars"] + COLS:
        errs.append(f"header {head}")
    seen = set()
    for n, row in enumerate(rows[1:], 2):
        cells = row.split("\t")
        if len(cells) != len(head):
            errs.append(f"line {n}: {len(cells)} cells, want {len(head)}")
            continue
        key, maxc, vals = cells[0], int(cells[1]), dict(zip(COLS, cells[2:]))
        if key in seen:
            errs.append(f"line {n}: duplicate key {key}")
        seen.add(key)
        want = sorted(PH.findall(vals["en"]))
        if not vals["en"]:
            errs.append(f"{key}: empty en")
        for c, s in vals.items():
            if not s or s.startswith("@game:"):
                continue
            if sorted(PH.findall(s)) != want:
                errs.append(f"{key} [{c}]: placeholders {PH.findall(s)} vs en {want}")
            # printf templates (menu.version_newer): only %s, and as many as en has
            if s.replace("%s", "").count("%") or s.count("%s") != vals["en"].count("%s"):
                errs.append(f"{key} [{c}]: % use {s.count('%s')}x %s vs en {vals['en'].count('%s')}, other % not allowed")
            bad = sorted({ch for ch in s if ch in NORM})
            if bad:
                errs.append(f"{key} [{c}]: characters the game font lacks: {bad!r}")
            if "\r" in s:
                errs.append(f"{key} [{c}]: carriage return")
            px = len(s) * CHAR_PX.get(c, LATIN_PX)
            if px > maxc * LATIN_PX + 0.01:
                if maxc == LABEL_MAXC:
                    tag = f"{key}:{c}"
                    hit.add(tag)
                    (wide_warn if tag in pending else wide).append(
                        f"{key} [{c}] ~{px:.0f}px > {maxc * LATIN_PX:.0f}px ({len(s)} chars)")
                else:
                    wide_warn.append(f"{key} [{c}] ~{px:.0f}px > {maxc * LATIN_PX:.0f}px (not a checked row)")
            if len(s) > maxc:
                long_.append(f"{key} [{c}] {len(s)}/{maxc}")
    errs += [f"over pixel budget: {w}" for w in wide]
    errs += [f"width_pending entry no longer over budget (remove it): {t}" for t in sorted(pending - hit)]
    print(f"{TABLE.relative_to(ROOT)}: {len(rows) - 1} keys x {len(COLS)} languages, "
          f"{len(wide_warn)} width warnings, {len(errs)} errors, {len(long_)} cells over max_chars")
    for e in errs:
        print("ERROR", e)
    if "-v" in sys.argv:
        for w in wide_warn:
            print("WIDE", w)
        for l in long_:
            print("LONG", l)
    print("RESULT:", "FAIL" if errs else "PASS")
    return 1 if errs else 0


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "build":
        build(sys.argv[2])
    sys.exit(check())

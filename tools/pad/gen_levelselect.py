#!/usr/bin/env python3
"""Pad script (PAD_SCRIPT v2 text, appended after tools/pad/menu-fast.pad by tools/level-run.sh --pad) that takes a
FINISHED profile (the maintainer's saves, RE pass 97) from the campaign lobby to a chosen campaign level.
  python3 tools/pad/gen_levelselect.py K [--node N] [--gap G] [--start S] > FILE      K = 0..8 (200 mission_0 .. 208 Space_Station)
  --gap G (default 3 s) and --start S (default 46 s): spacing between presses and the first press. Under a shared GPU the
  menu pages open late and a 3 s press can land on the previous page; then every later press is one page off and the
  run starts mission_0 (maintainer 2026-10-13, runs lvlr-K1 and lvl-K8). Use --gap 6 --start 52 there.
Found on saves-explore (2026-10-12): menu-fast's CROSS at 39/40 opens campaignlobby.gum (run clock 41.3 s);
its default focus is the DISABLED "Continue Campaign" item (campaign finished), so the 42/43 CROSS retries do nothing.
DOWN = Select Chapter, DOWN again = New Campaign (a 40-poll DOWN moves exactly one item). Select Chapter -> the level
list (9 entries, all unlocked), DOWN x K, CROSS -> node page (first menu node focused; --node N adds N DOWNs), CROSS ->
difficulty (default highlighted), CROSS -> level load. Pages open ~1.3 s after a press; 3 s spacing, no DOWN retries
(a retry would move one item further); the last CROSS gets a retry (extra CROSS on the loading screen does nothing).

Why it needs a FINISHED profile (RE pass 124): menu-fast.pad presses CROSS at 36/37/39/40/42/43 s, which lands on the
lobby (open from ~34-35 s). If the profile's Continue is ENABLED those presses take it and the run loads the saved level
(r52: saves-explore has a Continue record level 200, done byte 0 -> mission_0 at 38 s, on 1.00 and 1.14 alike); the later
presses then hit the loading screen. Only a profile with the done byte 1 (owner's run-us: record level 208, done 1)
keeps Continue disabled so those presses do nothing. --profile DIR|PROFILE.DAT checks this (exit 3 if Continue would be
enabled). The gate (stderr) is the first `LEVEL LOAD levels.single_player.<dir>.leveldescription_optimized.core` line
of the run log (tools/boot_timeline.py prints it).
"""
import sys, os
LEVELS = ["mission_0", "flooded_pyrrhus", "pyrrhus_outskirts", "jungle", "frozen_shores",
          "stahl_arms_research", "scrapyard", "escape_from_helghan", "space_station"]
k = int(sys.argv[1])
def opt(name, default):
    return int(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default
prof = sys.argv[sys.argv.index("--profile") + 1] if "--profile" in sys.argv else None
if prof:
    if os.path.isdir(prof):
        import glob
        c = glob.glob(os.path.join(prof, "**/PROFILE.DAT"), recursive=True)
        prof = c[0] if c else prof
    b = open(prof, "rb").read()
    lvl, done = b[0x119AB], b[0x119AF]  # ASSUMPTION: offsets from the two profiles compared in pass 124
    print(f"# profile {prof}: continue record level {lvl}, done {done}: Continue {'DISABLED (ok)' if done else 'ENABLED'}", file=sys.stderr)
    if not done:
        print("ERROR: Continue is enabled; menu-fast's lobby CROSS would load the saved level, not the chosen one. Use the finished run-us profile.", file=sys.stderr); sys.exit(3)
node = opt("--node", 0); gap = opt("--gap", 3)
t = opt("--start", 46); out = []
def ev(mask, c): 
    global t; out.append(f"{t}:{mask}      # {c}"); t += gap
ev("0x0040", "lobby: DOWN from the disabled Continue to Select Chapter")
ev("0x4000", "Select Chapter -> campaignlevelselect.gum")
for i in range(k): ev("0x0040", f"level list: DOWN {i+1}/{k}")
ev("0x4000", f"pick level index {k} (id {200+k}) -> campaignmissionselect.gum")
for i in range(node): ev("0x0040", f"node list: DOWN {i+1}/{node}")
ev("0x4000", "first node -> campaigndifficulty.gum")
ev("0x4000", "difficulty: the highlighted default -> level load (loading cinematic ~73-86 s)")
out.append(f"{t - gap + 1}:0x4000      # retry")
print(f"GATE K{k}: LEVEL LOAD levels.single_player.{LEVELS[k]}.leveldescription_optimized.core (first LEVEL LOAD line; id {200+k})", file=sys.stderr)
print(f"# level index {k} (id {200+k}), node offset {node}; expected level load at ~{t - gap + 2} s: use level-run.sh --load {t - gap + 2}")
print("\n".join(out))

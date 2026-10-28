#!/usr/bin/env python3
"""Focused test for ps3recomp patch 3001 (cellSaveDataUserAutoSave keeps its ninth argument), the upstream fix list.

cellSaveDataUserAutoSave takes nine arguments (version, userId, dirName, errDialog, setBuf, funcStat, funcFile,
container, userdata); the ninth lives in the caller's parameter save area (SP+112), which the generic r3..r10
registration never sees. ps3recomp v0.15.0 absorbed the fix in its own form (PR #276: the context handler calls
cellSaveDataAutoSave directly, so 3001 was dropped) with its own test, tools/test_savedata_user_autosave.py
(libs/system/tests/test_savedata_user_autosave.c: registers the context handlers, calls UserAutoSave/UserAutoLoad with
userdata at SP+112 and checks what the stat callback receives). Until v0.15.0 this file extracted 3001's handler; it now
runs upstream's test in the patched tree (default work/ps3recomp-patched; NEW_TREE overrides), so a later patch that
breaks the ninth argument fails here. Needs a C compiler (cc). Prints one PASS/FAIL line."""
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import _extract as X

_, new_t = X.trees()
t = os.path.join(new_t, "tools/test_savedata_user_autosave.py")
if not os.path.exists(t):
    sys.exit(f"FAIL: 3001 UserAutoSave ninth argument (no upstream test at {t})")
r = subprocess.run([sys.executable, t], cwd=new_t, capture_output=True, text=True)
tail = (r.stdout + r.stderr).strip().splitlines()[-3:]
print("\n".join(tail))
ok = r.returncode == 0
print(f"{'PASS' if ok else 'FAIL'}: 3001 UserAutoSave ninth argument, absorbed upstream (PR #276, v0.15.0): "
      f"upstream's test on the tree under test exit {r.returncode}")
sys.exit(0 if ok else 1)

#!/usr/bin/env bash
# Focused test for patch 5002 (writable EDAT cache), the 1.14 VFS overlay (port/vfs/kz3_overlay.c) and its self key
# (pass 43: the klicensee at guest VA 0xE7908B, dev_hash-checked; a wrong key and KZ3_OVERLAY_SELFKEY=0 are rejected).
#   nix develop .#re --command tools/sandbox.sh -- bash port/vfs/test_vfs_overlay.sh
# Needs the installed 1.14 update (GameUpdate/USRDIR), the disc psarc and work/bin/EBOOT-114.elf. All output stays in
# work/upstream-tests/ (git-ignored, never committed). SWEEP=0 skips the all-patches sweep (about a minute).
# The C side resolves a few paths from each layer; tools/psarc.py + tools/assets/pspatch.py give the reference.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
O="$ROOT/work/upstream-tests/vfs-overlay"
EDAT="${EDAT:-$ROOT/GameUpdate/USRDIR/added_data_ps3.psarc.edat}"
ELF="${ELF:-$ROOT/work/bin/EBOOT-114.elf}"
DISC="${DISC:-$ROOT/Game/PS3_GAME/USRDIR/data_ps3.psarc}"
PDIR="${PDIR:-$ROOT/GameUpdate/USRDIR/patch_data_ps3_mp}"
KEYS=(
  "Levels/Multi_Player/MP_09/Scripts/MainFlow.lua"                    # added (also on the disc: added wins)
  "levels/multi_player/mp_13/scripts"                                  # directory only in added_data
  "interface/menu/gum/multiplayer/includes/squadright.gum"             # patched .gum
  "LocalCachePS3/lumps/assets_description.menu.basemenu_styles.core"   # patched .core
  "ICONS/SAVEDATA/SAVEICON.PNG"                                        # untouched disc file
  "LocalCachePS3/lumps/assets_description.defaultshared_defaultshared_assets.core"   # patched in the MP set only (s.36.12)
)

rm -rf "$O"; mkdir -p "$O"
T="$O/tree"; mkdir -p "$T"
tar -C "$ROOT/third_party/ps3recomp" --exclude=.git -cf - libs | tar -C "$T" -xmf -
for p in "$ROOT"/port/patches/500[12]-*.patch; do
  (cd "$T" && patch -p1 -s < "$p") || { echo "FAIL: $(basename "$p") does not apply"; exit 1; }
done
gcc -std=gnu17 -O2 -Wall -Wextra -Werror -c "$ROOT/port/vfs/kz3_overlay.c" -o "$O/kz3_overlay.o" || { echo "FAIL: overlay build"; exit 1; }
gcc -std=gnu17 -O2 -w -I "$T/libs/filesystem" -I "$ROOT/port/vfs" -o "$O/t" "$ROOT/port/vfs/test_vfs_overlay.c" \
    "$O/kz3_overlay.o" -lz -lpthread || { echo "FAIL: test build"; exit 1; }
export PS3_EDAT_CACHE="$O/edat-cache"
"$O/t" "$EDAT" "$ELF" "$DISC" "$PDIR" "$O/cache" "${KEYS[@]}" > "$O/c.txt" 2> "$O/c.err"
RC=$?
grep -v "^\[" "$O/c.txt" | cut -c1-160
[[ $RC == 0 ]] || { grep -m5 "overlay\|FAIL" "$O/c.err"; echo FAIL; exit 1; }

# Reference (independent: Python psarc reader, pspatch.py, hashlib) and comparison.
python3 - "$ROOT" "$DISC" "$PDIR" "$PS3_EDAT_CACHE" "$O/c.txt" <<'EOF'
import sys, os, glob, hashlib
root, disc, pdir, ecache, ctxt = sys.argv[1:]
sys.path[:0] = [os.path.join(root, 'tools'), os.path.join(root, 'tools/assets')]
from psarc import Psarc
import pspatch
dec = glob.glob(os.path.join(ecache, '*', 'added_data_ps3.psarc.edat.dec'))[0]
A, D = Psarc(dec), Psarc(disc)
def index(p): return {n.lstrip('/').lower(): i for i, n in reversed(list(enumerate(p.names))) if i}
ai, di = index(A), index(D)
adirs = {k.rsplit('/', 1)[0] for k in ai} | {'/'.join(k.split('/')[:j]) for k in ai for j in range(1, k.count('/') + 1)}
bad = 0
for line in open(ctxt):
    f = line.rstrip('\n').split('\t')
    if len(f) != 5: continue
    key, layer, size, md5 = f[0], f[1], f[2], f[3]
    k = key.lower()
    pname = hashlib.md5(('/data_ps3/' + k).upper().encode()).hexdigest().upper()
    ppath = os.path.join(pdir, pname + '.pspatch')
    if k in ai:
        want = 'added'; data = A.read(ai[k])
    elif k in adirs and k not in di:
        want = 'dir'; data = None
    elif k in di and os.path.exists(ppath):
        want = 'patched'; base = D.read(di[k]); data = pspatch.apply(open(ppath, 'rb').read(), base)
    else:
        want = 'disc'; data = None
    ok = layer == want
    note = ''
    if data is not None:
        ok = ok and int(size) == len(data) and md5 == hashlib.md5(data).hexdigest()
        note = '%d bytes md5 %s' % (len(data), hashlib.md5(data).hexdigest()[:12])
        if want == 'patched': note += ', base %d bytes' % len(base)
        if k.endswith('.core'): note += ', RTTIBin<1.73>' if data.startswith(b'RTTIBin<1.73>') else ', NOT RTTIBin'; ok = ok and data.startswith(b'RTTIBin<1.73>')
    print('%-4s %-8s %s %s' % ('ok' if ok else 'BAD', want, key, note))
    bad += not ok
print('PASS' if bad == 0 else 'FAIL (%d)' % bad)
sys.exit(1 if bad else 0)
EOF

#!/usr/bin/env bash
# Static guard (patch 2206): port/kz3/main.cpp cannot be compiled on the host, so the three places where the multiplayer lock of the
# Graphics page is wired are checked as text, with negative controls (a scratch copy of main.cpp with the wiring broken must FAIL):
#   kz3_bloom_body    the Bloom consumer reads rsx_gfx_effective_bloom(<run's bloom>, kz3_game_type()) inside the _WIN32 branch,
#   kz3_gfx_bridge    rsx_gfx_cmd(&st, a, kz3_game_type(), reply, cap) answers the page, saved values written back only on a change,
#   kz3_kb_miss_hook  serves graphicssettings_mp.gum (generated with rsx_gfx_page_gum_l(..., glk, ...), glk from rsx_gfx_locked(kz3_game_type())),
#   kz3_kb_set        routes both "gfx" and the older "bloom" commands to kz3_gfx_bridge.
# No game data, no GPU, no compiler needed.      tools/smoke/test_gfx_callsites.sh
set -euo pipefail
D="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 -I - "$D/port/kz3/main.cpp" <<'PY'
import re, sys

def body(src, sig):
    i = src.find(sig)
    if i < 0:
        return None
    j = src.index("{", i)
    depth, k = 0, j
    while k < len(src):
        c = src[k]
        if c == "{": depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0: return src[j:k + 1]
        k += 1
    return None

def checks(src):
    """list of (name, ok)"""
    r = []
    bb = body(src, "static void kz3_bloom_body(")
    win = None
    if bb:
        m = re.search(r"#ifdef _WIN32\n(.*?)#else", bb, re.S)
        win = m.group(1) if m else None
    r.append(("bloom-body-lock", bool(win) and re.search(
        r"rsx_gfx_effective_bloom\(\s*kz3_seq_now\(&s_bloom_seq,\s*\(int\)s_bloom_eff\),\s*kz3_game_type\(\)\s*\)", win) is not None))
    br = body(src, "static void kz3_gfx_bridge(")
    r.append(("bridge-lock", bool(br) and "rsx_gfx_cmd(&st, a, kz3_game_type(), reply, cap)" in br))
    r.append(("bridge-saved-values", bool(br) and "st.v.v[0] = s_bloom_saved;" in br and "s_bloom_saved = st.v.v[0];" in br
              and "kz3_display_save_extra();" in br))
    r.append(("bridge-win32", re.search(r"#ifdef _WIN32\n/\* Patch 2206: \"kb:gfx[^\n]*\n(?:[^\n]*\n){0,3}?static void kz3_gfx_bridge\(", src) is not None))
    r.append(("fwd-decl-game-type", re.search(r"#ifdef _WIN32\nstatic int kz3_game_type\(void\);", src) is not None))
    mh = body(src, "static void kz3_kb_miss_hook(")
    r.append(("hook-lock-variant", bool(mh) and "rsx_gfx_locked(kz3_game_type())" in mh and "graphicssettings_mp.gum" in mh
              and "rsx_gfx_page_gum_l(kz3_gfx_levers(), glk, &gn)" in mh))
    ks = body(src, "static void kz3_kb_set(")
    r.append(("set-routes-gfx", bool(ks) and '!strncmp(cmd, "gfx", 3)' in ks and '!strncmp(cmd, "bloom", 5)' in ks
              and "kz3_gfx_bridge(" in ks))
    return r

src = open(sys.argv[1], encoding="utf-8").read()
fails = 0
base = checks(src)
for n, ok in base:
    print(("[PASS] " if ok else "[FAIL] ") + "call site: " + n)
    fails += not ok

def mutate(name, a, b, must):
    global fails
    if src.count(a) != 1:
        print("[FAIL] control %s does not apply (%d matches)" % (name, src.count(a))); fails += 1; return
    got = {n for n, ok in checks(src.replace(a, b)) if not ok}
    if must in got: print("[PASS] control %s: caught by %s" % (name, must))
    else:
        print("[FAIL] control %s: not caught by %s (failing: %s)" % (name, must, sorted(got))); fails += 1

mutate("bloom-ignores-lock", "(int)s_bloom_eff), kz3_game_type());   /* 2206", "(int)s_bloom_eff), -1);   /* 2206", "bloom-body-lock")
mutate("bloom-no-wrapper", "rsx_gfx_effective_bloom(kz3_seq_now(&s_bloom_seq, (int)s_bloom_eff), kz3_game_type())",
       "kz3_seq_now(&s_bloom_seq, (int)s_bloom_eff)", "bloom-body-lock")
mutate("bridge-no-game-type", "rsx_gfx_cmd(&st, a, kz3_game_type(), reply, cap)", "rsx_gfx_cmd(&st, a, 0, reply, cap)", "bridge-lock")
mutate("bridge-no-save", "        kz3_display_save_extra();\n    }\n}\n#endif", "    }\n}\n#endif", "bridge-saved-values")
mutate("hook-no-lock", "rsx_gfx_locked(kz3_game_type()) ? 1 : 0", "0", "hook-lock-variant")
mutate("hook-always-unlocked-page", "rsx_gfx_page_gum_l(kz3_gfx_levers(), glk, &gn)", "rsx_gfx_page_gum_l(kz3_gfx_levers(), 0, &gn)", "hook-lock-variant")
mutate("set-drops-gfx", '(!strncmp(cmd, "gfx", 3) && (cmd[3] == 0 || cmd[3] == \' \'))', "(0)", "set-routes-gfx")
sys.exit(1 if fails else 0)
PY

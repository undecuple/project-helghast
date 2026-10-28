#!/usr/bin/env bash
# Pass 106: patch 1127 (diagnostic getenv read once on hot paths, upstream issue #243) host test, against the patched
# toolkit (work/ps3recomp-patched or KZ3_PATCHED_DIR). Three parts:
#  1. no per-call getenv("NAME") is left for the switches 1127 caches (only the one-time reads it keeps by name);
#  2. every changed file still compiles on POSIX (-fsyntax-only, implicit declarations are errors);
#  3. the KZ3_ENV_ON / KZ3_ENV_STR macros, taken from the patched spurs_job.c, call getenv once per slot (getenv
#     wrapped with -Wl,--wrap=getenv) and give the same answer as getenv for set, unset and empty switches.
# Self-contained (no game data); SKIP without the patched tree. Exit 1 on any failure.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T="${KZ3_PATCHED_DIR:-$ROOT/work/ps3recomp-patched}"
[[ -d $T/runtime/spu ]] || { echo "SKIP: no patched toolkit at $T"; exit 0; }
grep -q "KZ3_ENV_ON" "$T/runtime/spu/spurs_job.c" || { echo "FAIL: patch 1127 not applied"; exit 1; }
fail=0; pass=0
# file  name  per-name getenv calls allowed (the one-time reads; v0.18.0, SPU pass 108: upstream 634871d1 caches
# SPU_ARGWATCH, SEMTID and SEMCHAIN itself in a static per site, so those lines count its one-time reads)
while read -r f n want; do
  [[ -z $f || $f == \#* ]] && continue
  got=$(grep -c "getenv(\"$n\")" "$T/$f")
  if [[ $got == "$want" ]]; then pass=$((pass+1)); else echo "FAIL $f $n: $got getenv(\"$n\") left, want $want"; fail=1; fi
done <<'L'
runtime/spu/spurs_job.c SPURS_JOB_DUMP 0
runtime/spu/spurs_job.c SPURS_JOB_DESCDUMP 1
runtime/spu/spu_lifted_job.h SPURS_TASKSET_TRACE 1
runtime/spu/spu_lifted_job.h SPU_WORKER_TRACE 1
runtime/spu/spu_channels.c SPU_ARGWATCH 1
runtime/syscalls/sys_cond.c PS3_COND_PEEK 0
runtime/syscalls/sys_cond.c PS3_COND_SPURIOUS_MS 0
runtime/syscalls/sys_event.c YDKJ_Q23STACK 0
runtime/syscalls/sys_event.c PS3_EVT_RECV_TRACE 0
runtime/syscalls/sys_event.c YDKJ_NO_PORTROUTE 0
runtime/syscalls/lv2_register.c RD_SPU_INTERP 0
runtime/syscalls/lv2_register.c YDKJ_CRIBT 0
runtime/syscalls/lv2_register.c YDKJ_ASSERTBT 0
runtime/syscalls/lv2_register.c FLOW_PSSGTRACE 0
runtime/syscalls/sys_semaphore.c SEMTID 2
runtime/syscalls/sys_semaphore.c SEMCHAIN 2
runtime/syscalls/sys_semaphore.c PS3_SEMTRACE 0
runtime/syscalls/sys_fs.c FLOW_TITLEOPEN 0
runtime/syscalls/sys_fs.c FLOW_TITLEFIX 0
runtime/syscalls/sys_fs.c FLOW_FSDBG 0
runtime/ppu/ppu_sysprx.cpp LWM_COUNT 0
runtime/ppu/ppu_loader.cpp FLOW_PSSGTRACE 0
L
echo "part 1: $pass switch checks passed"
O="$(mktemp -d)"
python3 -c "import sys; sys.path.insert(0,'$T/tools'); from check_ppu_scaffold import stub_header; open('$O/ppu_recomp.h','w').write(stub_header())" 2>/dev/null || :
n=0
for f in runtime/spu/spurs_job.c runtime/spu/spu_channels.c runtime/syscalls/sys_cond.c runtime/syscalls/sys_event.c \
         runtime/syscalls/lv2_register.c runtime/syscalls/sys_semaphore.c runtime/syscalls/sys_fs.c \
         runtime/ppu/ppu_sysprx.cpp runtime/ppu/ppu_loader.cpp; do
  if [[ $f == *.cpp ]]; then cc=(${CXX:-c++} -std=c++17); else cc=(${CC:-cc} -std=gnu17 -Werror=implicit-function-declaration); fi
  [[ $f == */sys_event.c ]] && cc+=(-include runtime/platform/win32_compat.h)   # its kz3_semdump uses LONG (Windows types)
  if (cd "$T" && "${cc[@]}" -fsyntax-only -w -I"$O" -I include -I runtime -I runtime/spu -I runtime/ppu -I runtime/platform \
        -I runtime/diag -I runtime/syscalls -I runtime/memory -I libs "$f") >"$O/c.log" 2>&1; then n=$((n+1))
  else echo "FAIL compile $f: $(grep -m1 error "$O/c.log" | cut -c1-200)"; fail=1; fi
done
echo "part 2: $n files compile"
{ echo '#include <stdio.h>'; echo '#include <stdlib.h>'; echo '#include <string.h>'
  sed -n '/^#ifndef KZ3_ENV_ON/,/^#endif/p' "$T/runtime/spu/spurs_job.c"
  cat <<'C'
static int calls;
char* __real_getenv(const char*);
char* __wrap_getenv(const char* n) { calls++; return __real_getenv(n); }
int main(void) {
    int bad = 0;
    static int s_a = -1, s_b = -1, s_c = -1; static const char* s_s = (const char*)1; static const char* s_u = (const char*)1;
    setenv("T1127_SET", "1", 1); setenv("T1127_EMPTY", "", 1); unsetenv("T1127_UNSET");
    for (int i = 0; i < 1000; i++) {
        bad |= KZ3_ENV_ON(s_a, "T1127_SET") != 1;
        bad |= KZ3_ENV_ON(s_b, "T1127_UNSET") != 0;
        bad |= KZ3_ENV_ON(s_c, "T1127_EMPTY") != 1;           /* set but empty: getenv is non-NULL, as before */
        const char* v = KZ3_ENV_STR(s_s, "T1127_SET"); bad |= !v || strcmp(v, "1") != 0;
        bad |= KZ3_ENV_STR(s_u, "T1127_UNSET") != NULL;
    }
    printf("getenv calls %d (want 5), mismatches %d\n", calls, bad);
    return (calls == 5 && !bad) ? 0 : 1;
}
C
} > "$O/m.c"
if ${CC:-cc} -std=gnu17 -O2 -Wall -Wl,--wrap=getenv -o "$O/m" "$O/m.c" >"$O/m.log" 2>&1; then
  r="$("$O/m")" && echo "part 3: PASS $r" || { echo "part 3: FAIL $r"; fail=1; }
else echo "part 3: FAIL build $(grep -m1 error "$O/m.log" | cut -c1-200)"; fail=1; fi
rm -rf "$O"
exit $fail

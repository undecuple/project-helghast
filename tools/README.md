# tools/

Index of the scripts and folders in this directory. Build and test details are in [docs/BUILDING.md](../docs/BUILDING.md).

## Entry points

| Script | What it does |
|---|---|
| `build-kz3.sh` | Configures and builds the Windows exe (`kz3.exe`) with the MinGW-w64 cross toolchain. |
| `lift.sh` | Lifts the PPU code of the player's own EBOOT to C (static recompilation). |
| `kz3-stage.sh` | Stages a runnable folder: the built exe next to the player's own game files, read in place. |
| `sandbox.sh` | Runs a command with the game and the repository read-only; only `work/` and `build/` are writable. |
| `proton-test.sh` | Runs a Windows program under Proton on Linux, hidden and muted (used for tests on Linux). |
| `lint.sh` | Lints the files a commit touches: ruff, shellcheck and clang-format. |
| `run-host-tests.sh` | Runs every host test listed in `host-tests.txt`; no game data and no GPU needed. |
| `kz3_metrics.py` | Boot regression metrics; `--benchmark-v1` opts into strict windowed benchmark JSON. |
| `host-tests.txt` | The list of host tests CI runs. |
| `ps3recomp-patched.sh` | Applies the project's patches to the pinned ps3recomp checkout. |
| `safe_rm.sh` | Deletes files inside the repository only. |

## Folders

| Folder | Contents |
|---|---|
| `spu/` | SPU lift (`lift_spu.sh`, `lift_spu.py`), version tables and the SPU host tests. |
| `smoke/` | Host-side test harness: small C programs and Python checks that run without a GPU or game data. |
| `perf/` | Native-Windows mission-0 benchmark runner and synthetic, game-free checks. |
| `release/` | Installer, portable zip and first-run build: payload assembly, toolchain fetch, disc and update checks, launcher. |
| `upstream/` | Host tests for the general ps3recomp fixes the patches carry. |
| `pad/` | Scripted pad-input files (`.pad`) that drive the menus and levels in automated runs. |
| `assets/` | `profile_edit.py` (reads and edits the profile save) and `lift_table.sh` (checks the lift is current). |
| `agents/` | Optional build and GPU slot locking (`lock.sh`, `slotlock.sh`): sourced by the build and lift scripts so two jobs do not compete for the machine. Not needed for a single-user build. |

Single-file helpers: `disc_info.py` (disc layout), `psarc.py` (archive list and extract), `elf_info.py` and `jobmods.py` (EBOOT inspection), `features.py` (generates `docs/FEATURES.md`).

## Windows mission-0 benchmark

From the repository root, preview the path-free plan and run the synthetic tests without starting the game:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\perf\mission0.ps1 -Baseline -Plan
powershell -NoProfile -ExecutionPolicy Bypass -File tools\perf\mission0.ps1 -Candidate GCM_FAST_METHODS -Plan
python -B tools\perf\test_mission0.py
```

For an actual benchmark, first close other GPU-intensive applications, leave all input devices idle (disconnect
controllers if necessary), and explicitly choose either an all-off baseline or a single candidate:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\perf\mission0.ps1 -Baseline -ConfirmExclusiveGpu
powershell -NoProfile -ExecutionPolicy Bypass -File tools\perf\mission0.ps1 -Candidate GCM_FAST_METHODS -ConfirmExclusiveGpu
```

`-Baseline` and `-Candidate` are mutually exclusive; neither is inferred when omitted. The candidate must be one
of `GCM_FAST_METHODS`, `LD_CB_REUSE`, `KZ3_SPU_TASK_POOL`, or `KZ3_PARK_IDLE_WAKE`. `-SamplesPerSide` defaults
to 5 and accepts 5–100. In baseline mode it requests **exactly** that many valid all-off windows. In candidate
mode it requests at least that many valid windows per side; complete off/on/on/off blocks are always run, so the
default produces **six** valid windows per side, not five. Both modes use the same conditions and evidence rules.
A failure stops the session and exits nonzero rather than silently retrying or substituting a run. `-Plan` needs
only Python and the checked-out helper; it does not validate an installation or GPU.

The runner requires native Windows PowerShell, Python 3, Git, and a built, link-free
installation. It privately resolves the executable, working directory and environment through the launcher's
`--print` decision; a moved installation needing launcher repair is refused, not repaired. The executable is
validated and fingerprinted; the build stamp, patched-toolkit stamp, current ordered patch digest and clean pinned
submodule must agree. By default the
installation root is this checkout; `-InstallRoot` selects another existing installation. `-Python` selects a
specific Python executable. Quote directory values containing spaces; malformed arguments produce a fixed,
path-free error. `-ProfileSource` selects the source **gamedata directory** instead of the launcher's
data directory's `gamedata` child. It must contain exactly one initialized, valid profile at
`dev_hdd0\home\<user>\savedata\<slot>\PROFILE.DAT`, with no symlinks or junctions in the source or its ancestors.
The source and the session directory must not be equal, or contain one another; this is checked before creating
the session or copying anything. The copied profile's Continue record is cleared; the original is never edited.
There is no automatic first-boot/profile fallback.

To benchmark a separate current CMake build against a portable install whose launcher reports `rebuild`, opt in
explicitly. Substitute your own installation and build directories; the plan is path-free and starts nothing:

```powershell
$install = 'C:\path\to\portable-install'
$developerBuild = '.\build\kz3-current-llvm'
powershell -NoProfile -ExecutionPolicy Bypass -File tools\perf\mission0.ps1 -Baseline -InstallRoot $install -DeveloperBuildDir $developerBuild -Plan
powershell -NoProfile -ExecutionPolicy Bypass -File tools\perf\mission0.ps1 -Baseline -InstallRoot $install -DeveloperBuildDir $developerBuild -ConfirmExclusiveGpu
```

In developer mode the launcher still supplies the install's recorded game/update paths, data/profile location,
and INI settings; only its `run` and `rebuild` decisions are accepted. Missing or linked inputs, moved installs,
and legacy VFS links are rejected. The developer build must contain `kz3.exe`, `CMakeCache.txt`, and its
generator manifest. Its CMake source must be this checkout's `port\kz3`, and its toolkit source must be this
checkout's `work\ps3recomp-patched`. The toolkit patch stamp must match the current patch digest and pinned
submodule; a build-local `.kz3-toolkit-stamp`, when present, must also match. The MZ executable must be newer
than the toolkit stamp and recorded configure outputs. The runner then performs a non-mutating Ninja dry run for
the executable target and rejects the build if any compile, generation, or link command would run. Developer
benchmark verification therefore supports Ninja and Ninja Multi-Config only; other generators fail explicitly
rather than relying on timestamp heuristics. The runner reads `KZ3_EMBED_EBOOT` from that build's cache.
When it is empty, the verified EBOOT recorded by the install is passed as one command-line argument and checked
again before every process. When it is nonempty, its regular, link-free input must match the generated embed
header's path and SHA-256, its contents must match the install's recorded EBOOT, and the executable must be newer
than both the input and header; no EBOOT argument is supplied. Missing or inconsistent cache entries, inputs,
headers, or hashes fail explicitly. The summary records only `eboot_mode` (`argument` or `embedded`) and the
EBOOT SHA-256, never its path.

The staged executable comes from the developer build, while
DLLs and required `runtime\SDL3.dll` and `fonts\Rajdhani-{Medium,Bold}.ttf` come from the install. The staged
`lang\port_ui.tsv` instead comes from this checkout's tracked `port\text\port_ui.tsv`, so older installs need no
language table. The repository table must be a tracked regular file with no linked ancestors. Missing or linked
required resources fail; neither SDL nor the fonts fall back to another source. The install is not modified, and
the portable build stamp is never edited or impersonated.
The summary records `developer_cmake`, launcher decision, executable and CMake-cache hashes, generator,
configure-stamp status, freshness result, and the language table's sanitized source and SHA-256 without recording
private paths. Without
`-DeveloperBuildDir`, the stricter installed-build checks remain unchanged.

If no profile exists, developer builds may explicitly opt into a one-time private initialization:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\perf\mission0.ps1 -Baseline -InstallRoot $install -DeveloperBuildDir $developerBuild -InitializeProfile -Plan
powershell -NoProfile -ExecutionPolicy Bypass -File tools\perf\mission0.ps1 -Baseline -InstallRoot $install -DeveloperBuildDir $developerBuild -InitializeProfile -ConfirmExclusiveGpu
```

`-InitializeProfile` requires `-DeveloperBuildDir` and never initializes over an existing profile. With exactly
one source profile the usual validation and copying apply; multiple profiles, an invalid profile, inaccessible or
linked sources, and overlapping output paths fail instead of triggering initialization. Only when the selected
source has zero profiles (including a missing `gamedata` directory) does the runner create a separate ignored
bootstrap session. It stages the same verified developer executable, resources, launcher paths and fixed settings,
with all candidate switches off, then runs the existing `shaders.py` first-boot route unmeasured. The current route
is 990 seconds plus a 15-second margin and up to 10 seconds for shutdown; it is supervised and checked for
crashes, timeouts, pad warnings, missing, mismatched, out-of-order or late events, and shader failures. No
benchmark window is counted during initialization. Missing, incomplete, multiple or invalid resulting profiles
stop the session.

The original installation and profile source are not modified. The resulting single profile is copied and its
Continue record cleared using the existing profile editor; each measured run receives an identical private copy.
The normalized reusable seed stays in that benchmark session's `seed\gamedata` below ignored `work\helsari-p0`;
it can be supplied as `-ProfileSource` in a later run, without `-InitializeProfile`. Bootstrap logs, data and
seed are private local material and also stay under ignored `work\helsari-p0`. The plan describes the conditional
bootstrap without running it, and the summary records only its status, route hash and sanitized build provenance.

Every process gets a fresh copy of the same normalized profile, separate writable data and application caches,
and a staged executable and assets. The asset fingerprint must remain unchanged across the matrix. The pipeline
template is disabled; OS and GPU-driver caches are not cleared or controlled. Runs use the existing shader-warmup
profile route, 1920×1080 windowed/stretch/bilinear, VSync on, 30 FPS, English-US, keyboard input and mouse look
disabled. All candidate flags are explicitly off except the selected flag on the on side. In baseline mode all four candidate flags remain explicitly off. Each process runs for
340 seconds; the measured 60-second window is **[265, 325) seconds on the guest-flip clock**, not the pad clock.
Mission movement begins at 145 seconds. Retained cold-cache native-Windows runs include a late outlier whose final
greater-than-two-second no-flip interval ended at 235.0 seconds. Starting measurement at 265 seconds leaves a
30-second observed margin. The movement route repeats every 20 seconds, so shifting the original window by exactly
100 seconds preserves its periodic input sequence while excluding the observed loading interval. The route was
extended by one cycle to end at 325 seconds.
This policy applies equally to both sides, but cold application caches and `PS3_FSLOG`/flip logging add overhead.
The low-overhead matrices do not enable `RSX_PERF`: a bounded diagnostic showed that the reporter materially
perturbs this workload, producing multi-second flip gaps even with the candidate off. Candidate-counter diagnostics
using that reporter must therefore be recorded separately and excluded from timing comparisons. Its clock is not
treated as aligned with the guest-flip clock.
The runner tracks the concrete process by PID and creation time, requests window close, and terminates that
process on a crash, lost supervisor or shutdown timeout; it never kills processes by name.

The child environment is rebuilt from an explicit platform allowlist: `SystemRoot`, `WINDIR`, `SystemDrive`,
`ComSpec`, `PATH`, `PATHEXT`, `TEMP`, `TMP`, `USERPROFILE`, `HOMEDRIVE`, `HOMEPATH`, `HOME`, `APPDATA`,
`LOCALAPPDATA`, `ALLUSERSPROFILE`, `ProgramData`, `ProgramFiles`, `ProgramFiles(x86)`, `ProgramW6432`,
`CommonProgramFiles`, `CommonProgramFiles(x86)`, `CommonProgramW6432`, `PUBLIC`, `OS`, `NUMBER_OF_PROCESSORS`
and the `PROCESSOR_*` names listed in the runner. Only explicitly recognized launcher path variables and fixed
benchmark overrides are then added. Unknown launcher variables are rejected; other inherited runtime, graphics,
and Python overrides are not passed through. Each run's JSON records every effective non-path benchmark switch
and the exact pad-script SHA-256; path-variable names are recorded with values redacted. Fixed display settings
are recorded in the session conditions.

`-ConfirmExclusiveGpu` is an operator acknowledgement, not proof of exclusivity. A cooperative global mutex
prevents concurrent copies of this runner. Best-effort GPU-engine snapshots use Windows' invariant WMI class
before and after runs; unavailable or unrecognized counters never block preflight, and detected activity is
reported rather than silently treated as exclusive. The summary always records `gpu_exclusivity: unverified` and
the snapshot status and observed/unknown foreign activity. Snapshots are not continuous, other tools need not
honor the mutex, and driver caches and physical input cannot be ruled out.

The Windows runtime stamps pad delivery and guest flips from the same monotonic `ps3_ms_now` clock at the actual
event points. The analyzer derives the flip-clock origin from the paired flip timestamps and aligns each pad event
directly; cross-thread log-write order is not timing evidence. It also checks the complete fired action against the
route: digital mask and poll/timed hold, or all six encoded analog bytes and duration. Expected mission events must
be on time and fall within their appropriate pre-window or measured workload interval. Missing shared-clock
timestamps, inconsistent clock origins, wrong actions, events
not provably within two seconds of schedule, or events outside the interval invalidate the run. A timeout, crash,
shader failure, missing mission-open evidence, incomplete pad events, missing/nonmonotonic flips, or a gap over
two seconds in the measurement window also invalidates it. Keep the external game/update and firmware inputs unchanged
throughout the matrix. These logs do not prove the candidate's optimized path executed or that the scene stayed
identical; confirm that independently before drawing performance conclusions.

Results and raw per-run files are written only under ignored `work\helsari-p0\run-*` directories. `summary.json`
and `summary.csv` identify the mode and candidate (`null` in JSON and empty in CSV for a baseline), and contain
sanitized statuses, fixed reason codes, fingerprints and numerical measurements. JSON also records planned and
effective switches and valid window counts. FPS is an **approximate flip-count mean**, and GPU execution time is
unavailable. The raw combined log, staged executables, and profile copies can contain private paths or game material:
keep them local, never commit them, and remove them when no longer needed. The synthetic tests exercise process
control and a complete mocked matrix, but do not validate a real game installation.

## Benchmark metrics v1

The original boot-check CLI, table, JSON and baseline rules are unchanged. The separate v1 contract is opt-in:

```powershell
python tools\kz3_metrics.py synthetic.log --benchmark-v1 --window 0,0.1 --out synthetic-result.json
python tools\test_kz3_metrics.py
```

`LOG` may be `-` for stdin. `--window A,B` is required and gives elapsed seconds on the **frame source's clock**.
The output is JSON both on stdout and, when requested, in `--out`. Its `schema` is `kz3-benchmark-v1`; `status`
is `pass` or `fail`, and `failures` contains codes, line numbers and descriptions. Exit is nonzero for invalid or
incomplete windows/records, an unterminated final line, `--alive 0`, a nonzero `[exit code N]`, crash markers,
or pad-script warnings. Crash markers include `[HardExit]`, `[fatal]`, and undrained-SPU shutdown markers, matched
case-insensitively. Native
integers are limited to 20 decimal digits and unsigned 64-bit magnitude (exit codes may be negative); oversized
values are invalid rather than truncated. A parsed run, invalid window, or unreadable input writes failure JSON;
other argument or output-write errors exit without it. Boot-only baseline, drift, row and run-provenance options
cannot be combined with v1. The default `--alive 1` means the process was alive at the **end of the measured
window**; a caller must supply `--alive 0` when it was not.

`frames` reports mean FPS (frame count divided by total frame time), median instantaneous FPS, median frame time,
p95/p99 frame times and 1% low (FPS from the mean duration of the slowest `ceil(1% * N)` frames). Medians use
the middle value or mean of the two middle values; p95/p99 use nearest rank. Intervals crossing window boundaries
are excluded, and `actual_start_ms`/`actual_end_ms` identify the complete frames used. A window must be covered,
not merely contain some samples. A native `[flip]` series (including the optional `host=<ms> ms` shared-clock suffix) uses the guest-flip clock and millisecond resolution,
not present timestamps. Sparse flips support only an approximate mean between recorded points, without
interpolating the edges. Native `[pace]` summaries use the
present-clock histogram: p95 is not printed, even-sized windows do not give both middle values, and percentiles
or 1% lows from multiple windows cannot be pooled. Such figures stay `null`; the individual p50/p99/low1
observations remain in `native.pace`. Each frame result names its source, clock and quality.

For collectors and synthetic fixtures that have actual per-frame intervals or windowed telemetry, v1 also
accepts these explicit records (times are milliseconds on the named clock):

```text
[benchmark-frame] clock=benchmark end_ms=10 duration_ms=10
[benchmark-frame] clock=benchmark end_ms=100 duration_ms=90
[benchmark-metric] clock=benchmark category=walker start_ms=0 end_ms=100 op=sum run_ms=42 wait_ms=8 park_ms=5
[benchmark-metric] clock=benchmark category=cpu name=process start_ms=0 end_ms=100 op=sum cpu_ms=70
[benchmark-metric] clock=benchmark category=cpu name=rsx start_ms=0 end_ms=100 op=sum cpu_ms=30
[benchmark-metric] clock=benchmark category=gpu method=timestamp start_ms=0 end_ms=100 op=sum gpu_ms=55 dropped=0
[benchmark-metric] clock=benchmark category=candidate name=example start_ms=0 end_ms=100 op=sum hits=4 misses=0
```

These are an input contract, not a claim that current runtime instrumentation emits them. Omitted `clock` means
`benchmark`. `category` can be `walker`, `draw`, `submit`, `fence`, `writeback`, `spu`, `cpu`, `gpu`, or
`candidate`; CPU and candidate records require a `name`. All remaining fields are arbitrary numeric metrics.
For each metric, intervals must tile the requested window exactly once, with a consistent `op`: `sum` for
interval deltas, `max` for interval maxima, or `last` for a final gauge. Partial/overlapping data fail and remain
`null`; a recorded zero stays zero. Output preserves each metric's operation and sample count. CPU `name=process`
is the process record; other names are thread records.

Aligned GPU timing has status `exact` for `method=timestamp`, `approximate` for `method=fence_poll`, and
`unavailable` without usable evidence. The former requires an explicit zero `dropped`/`dropped_samples`; the
latter requires zero `missed`/`missed_fences`. Time totals must use `op=sum`; a `gpu_ms_per_frame` value must
cover the whole window in one record and include a positive `frames`. Every recorded interval must have zero
loss; a later zero cannot erase an earlier dropped/missed value. Dropped or incomplete samples fail rather than
masquerade as exact. If both methods are present, timestamp timing takes precedence and the other observation is
retained under `alternatives`. A CPU fence wait is not GPU execution time.

Existing `[perf]`, `[perf-walk]`, `[perf-thr]`, `[wacct]`, `[spurs-cpu]`, `[spurs-ready]`, `[usercmd-wait]`,
park-wake and GPU-monitor summaries are preserved under `native` with their own clock names. `[perf-thr]` is a
sampled process total and a top-thread list, not a complete list of zero-valued threads; a newly seen thread's
first sample may include CPU time from before that reporting period. Each `[wacct]` thread's `sites` list
preserves per-site kind, name (`site`), `ms` and `count` in log order; a malformed site fails the run. Native
timestamps, the guest-flip clock and the present clock have different origins: these observations are **not** silently
added to the requested window. Explicit metric records whose clock differs from the frame source appear in
`unscoped_metrics`. Native GPU timestamp and fence-poll samples carry their own exact/approximate/unavailable
status; dropped queries or missed fences are failures.
An exact native 10-report timestamp sample also requires all ten corresponding loss-free fence-poll reports.

Comments cite internal numbering (pass, round, plan items) from the development notes; they are labels, not links.

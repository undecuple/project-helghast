# Experimental XeSS Super Resolution implementation

Project Helghast has an opt-in experimental presentation path built around Intel XeSS Super Resolution (XeSS-SR).
It stages the unmodified `libxess.dll` from Intel XeSS SDK 3.0.2 and uses the SR API reported by that runtime,
`2.0.2`. This is an **ENHANCEMENT**: the PS3 game has no XeSS path, and Bilinear remains the default filter.

In brief, the implementation:

- keeps the game rendering its original 1280x720 scene;
- identifies the game's primary deferred pass and full-resolution depth;
- applies XeSS jitter and the mip bias Intel requires to scene geometry, and skips FXAA on those frames;
- reconstructs camera motion from the current and previous camera poses plus depth, using the camera paired with the
  frame being replayed;
- compares coherent opaque and final scene captures to generate XeSS's responsive-pixel mask, and masks the HUD;
- dispatches XeSS-SR into the selected larger output resolution; and
- falls back to the existing Bilinear presentation path if any prerequisite fails.

The current implementation is Super Resolution only. It does not include XeSS Frame Generation or Xe Low Latency.
Moving objects do not yet have their own motion vectors. XeSS runs on the finished display image, after tone-mapping,
the SPU post chain and the HUD; Intel's guide calls that placement experimental, with no guarantee of good quality.

## Enabling it

Use **Options > Display > Scaling filter > XeSS-SR (experimental)** and accept the warning. The selection is stored
in `display.ini`:

```ini
[display]
filter = xess
```

For one run without changing the saved setting:

```powershell
$env:KZ3_DISPLAY_FILTER = "xess"
```

XeSS-SR is attempted only when the output is larger than 1280x720 and has the same aspect ratio, allowing one output
pixel of rounding (1366x768 and rounded Fit sizes pass; 1920x1200 does not). A 1920x1080 output is the primary test
case. The runtime chooses a XeSS quality mode from the requested scale; there is no separate quality selector yet.
When XeSS is selected, the independent `RSX_UPSCALE` spatial upscaler is bypassed, including when XeSS falls back to
Bilinear. A requested spatial backend is reported in the log; selecting another display filter restores its usual use.

Switches for one run, each read once at the point named:

| Switch | Default | Effect |
|---|---|---|
| `KZ3_DISPLAY_FILTER` | `display.ini` | `xess` selects this path |
| `KZ3_XESS_MIP_BIAS` | Intel's `log2(input / target)` | LOD bias of scene draws, in mip levels; `0` turns it off |
| `KZ3_XESS_CAMERA_LAG` | `0` | Whole flips to step back from the paired camera; negative takes the latest published camera |
| `KZ3_XESS_HUD_MASK` | on | `0` turns the HUD mask off |
| `KZ3_XESS_INPUT_DELAY` | on | `0` turns off the one-frame input alignment (see Motion vectors) |

### Jitter diagnostics

These one-run environment variables are experimental controls, not image-quality fixes. They are read once when XeSS
is selected. Leaving them unset preserves the normal Halton sequence and the same projection and execute offsets;
enabling only the trace also leaves rendering unchanged. Invalid values are reported in `[xess-diag]` and use the
documented default.

| Switch | Default | Effect |
|---|---|---|
| `KZ3_XESS_JITTER_PATTERN` | `halton` | `two-phase` alternates `(+0.25, 0)` and `(-0.25, 0)` input pixels by renderer-frame parity |
| `KZ3_XESS_PROJECTION_SCALE_X`, `KZ3_XESS_PROJECTION_SCALE_Y` | `1`, `1` | Scale each axis applied to scene draws and removed during motion reconstruction |
| `KZ3_XESS_EXECUTE_SCALE_X`, `KZ3_XESS_EXECUTE_SCALE_Y` | `1`, `1` | Independently scale each axis passed to XeSS execute |
| `KZ3_XESS_EXECUTE_JITTER_LAG` | `0` | `1` uses the previous nominal sample for execute when it belongs to a contiguous successful dispatch. With the default input alignment this delays the execute jitter twice; use it only with `KZ3_XESS_INPUT_DELAY=0` |
| `KZ3_XESS_JITTER_TRACE` | `0` | Log every Nth renderer frame; `1` logs every frame, `0` disables logging, maximum `60000` |

Scale values must be finite numbers from `-2` to `2`; `0` disables an axis and `-1` reverses it. The lag applies the
current execute scales to the previous nominal sample. It uses the current sample after a hold, skipped/failed frame,
camera reset or discontinuity, or XeSS context recreation. When a diagnostic intentionally zeros projection jitter,
the affected scene draws still receive their usual XeSS mip bias and FXAA remains skipped, so those unrelated changes
do not confound the experiment. Fallback's jitter hold continues to depend on whether the projection was actually
shifted.

For example, set the following in PowerShell together with `KZ3_DISPLAY_FILTER=xess` before starting the game from
the same shell, enter gameplay, and pause on a still scene. Compare this with an otherwise identical run with the
pattern unset. Set `KZ3_XESS_JITTER_TRACE` to `1` when checking both phases; an even interval can sample only one
phase.

```powershell
$env:KZ3_DISPLAY_FILTER = "xess"
$env:KZ3_XESS_JITTER_PATTERN = "two-phase"
$env:KZ3_XESS_JITTER_TRACE = "1"
```

For projection-only or execute-only runs, respectively set both execute scales or both projection scales to `0`; for
the zero-jitter control set all four to `0`. Clear the diagnostic variables between runs. Keep logs and captures that
contain game material under `work/` and do not commit them.

Each trace line identifies the renderer frame, dispatch result, whether a sample was active or held, nominal,
projection and execute offsets in input pixels, and whether lag was applied. On fallback the execute values are the
planned values, not a successful XeSS call. Counts describe draws **evaluated at the jitter-selection point**, before
final submission; they are not counts of pixels or guaranteed GPU submissions. `deferred` is a local full-size target
with at least four render targets, `scene` is the identified full-size scene target excluding those deferred draws,
and `other` is everything else. For the deferred/scene candidates, `shifted`, `partial-viewport`, `depth-disabled`,
`depth-mismatch`, `zero-projection`, and `unavailable` are exclusive outcomes in that order. `depth-mismatch` includes
an absent depth slot or a scene draw whose slot/frame does not match the primary; `zero-projection` is an available
sample with zero projection offset, while `unavailable` counts eligible draws whose jitter sample was held or could not
be obtained. The logs do not establish which screen regions a draw covers.

## Architecture

The Intel ABI and Killzone 3 hooks are kept outside the general ps3recomp runtime. The KZ3 target defines
`KZ3_XESS` and supplies a port-owned adapter. Other ps3recomp users do not gain an Intel runtime dependency.

```text
Recompiled game camera hooks
  -> current pose, effective FOV, guest flip and view ID
  -> validated near/far planes from the primary scene projection
  -> port-owned camera bridge, which keeps recent states by guest flip

RSX draw replay
  -> identify the full-resolution four-target deferred pass
  -> take the camera paired with the frame being replayed
  -> remember, jitter and mip-bias its depth-tested geometry
  -> jitter and mip-bias later depth-tested draws into the scene target
  -> capture the opaque deferred resolve
  -> capture the finished scene and its depth at the scene copy (the NV0039 write-back to the SPU post chain)
  -> skip FXAA on that surface
  -> capture the display surface right after the 3D compose, before the HUD

Presentation
  -> generate an R8_UNORM responsive-pixel mask, with the HUD marked
  -> generate R16G16_FLOAT current-to-previous NDC motion, zero under the HUD
  -> dispatch XeSS-SR into an RGBA8 output
  -> apply scan-out gamma and the existing display blit
  -> present

Any missing, unsupported, or incoherent input
  -> log the reason
  -> restore the normal resource states
  -> use Bilinear presentation, and hold jitter off until a dispatch works
```

### Components

| Component | Role |
|---|---|
| [`port/kz3/xess.cpp`](../port/kz3/xess.cpp) | Secure runtime loading, exact API validation, XeSS context and resource ownership, motion and responsive-mask compute passes, scene captures, and dispatch |
| [`port/kz3/xess_api.h`](../port/kz3/xess_api.h) | Minimal independently authored XeSS-SR 2.0 ABI declarations used for dynamic loading |
| [`port/kz3/xess.h`](../port/kz3/xess.h) | C boundary between the KZ3 adapter and the patched C renderer |
| [`port/kz3/xess_motion.h`](../port/kz3/xess_motion.h) | Platform-independent depth reconstruction and camera reprojection |
| [`port/kz3/xess_camera.h`](../port/kz3/xess_camera.h) | Platform-independent camera bridge, flip pairing, and freshness rules |
| [`port/kz3/xess_inputs.h`](../port/kz3/xess_inputs.h) | Pure deferred-pass, scene-resolve, jitter, capture-pair, aspect, and mip-bias rules |
| [`port/kz3/xess_diagnostics.h`](../port/kz3/xess_diagnostics.h) | Host-tested diagnostic parsing, phase/lag sampling, and draw accounting |
| [`port/kz3/main.cpp`](../port/kz3/main.cpp) | Camera, FOV, and projection-plane publication |
| [`port/patches/2149-xess-super-resolution.patch`](../port/patches/2149-xess-super-resolution.patch) | Renderer integration: input selection, jitter and mip bias, FXAA skip, camera pairing, captures, HUD mask, presentation ordering, UI, fallback and re-init safety |
| [`port/patches/2150-xess-jitter-diagnostics.patch`](../port/patches/2150-xess-jitter-diagnostics.patch) | Opt-in jitter experiments and per-frame draw-coverage tracing |
| [`tools/release/stage_xess.py`](../tools/release/stage_xess.py) | Release-time validation and staging of Intel's unmodified runtime and required notices |
| [`tools/smoke/test_xess.cpp`](../tools/smoke/test_xess.cpp) | Native D3D12 context, dispatch, mask and motion contents, readback, and fallback smoke test |
| [`tools/smoke/test_xess_temporal.cpp`](../tools/smoke/test_xess_temporal.cpp) | Native check that XeSS reconstructs with the port's jitter and motion conventions |

## Frame pipeline

### 1. Camera and projection publication

The camera hook publishes the final interpolated pose after the port's camera adjustments. The pose stores the
right, forward, and up vectors followed by position. Horizontal and vertical FOV values include the port's
widescreen adjustment.

Pose, FOV, projection planes, guest flip, and view ID are published atomically only after both camera and projection
data for that guest flip are available. Until then, the last coherent snapshot remains visible, and the last 16
published states stay available by guest flip.

The renderer picks a camera when it reaches the primary deferred draw. The PPU may already have moved on by then, so
the latest published state can belong to the next frame. A frame's own camera is published before its draws are built,
which means `latest flip - renderer frame` is never below the true offset, and its smallest value over the last 120 to
240 frames is that offset. The renderer uses the flip that offset gives. This assumes no fixed relationship between the
two counters. The renderer frame counter advances after every presentation; the older FIFO-only flip counter does not
advance on every KZ3 presentation path and is not used here. If the PPU is ahead on every frame of the window the answer
is the latest flip, the same as taking the latest state, and if the flip is no longer in the ring the latest state is
used. `[xess] camera pairing` log lines show the numbers, including `misses`, the frames that fell back this way. If they
show a constant extra lag,
`KZ3_XESS_CAMERA_LAG` steps back by that many flips.

The guest flip is the flip-request counter read at the camera hook, and another thread advances it. The projection
planes are therefore filed under the flip read at the camera hook, not a fresh read: a tick in between used to file
camera and planes under different flips, so that frame's camera was never published. The counter can also move twice
between two camera updates, leaving one flip with no camera; the bridge fills that single flip as a hold of the
previous camera (no motion) and the next flip continues from it, so the renderer sees an unbroken sequence
(`[xess] ... held at the previous camera` in the log). A gap of two or more flips still resets history. Before this,
both cases made the renderer take a later camera and reset history about twice a second, which showed as a shudder
in gameplay and on the pause screen.

Each dispatch then checks that the camera moved on by exactly one flip since the last dispatch. The same flip again
means nothing moved, so the frame gets zero camera motion instead of repeating the old delta (`repeats` in the log). A
skipped flip resets history (`jumps`).

The primary 1.14 scene caller of `Camera::vf17` supplies the near and far values used for depth reconstruction. An
observed campaign frame used near `0.2` and far `2048`. A different projection path using `0.1` and `512` is ignored.
The equivalent 1.00 caller is mapped but has not received the same runtime validation.

Camera history resets when:

- there is no coherent immediately previous guest flip;
- the view ID changes;
- more than one view appears in one guest flip;
- the position changes by more than 5 guest units; or
- the camera rotates by more than 1.5 radians.

The two numeric cut thresholds are experimental adapter policy, not recovered gameplay constants.

### 2. Deferred input selection

The stable full-resolution deferred pass contains four color targets and one primary depth target:

| Local offset | Validated use |
|---|---|
| `0x02200000` | member of the four-target deferred MRT set; channel semantics remain unidentified |
| `0x01E40000` | deferred MRT and source sampled by the first scene-target resolve |
| `0x016C0000` | packed view normals |
| `0x01A80000` | member of the deferred MRT set; channel semantics remain unidentified |
| zeta `0x025C0000` | primary full-resolution depth |

No unidentified G-buffer target is treated as velocity. Slot number and texture format do not establish motion
direction, units, or jitter convention.

The renderer selects the primary deferred pass structurally: a local full-render-size target, full viewport, four
or more color targets, a bound depth surface, and depth testing. It records that depth for the current renderer
frame. The same XeSS jitter is applied to the primary pass and to later depth-tested draws into the validated scene
target that reuse that depth.

Jitter starts only after the runtime, context, camera, projection, and output size are ready. A missing or rejected
XeSS runtime therefore cannot leave a visibly jittered frame before Bilinear fallback. If a jittered frame falls back
later in the frame, jitter is held off and the following frames go through XeSS unjittered. The first dispatch that
works clears the hold, so at most one jittered frame reaches Bilinear.

Jittered draws also get the mip bias Intel's guide requires, `log2(input / target)`: about -0.585 at 1080p, -1 at 1440p
and -1.585 at 4K. It adds to the guest's own bias on texture units that have a mip chain, in a separate sampler. If the
128-entry sampler cache is full the draw keeps the guest's sampler.

FXAA, which runs in place of SPU MLAA, is skipped on jittered frames: a spatial filter would work against the temporal
solve. Frames that do not go through XeSS keep it.

### 3. Motion vectors

The adapter generates `R16G16_FLOAT` motion at input resolution. For each pixel it:

1. removes the current XeSS jitter from pixel NDC;
2. reconstructs view-space position from normal depth, near/far planes, and current FOV;
3. transforms the point through the current camera pose into world space;
4. projects that point into the previous camera pose; and
5. writes `previousNdc - currentNdc`.

This matches the current-to-previous NDC velocity convention requested through
`XESS_INIT_FLAG_USE_NDC_VELOCITY`. Jitter is excluded from motion.

**One-frame input alignment.** The colour XeSS receives is the finished display surface. The scene reaches it
only through the SPU post chain and the next frame's compose, so at frame N it shows the scene of frame N-1. The
adapter therefore dispatches frame N with the previous frame's camera, jitter (both the jitter removed from motion
and the one passed to XeSS), depth and opaque/scene captures: captures land in hold textures and are copied into
the textures XeSS reads after each dispatch. The alignment applies when the previous successful dispatch was the
previous guest flip (or the same one, a held camera); otherwise the frame's own inputs are used. The `[xess]
history` log line counts aligned dispatches. `KZ3_XESS_INPUT_DELAY=0` turns it off. Measured in gameplay
(2026-11-03): with the jitter alone delayed the picture was steadier than undelayed, with the camera delayed too
steadier again, and with every input aligned the moving picture showed no visible jitter.

Depth makes camera translation spatially correct: nearby geometry moves farther than distant geometry. Camera
rotation and translation are represented. Object, skeletal, vegetation, particle, and shader-driven motion are
not. Those pixels currently receive camera motion only.

Depth comes from an adapter-owned copy taken at the scene copy, so a later write to the same zeta in the frame cannot
reach XeSS. Pixels the HUD changed get zero motion, because static text must not be reprojected with the world behind
it. The conventions are checked against the runtime by `tools/smoke/test_xess_temporal.cpp`.

### 4. Responsive-pixel mask

The game resolves deferred lighting into the local scene target at `0x01300000`, then adds translucent effects and
other forward composition. At the NV0039 copy the scene leaves for the SPU post chain, and it comes back through the
3D compose into the display surface, where the HUD is drawn. The adapter captures:

- the first validated opaque lighting resolve;
- the final same-resource scene color at the scene copy;
- the display surface right after the 3D compose, which is the frame without its HUD; and
- the presented surface at dispatch.

A pair is accepted only when frame number, resource identity, dimensions, and XeSS context generation match.
A compute pass writes an input-resolution `R8_UNORM` responsive-pixel mask:

```text
translucent: max(abs(scene.rgb - opaque.rgb)) >= 0.2 gives 0.8
HUD:         max(abs(presented.rgb - composed.rgb)) >= 0.006 gives 1.0
```

The thresholds and mask values are experimental heuristics, not game-derived transparency semantics.
The first marks composition that changed after the opaque resolve so XeSS favors the current frame there. The second
marks pixels the HUD touched: outside the HUD the two images are identical, so the threshold only has to clear
rounding. The motion pass reads the 1.0 value, and XeSS clamps the mask to its default 0.8. If a pair is missing or
stale, that part of the mask is left out instead of reusing old data. Anything else drawn over the display surface
after the compose, such as a full-screen fade, also counts as HUD; `KZ3_XESS_HUD_MASK=0` removes this part for one run.

### 5. XeSS dispatch and presentation

The adapter initializes XeSS with:

- responsive-pixel-mask input;
- NDC velocity;
- LDR input color; and
- fixed exposure scale `1.0`.

The color and output are both `R8G8B8A8_UNORM`. Depth is `R32_FLOAT`; motion is `R16G16_FLOAT`; the responsive mask
has a float-readable R channel. Input resources enter XeSS in
`D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE`; output enters in
`D3D12_RESOURCE_STATE_UNORDERED_ACCESS`.

After a successful dispatch, the output enters the renderer's normal presentation sequence. The renderer applies
the existing scan-out gamma and display rectangle logic. Bilinear and Nearest behavior is unchanged when XeSS is
not selected.

The initial quality policy is:

| Output/input scale | XeSS quality |
|---:|---|
| `1.0` | Native AA |
| up to `1.3` | Ultra Quality Plus |
| up to `1.5` | Ultra Quality |
| up to `1.7` | Quality |
| up to `2.0` | Balanced |
| up to `2.3` | Performance |
| up to `3.0` | Ultra Performance |

Before initialization, the adapter asks XeSS for the minimum, optimal, and maximum input resolution for the chosen
output and quality. It rejects the context if the game's actual input is outside that range.

## Runtime loading and redistribution

The adapter does not use Intel's import library. It dynamically resolves only the XeSS-SR exports it needs and
loads `libxess.dll` only from beside `kz3.exe`, with Windows system dependencies restricted to System32. Intel lists
the Visual C++ Redistributable 14.40.33810 or later as a requirement; without it the DLL cannot load and the log names
it. On Intel GPUs Intel's dispatcher may use the implementation bundled with the driver.

The SDK package version is 3.0.2, while the runtime's exported XeSS-SR API and Windows file version are 2.0.2. The
adapter requires exactly API 2.0.2 and fails closed on a different ABI.

Release staging pins these official Intel files:

| File | Bytes | SHA-256 |
|---|---:|---|
| `libxess.dll` | 77,795,704 | `251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7` |
| `LICENSE.txt` | 4,194 | `784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df` |
| `third-party-programs.txt` | 35,788 | `e8d8899dbfc4de0e8ca25560be9358e03c1405d7c956d20a1838c3c709b9b51c` |

The release workflow also pins the SDK archive SHA-256 and verifies the runtime's Intel Authenticode signature. The
DLL is redistributed unmodified with Intel's licence and third-party notices. Intel's SDK headers are not bundled;
the small ABI declarations in `xess_api.h` were independently authored and their layouts checked against the
official headers.

XeSS-SR does not contact the network.

## Fallback behavior

Selecting XeSS never makes it a startup requirement. The renderer uses Bilinear for the frame and logs the reason,
once per change of reason, when any of these conditions applies:

- `libxess.dll` is missing, invalid, or exposes a different API;
- D3D12 or the current driver cannot create an XeSS context;
- input and output do not share an aspect ratio;
- output is not larger, or exceeds the supported 3x scale;
- XeSS rejects the actual input resolution for the selected quality;
- the deferred depth, camera, projection, or target is missing for this frame;
- resources have incompatible dimensions or formats; or
- motion generation, responsive-mask generation, or XeSS dispatch fails.

A context that cannot be built for one device and size is not retried every frame; a size change tries again. A stale
camera does not fall back: it keeps the frame in XeSS with zero camera motion (see Camera and projection publication).
Switching the filter away from XeSS releases the context and its textures at the next present. Resizing rebuilds them,
after flushing any open work that still uses them.

The Options page warns before enabling the experimental path. Cancel restores the currently active filter. The row
keeps showing XeSS-SR during a fallback; the reason is only in the log.

## Validation

Validation that needs no game data:

- the adapter compiles as C++20 with strict warnings;
- local XeSS ABI sizes and offsets match Intel's official 2.0 headers;
- the official runtime reports XeSS-SR API 2.0.2;
- motion tests cover identity, rotation, depth-aware translation, FOV changes, and invalid projection data;
- input tests cover deferred-pass selection, jitter eligibility, opaque resolve selection, capture coherence, aspect
  rounding, and the mip bias field;
- diagnostic tests cover invalid settings, independent axes, default compatibility, phase/lag resets, and draw counts;
- camera tests cover publication, resets, the flip ring, pairing, freshness, and pose handedness;
- display tests cover parsing, `display.ini`, the options row, warning/cancel behavior, reset, and no unsupported
  sharpness control;
- release tests cover exact hashes, AMD64 PE validation, required notices, payload assembly, portable ZIP inclusion,
  and launcher placement;
- the native D3D12 smoke test dispatches 1280x720 to 1920x1080 with normal and independent execute jitter, reads back the mask and motion contents and a non-empty
  output, and verifies near-16:9 re-init and missing, wrong, and unsupported-aspect fallback; and
- the native temporal test feeds a moving scene for 64 frames and checks that the port's jitter and motion conventions
  reconstruct, that the NDC path matches pixel-unit motion, and that flipped axes do not.

On this series (v0.1.8 plus patches 2149 and 2150), on Windows with an NVIDIA RTX 5070: the whole patch series
applies to the pinned ps3recomp, the XeSS host tests above pass, and `tools/smoke/test_xess.ps1` (dispatch, mask and
motion readback, re-init, temporal conventions, missing- and wrong-runtime fallback) passes.

Gameplay on this series, 2026-11-03: a first-run build (game 1.14, bundled clang) run natively on Windows with the
hardware D3D12 path on an NVIDIA RTX 5070, a 1280x720 game image, a 1920x1080 Fit output, `KZ3_DISPLAY_FILTER=xess`
and `KZ3_XESS_EXECUTE_JITTER_LAG=1`. Before the camera-pairing fix above, the log showed about 250 pairing misses and
252 history resets in 3,600 dispatches, seen as a shudder about once a second, paused or not. With it: 0 misses, 0
jumps, 1 reset (level start) in 3,600 dispatches, over 300 single skipped flips held, no fallback after the
pre-scene frame, and the shudder gone. A finer whole-scene jitter remained. Delaying the execute jitter alone was steadier than no
delay, delaying the camera as well steadier again, and with the one-frame input alignment (no switches set) the
moving picture showed no visible jitter: 2,999 of 3,000 dispatches aligned, 1 history reset. The pause screen still
jitters slightly (see Known issues).

Earlier builds of the same code, which also carried unrelated, unsubmitted renderer experiments, were run in the same
configuration without the lag. On 2026-10-31 the log showed:

- XeSS-SR API 2.0.2 dispatching 1280x720 to 1920x1080, with the camera basis at the required handedness, `+1.000`;
- normal dispatch with `responsive=generated`, `hud=masked` and mip bias `-0.586`;
- the first 600 dispatches with 4 resets, 3 repeats, 2 jumps and 2 ring misses; and
- 1,200 dispatches with 7 resets, no fallback after the pre-scene frame, no dispatch failure and no device removal.

The picture has a constant small whole-scene jitter (see Known issues). Disabling only the HUD mask with
`KZ3_XESS_HUD_MASK=0` did not change it. A temporary build that forced both the projection jitter and the jitter given
to XeSS to zero removed it while XeSS kept dispatching with depth, motion and the scene mask; that does not separate
wrong jitter from history XeSS cannot use. On 2026-11-03, in a paused scene with horizontal-only diagnostics, a
reversed or one-frame-lagged execute offset looked calmer than the default; short in-memory desktop samples gave
95th-percentile horizontal shifts between consecutive samples of about 0.84 output pixels (default), 0.36 (reversed)
and 0.17 (lagged). Those are single short samples, not synchronised GPU-frame measurements, and neither change is
made a default on this evidence.

### Diagnosing the whole-scene jitter

Candidate causes, most likely first:

| # | Hypothesis | What would settle it |
|---|---|---|
| C1 | XeSS cannot use its history (motion or pairing wrong, near/far wrong, pose differs from the rendered one, large responsive mask, depth that no longer matches post-processed colour), so each output is one jittered frame | Pause test; velocity-scale test |
| C2 | Parts of the picture are not shifted while XeSS assumes they are (depth-test-off scene draws, viewports that are not exactly full, other targets composited later) | `KZ3_XESS_JITTER_TRACE` draw-class counts; two-phase input comparison |
| C3 | The colour XeSS receives carries a different phase than the one it is given (a frame of latency in the post chain or compose) | `KZ3_XESS_EXECUTE_JITTER_LAG=1` |
| C4 | One axis, or the whole sign, is inverted in game | Per-axis `KZ3_XESS_EXECUTE_SCALE_*` and `KZ3_XESS_PROJECTION_SCALE_*` sweeps |
| C5 | The mip bias adds shimmer where accumulation fails (amplifies C1-C4) | `KZ3_XESS_MIP_BIAS=0` |

Still needing code or capture tooling: a velocity scale of 0 and +/-1 through `xessSetVelocityScale`; a forced-zero
responsive mask; capturing the pre-XeSS colour at two phases with the camera paused and cross-correlating it; and
Intel's `xessStartDump` on a bad frame. Measure the shift between consecutive output frames in a paused scene rather
than judging by eye; the target is below a tenth of an output pixel. Anything dumped from the game stays under `work/`.

## Known issues

- A still picture (the pause screen) still jitters slightly. The draw trace shows every deferred draw shifted but
  none of the forward draws into the scene target: most are bound to a different depth buffer than the primary
  pass (likely the first-person weapon and similar), the rest have depth testing off. XeSS removes a jitter those
  draws never had. The moving picture shows no visible jitter. See
  [Diagnosing the whole-scene jitter](#diagnosing-the-whole-scene-jitter).
- Motion is camera-only. Moving characters, weapons, particles, vegetation, and shader animation can ghost or
  shimmer.
- XeSS runs after tone-mapping, the SPU post chain (including motion blur) and the HUD, on display-referred color.
  Intel recommends the start of the post chain, before tone-mapping, and calls later placement experimental.
- UI and scene color are combined before XeSS. HUD pixels are masked and get zero motion, but they still pass through
  the upscaler. `KZ3_XESS_HUD_MASK=0` disables only that classification; it does not hide the HUD.
- The responsive mask is inferred from scene captures. It is not a semantic transparency or composition mask, and it
  carries the frame's jitter, which Intel's guide says it should not.
- Exposure is fixed for the LDR input; the game's exposure buffer is not integrated.
- The game still renders at 1280x720. XeSS does not increase geometry, shadow, or material resolution.
- Output must keep the 16:9 input aspect ratio. Ultrawide temporal output needs a separate policy.
- The primary scene projection mapping is runtime-validated for game version 1.14 only.
- Split-screen and multiple views bypass the temporal path.
- There is no in-game quality selector or debug visualization.

## Future work

1. Extend the jitter to the forward draws into the scene target that use another depth buffer, after identifying
   them, and check the pause screen again.
2. Publish coherent per-object and skeletal transforms and generate object motion vectors.
3. Separate scene and UI composition so the HUD can be drawn after temporal upscaling.
4. Replace the difference mask with semantic transparency/reactive information where the renderer exposes it.
5. Integrate stable scene exposure or pre-exposure if it improves image stability.
6. Add a user-facing quality policy after visual and performance measurements.
7. Validate more Intel, NVIDIA, and AMD GPUs and the supported Windows/Proton paths.
8. Investigate an ultrawide render-resolution policy without stretching the 16:9 temporal input.
9. Evaluate XeSS Frame Generation and Xe Low Latency only as separate opt-in experiments after XeSS-SR is stable.

## Intel references

Implementation behavior was written from the public
[Intel XeSS SDK](https://github.com/intel/xess), the
[XeSS SDK 3.0.2 release](https://github.com/intel/xess/releases/tag/v3.0.2), and Intel's
[XeSS-SR developer guide](https://github.com/intel/xess/blob/v3.0.2/doc/xess_sr_developer_guide_english.md).
No Intel source code is copied into the adapter.

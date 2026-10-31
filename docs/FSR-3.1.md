# Experimental FSR 3.1 implementation

Project Helghast has an opt-in experimental temporal upscaler built around AMD FidelityFX Super Resolution 3.1.
It uses the FSR provider `3.1.4` in FidelityFX SDK v1.1.4. This is an **ENHANCEMENT**: the PS3 game does not have
this presentation path, and Bilinear remains the default filter.

The current implementation is the first stage of the work:

- temporal upscaling is implemented;
- the game still renders its fixed 1280x720 image;
- motion vectors describe camera rotation only;
- the final scene, post-processing and UI are one color input;
- frame generation is not implemented.

The Options page labels the feature experimental and asks for confirmation before enabling it. A failed
prerequisite or dispatch falls back to Bilinear instead of preventing the game from starting.

## Architecture

The implementation deliberately keeps AMD's API and the KZ3-specific hooks outside the general ps3recomp
runtime. The KZ3 target defines `KZ3_FSR31`; a normal ps3recomp runtime build does not include the adapter or an
FidelityFX dependency.

```text
Recompiled game camera hooks
  -> current pose, effective FOV, frame/view id, near/far assumption
  -> port-owned camera bridge

RSX draw replay
  -> identify eligible primary display-target 3D draws
  -> remember the associated depth surface
  -> apply the current FSR jitter to eligible draws only

Presentation
  -> take a full-size R32_FLOAT depth snapshot
  -> generate R16G16_FLOAT camera-rotation motion vectors
  -> dispatch FidelityFX FSR 3.1 into an RGBA8 output
  -> apply the game's scan-out gamma
  -> use the existing display blit for Fit/Stretch and any final scaling
  -> present

Any missing or incoherent input
  -> log the reason
  -> restore the normal resource states
  -> use the Bilinear presentation path
```

### Components

| Component | Role |
|---|---|
| [`port/kz3/fsr31.cpp`](../port/kz3/fsr31.cpp) | Secure runtime loading, exact provider selection, FSR context and resource ownership, motion-vector compute shader and dispatch |
| [`port/kz3/fsr31.h`](../port/kz3/fsr31.h) | Stable C boundary between the C++ KZ3 code and the patched C renderer |
| [`port/kz3/fsr31_motion.h`](../port/kz3/fsr31_motion.h) | Platform-independent reference implementation of the camera-rotation reprojection math |
| [`port/kz3/main.cpp`](../port/kz3/main.cpp) | Camera/FOV publication, projection-plane assumption, settings persistence and Options-page commands |
| [`port/patches/2147-fsr31-upscaler.patch`](../port/patches/2147-fsr31-upscaler.patch) | Renderer-side jitter, depth association, dispatch, presentation ordering, UI rows and fallback |
| [`third_party/fidelityfx-sdk/`](../third_party/fidelityfx-sdk/) | Minimal unchanged MIT-licensed FidelityFX C API headers needed by the adapter |
| [`tools/release/stage_fidelityfx.py`](../tools/release/stage_fidelityfx.py) | Release-time validation and staging of AMD's official signed DirectX 12 DLL and licence |

## Frame pipeline

### 1. Camera publication

The existing camera writer hook publishes the final interpolated and late-latched camera pose after the port's
camera adjustments. The pose is row-major:

| Row | Meaning |
|---|---|
| 0 | right vector |
| 1 | forward vector |
| 2 | up vector |
| 3 | position |

The bridge also publishes the effective horizontal and vertical fields of view. The horizontal value includes the
port's Hor+ widescreen adjustment; the vertical value includes the corresponding Vert+ behavior for narrower
views. Pose and FOV are associated by camera/view pointer and guest flip number so values from different views are
not combined.

Two different view IDs in one guest flip set `KZ3_FSR31_CAMERA_MULTIPLE_VIEWS`. The renderer then bypasses FSR and
resets temporal history. This is the current split-screen safety behavior.

Camera history is reset when:

- there is no coherent immediately previous guest flip;
- the view ID changes;
- more than one view appears in the flip;
- camera position changes by more than 5 guest units; or
- the forward vectors differ by more than 1.5 radians.

### 2. Projection planes

The renderer needs near and far distances for the FidelityFX dispatch. The current hook reads the first two values
returned by `Camera::vf17`.

**ASSUMPTION:** those two fields are near and far. The surrounding frustum fields are known, but this mapping has
not yet been confirmed in a legal game run. Values are accepted only when the near plane is finite and between
`0.001` and `10`, and the far plane is finite, above `10`, and greater than the near plane. Anything else leaves
FSR unavailable for that frame.

The observed renderer behavior uses normal depth: depth clears to 1.0 and the usual comparisons treat smaller
values as nearer. The adapter therefore does not currently set FidelityFX's inverted-depth or infinite-depth
flags.

### 3. Draw jitter and depth association

Patch 2147 considers a draw eligible for FSR jitter when it:

- writes the current display target;
- uses a full 16:9 guest viewport;
- has a depth surface and depth testing enabled; and
- is not classified as a later sampled render-target composition pass.

For an eligible draw, the renderer remembers the depth slot used with that display surface. Jitter is applied only
after the DLL, provider, camera, projection and output context are ready. This ordering is important: a missing DLL
must not produce a visibly jittered frame followed by a non-temporal Bilinear fallback.

The jitter offset comes from FidelityFX's jitter query. It changes the viewport transform for eligible 3D draws;
UI and unrelated render targets are not jittered.

### 4. Depth snapshot

At presentation, the remembered depth surface is copied through the renderer's existing depth snapshot path into
an `R32_FLOAT` resource. Color and depth must both match the fixed render dimensions and the current renderer
frame. A stale, smaller or otherwise mismatched snapshot causes a Bilinear fallback.

The final color surface can contain later post-processing and UI while the snapshot represents the selected
primary 3D depth pass. That mismatch is one of the main current quality limitations.

### 5. Camera-rotation motion vectors

The adapter owns one render-resolution `R16G16_FLOAT` motion-vector texture. A small compute shader reconstructs a
world-space ray from the current camera basis and projects that ray into the previous camera basis:

```text
ray =
    currentForward
  + currentRight * currentNdc.x * tan(currentHorizontalFov / 2)
  + currentUp    * currentNdc.y * tan(currentVerticalFov / 2)

previousNdc.x =
    dot(ray, previousRight)
  / (dot(ray, previousForward) * tan(previousHorizontalFov / 2))

previousNdc.y =
    dot(ray, previousUp)
  / (dot(ray, previousForward) * tan(previousVerticalFov / 2))

motion = previousNdc - currentNdc
```

The current projection jitter is removed before ray reconstruction and is also supplied separately to
FidelityFX. The motion scale is `{renderWidth, renderHeight}`, following AMD's documented NDC-vector integration
convention:
[FidelityFX Super Resolution upscaler documentation](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/docs/techniques/super-resolution-upscaler.md).

Camera translation is deliberately excluded. Rotation can be reconstructed without scene depth, while translation
requires depth-aware per-pixel reprojection to avoid treating every pixel as if it were infinitely far away.
Object, animation, particle and shader-driven motion are also absent.

### 6. FSR dispatch

The adapter converts the caller's Direct3D 12 resources to FidelityFX API resources and supplies:

- final RGBA8 color at render resolution;
- the matching R32_FLOAT depth snapshot;
- camera-rotation motion vectors;
- render and output dimensions;
- jitter, frame time, near/far, vertical FOV and reset state;
- automatic exposure; and
- optional RCAS sharpness.

Reactive and transparency/composition resources are supported by the low-level adapter boundary but are currently
passed as null by the renderer.

Input color, depth and motion resources are in
`D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE`. The adapter-owned output starts as an unordered-access resource
and is returned in `D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE` for the display path.

### 7. Gamma and final display scaling

FSR runs before the existing scan-out gamma pass:

```text
scene color -> FSR -> scan-out gamma -> display blit -> swap-chain back buffer
```

This preserves the port's existing interpretation of `cellVideoOutSetGamma`: gamma is a display operation, not
part of the temporal input.

The requested FSR output is the aspect-correct Fit rectangle or the full Stretch rectangle. Each output dimension
is capped at three times the fixed render dimension. A larger window first receives at most a 3x FSR output and
then uses the existing Bilinear display blit for the remainder. Nearest and Bilinear behavior are unchanged when
FSR is not selected.

FSR is bypassed when the output is not larger than the render image or when either output dimension is smaller
than its input dimension.

## Runtime loading and provider selection

The release ships AMD's unmodified signed `amd_fidelityfx_dx12.dll`; no FidelityFX binary is committed to the
repository.

The adapter:

1. obtains the executable's absolute path;
2. appends `amd_fidelityfx_dx12.dll`;
3. loads only that adjacent path with
   `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`;
4. resolves the five required FidelityFX C API entry points;
5. queries all providers for the upscaler context type; and
6. selects the provider whose reported name is exactly `3.1.4`.

The provider ID is not hardcoded. After context creation, the adapter queries the chosen provider again and
rejects a context that does not report the required version.

One mutex serializes context creation, dispatch, resize and shutdown. The adapter retains the Direct3D 12 device
while a context exists. Before an output-size change destroys output-dependent resources, the renderer waits for
the previously submitted GPU work that may still reference them. Shutdown drains the renderer and clears both
adapter and renderer-side history.

## History resets and fallback

FSR history is reset after:

- the first successful dispatch;
- any skipped or failed FSR frame;
- a non-consecutive renderer frame;
- a display target change;
- a context/output-size generation change;
- invalid frame timing;
- a camera cut; or
- loss of coherent previous-camera state.

The following conditions explicitly use Bilinear:

| Condition | Behavior |
|---|---|
| DLL missing or fails to load | Log the Win32 error; no jitter; Bilinear |
| FSR 3.1.4 provider missing | Log available providers; no jitter; Bilinear |
| Output is not an upscale | Bilinear |
| Color target is not the primary RGBA8 image | Bilinear |
| Matching full-size depth snapshot is unavailable | Bilinear |
| Camera, FOV, frame ID or projection is incoherent | Bilinear |
| Multiple views are present | Bilinear and history reset |
| Context/resource creation fails | Log the FidelityFX or HRESULT status; Bilinear |
| Dispatch fails | Restore color/depth states, log the FidelityFX status, then Bilinear |
| Final display blit cannot be recorded | Refuse an incompatible `CopyResource`; keep the device from receiving an invalid copy |

Fallback messages start with `[fsr31] unavailable, bilinear fallback:` and are emitted when the reason changes.

## Settings and user interface

`display.ini` accepts:

```ini
[display]
filter = fsr31
fsr_sharpness = 0.2
```

`fsr_sharpness` is stored as a locale-independent value from `0.0` through `1.0`, rounded to the Options page's
`0.1` steps. `0.2` is the default. It affects only FSR.

One-run overrides are:

```text
KZ3_DISPLAY_FILTER=fsr31
KZ3_FSR_SHARPNESS=0.0..1.0
```

The filter row accepts `fsr31` and `fsr3.1`; the shorter ambiguous value `fsr` is rejected. Selecting FSR in the
Options page opens a confirmation dialog that names the camera-only motion and combined scene/UI limitations.
The label, warning and tooltip are present in all 19 supported UI languages.

## Release and licensing

[`tools/release/toolchain.json`](../tools/release/toolchain.json) is authoritative for the FidelityFX SDK archive
version, URL and SHA-256. The release fetcher extracts only:

- `PrebuiltSignedDLL/amd_fidelityfx_dx12.dll`; and
- `docs/license.md`.

[`tools/release/stage_fidelityfx.py`](../tools/release/stage_fidelityfx.py) checks the DLL's exact size and hash,
its AMD64 PE/DLL identity, and the licence hash and identifying text. The Windows release workflow additionally
requires a valid Authenticode signature whose certificate subject names Advanced Micro Devices. Payload assembly
checks the DLL hash again.

The installer and portable archive carry the runtime under `runtime/` and its MIT text under `licenses/`.
First-run and launcher code copy the DLL beside `kz3.exe`, which is the only location the adapter loads. The
runtime performs no network access. Licence and attribution details are in [`NOTICE.md`](../NOTICE.md) and
[`licenses/README.md`](../licenses/README.md).

## Tests and current evidence

| Test | Coverage |
|---|---|
| [`tools/smoke/test_fsr31_motion.c`](../tools/smoke/test_fsr31_motion.c) | Identity, known yaw, deliberate translation exclusion, FOV change and invalid input |
| [`tools/smoke/test_display_mode.c`](../tools/smoke/test_display_mode.c) | Filter aliases, Fit dimensions, sharpness parsing/persistence, warning dialog, reset and display bridge |
| [`tools/smoke/test_fsr31.cpp`](../tools/smoke/test_fsr31.cpp) | Real D3D12 device with WARP fallback, exact provider query, external dispatch, adapter-owned motion dispatch, output readback, jitter and missing-DLL fallback |
| [`tools/release/test_stage_fidelityfx.py`](../tools/release/test_stage_fidelityfx.py) | DLL/licence staging and rejection of wrong architecture, type, hash or licence |
| Release host tests | Payload, launcher, first-run, portable ZIP, licence inventory and uninstall coverage |
| Patch-series build | Patch 2147 applies after the complete numbered series; the FSR-guarded renderer translation unit builds |

The native Windows probe has successfully dispatched both external-resource and adapter-owned camera-motion paths
with the pinned FSR 3.1.4 provider, read back a non-empty 640x360 RGBA8 output, and confirmed that removing the
adjacent DLL produces the expected unavailable status.

On 2026-10-31, a game executable built from this branch ran natively on Windows at 1920x1080 with `filter fsr31`
from the main menu through the first campaign level on an NVIDIA GeForce RTX 5070 with Windows driver
32.0.16.1742. The runtime log confirmed the selected filter and the expected Bilinear fallback in menu frames
without matching depth; the user confirmed FSR worked during gameplay without a crash. This is an initial
functional check rather than broad image-quality coverage.

Not yet tested:

- a Proton/vkd3d game run;
- the standalone probe under Proton/vkd3d;
- systematic visual quality across gameplay, movies, menus, particles and loading screens;
- cross-vendor coverage beyond the one default native adapter used by the probe; or
- device recovery during a game run.

This evidence supports experimental availability, not a broader feature-quality or platform-support claim.

## Known issues and limitations

1. **Camera-only motion.** Translation, moving characters, animation, particles, vegetation and shader motion have
   no vectors. These pixels can ghost or lose detail.
2. **Combined scene and UI.** HUD, menus, subtitles and text are already in the color input. They have no matching
   motion and can shimmer or leave trails.
3. **No reactive or transparency/composition masks.** Transparencies, particles and disocclusions do not receive
   the hints FSR normally uses to reduce temporal artifacts.
4. **Color/depth mismatch.** The color input includes late post-processing while depth comes from the selected
   primary 3D pass. Depth of field, bloom, motion blur and composited effects may not correspond to that depth.
5. **Projection fields are not fully verified.** Near/far extraction from `Camera::vf17` remains an assumption.
   Strict validation makes bad values a fallback, but plausible wrong values are still possible.
6. **No FSR-specific mip bias.** Primary scene sampling is not biased for the lower input resolution, which can
   reduce reconstructed texture detail or increase shimmer.
7. **Fixed internal resolution.** FSR scales the final 1280x720 image; it does not make the game render at a
   configurable internal resolution.
8. **Split-screen bypass.** Multiple views in one guest flip are intentionally refused rather than sharing one
   temporal history.
9. **Movies, menus and loading screens can fall back.** Frames without a coherent primary 3D camera and matching
   depth use Bilinear by design.
10. **Outputs above 3x use two filters.** FSR stops at the 3x cap and the existing Bilinear pass performs the final
    enlargement.
11. **Platform and game quality coverage is preliminary.** One native Windows gameplay confirmation and a successful
    synthetic dispatch do not establish acceptable image quality across scenes, GPUs or Proton.
12. **No frame generation.** This implementation does not create a proxy swap chain, generate intermediate
    frames or change presentation pacing.

## Future work

### 1. Validate the current upscaler

- Run the standalone probe under Proton/vkd3d.
- Repeat native Windows coverage and run the game on Proton at several Fit and Stretch sizes.
- Exercise resize, borderless, exclusive fullscreen, Alt+Enter, device recovery and shutdown.
- Inspect camera cuts, level loads, menus, movies, particles, moving characters, weapons and split-screen.
- Verify the near/far fields against runtime evidence.
- Compare Bilinear and FSR captures for ghosting, shimmer, post-process mismatch and sharpness.
- Confirm fallback frames are never left jittered.

### 2. Improve temporal inputs

- Separate the primary scene from HUD/UI and composite UI after FSR.
- Generate reactive and transparency/composition masks.
- Add depth-aware camera translation.
- Investigate object and animation motion from engine or draw state.
- Apply a primary-scene mip bias appropriate for the upscale ratio.
- Identify post-processing stages whose color and depth must be captured together.
- Add quality tests with repeatable camera paths and image comparisons.

### 3. Broaden robustness and support

- Validate AMD, NVIDIA and Intel hardware.
- Add Proton/vkd3d coverage to the release gate.
- Expand resize and device-loss automation.
- Keep fallback reasons actionable and rate-limited.
- Revisit camera-state buffering if logs show the PPU producer and renderer consumer advancing by different frame
  counts.

### 4. Consider frame generation only after the upscaler passes

Frame generation is a separate project, not a small extension of the current dispatch. It requires a proxy
swap-chain path, prepare/configure dispatches, UI-safe composition, presentation scheduling and measured latency.
Before it is exposed:

- proxy presentation must work on both native Windows and Proton/vkd3d;
- generated frames must never block the game's frame-clock thread indefinitely;
- the default must remain off;
- a 30-to-60 option must be warning-gated;
- low or unstable input frame rates need bounded hysteresis and pass-through behavior; and
- camera cuts, menus, loading, resize, fullscreen changes, Alt+Tab and shutdown must all reset or bypass safely.

If those gates are not met, frame generation remains unshipped while temporal upscaling can continue independently.

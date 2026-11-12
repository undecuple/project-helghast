# Experimental XeSS Super Resolution

Project Helghast has an opt-in Intel XeSS Super Resolution (XeSS-SR) backend. It is an **ENHANCEMENT**: Killzone 3
renders at 1280x720 and has no temporal upscaler. The ordinary Bilinear presentation path remains the default and is
the explicit fallback whenever XeSS cannot run.

The integration targets Intel XeSS SDK **3.0.2**, whose XeSS-SR API reports **2.0.2**. It does not use XeSS Frame
Generation or Xe Low Latency.

## Enabling it

Choose **Options > Display > Upscaler > XeSS (experimental)** and accept the warning. Upscaling is separate from
**Scaling filter**, which remains Bilinear or Nearest.

The choice is saved in `display.ini`:

```ini
[display]
filter = bilinear
upscaler = xess
upscaler_previous = fsr3
```

`upscaler_previous` is written when XeSS is selected. If its optional runtime cannot be installed or validated, the
launcher restores that backend (`none` or `fsr3`) instead of leaving a broken XeSS selection active. A selection made
while the game is running also keeps the current backend active until the next launcher start when the runtime is
missing.

For a one-run developer override:

```powershell
$env:RSX_UPSCALE = "xess"
```

XeSS runs only when the output is larger than the render image, has the same aspect ratio within one output pixel of
rounding, and does not exceed Intel's 3x scale limit. The backend chooses XeSS's quality setting from the requested
scale.

Optional developer controls:

| Variable | Default | Effect |
|---|---|---|
| `KZ3_XESS_MIP_BIAS` | Intel's `log2(input/output)` | Scene sampler LOD bias in mip levels; `0` disables it |
| `KZ3_XESS_CAMERA_LAG` | `0` | Whole guest flips to step back from the renderer-frame camera pairing; a negative value uses the latest camera |
| `KZ3_XESS_HUD_MASK` | on | `0` disables the HUD mask |
| `KZ3_XESS_INPUT_DELAY` | on | `0` disables the measured one-frame camera/depth/capture alignment |

## Optional runtime download

`libxess.dll` is **not** shipped in the installer or portable archive. When `upscaler = xess` is selected and a valid
runtime is not already beside `kz3.exe`, the launcher:

1. explains the download, its source, size, installed files, and checks;
2. asks for explicit Yes/No consent before making a network request;
3. downloads Intel's official
   [XeSS SDK 3.0.2 archive](https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip);
4. verifies the archive size and SHA-256;
5. extracts only `bin/libxess.dll`, `LICENSE.txt`, and `third-party-programs.txt`;
6. verifies each file's size and SHA-256;
7. checks that the runtime is an AMD64 PE DLL;
8. requires a valid Intel Authenticode signature and the pinned signing-certificate thumbprint;
9. checks for Microsoft Visual C++ Redistributable x64 14.40 or later; and
10. stages all files and replaces the DLL last, rolling back if installation fails.

Nothing is downloaded when XeSS is not selected or the player chooses No. A declined download, network error, changed
archive, invalid signature, missing VC++ runtime, extraction error, or installation error restores
`upscaler_previous` and starts the game with that backend.

Current pins:

| Item | Bytes | SHA-256 |
|---|---:|---|
| `XeSS_SDK_3.0.2.zip` | 76,839,749 | `88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990` |
| `libxess.dll` | 77,795,704 | `251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7` |
| `Intel-XeSS-LICENSE.txt` | 4,194 | `784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df` |
| `Intel-XeSS-THIRD-PARTY-NOTICES.txt` | 35,788 | `e8d8899dbfc4de0e8ca25560be9358e03c1405d7c956d20a1838c3c709b9b51c` |

The pinned signer is `CN=Intel Corporation` and the certificate thumbprint is
`2B81BFC21940CD420FC263C975FB41A79B643DF4`. Updating the SDK requires an explicit source, archive, file, API, and
certificate-pin review.

## Architecture

XeSS uses the same registered-upscaler contract as FSR 3.1:

```text
ps3recomp generic renderer
  -> one operation table per registered upscaler backend
  -> one draw-hook table per registered backend
  -> render/output sizes, target/depth/MRT state and resource handles
  -> actions: select depth, apply projection jitter and signed 4.8 mip bias,
              capture after draw, suppress host AA before scene writeback

port/kz3 XeSS bridge
  -> all Killzone 3 offsets and pass recognition
  -> renderer-frame to guest-flip camera pairing
  -> immediate selected-depth snapshot
  -> opaque, pre-upscale and first-display-compose captures
  -> camera-derived motion, responsive mask and HUD mask
  -> one-frame input alignment and history-reset policy

port/kz3 XeSS adapter
  -> exact adjacent runtime loading and API check
  -> D3D12 resources and motion/mask compute passes
  -> XeSS-SR dispatch and output resource
```

The generic ps3recomp patch contains no Killzone 3 offsets, `KZ3_XESS` build define, Intel ABI, runtime header, or
XeSS-specific pass selection. FSR 3.1 and XeSS can register simultaneously; selecting one backend activates only its
operation and hook tables.

Draw inspection runs before sampler-table construction because Intel's scene mip bias must be part of the sampler
cache key. Descriptor-ring wrapping can replace the active command list while a draw is prepared, so post-draw
callbacks receive refreshed command-list and resource handles.

## Inputs and frame alignment

### Depth and motion

The bridge selects the structural full-size deferred pass with at least four MRTs and snapshots its R32 depth
immediately after that draw. The adapter reconstructs current-to-previous NDC velocity from:

- the selected depth;
- current and previous KZ3 camera poses;
- horizontal and vertical FOV;
- validated near/far planes; and
- the projection jitter applied to the scene.

This is camera-only motion. Moving characters, weapons, vegetation, particles, animated materials, and deformed
geometry do not have per-object velocity.

### Camera pairing

The game publishes camera states by guest flip, while the renderer counts completed presentation frames. The bridge
tracks the smallest recent offset between those counters and selects the camera for the renderer frame being built.
A repeated guest flip is treated as a held camera. A skipped flip resets temporal history.

### Responsive and HUD masks

The bridge captures:

- the opaque deferred resolve;
- the same scene surface immediately before its SPU writeback; and
- the first full display composition after a depth draw, before later HUD draws.

The adapter compares the coherent opaque/pre-upscale pair for its responsive mask. The first display composition is
used to identify HUD pixels, which receive zero camera motion and a fully responsive value.

### One-frame alignment

The finished display image carries the prior scene through KZ3's SPU post chain and composition. After consecutive
successful dispatches, the adapter therefore aligns the prior camera, depth, jitter, and scene captures with the
current finished color. A discontinuity uses current inputs and resets history.

## Presentation and fallback

Temporal evaluation occurs before scan-out gamma. A successful output-sized XeSS image is sampled 1:1 with point
sampling for the final copy. If any prerequisite fails, the renderer:

- restores the input resource state;
- reports the adapter status;
- explicitly presents with Bilinear, even if the spatial filter is Nearest; and
- holds projection jitter off after a failed jittered dispatch until a dispatch succeeds.

Backend switching and device-loss cleanup drain prior GPU work before destroying XeSS resources.

## Validation

Contributor-side checks completed for the redesigned path:

- generic renderer translation unit compiles with Clang;
- XeSS adapter compiles with MSVC and strict MinGW warnings;
- FSR 3.1 and XeSS operation tables coexist in the host policy test;
- separate Upscaler UI, warning, tooltip, and reset behavior pass the display-mode test;
- the launcher installer tests cover no selection, consent declined, network failure, archive/hash failure,
  signature failure, missing VC++ 14.40, successful installation, revalidation, and no network before consent;
- Intel's real 76,839,749-byte archive passes archive, file, PE, Authenticode signer, and certificate checks;
- the registered XeSS operation table dispatches on a hardware D3D12 adapter at 1280x720 -> 1920x1080;
- readback reports 2,073,600 / 2,073,600 non-black pixels;
- responsive-mask, HUD-mask, camera-motion, independent execute-jitter, missing-DLL, and wrong-DLL probes pass.

Still required before calling the visual result validated:

- an off/on screenshot pair from the same gameplay frame;
- XeSS pass time per frame on named hardware;
- current still-scene and pause-screen jitter observations;
- a full game build with lifted KZ3 output; and
- broader gameplay, GPU, driver, and Proton coverage.

The older direct-renderer prototype reached gameplay and was steady in moving scenes after camera/input alignment.
It still showed slight pause-screen jitter. That evidence does **not** validate this registered-backend redesign and
must be repeated.

## Known limitations

- Camera-only motion cannot correctly track independently moving objects.
- The game has no separate UI layer at presentation; masking reduces, but cannot eliminate, temporal treatment.
- XeSS runs on display-referred color after the game's post chain.
- Split-screen or conflicting camera views are rejected.
- The game still renders geometry, shadows, and materials at 1280x720.
- XeSS requires Windows D3D12, the optional Intel runtime, and VC++ x64 14.40 or later.

## Developer test

Extract Intel's official SDK archive outside the repository, apply the port patches to ps3recomp, then run:

```powershell
tools\smoke\test_xess.ps1 -Dll C:\path\to\XeSS_SDK_3.0.2\bin\libxess.dll
python tools\release\test_xess_runtime.py
```

The PowerShell test verifies the DLL hash before copying it beside the test executable. No Intel binary is committed
or added to a release payload.

## References

- [Intel XeSS SDK](https://github.com/intel/xess)
- [XeSS SDK 3.0.2 release](https://github.com/intel/xess/releases/tag/v3.0.2)
- [XeSS-SR developer guide](https://github.com/intel/xess/blob/v3.0.2/doc/xess_sr_developer_guide_english.md)

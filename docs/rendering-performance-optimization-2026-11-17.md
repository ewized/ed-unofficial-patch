# Rendering performance optimization — 2026-11-17

## Status

- Implements findings 1, 2, 5 and 6 from the [performance audit](performance-audit-beyond-2026-11-16.md).
- Exact RGBA8 FG input bypasses the redundant normalization copy; immutable interop capture remains immediate.
- HUD snapshots reuse three bounded storage slots per eye. Interop retains their owners until existing GPU fences and leases retire.
- Authoritative empty UI images are initialized once per allocation instead of cleared per source frame.
- VRS Off skips native classification and projection preparation while retaining full-rate reset/recovery boundaries and independent diagnostics.
- Portable checks pass. Native differential regressions, full Windows validation and controlled VR movement comparisons are pending.
- No game frame-time improvement or 4000-per-eye/90 Hz result is claimed at this stage. The HUD remains separate from interpolation.

## Changes and invariants

`FrameGenerationColor::normalize` returns the borrowed source only for exact
`R8G8B8A8_UNORM`, after the existing owner, device, scratch-alias and descriptor
checks. `nativeFgCaptureWorld` immediately enqueues the immutable interop copy
before source reuse. Typeless, sRGB, BGRA and floating-point conversion routes
retain their previous behavior. Removing one RGBA8 copy per eye avoids
128,000,000 logical texel bytes per stereo source frame at 4000 by 4000; this
arithmetic is neither measured bandwidth nor an FPS result.

`FrameGenerationColor::emptyUi` initializes each private eye/identity image
after successful view creation. Repeated reads require no clear. Resize or a
failed view allocation still initializes the next successful image. Consumers
must treat these borrowed identity planes as read-only.

The UI capture adapter keeps fresh publication metadata and pools only copied
texture/SRV storage. A slot is reusable only when its cache has exclusive
ownership and the texture and SRV descriptors match. Three held slots cause
native fallback rather than overwriting an image. The interop copy retains the
snapshot owner through producer completion, consumer completion and external
lease retirement, including aborted captures. No new GPU waits are introduced.
Raw and empty captures also avoid the compute-state guard needed by SMAA.

`nativeVrsCoarseRequested` reads the existing configuration flag. Off skips
native draw classification and projection preparation, but sticky
`nativeVrsLive` still protects full-rate boundaries and driver-error recovery.
Live render-target/viewport getters remain: their removal needs a complete
binding-mutation epoch and is outside this change.

## Validation plan and current evidence

The new native color and UI fixtures must run against the unfixed production
code first, fail at their explicit copy/clear/allocation assertions, and then
pass against the candidate. Compiler errors do not qualify as baseline failure.
Color tests retain format conversion, state restoration, descriptor/owner
refusals and failed-allocation retry. UI tests retain every pixel of leased
layer/multiply planes across reuse, exhaustion, resizing and device replacement.
The NVIDIA interop test adds exact combined normalization/capture pixels after
source mutation and source-owner retention across pending GPU retirement.

A separately compiled fixture containing the production VRS draw bodies passes
12 assertions with the gate and fails four against the baseline; removing
either gate fails two. Across 256 On-then-Off draws, counted dependencies drop
from 512 shader reads, 256 depth classifications and 16 geometry getter calls
to zero. These dependencies are stubs, not native GPU or CPU-duration evidence.
Existing full-rate boundaries remain exercised.

The live comparison will use matched first-tutorial scenes, the same render
settings and repeatable smooth/rapid head and gaze motion. Compare CPU/GPU
frame-time percentiles, spikes, FG delivery/refusals and VRS admission. Record
native and generated cadence separately; virtual runtime submissions do not
establish physical display scanout or calibrated eye tracking.

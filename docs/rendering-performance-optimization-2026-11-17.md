# Rendering performance optimization — 2026-11-17

## Status

- Implements findings 1, 2, 5 and 6 from the [performance audit](performance-audit-beyond-2026-11-16.md).
- Compatible typed/typeless RGBA8 FG input bypasses the redundant normalization copy; immutable typed interop capture remains immediate.
- HUD snapshots reuse three bounded storage slots per eye. Interop retains their owners until existing GPU fences and leases retire.
- Authoritative empty UI images are initialized once per allocation instead of cleared per source frame.
- VRS Off skips native classification and projection preparation while retaining full-rate reset/recovery boundaries and independent diagnostics.
- Differential native regressions pass: five old-behavior controls fail their intended assertions; current source passes 672 color, 557 UI and 4,626 NVIDIA-sharing checks.
- Current source `54021cfd` passed the full native Windows build: 214 native jobs, both Python gate groups, focus guard and installer checks. Native VRS passes 130 checks, including WARP query retirement and the new draw-scope cases.
- Live baseline confirms DXGI 27 typeless RGBA8 input. Source `8d725293` extends the copy shortcut to that path; `54021cfd` prevents old producers from stranding the HUD pool during replacement. Differential checks and the full native build pass.
- Both builds completed all nine 75-second synthetic motion phases with clean command delivery and cleanup. Thirteen baseline and twelve candidate 30-second benchmark windows cover every phase.
- Live timing remains around 30–32 ms GPU p50. Scene alignment is approximate, so no causal game frame-time improvement or 4000-per-eye/90 Hz result is claimed. The HUD remains separate from interpolation.

## Changes and invariants

`FrameGenerationColor::normalize` borrows `R8G8B8A8_UNORM` and
`R8G8B8A8_TYPELESS`, after the existing owner, device, scratch-alias and descriptor
checks. `nativeFgCaptureWorld` immediately enqueues the immutable interop copy
before source reuse. Only the Color plane admits typeless RGBA8, creating actual
UNORM D3D11/D3D12 storage and publishing a UNORM descriptor while retaining the
original object identity. sRGB, BGRA and floating-point routes retain their
previous behavior. Removing one RGBA8 copy per eye avoids
128,000,000 logical texel bytes per stereo source frame at 4000 by 4000; this
arithmetic is neither measured bandwidth nor an FPS result. A once-per-producer
per-eye successful-capture diagnostic records the actual source, preparation
path and immutable format. The live baseline identifies typeless RGBA8 as the
game submit format; internal DLSS output alone would not establish that.

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
The active pool retains up to three layer/multiply sets per eye; memory depends
on captured sizes and formats. At producer replacement, both eye caches validate
ownership before dropping only their references to externally held storage.
Old interop owners retain those pixels until retirement. Exclusive free planes,
SMAA and context/thread/device bindings remain cached. Retired generations can
remain live under the existing fence/quarantine policy; three active slots per
eye is not a global memory cap.

`nativeVrsCoarseRequested` reads the existing configuration flag. Off skips
native draw classification and projection preparation, but sticky
`nativeVrsLive` still protects full-rate boundaries and driver-error recovery.
Live render-target/viewport getters remain: their removal needs a complete
binding-mutation epoch and is outside this change. Pending PS diagnostic queries
still receive owner-only service when a draw has no eligible world decision.
The empty service draw cannot begin a query or read shader/geometry metadata.

## Validation and current evidence

The initial native color and UI controls use frozen `17c21db6` fixtures against
the unfixed production code and fail explicit copy/clear/allocation assertions.
The candidate passes the corresponding regressions plus the current extended
fixtures on native Windows. All three baseline failures are runtime assertions
with exit 1; no compiler error counted as red.
Color tests retain format conversion, state restoration, descriptor/owner
refusals and failed-allocation retry. UI tests retain every pixel of leased
layer/multiply planes across reuse, exhaustion, resizing and device replacement.
The NVIDIA interop test adds exact combined normalization/capture pixels after
source mutation and source-owner retention across pending GPU retirement.

The typeless control replaces only the color implementation with `17c21db6`
and runs the current tests. It fails at format 27 with one normalization copy.
The HUD handoff control replaces only the adapter header with `8d725293`; its
first replacement capture fails while the old configured producer remains alive.
Current NVIDIA tests also cover the exact completed-work state with no external
old leases, pending GPU work, repeated replacement/resize and retained pixels.
All five controls fail with exit 1 at their expected runtime assertions.

A separately compiled fixture containing the production VRS draw bodies passes
12 assertions with the gate and fails four against the baseline; removing
either gate fails two. Across 256 On-then-Off draws, counted dependencies drop
from 512 shader reads, 256 depth classifications and 16 geometry getter calls
to zero. These dependencies are stubs, not native GPU or CPU-duration evidence.
Existing full-rate boundaries remain exercised. Nine additional checks compile
the actual production PS draw scope against counted dependencies: the initial
gate and a no-service mutant each fail seven; the corrected scope passes all
nine. Existing WARP cases separately exercise actual query retirement.

Both builds ran the first Pilot Assessment after Auto Launch with zero throttle,
4000-square output per eye, 2320-square DLSS input, preset K, fixed NVIDIA FG 2x,
eye-tracked VRS on and turbo off. Fixed 2x makes the capture path repeatable;
this does not qualify auto mode or compare turbo. The exact original INI bytes
are retained separately from this temporary test configuration.

The same nine phases cover neutral, smooth head movement, abrupt head reversals,
smooth gaze, abrupt gaze reversals, optical-origin offsets, eye loss/recovery,
head loss/recovery and recentering. Every phase has at least one complete
30-second native sample in both builds, wholly inside its actual phase boundary
with clock uncertainty included. Percentiles remain per-window; they are not
pooled or inverted into FPS. Candidate focus interruptions are excluded. The
keeper's only focus recovery occurred before the timed candidate run.

| Phase | Baseline CPU / GPU p50, ms | Candidate CPU / GPU p50, ms |
|---|---|---|
| Neutral | 6.548 / 30.369 | 7.925–8.242 / 29.988–30.264 |
| Smooth head | 6.634–7.316 / 29.919–30.568 | 7.658 / 30.617 |
| Abrupt head | 3.973 / 30.506 | 5.068 / 31.727 |
| Smooth gaze | 5.990 / 30.963 | 7.324–7.382 / 30.040–30.065 |
| Abrupt gaze | 5.951–6.681 / 30.582–31.072 | 7.236 / 30.533 |
| Optical origins | 6.204–6.410 / 30.794–31.042 | 7.085 / 30.608 |
| Eye loss/recovery | 6.458 / 30.743 | 6.994 / 30.535 |
| Head loss/recovery | 6.153 / 30.837 | 6.699–6.990 / 30.434–30.606 |
| Recenter | 5.510–6.293 / 30.531–31.175 | 6.891 / 30.347 |

Autolaunch left different ship roll. Subsequent alignment was approximate;
planet/ring and station placement, exposure and animation visibly differ.
These are descriptive observations under matching native metadata, not a
controlled estimate of code benefit. Candidate CPU p50 is higher in all nine
chronological pairs by 0.342–1.694 ms; the scene difference prevents attributing
that adverse observation to a code regression. Candidate GPU p95 spans
32.603–33.604 ms and p99 spans 33.678–34.469 ms. GPU measurements are elapsed
query intervals, not exclusive GPU busy time. The deterministic reductions in
copies, clears, allocations and classification calls stand independently.

Outside abrupt-head stress, admitted FG windows have zero busy, refused,
repeated and empty deltas, without history changes. Abrupt-head stress still
produces camera refusal and backpressure in both builds. Phase-wide refused
pairs are 408 / 121, busy pairs 204 / 6 and repeated presentations 277 / 278
(baseline / candidate). History changes make these audit-only totals; the lower
refusal count does not establish a causal stability improvement. No empty
frames were observed. The available diagnostic identifies the Camera failure
class but does not resolve its exact predicate. No accepted-window long-frame
or long-cycle events were logged; those throttled streams cannot prove there
were no spikes.

Available VRS windows show no driver, context, frame or target errors. Eye-loss
windows contain 49 / 50 invalid pairs, consistent with the exercised loss and
recovery. One candidate smooth-gaze window has no comparable VRS aggregate;
missing data is not a failure. Separately observed runtime optical origins
retain 64 mm IPD. Live candidate capture diagnostics confirm both eyes use
source format 27, borrowed typeless preparation and immutable format 28.

The GPU timing range remains above the 11.111 ms native 90 Hz budget and the
22.222 ms source budget for 2x at 90 Hz. App source and generated submissions
must remain separate. Virtual 90 Hz metadata and submission counters do not
establish physical display scanout, motion-to-photon latency or calibrated
eye tracking. Stable 4000-per-eye/90 Hz remains unqualified.

A subsequent [natural idle check](headless-vr-testing.md) uses a new coordinated
head/gaze sampler and therefore remains separate from those nine-phase trials.
Forty-second and sixty-second clips both passed runtime motion, fixed-IPD,
absolute lifetime and cleanup checks. During the full clip, sampled FG has no
busy/refused/repeated/empty deltas, while VRS admits 900 valid pairs and 26,080
world draws with zero driver errors. Logged long Present intervals remain
57.5–65.5 ms; this is not a stable 90 Hz result. Clock-expanded boundaries admit
no complete 30-second benchmark window in that clip. Original game/mirror INI
and keyboard bytes were restored after normal game/runtime shutdown. The
installed renderer and new reusable idle capability remain available.

# Beyond VR performance audit — 2026-11-16

## Status

- Independent read-only source audit of rendering candidate `4882db85`, as
  carried by branch head `32b2ad26`; comparison base `accc1207`.
- Eight opportunities are ranked below. Repeated operations are source
  observations; their contribution to game frame time remains unmeasured.
  No rendering changes or claimed FPS gains accompany this audit.
- [Headless testing](headless-vr-testing.md) now provides virtual tracking and
  capture verification. Its first live pass was SteamVR-only, with no game
  scene, so it does not validate these rendering hypotheses.
- First experiments: count canonical-color copies and UI allocations; exercise
  adjacent diagnostic draw ordinals before treating a low-PS sample as proof
  of low world shading. Preserve recently qualified HUD ownership/composition.
- Runtime/GPU regression tests, exact-build validation, matched game windows,
  and physical-headset checks remain required for any rendering change.

## Findings and tests

### 1. Canonical frame-generation color takes two copies

[`normalize`](../src/d3d11/frame_generation_color.cpp) around line 125 copies
exact `R8G8B8A8_UNORM` input into scratch. The coordinator around line 704 then
calls immutable interop capture, which performs another `CopyResource` in
[`frame_generation_interop.cpp`](../src/d3d11/frame_generation_interop.cpp)
around line 270. This happens per eye on active-FG source frames. Auto at
native 1x does not imply this cost.

A direct canonical-RGBA8 capture could remove one stereo copy: at 4000x4000,
128 MB of logical texels per source frame. This is arithmetic, not measured
bandwidth or frame-time savings. Keep descriptor/device checks, immediate
immutable capture, and the existing conversion routes for BGRA, typeless and
sRGB until separately proved.

Measure input format, normalization/interop copy counts and GPU capture times.
A GPU regression must prove byte-identical output and retained snapshot pixels
after source mutation; noncanonical inputs must preserve conversion behavior.

### 2. UI snapshots allocate full-resolution storage each source frame

[`copyPlane`](../src/d3d11/ui_layer_fg_adapter.h) around lines 143–155 creates
a texture and SRV per call. `capture` around lines 200–218 calls it for layer
and optional multiply planes: two to four stereo texture/SRV creations per
source frame with UI, before the later interop copy. SMAA scratch is already
cached and should not be counted as repeated allocation.

Test a bounded owner-thread pool whose slots return only after all retained
snapshot owners and relevant GPU work retire. Initially retain the deep copy;
removing it would change the ownership contract. Measure allocations, pool
misses, live bytes and CPU capture duration. Hold old snapshots across new
captures, resize and device changes; verify unchanged pixels and fallback on
pool exhaustion rather than overwriting a leased image.

### 3. Map reuse still pays binding queries per admitted world draw

[`prepareMap`](../src/d3d11/native_vrs.cpp) around lines 237–253 performs
`OMGetRenderTargets` and `RSGetViewports` before returning for same-eye/frame
reuse. Repeated admitted draws therefore retain driver getters and COM
reference operations even though rate-map generation/upload is already cached.

First count these getters and successful admission reuse per frame. A cache
would need an actual binding/viewport mutation epoch; classification shadows
alone are insufficient. Private pane scopes, reissues, command-list execution
and foreign mutations must immediately invalidate or force full rate. This is
higher risk than the color-copy experiment. Regression cases must cover those
mutations as well as unchanged draw runs.

### 4. Diagnostic discovery can repeatedly miss adjacent useful draws

[`Discovery`](../src/d3d11/native_vrs_ps_measure.h) around line 29 advances its
starting ordinal by 16 modulo previous draw count. `Recorder` around lines
137–157 issues at most one query per eye/frame and recycles rejected candidates.
A fixed stride can repeatedly query only a subset of stable draw ordinals.

A synthetic transcription of this scheduling/rejection policy with 40 stable
eligible draws queried `{0,8,16,24,32}`. Useful ordinal 24 was reached at frame 5;
useful ordinal 25 was still unseen after 600 frames. This is synthetic/source
evidence, not execution of the production WARP fixture and not proof of the
cause of the previous inconclusive flight. The existing
[WARP fixture](../tools/native_vrs_test/ps_measure_cases.h) around lines
103–107 chooses useful ordinal 24, aligned with that traversal.

First extend the real fixture to stable tiny-prefix identities and adjacent
useful ordinals including 25; demonstrate failure before changing selection.
Then test within-window rotation or a traversal avoiding fixed-stride aliasing.
Retain the 16-metadata/one-query-per-eye/frame budgets, useful-candidate priority,
pending-query ownership, triple matching and bounded drain. Log sampled
ordinals and useful discovery before making a world-shading conclusion.

### 5. VRS Off retains coarse-preparation work after initialization

[`nativeVrsLive`](../src/d3d11/native_vrs.h) intentionally remains true after
initialization so full-rate reset/recovery stays reachable. Consequently
[`observedDraw`](../src/d3d11/vscreen.cpp) around line 4692 still classifies
world draws, and `nativeBeforeOriginal` around line 1058 prepares projection
before disabled rejection in `native_vrs.cpp` around line 386.

Measure cold Off against On-then-Off: classification counts, geometry reads and
draw-path CPU time. A separate cheap coarse-activation gate could skip that
preparation while retaining full-rate boundaries and owning-frame reset retry.
Do not clear the live latch. Regress failed reset, permitted retry ordinals,
fresh admission after re-enable, and no coarse preparation while Off.

### 6. Cached immutable empty UI planes are cleared repeatedly

[`emptyUi`](../src/d3d11/frame_generation_color.cpp) around line 153 caches its
textures but calls `ClearRenderTargetView` each request. The coordinator around
line 683 requests identity multiply coverage when no multiply plane exists.

Test initializing once per allocation/resize, keeping the planes private
read-only inputs thereafter and preserving authoritative-empty admission.
Count clear calls and measure GPU time; driver fast clears may make savings
small. Regress both eyes, repeated requests, resize and alternating identity
and transmittance planes against exact expected pixels.

### 7. Worker completion detection polls at millisecond granularity

The [`worker`](../src/d3d11/frame_generation_coordinator.cpp) around line 347
uses `Sleep(1)` while awaiting completion. Scheduler-dependent detection delay
can increase source age; its actual magnitude is unknown.

Measure fence completion to output submission, wakeups, source age, repeats
and `busyPairs`. An interruptible worker-only fence event wait could preserve
timeouts/suppression while reducing polling delay. Never wait on render/XR
owners. Regress completion races, device removal, configuration changes,
suppression, shutdown and every lease-retirement path.

### 8. An idle warmed worker wakes every two milliseconds

The same coordinator around line 281 uses a timed 2 ms wait with no queued
work. Auto may retain the worker after returning to native 1x.

Measure wakeups and CPU during sustained native 1x after an FG interval.
Indefinite condition-variable sleep is safe only when no backend/output
retirement still needs servicing; otherwise keep bounded servicing or explicit
completion wakeups. Regress new-source notification, close and outstanding
consumer retirement. Expected benefit is reduced background work, not an
assumed increase in game FPS.

## Existing efficiencies and stability constraints

Keep once-per-eye/frame rate-map uploads, cached NVAPI setters, bounded reset
retries, frame-boundary gaze polling, and normal per-sequence projection caches.
Stable diagnostic Off already stops new query/copy work. The composited native
fallback and raw native world/UI serve different fade and reprojection paths;
removing either as a duplicate risks the HUD behavior qualified in
[the Beyond verification report](beyond-vrs-verification.md).

Optimization requires a counter or timing result that identifies material cost,
a regression that fails before the fix, and matched game evidence afterward.
Keep native cadence, generated cadence, shader-work reduction and headset
appearance as separate results. Neither this audit nor a virtual dashboard
smoke pass substitutes for them.

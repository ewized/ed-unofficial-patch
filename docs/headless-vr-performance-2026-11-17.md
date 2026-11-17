# Headless VR performance investigation — 2026-11-17

## Status

- Rendering candidate: installed ED-UP `8fe2295`, with its sealed graphics/runtime
  pair. No rendering C++ changes accompany this investigation.
- A complete corrected-controller cycle passed: Start running/exit 0, launcher
  exit 0, Frontier closed, smoke exit 0 and MCP ready; then Stop stopped_clean/
  exit 0. No errors, held input, owned driver lease or active receipt remained;
  game/runtime process counts were zero. All closure was normal, without force.
- First Pilot Assessment is verified at Mawson Dock. A fresh start now confirms
  2320x2320 input to 4000x4000 output, DLSS Balanced/model-K configuration, using
  `nvngx_dlss.dll` 310.9.1.0. Internal evaluated model identity is not inferred.
- The earlier full-input DLAA run had mismatched High/Custom preset evidence.
  No Settings.xml correction was written: the compare-and-swap check found the
  game had saved Custom itself during normal exit. Restart with persisted Custom
  resolved dimensions; a PresetName change is not proved to be the root cause.
- Windowless 30-second Turbo Off/On/Off collection passed timing qualification
  and 302/302 foreground samples matched the game in each window. GPU-span
  medians were 37.7614 / 33.2240 / 33.3864 ms. The return-Off result nearly matched
  On; return-Off native 28.767 Hz also matched On 28.885 Hz. No repeatable Turbo
  advantage is established. Earlier console runs are excluded.
- AA-Off GPU median/p95 was 6.2547/7.4081 ms with native cadence about 32 Hz.
  Requested FSR used own-history TAA because EDVR_HAVE_FSR3 is absent. TAA fallback
  Off/On medians were 8.7047/8.5024 ms; native Off/On was 31.467/31.700–31.717 Hz.
  No equal-quality, major Turbo boost or 90 Hz result is established.
- The virtual driver declares 90 Hz, but 950/950 sampled
  `GetTimeSinceLastVsync` queries returned false. Actual 90 Hz remains unverified.
- Enabling existing application-GPU timing allowed Auto to admit 2x. Initial
  camera refusal 0x31 concerned invalid/zero camera rows, not projection shift.
  After undocking, NGX capability initialization failed (failure 5/stage 2,
  available=0, maximum_generated=1, ngx=0xbad00010); Auto returned to native 1x
  with generation fault. No frames were generated. NVIDIA's support table excludes
  RTX 30 from DLSS FG: this backend cannot meet the FG target on the RTX 3080 Ti.
  DLSS SR/DLAA remain supported; another compatible FG backend needs different code.
- Saved pre-benchmark INI/mirror were restored: Auto, Turbo Off, DLSS K, GPU diagnostic
  Off, private keyboard. Simulator stop confirmed owned=true/stopped=true.
- A later [VRS probe](headless-vrs-shader-work-2026-11-17.md) found matched shader-work
  reductions in both eyes. Whole-frame GPU benefit remains unverified.
- See the [testing contract](headless-vr-testing.md),
  [source audit](performance-audit-beyond-2026-11-16.md) and
  [physical Beyond evidence](beyond-vrs-verification.md) for their own scopes.

## Startup, controls and recovery

ED-UP's OpenVR proxy/native OpenXR runtime runs on SteamVR with Mod Linker's
virtual HMD. Earlier startup verified game/MCP readiness, launcher exit 0,
normal Frontier closure and smoke exit 0. After the old harness exited naturally,
a complete corrected-controller Start/Stop cycle passed: Start reported running
and exit 0 with MCP ready; Stop reported stopped_clean and exit 0. Session state
was stopped, error was null, and MCP shutdown errors were empty. No keys or mouse
button remained held, no driver lease was owned, the receipt was inactive, and
no game/runtime process remained. Closure was normal throughout, without forced
termination. This qualifies the tested cycle, not every startup/failure route.
Argument/lifetime/uncertainty fixes also passed independent review and harmless
native tests.

Simulator smoke and menu/pitch/roll checks qualify separate scopes. MCP settling
helped menus; low-FPS gameplay still lagged. Keyboard-profile and earlier control
evidence remain in the linked testing contract.

In the latest 4k tutorial, Down for 300 ms followed by Enter for 400 ms worked:
the ship visibly released and the subsequent game status had docked=false. This
is observed input/undocking evidence, not a general reliability or cadence claim.
Private configuration, receipts and raw captures remain outside the repository.

## Resolution: persisted settings and fresh runtime proof

Earlier 4000x4000 DLAA input contradicted archived HMD scale 0.58: Settings.xml
selected High while graphics StartPreset.start named Custom. No Settings write
was applied; the compare-and-swap check found the game itself had saved Custom
during normal exit.

Fresh startup with persisted Custom confirmed 2320x2320 to 4000x4000 in the
tutorial, Balanced/model-K configuration and nvngx_dlss.dll 310.9.1.0. Restart
resolved dimensions without isolating PresetName as the cause; the K hint does
not identify the internally evaluated model. Separately, a 2560x1440 boot refused
independent native_input_scale=0.58 through its 3200-square guard; backed-up
INI/mirror correction to 1 restored VR, distinct from the unapplied Settings edit.

## Windowless Turbo Off/On/Off comparison

The collector now records foreground identity at approximately 100 ms intervals.
Initial console-driven OffA/OffB windows are excluded from this comparison.
The following windowless 30-second windows passed timing qualification; each
recorded 302/302 foreground samples matching the target game. Sampling does not
exclude transitions between observations. The table uses SteamVR SDK
`m_flTotalRenderGpuMs`: GPU span from work immediately after present through
completion of compositor work, not isolated application GPU work or native
frame-time percentiles. All six copied summary/focus artifacts agree: each has
302/302 game-focus matches and strictly increasing monotonic observation times.

| Window | Turbo | Selected samples | Reported GPU span median | p95 |
|---|---|---:|---:|---:|
| OffC | Off | 256 | 37.7614 ms | 40.1252 ms |
| OnA | On | 289 | 33.2240 ms | 34.9564 ms |
| OffD | Off, return | 289 | 33.3864 ms | 35.0631 ms |

Complete native windows reported Off 25.753 Hz, On 28.467/28.885 Hz and return-Off
28.767 Hz. Return-Off had 863 valid cycles in 30000 ms, pacing=0, scene_ready=1
and all missing counters zero; its reported perf metric was 33.6243 ms. Native
and collector boundaries differ; do not present them as exactly coincident.

The return-Off result nearly matched On, so the initial delta is not repeatable
Turbo evidence. Warmup/scene variance remains a hypothesis; do not prefer On
without matched repeats. Headset comfort is outside this virtual test.

## AA-Off diagnostic and cadence limit

A subsequent AA-Off diagnostic retained the configured input/output, FG Off,
Turbo Off and synthetic-gaze conditions while changing the AA route. Its
qualified 30-second collection contained 318 selected samples, with SDK total-GPU
span median 6.2547357 ms and p95 7.4080974 ms, confirmed from the copied summary.
All 302 foreground samples matched the game. Native cadence nevertheless remained
about 32 Hz with roughly 6–7 ms median GPU spans.

AA-Off changes reconstruction/image quality, so this is not a quality-equal
speedup against DLSS Balanced. It does suggest that the observed native cadence
cannot be explained solely by the reported GPU span; it does not identify the
remaining limiter.

Requested FSR measured the pass's own TAA, not FSR: owner logging explicitly
reports the build lacks `EDVR_HAVE_FSR3`, and temporal-price logging confirms
`own history (no ngx, lean)`. The FSR 3.1 selection toast was not execution proof.
Both qualified 30-second windows had 302/302 foreground samples matching the game:

| Actual route | Turbo | Samples | SDK total-GPU median | p95 | p99 |
|---|---|---:|---:|---:|---:|
| TAA fallback, requested FSR | Off | 317 | 8.7047367 ms | 9.506605 ms | 10.01837 ms |
| TAA fallback, requested FSR | On | 318 | 8.5024004 ms | 9.5607983 ms | 9.9899559 ms |

Complete native windows reported Off 31.467 Hz and On 31.700/31.717 Hz. This is
neither a major Turbo boost nor a 90 Hz result. These routes are not established
as equal quality to DLSS or AA Off. Native CPU p50 was about 12.5 ms in one
953-cycle window while all 953 native producer-GPU observations were missing;
missing GPU data is not zero work. SteamVR SDK GPU spans have a separate scope.

The virtual driver declares 90 Hz but provides no independently usable vsync
clock in this setup: all 950 sampled `GetTimeSinceLastVsync` calls returned false.
Earlier 2560 observations around 32 Hz and the current native windows do not
qualify 90 Hz. Selected SteamVR records, index stride three and three
presentations per record must not be relabeled native application FPS or
ED-UP-generated-frame cadence.

The owned VR companion Present uses sync interval zero in current
[`desktop_vsync.h`](../src/d3d11/desktop_vsync.h); the `desktop vsync (VR)` log
proves execution. `present_sync_nonzero` counts Elite's original request instead.
Native phase logs partition wall time, not GPU time. `provider_size` invalidates
the CPU Present-trace split, independently of FG guide admission. Earlier
loading/menu/input-contaminated 2.5–3.1 Hz windows are excluded from comparisons.

## Auto admission, camera evidence and NGX refusal

The existing Auto-observation path in
[`device_hook.cpp`](../src/d3d11/device_hook.cpp) requires GPU timing enabled,
a valid recent ApplicationRender result and qualified CPU-work evidence.
Before `advanced.app_gpu_timing` was enabled, native GPU observations were missing.
Enabling that existing diagnostic yielded valid native GPU data. With DLSS K,
Auto and Turbo Off, Auto then admitted 2x: CPU work 12.835 ms, application GPU
36.524 ms, display budget 11.111 ms. This establishes admission under the observed
measurement conditions, not successful generation or a general requirement to
change default settings permanently.

Initial `camera refused` mask 0x31 means cameraValid=false and non-rotation current
and previous camera rows; logged rows were zero and provenance lacked rowsOwn.
This is not a projection-mismatch verdict. SceneProjection A/B are scene-depth
encoding parameters, not optical-center shifts. The later plus/minus 2-degree head
replay happened after NGX failed; it cannot be credited with clearing camera
admission. See the mask predicates in
[`frame_generation_coordinator.cpp`](../src/d3d11/frame_generation_coordinator.cpp).

After the successful Down/Enter undocking input, the owner recorded:

```text
NGX initialization failed failure=5 stage=2 failed_stage=2 eye=2
ngx=0xbad00010 hr=0x00000000 available=0 maximum_generated=1
private_init=1 borrowed_init=0
```

[`frame_generation_ngx.h`](../src/d3d11/frame_generation_ngx.h) defines failure 5
as `MultiplierUnsupported` and stage 2 as `Capabilities`. Queries precede feature
creation. The capability error was followed by `effective=1x reason=generation
fault`; the existing fatal fallback was observed to work. No generated frames
were produced. Coordinator latest failure 6 is a different namespace (`Guides`;
NGX is 10), so later guide refusals must not replace this fatal log.

NVIDIA's [DLSS Supported Hardware table](https://www.nvidia.com/en-us/geforce/technologies/dlss/),
checked 2026-11-17, supports FG on RTX 50/40, excludes RTX 30/20, and supports
SR/DLAA on all four. The refusal is consistent: this NVIDIA DLSS FG backend cannot
meet the target on the RTX 3080 Ti. The capability log alone was not complete
hardware proof; VRAM causality remains separate. Other frame-generation methods
are not ruled out but require a compatible implementation. The pending choice
is compatible FG, current-backend testing on RTX 40/50, or native optimization.

A later outside-scene Auto window had 835 valid samples, CPU p50 10.613 ms,
application-GPU p50 34.706 ms and zero missing native GPU observations. Its scene
and boundaries differ from the docked tables; do not use it as a matched
performance comparison. It demonstrates available diagnostic data, not FG output.

At the diagnostic boundary the saved pre-benchmark INI and mirror were restored,
including Auto, Turbo Off, DLSS K, GPU timing diagnostic Off and private keyboard.
Simulator stop returned owned=true/stopped=true. The final controller cycle then
completed normal startup and clean shutdown, with receipt/input/process cleanup
verified. Game/runtime remain stopped while the next FG-backend scope is chosen.

## Synthetic-eye diagnostic

In the earlier synthetic-eye check, active/tracked eyes plus a neutral 60000 ms
head replay produced `valid_pairs=179` and `world_accepted=1284`, versus zero
baseline values. Map updates were 1/1 with `map_bytes=125000`; stale refusals were
237480, `coarse_ack=0` and `projection_mapping_verified=0`. These establish changed
provider/world-admission activity, not effective shading coverage. `coarse_ack`
is a last-state snapshot. A later [bounded probe](headless-vrs-shader-work-2026-11-17.md)
proved sampled shader-work reduction. Whole-frame benefit and physical gaze latency
remain outside that evidence.

## Memory and retained-resource investigation

Earlier full-input 4k Auto GPU windows reached roughly 184–214 ms near memory
capacity. Fresh FG-Off also used roughly 11.27 GB, so FG retention is not established
as the primary cause. Scene/input dimensions confound comparisons with scaled input.

Source hypotheses remain narrower than a measured memory leak. The coordinator
capture gates test requested/closed state rather than FAILED, so motion preparation
may continue until Auto consumes a failure. Off/fatal exit can retain producer,
partial/queued work, leases, UI snapshots and independent scratch/fallback owners.
The latest run directly observed Auto's fatal latch returning to native 1x.

Any justified release change must seal/abort partial work on its owner and retain
storage through producer fences/exported consumer retirement.
[`FrameGenerationInterop`](../src/d3d11/frame_generation_interop.cpp) deliberately
keeps unsafe outstanding storage. Preserve first fatal causes without disabling
recoverable warmup/backpressure. No C++ changes were made.

## Remaining qualification

Partition the roughly 32 Hz ceiling independently of GPU work and declared refresh.
No repeatable Turbo preference is established. Real FSR needs its backend built
and execution confirmed; whole-frame VRS benefit remains unproved. The tested
controller cycle is clean. Next scope awaits the FG-backend choice; preserve exact
restoration receipts and existing HUD/native fallback behavior.

No 4k/90 native, native 1% low, generated-cadence or physical-headset result is
established. Preserve HUD protection and native fallback during optimization.

# Headless VR performance investigation — 2026-11-17

## Status

- Rendering candidate: installed ED-UP `8fe2295`, with its sealed graphics/runtime
  pair. The automation tools do not change rendering C++.
- Automatic startup, normal launcher closure after game ownership, runtime smoke
  and loopback MCP readiness succeeded. These qualify the tested handoff/control
  path, not the performance target or every startup/recovery route.
- A previous first Pilot Assessment run undocked and responded visually to pitch
  and roll with synthetic head neutral; afterward no keys or mouse button remained
  held. That flight does not qualify the current 4k run.
- Pilot Assessment has loaded at Mawson Dock and its cockpit was captured.
  Current output and input are both 4000x4000, with mode DLAA in-game. Graphics
  Settings.xml selects High while StartPreset.start names Custom, whose fxcfg
  stores HMD scale 0.58. Correction/retest is pending; causality is unproved.
- Native MCP input settling successfully navigated Private Group, Training,
  Simulations and the first Pilot Assessment in this 4k session. Queued input
  acceptance remains distinct from observed game behavior. Low-FPS in-game
  input is still lagged/unreliable; settling is not a general input fix.
- The virtual driver declares 90 Hz, but 950/950 queried `GetTimeSinceLastVsync`
  results were false. An earlier 2560 baseline observed roughly 32 Hz native
  cadence. Actual 90 Hz operation remains unverified.
- FG initialization reported NGX failure 5, stage/failed_stage 2, `available=0`,
  `maximum_generated=1`, `ngx=0xbad00010`. Requested 2x had active multiplier 0
  and no generated pairs. This capability-stage refusal does not prove a hardware
  limitation or a VRAM cause.
- A fresh 4k FG-Off run also reported roughly 11.27 GB GPU-memory use. Retained
  FG allocations are not established as the primary cause of high memory use.
- Active tracked synthetic eyes with a neutral 60000 ms head replay produced
  VRS valid_pairs=179 and world_accepted=1284, both previously 0; map updates
  were 1/1 and map_bytes=125000. Stale refusals remained high, coarse_ack=0 and
  projection_mapping_verified=0. Shading reduction and GPU benefit are unproved.
- Three docked native-cycle windows observed about 3.016, 3.137 and 2.506 Hz.
  Loading/menu/input activity contaminated them; these are diagnostic windows,
  not qualified benchmarks. Turbo A/B and a stable 4k/90 baseline remain pending.
  Native source cadence and generated presentation cadence stay separate.
- No rendering changes or FPS improvement are claimed. See the
  [testing contract](headless-vr-testing.md),
  [source audit](performance-audit-beyond-2026-11-16.md) and
  [physical Beyond evidence](beyond-vrs-verification.md) for their own scopes.

## Startup and control evidence

The tested route uses ED-UP's OpenVR proxy and native OpenXR runtime on SteamVR,
with Mod Linker's external virtual HMD. The external supervisor provisions the
runtime, starts the game in the interactive session, verifies scene ownership,
then closes the native launcher through its normal window-close message.
Launcher exit, smoke success and MCP readiness were observed while Elite
continued running in VR. Machine configuration, receipts, raw logs and captures
remain outside the repository.

The initial runtime-only smoke had scene PID 0 and dashboard images. Its head
translation, stereo captures and cancel/stop success qualify the simulator.
Later game captures and menu transitions independently qualify the game-specific
subset; neither observation substitutes for the other.

General Controls initially showed preset **BLANK**. WMI listed neither keyboard
nor mouse, but native DirectInput found an attached system keyboard and successful
keyboard creation (`DI_OK`). Mouse enumeration was empty and system-mouse status
was `DIERR_DEVICENOTREG`. WMI absence did not prove an unusable keyboard. A
separate `EDUPKeyboardTest` profile preserved stock keyboard bindings while
removing only seven Mouse slots. Actual Insert/Home page hints and ED-UP profile
adoption confirmed that it loaded. Stock files and exact configuration preimages
remain preserved for checksum-guarded restoration; no HID driver or hardware was
installed.

In the earlier tutorial run, Down selected Auto Launch and Enter after the
instructor made the action ready produced `LaunchGranted`, an unset docked flag
and a view outside Mawson station. Bounded S/X pitch and COMMA/PERIOD roll probes
visibly moved the world with synthetic head pose neutral. Afterward no keys or
mouse button remained held. Space/Shift requests were queued, but throttle effect
was not qualified during the tutorial's roll stage.

The current 4k session verified native `--input-settle-ms` behavior: Right selected
Private Group, subsequent Right presses selected Training, Enter opened its cards,
Enter opened Simulations, and Right/Enter selected the first Pilot Assessment.
Pilot Assessment has now loaded at Mawson Dock and its cockpit was captured.
No current 4k flight or repeatable performance scene is qualified. Native MCP
1500 ms settling worked for these menu transitions, but low-FPS in-game input
remains lagged/unreliable; general input reliability has not been established.

## Resolution and timing boundaries

Output is confirmed as 4000x4000 per eye. The loaded cockpit's DLSS audit still
shows 4000x4000 input and mode DLAA. Graphics `Settings.xml` selects preset `High`,
whereas graphics `StartPreset.start` names `Custom` and `Custom.4.4.fxcfg` stores
HMD scale 0.58. This mismatch was identified; correction after graceful shutdown
and a fresh runtime check are pending. It is not yet proved to cause the full-size
input. Confirm actual dimensions after retest before calling the scene DLSS
Balanced or attributing a memory/frame-time change to the correction.

The independent `native_input_scale=0.58` path previously refused a 2560x1440 boot
because its eligibility guard accepts only 3200-square output. Backed-up,
checksum-checked correction of the live INI and mirror to `native_input_scale=1`
restored VR. Process liveness alone had not established successful VR startup.

The virtual driver declares 90 Hz but provides no independent vsync clock in the
tested setup: all 950 sampled `GetTimeSinceLastVsync` calls returned false.
Declared refresh is not measured display cadence. The previous 2560 baseline's
roughly 32 Hz native observation does not establish 90 Hz. Selected SteamVR
records, frame-index stride three and three presentations per record must not be
relabeled native application FPS or ED-UP-generated-frame cadence.

Current source forces the owned VR companion Present's sync interval to zero;
the flat profile and unrelated swapchains retain the caller's value. See
[`desktop_vsync.h`](../src/d3d11/desktop_vsync.h) and the real Present call in
[`device_hook.cpp`](../src/d3d11/device_hook.cpp). The graphics log's
`desktop vsync (VR)` line is execution evidence for that override. In contrast,
`native_post_submit_window.present_sync_nonzero` counts Elite's original request;
it cannot establish the forwarded interval.

Existing `native_frame_cycle_window` and phase records distinguish caller-cycle
cadence from time in real Present, outside Present, runtime wait, submission and
frame-end work. They measure wall time, not GPU time. Post-submit splits remain
unavailable when provider validation fails. The separate `provider_size` diagnostic
rejects a Present-trace structure smaller than the consumer expects; it does not
reject FG depth/motion guides.

## Docked timing and synthetic-eye diagnostic

Three native caller-cycle windows in the first docked 4k scene recorded:

| Valid cycles | Window elapsed | Valid cycles / second |
|---:|---:|---:|
| 91 | 30.172 s | 3.016 Hz |
| 95 | 30.281 s | 3.137 Hz |
| 76 | 30.328 s | 2.506 Hz |

These windows included loading, menu or input activity and are diagnostic,
not a qualified scene benchmark or a native low-percentile result. They show
that the 90 Hz declaration did not establish current native rendering cadence.

With synthetic eyes active/tracked and a neutral 60000 ms head replay, the
installed graphics log reported `valid_pairs=179` versus baseline 0 and
`world_accepted=1284` versus 0. Map updates were 1/1, with `map_bytes=125000`.
The same observation reported 237480 stale refusals, `coarse_ack=0` and
`projection_mapping_verified=0`. These counters establish changed provider/world
admission activity, not effective shading coverage. `coarse_ack` is a last-state
snapshot and does not by itself describe every admitted world draw. Projection
mapping, shader-work reduction and repeatable GPU benefit remain unverified.
The neutral head replay and diagnostic counters do not qualify physical gaze
latency or headset behavior. No Turbo On/Off comparison has run.

## Frame-generation refusal

The owner log reported:

```text
NGX initialization failed failure=5 stage=2 failed_stage=2 eye=2
ngx=0xbad00010 hr=0x00000000 available=0 maximum_generated=1
private_init=1 borrowed_init=0
```

In [`frame_generation_ngx.h`](../src/d3d11/frame_generation_ngx.h), failure 5 is
`MultiplierUnsupported` and stage 2 is `Capabilities`. The backend queries NGX
FrameGeneration availability, driver-update requirement and maximum generated
count before feature creation. These capability values refused the requested
multiplier. The raw NGX result is preserved without guessing its underlying
cause from the GPU model. The GPU is an RTX 3080 Ti; official feature-support
assumptions or an assumed custom optical-flow backend do not replace this query.

The coordinator uses a different numeric namespace: `last_failure=6` means
`Guides`, and NGX is 10. Later guide refusals can overwrite the latest failure
while FAILED state remains sticky. A lifecycle snapshot therefore cannot replace
the first worker initialization log. Two admitted source pairs establish that
complete captures reached the worker queue, not that NGX initialized or generated
an image.

Useful existing owner records are `native fg: NGX initialization failed`,
`native fg: worker initialized`, `native fg: source refused`,
`native_fg_lifecycle` and `native_fg_xr`. Keep requested/active multiplier, state,
source/generated/refused counters and the first backend failure together.
Synthetic gaze testing is a separate VRS experiment; gaze validity cannot
establish valid FG depth or physical-motion guides.

## Memory observations and source hypotheses

The first 4k Auto attempt approached GPU memory capacity; reported usage stayed
high after switching Auto to Off. Some GPU windows were roughly 184–214 ms in
that state. These observations do not establish a repeatable cost or its cause.
A fresh 4k FG-Off run also reported roughly 11.27 GB memory use. Retained FG
allocations are therefore not established as the primary cause. Menu/loading
input dimensions and scene resource loading must match before attributing a
memory difference to FG.

Read-only source inspection identified four relevant mechanisms:

- **Auto already latches fatal failure.**
  [`nativeFgAutoObserve`](../src/d3d11/frame_generation_coordinator.cpp) feeds
  FAILED state to [`Policy`](../src/d3d11/frame_generation_auto_policy.h), which
  latches fatal failure before validating timing samples and selects native 1x.
  The expected log is `effective=1x reason=generation fault`. Persistent requested
  2x after failure warrants checking the owned Present/Auto-observation route;
  it does not prove that a cache is absent.
- **Capture does not directly reject FAILED.** Coordinator `enabled()` and
  `nativeFgRequested()` check requested/closed state rather than fatal state.
  FG motion preparation and capture can continue until Auto observes failure.
  Their current frame-time contribution remains unmeasured.
- **Off retains resource owners.** The coordinator clears ready tickets but keeps
  the producer, partial capture and queued work. Fatal worker exit destroys its
  local backend/output objects but leaves queued `Work` items owning input
  leases, UI snapshots and producer references. The producer retains its bounded
  interop ring and color scratch. Temporal FG-motion textures and runtime
  fallback textures also survive Off until owner reset/shutdown. These paths
  are not a measured leak or an explanation of all reported GPU-memory use.
- **Retirement constrains release.**
  [`FrameGenerationInterop`](../src/d3d11/frame_generation_interop.cpp) checks
  producer/consumer fences and leases before storage reuse/destruction. Its
  destructor deliberately retains unsafe outstanding storage. Blindly clearing
  owners can preserve process-lifetime retention or violate ownership assumptions;
  freeing textures is not a safe first experiment.

If matched evidence justifies a code change, the narrow candidate is immediate
fatal admission rejection, then render-owner abort/sealing of partial capture
and cancellation/retirement of queued work. Retain resources until producer
fences complete and submitted/exported consumer leases retire. Preserve the
first fatal backend cause separately from transient refusals. Recoverable guide
warmup, busy backpressure and transient allocation failures must retain their
recovery behavior. No such C++ changes accompany this report.

## Remaining qualification

1. Correct the identified graphics-preset mismatch after graceful shutdown,
   then verify actual cockpit input/output dimensions and reconstruction mode.
   Establish a repeatable position; keep capture/menu/input intervals outside
   timing windows. Test input reliability independently of menu navigation.
2. Establish actual runtime/caller cadence independently of the 90 Hz declaration.
   Collect complete native frame-cycle windows with qualified SteamVR history;
   preserve missing-provider and unavailable-field verdicts.
3. Compare fresh Off and failed-Auto-then-Off at matched dimensions, scene and
   settings. Record process/budget memory after settling and correlate capture,
   queue, allocation and retirement evidence before changing resource lifetimes.
4. Repeat synthetic-gaze checks with fresh observations and verify projection
   acceptance. Require meaningful matched shader-work samples before claiming
   VRS shading reduction; valid-provider/world-admission counts are insufficient.
5. Run repeated matched Turbo Off/On/Off windows with one change at a time.
   Record native source timing separately from generated presentation cadence.
   Automation success establishes neither a target nor a performance gain.

Portable offline suites and live control checks qualify their stated scopes.
They do not establish 4k/90 native performance, real eye-tracking latency,
physical-headset appearance or long-flight comfort. Existing HUD protection and
native fallback remain constraints on any later optimization.

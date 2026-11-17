# Headless VR testing

## Status

- `tools/xr_headless.py` exercises Mod Linker's external virtual-HMD driver
  and background helper. `tools/edup_vr_mcp.py` hosts a separate loopback MCP
  adapter for runtime inspection, captures and bounded desktop input. These
  tools do not change rendering code.
- Initial runtime-only smoke passed six offline tests and 14 live operations,
  verified 8 cm head motion, captured both eyes and cleaned up without errors.
  Its scene PID 0 and dashboard images qualify simulator tracking/capture only.
- A subsequent live game check verified installed ED-UP candidate `8fe2295`
  running Elite in VR through ED-UP's OpenVR proxy and native OpenXR runtime
  on SteamVR, with Mod Linker's virtual HMD. The loopback MCP connection
  successfully navigated the main menu into the first Pilot Assessment
  tutorial. Escape opened the game's pause menu and Enter resumed it.
- The first Pilot Assessment tutorial now has a verified launch: Down selected
  Auto Launch, and Enter after the instructor made it ready produced
  `LaunchGranted`, cleared the docked status flag, and reached space outside
  Mawson station. Escape pause and Enter resume also worked through MCP.
- The General Controls preset initially showed **BLANK**. A dedicated
  `EDUPKeyboardTest` profile cloned the stock keyboard preset while removing
  only its seven Mouse bindings. The game then displayed actual Insert/Home
  page hints and ED-UP logged adoption of that profile. Stock files and exact
  configuration preimages remain preserved for checksum-guarded restoration;
  no HID driver or hardware was installed. An absent bindings folder alone
  did not establish the active preset or device availability.
- Native DirectInput observation found an attached system keyboard and successful
  keyboard creation (`DI_OK`), while mouse enumeration was empty and system-mouse
  status was `DIERR_DEVICENOTREG`. WMI had listed neither keyboard nor mouse;
  that observation did not prove the absence of a usable DirectInput keyboard.
- With synthetic head pose neutral, bounded 500 ms S/X probes visibly changed
  world pitch; 1500 ms COMMA/PERIOD probes rolled the world in both directions.
  The adapter afterward reported no held keys, no held mouse button and no
  owned driver-control lease. Space/Shift requests reached the input queue,
  but their throttle effect remains unverified.
- After game scene ownership was verified, the native launcher was closed through
  its normal window-close message. The launcher exited while Elite continued
  running in VR. This qualifies that observed handoff, not every startup route.
- On a 2560x1440 boot, `native_input_scale=0.58` was refused by the existing
  3200-square-only independent-scale guard. Backed-up, checksum-checked correction
  of the live INI and mirror to `native_input_scale=1` restored VR. Confirm actual
  eye dimensions after configuration changes; process liveness is insufficient.
- A minimized desktop game window prevented desktop capture/input targeting
  while the VR image remained available. Restoring the verified game window
  and checking its foreground identity recovered pause-menu interaction.
  Earlier focus observations cannot substitute for checks at the current action.
- The initial smoke environment was SteamVR 2.18.4, RTX 3080 Ti, driver
  32.0.15.6094. Its compositor sources were 3840x2160 per eye, reduced to
  768x432 PNGs. This does not qualify the requested 4000x4000-per-eye, 90 Hz
  game configuration. A separate square-resolution driver build and ABI probe
  do not establish the game's effective eye dimensions or cadence.
- Actual 4000x4000 output and tutorial loading now have live evidence; see the
  [performance investigation](headless-vr-performance-2026-11-17.md).
  Auto FG refused initialization; synthetic gaze reached eligible world draws,
  but whole-frame VRS savings and 90 Hz remain unqualified. See the
  [sampled shader-work evidence](headless-vrs-shader-work-2026-11-17.md).
  Native source cadence and generated presentation cadence must be measured
  separately. The verified startup/input subset does not qualify the full target.
- [Performance audit](performance-audit-beyond-2026-11-16.md) records source
  findings and discriminating experiments. No FPS improvement is claimed.

## Reuse and setup boundaries

Use the standalone `addons/xr-simulation` driver, helper, and receipt-owned
SteamVR setup scripts from Mod Linker. The ED-UP host uses an external copy of
Mod Linker's native MCP transport with a restricted Elite tool catalog. The
driver, helper and native transport require no Unity integration. The tested
bundle came from Mod Linker candidate `b67af29`;
its helper SHA-256 was
`54b2e6d865d579661f736f2a1342d9ec45f3b4603b3f7e09f191ea6820d0a318`.
The SDK provenance is Valve OpenVR commit
`0924064316de3effbcd1acf1e309182a2deb1c05`; retain its BSD notice. No driver,
helper binary, vendor library, or first-party Mod Linker source is vendored here.

The MCP host accepts `--input-settle-ms` (default 1500, range 0–2000).
After runtime preflight and game focus recovery, each keyboard request or mouse
click waits once with process identity, interactive desktop and exact restored
foreground window checks throughout. A lost window refuses new presses. Mouse
coordinates and point ownership are checked after this wait; keyboard sequences
retain their existing 10-second action bound. Read-only tools do not settle.
`get_state` and readiness JSON record the configured interval. This is an input
reliability measure, not evidence that the game consumed queued events.

Provisioning, interactive-session startup, game launch, normal runtime shutdown,
and receipt restoration belong to an external supervisor. Keep host names,
account information, machine paths, scripts, and raw captures outside this repo.
Use a fresh simulator cache and its own recovery receipt. Do not adopt another
run's active receipt or force-kill SteamVR to bypass restoration conflicts.

Windows needs an interactive desktop session and a GPU even though no physical
headset is needed. An SSH session may not share the interactive SteamVR session:
in the observed setup, direct SSH helper initialization failed with 124, while
running the same helper in the interactive session succeeded. The live test used
an external session bridge; the report hashes that invocation executable
separately from a verified copy of the actual Windows helper.

Give only the game child `XR_RUNTIME_JSON` pointing to SteamVR's registered
`steamxr_win64.json`. Global runtime registry changes are unnecessary. Verify
virtual HMD index 0, serial `modlinker_sim_head`, connected/valid `Running_OK`,
virtual-HMD driver mode, a single connected HMD, and ready live Chaperone bounds.
The runner enforces those observations before requesting head motion.

## Runner contract

Run from the checkout. The portable checks require only Python's standard
library and no Windows runtime:

```sh
python3 tools/xr_headless.py --self-test
python3 tools/xr_headless.py --help
python3 tools/check_tool_conventions.py
```

For a live invocation, supply `--client` with the absolute helper executable
path and `--output` with a fresh directory whose parent already exists. Add
`--capture` for both eye PNGs and `--expect-scene-pid` with the verified game's
positive PID. `--openvr-dll` accepts an absolute runtime DLL path; otherwise the
helper uses the registered runtime. When an external transport invokes the
Windows helper, `--source-helper` hashes a separately verified local copy of
that helper; it does not authenticate a remote process by itself.

`--dry-run` executes no helper and creates no files or directories. A live run
refuses to overwrite an existing output path. It validates runtime identity,
starts neutral simulated input, replays a bounded head translation/return, and
requires observed runtime poses plus a newly completed replay. It requests no
controller buttons or game actions. After any possible start, both cancel and
stop are attempted even when another operation fails. Failures, cleanup errors,
exact requests, helper metadata, binary hashes, and PNG hashes remain in
`report.json`; inline image bytes are stored separately. A failed cleanup makes
the run fail. These commands neutralize driver input; the external supervisor
still owns SteamVR shutdown and profile restoration.

The build gates the offline suite. It covers runtime/headset rejection,
Chaperone and PID mismatch, stale replay counters, failure after a possible
start, independent cleanup failures, helper exit/JSON/timeout failures,
PNG/report output, collision refusal, and dry-run nonmutation. The initial live
pass exercised the same runner using an external Windows-session bridge; it
did not launch or navigate Elite.

## Establish a game baseline before changing rendering

1. Finish installation and launch the game through its normal account flow.
   Install only through `tools/install_edvr.py`, including its dry-run and
   verification routes; preserve rollback and the live INI/mirror preimages.
2. Verify both installed DLL hashes, the source/build identity through
   `tools/edvr_log.py`, the game's supported build, the actual XR session, and
   input/output eye sizes. A running process or a dashboard image proves none
   of those. Pass the actual branch build hash rather than a documentation-only
   commit when checking a previously built candidate.
3. Record a repeatable scene, resolution, refresh rate, runtime, GPU/driver,
   DLSS configuration, UI state, VRS state/provider validity, and actual FG
   factor. Keep each comparison to one independent change. Use the runner's
   scene-PID guard; reject dashboard/menu/loading or scene changes.
4. Warm the scene, separate diagnostic and capture intervals from timing
   intervals, then collect repeated matched A/B/A windows. Collect native
   source cadence and true low-percentile frame times separately from generated
   presentation cadence; collect application GPU times separately from
   compositor times. Missing timestamps mean the corresponding result is unknown.
5. If measuring VRS, require meaningful matched full/native-map/full shader-work
   samples from both eyes. Runtime acceptance and world-admission counters alone
   do not establish shading reduction. A later tutorial probe obtained meaningful
   triples; the audit's discovery-bias finding remains a sampler limitation.

The helper's capture warm-up and readback latency are not game-frame timings.
Its per-eye compositor mirrors are sampled sequentially, without a same-frame
stereo or exact OpenXR-generation guarantee. Virtual tracking does not qualify
physical optics, motion comfort, real eye-tracking latency, or the recent HUD
fix on a headset. ED-UP queries SteamVR's official `GetEyeTrackedFoveationCenter` through its
native gaze adapter. Mod Linker publishes synthetic eye tracking through its
driver, but that alone does not prove a valid foveation-center response or pass
ED-UP's projection checks. Verify actual provider validity; unavailable gaze
must retain the full-rate fallback. Follow [the existing Beyond evidence](beyond-vrs-verification.md)
when interpreting hardware-specific results.

## SteamVR timing history

`tools/steamvr_benchmark.py` reads the official OpenVR timing history as a
background client. Supply an explicit absolute `--openvr-dll`, the verified
positive `--expect-scene-pid`, and a new `--output` directory. The default
collection interval is 30 seconds; `--duration` accepts 1–300 seconds.
`--dry-run` loads no DLL and writes nothing. `--self-test` runs offline fixtures.

`frames.jsonl` retains the selected SDK fields. The newest history entry and
zero-presentation entries are excluded from metric samples. `batches.jsonl`
retains each raw window's ordered index/system-time/presentation-count triplets,
its predecessor boundary, and overlapping indices. `rejected.jsonl` records
invalid timing observations. GPU/CPU median, p95 and p99 summaries preserve the
SDK field names and semantics; they do not infer native FPS, native 1% lows,
Auto FG cadence or game behavior from compositor presentation counts.

`frame_index_gaps` describes index skips between selected samples, **not lost
collection records**. Repeated presentation and excluded zero-presentation
slots can coexist with index spacing. Collection qualification requires
successive raw windows to overlap; absent overlap is a **possible** history
window overrun, without an invented number of missing records. Empty windows,
invalid fields, timing discontinuities, a changed scene PID, or a visible or
unknown dashboard prevent qualification. This qualifies observation coverage,
not scene stability, resolution, FG/VRS operation or performance improvements.

Overlapping windows alone are insufficient: an unchanged stale history can keep
overlapping indefinitely. Qualification also requires a completed collection
window, at least two selected samples and two observed SDK timestamp advances.
SDK time is compared with monotonic elapsed time relative to the initial query,
without assuming a shared clock epoch. A declared 250 ms allowance bounds
per-poll SDK/monotonic divergence, start/end sample freshness and recent SDK
progress. The selected SDK timestamp span must cover the requested/actual
window minus the two boundary allowances. `summary.json` records every freshness
check and its failure reasons; batch metadata records SDK progress and lag.
These checks qualify timestamp coverage, not application or generated FPS.

The earlier two-second loading probe lacks raw batch-overlap evidence. Preserve
its original unqualified artifacts; accepted frame indices alone cannot
requalify it. See the retained [SDK header](../src/d3d11/openvr026_official/openvr.h)
and [BSD notice](../src/d3d11/openvr026_official/LICENSE.txt), plus Valve's pinned
[C API](https://github.com/ValveSoftware/openvr/blob/0924064316de3effbcd1acf1e309182a2deb1c05/headers/openvr_capi.h)
for the ABI. The SDK's history ordering and presentation-count definitions do
not establish that every selected sample must have a consecutive index.

Windows validation passed 14 collector self-tests and a two-second live flight
probe with 21 selected samples spanning 1.8704 SDK seconds. Raw-window overlap
and freshness checks passed. This validates collection on that runtime; it is
not the requested 4000-square/90 Hz performance comparison.

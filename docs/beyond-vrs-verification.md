# Bigscreen Beyond VRS verification

## Status

- [Headless testing](headless-vr-testing.md) reuses Mod Linker's virtual HMD
  and loopback MCP. Installed ED-UP `8fe2295` reached Pilot Assessment, launched
  into space, and accepted pitch/roll probes with no keys left held. Normal
  Frontier launcher closure left Elite running in VR. See the linked report
  for input-profile, focus and configuration recovery evidence.
- A dedicated keyboard preset removed seven stock Mouse bindings; DirectInput
  found a usable keyboard but no mouse. Stock files and preimages are preserved.
- Headless 4000x4000 output and focused Turbo comparisons are recorded in the
  [headless performance report](headless-vr-performance-2026-11-17.md). Neither
  actual 90 Hz nor generated cadence is qualified. A [bounded VRS probe](headless-vrs-shader-work-2026-11-17.md)
  proved sampled world shader-work reduction, without a frame-time claim. The physical-headset
  evidence below does not transfer to the virtual display. [Source audit](performance-audit-beyond-2026-11-16.md)
  opportunities remain unmeasured.
- Installed and visually reviewed candidate: `8fe2295` (v0.22.1-21-g8fe2295).
  User reported the doubled orange HUD gone in the final fixed-2x cockpit
  check, then described subsequent gameplay as very playable. This is a short
  visual qualification, not a long-flight comfort or performance guarantee.
- Physical-headset play configuration, verified 2026-11-17 UTC: Beyond 2e at 90 Hz,
  4000x4000 output per eye, DLSS Balanced through the game's 0.58 HMD scale
  (2320x2320 input), very-narrow eye-tracked VRS, native-first Auto FG,
  Turbo Off. No desktop-control inputs during the latest cockpit diagnostic.
- The 4500-square launch initially fell back to flat rendering because the
  experimental independent input-scale path only accepted 3200-square output.
  Setting native_input_scale=1 and game HMD scale=0.58 restored VR at 4500;
  the user subsequently requested 4000. Actual runtime input/output was checked.
- HUD protection is integration-owned: recognized HUD/menu draws stay at output
  resolution and composite after full-frame DLSS and world FG. Candidate8fe
  uses one final XR projection, with rotation-mapped native HUD coverage.
  It supplies neither guessed UI depth nor positional reprojection.
- Physical-headset VRS bindings and admitted world draws are observed. Effective
  shading reduction and repeatable frame-time improvement there remain unproved.
  Latest bounded 600-frame PS query selected draws with negligible pixel work: no matched
  triple in either eye. Instrumentation is Off and both INIs restored exactly.
- Historical 3200 gameplay sample: about 89.98 native fps over three 30-second
  windows, application GPU medians 7.740-8.605 ms. Auto remained native 1x.
  SteamVR recorded 0 dropped frames and 4 mispresent events across 10799 records.
  No native 1% low was available; no 4000 performance result is inferred.
- The strict 90-fps native 1% low target remains unmet/unverified. User now
  prioritizes stable imagery and timing; generated cadence is not native cadence.
- Full native build/all gates and clean promotion passed for 8fe. Stereo WARP
  fixture:33039 checks/0 fail; CPU FG fixture:143 checks/0 fail; omit-HUD negative
  control:8 intended failures. Source, DLLs and exact rollback remain sealed.
- Original known-good restore points and protected HOTAS binding are preserved.
  No new C++ changes or installation accompany this report. Game left running.
- Existing temporary key advanced.native_vrs_ps_measure remains default Off.
  experimental.foveation_focus remains the permanent user-selectable aperture.


## Play configuration and enhancement report — 2026-11-17 UTC

This records the resulting integration and its practical limits. The installed
binary is commit `8fe229513ac23ab6964f6cbda1fbac700e539299`; this report is a
subsequent documentation update. Earlier dated entries below retain the settings
and conclusions of their own trials.

### What the rendering pipeline does

Eye-tracked VRS changes shading rates on admitted world draws before DLSS.
Standard DLSS Super Resolution reconstructs the complete eye image; reconstruction
itself is not foveated. Recognized HUD/menu draws are extracted separately at
output resolution. World frame generation runs before the final native HUD
composition. Neither DLSS nor an arbitrary exposed mask automatically identifies
Elite's UI. Unrecognized effects can remain in the world layer.

The final HUD correction changes how the native UI reaches the headset:
rotation-mapped native HUD coverage is composited with the world into a single XR
projection. The previous separate-projection trial still doubled orange UI under
head motion. The user reported that doubling gone on 8fe with 2x generation active.
That observation supports this candidate's short visual qualification; it does
not establish a general compositor defect, correct positional parallax, or
long-session comfort. Full native fallbacks remain for bootstrap and comfort fade.

Auto frame generation uses the physical 90 Hz/11.111 ms display budget rather than
mistaking a 2x application's 22.222 ms cadence for that budget. Load measurements
exclude intentional waits and visible menus; recovery retains the native fallback.
Auto is permitted to stay 1x when source rendering keeps up. Its selection must be
read from runtime counters, never inferred from the configured word `auto`.
Turbo remains Off at the user's request after discomfort during an earlier trial.

### Current settings and the high-resolution launch lesson

| Setting | Current play value / verification |
|---|---|
| Headset | Bigscreen Beyond 2e, 90 Hz |
| XR output | 4000x4000 per eye, confirmed in native_submit_window |
| DLSS input | 2320x2320 per eye, confirmed in the same runtime record |
| Scaling | native_input_scale=1.0; game HMDRenderTargetMultiplier=0.58; SSAA=1.0 |
| Reconstruction | DLSS, Balanced-sized input; modelK hint retained |
| Foveation | Eye-tracked world VRS On; focus Very narrow |
| Frame generation | Auto, native-first |
| Turbo | Off |
| UI | Recognized UI separately rendered at full output resolution |

The independent `native_input_scale=0.58` override had a 3200-square eligibility
guard. Raising only XR output to 4500 violated that guard: a running game process
and monitor image were insufficient evidence of a successful VR launch. The
config-only alternative leaves that override at 1 and uses the game's HMD scale.
Runtime then confirmed 2610-to 4500 and later 2320-to 4000. These are requested
render-target dimensions, not the headset's physical panel resolution.
The internal DLSS model actually evaluated by the driver was not independently
identified in this launch; a modelK hint is not such proof.

The F8 `Beyond` page groups reconstruction/model, input scale, sharpening,
texture LOD bias, desktop window, frame generation and VRS controls, together
with live status. At the current 4000 output, keep the independent input-scale
control at 1.0; lowering it re-enters the 3200-only path described above. The game
HMD setting supplies the current Balanced-sized input instead. Menu availability
does not expand a feature's supported resolution range. Settings need runtime
acknowledgement before treating the menu selection as effective.

### What the measurements establish

The historical passive 3200 sample lasted about 120 seconds. SteamVR supplied 10799
finalized records,0 drops and 4 mispresent events affecting 2 records. Three
matched ED-UP 30-second windows contained 8098 native game frames, approximately
89.98 native fps. Application GPU window medians ranged 7.740-8.605 ms. Auto stayed
native 1x, so that sample cannot demonstrate a frame-generation gain. SteamVR's
submit/compositor GPU timings describe their own scopes, not total application
GPU work. The privileged native frame trace failed to start; a true native 1% low
was therefore unavailable. These numbers do not describe the current 4000 setup
and do not establish improvement against all features Off.

Earlier matched VRS On/Off tests were approximately 90 fps average and 85 fps true
1% low, with application GPU medians 8.48 ms On versus 8.39 ms Off from one qualified
30-second GPU window each. That is not repeatable evidence of a VRS benefit.

The latest 4000 cockpit check used existing, default-Off pipeline-statistics
instrumentation:600-frame budget, alternating full/native-map/full on matched
forwarded opaque draws, no draw replay and no forced GPU flush. It produced 1188
samples but a maximum of only 4 PS invocations against the 4096 useful-work floor;
matched_left=0 and matched_right=0. It selected negligible pixel work and cannot
answer whether VRS reduces visible world shading. The diagnostic is not a
performance window. It completed and drained; both live INIs were then restored
to their exact preimage, with the measurement key absent as before.

The driver accepting a rate map, valid gaze, and rising worldAccepted counters
prove integration activity, not effective shading coverage or a time saving.
`coarse_ack` is a last-state snapshot and can be 0 after a full-rate UI boundary.
The current status strings `world_ps_reduction_verified=0` and
`gpu_benefit_verified=0` are literal placeholders, not computed test verdicts.
No further interruption of the user's current game was needed for this report.

### Validation, preservation and reusable lessons

The 8fe production build passed full native validation in 176.2 seconds and clean
DLL promotion in 45.9 seconds. The actual stereo WARP fixture passed 33039 checks;
removing inline HUD coverage caused 8 intended image-check failures. The CPU FG
fixture passed 143 checks under /WX. These tests constrain composition and
ownership behavior; they cannot replace a headset motion review.

Keep configuration changes transactional across the game INI and its mirror.
A whole-file preimage mismatch protects changes made through F8; inspect the new
preimage and recompute the typed plan rather than overwriting it. A failed edit
must stop dependent launch steps. Restore the game graphics XML together with
INI settings when changing between the legacy HMD-scale and independent-scale
paths. Confirm XR startup/session state and actual dimensions after launch.

For evidence, distinguish requested settings, acknowledged runtime state, actual
shader work, application GPU time, native cadence, generated cadence and headset
appearance. Changing model and input resolution in one comparison confounds the
result. A GPU work query that selects a draw with negligible pixel work needs a better selector
before it can support a world-shading conclusion; repeating that same query does
not turn it into proof. A successful visual fix is not a measured FPS gain.

The preserved known-good installation, sealed candidate/rollback and protected
HOTAS profile remain available. No controls were changed by these graphics trials.
Private evidence paths below are workspace-relative; raw game images/logs and
host-specific configuration were not copied into this tracked document:

- `outputs/Elite-Beyond-known-good-20261115T202051Z/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/single-projection-candidate-01/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/single-projection-8fe2295-cold-01/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/play-stats-8fe2295-20261117T001904Z/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/4500-balanced-hmd-auto-01/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/4000-balanced-hmd-auto-01/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/play-4000-hmd-balanced-auto-02/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/vrs-world-4000-ps-01/`

## Evidence and ruled-out hypotheses

2026-11-17 final result,8fe229513ac23ab6964f6cbda1fbac700e539299:
user answers "The doubled HUD is gone" for small head movements with actual
fixed2x generation active. Build-verified native_fg_ui reports
composition=single_projection_after_fg; native and generated counters each
advanced45 while native_pose_composites advanced90 in a one-second sample.
No independent physical scanout or90-native-fps inference is made. The new
path's image tests prove one world swapchain contains the native UI; this
successful short headset check does not establish long-flight comfort or a
repeatable performance gain. Auto was restored, acknowledged at16:13:40.524
local; the final watchdog35916 was stopped, the collector completed, and
no further live experiments are authorized by this request. Output3200,
DLSS Balanced/modelK hint, very-narrow native VRS and TurboOff remain.
Source/patch/DLL pins and exact rollback are sealed in
world-diagnostics/single-projection-candidate-01; final capture lives in
world-diagnostics/single-projection-8fe2295-cold-01. Original known-good
restore points and protected HOTAS bytes remain unchanged.

The final15-second SteamVR correctness capture has1349 finalized records,
55 mis-presents and0 drops. Its per-frame scene attribution remains unverified;
the user was reviewing head motion and proceeded into auto launch. This is
not a matched performance comparison and does not establish perfect90Hz
cadence. The visual pass must not be promoted into a stable-frame-rate claim.

2026-11-16 final-attempt limit: pose-alignment lease6e8289bd was adopted
and qualified (2705 finalized SteamVR frames,0 drops/mis-presents; no motion
smoothing flags) but user still reports orange-HUD doubling. Ruled out:
render/game/direct-now alignment alone as the cure. Exact preimage df35a369
was restored to both INIs. Literal duplicate text in static FG world color
is also ruled out by eye run152331949001, archived with image hashes under
world-diagnostics/hud-double-capture-01. Actual UI depth is unavailable:
the captured R8 coverage plane is all zero; layer DSVs begin as scene depth
and are not a complete HUD surface-depth map. Do not invent a HUD distance.

Final candidate hypothesis: independently reprojected B-native coverage and
intermediate-pose world layers interact badly during head motion. Discriminator:
preserve the same full3200 UI, source cadence, DLSS/VRS and FG, but rotation-map
the coverage into each world phase and composite both in one final shader,
before one XR projection is submitted. GPU pixel fixtures must prove that
both eyes contain their native UI inside the world's swapchain, no unrotated
second copy is introduced, invalid lookups stay transparent and legacy shader
paths remain exact. A headset review remains decisive; failure means FG Off,
retaining DLSS/VRS, and no further live experiments in this request.

Single-projection preflight: actual stereo WARP fixture passes33039 checks,
both deferred/owned renderers and sRGB/UNORM targets. The omission control
returns1 with exactly8 intended image failures (body presence, eye identity,
rotated placement and linear premultiplied composition). CPU pose tests pass
143 checks under /W4 /WX, including invalid spans. The first full validations
caught an uncompiled new shader in the generator's test-header path, one
obsolete auxiliary-renderer shutdown reference, a wrong fixture member name,
and MSVC's constant-folded zero-divisor warnings for invalid input. All remain
preserved as failed logs; no failing build was installed. Final full validation
is required after these corrections. Source/capture tests cannot establish
SteamVR head-motion behavior or a performance gain.

2026-11-16 57d96ac live result: game PID43016, exact startup ticks
639304674429172511, Pilot Assessment/Mawson Dock cockpit. The user reports
"nope, its like double ui elements when that shakes". Treat this as FAILED,
despite the passing source/GPU fixtures and the qualified 30-second SteamVR
window (2704 finalized records,2 mis-presents,0 dropped). That client-scoped
window does not measure whole-game GPU cost or prove visual stability.

The previously proved native/split filtering discontinuity is corrected in V3
but cannot explain the entire remaining artifact as a fixed issue. Source
review confirms ordinary CaptureWorld precedes uiLayerComposite; no second
ordinary door composite was found. Legacy applyUiResolve precedes CaptureWorld
and can restore raw scene pixels, but never samples the extracted layer itself.
World bloom intentionally retains HUD halo; radar/sun corona and some reticles
also remain world content. Generated world uses an A-to-B interpolated pose,
native coverage uses B's pose, and native phase0 uses B for both. Those facts
alone do not prove a wrong pose or the user's doubling mechanism. Existing eye
dump C/P/T/FinalCrisp captures will discriminate world-content leakage before a
further rendering fix. _UI.bin is coverage, not extracted color/transmittance.

2026-11-16 output Busy correction: saved cold-transition logs contain output
failure 9, HRESULT S_FALSE, nonquarantined, followed by coordinator reason 11.
The publisher explicitly returns that result when no output slot is reusable.
The worker previously converted it to generic Output, which sets unavailable;
the consumer excludes that state and can stop draining the leases causing
backpressure. The actual coordinator CPU fixture now checks ARMING/READY and
public counters/history for that exact result, seven closely related refusal
cases, and preservation of failed/closing state. A private old-classification
mutation fails four of the same 63 checks. CPU fixture results and logs are in
`outputs/Elite-Beyond-foveation-experiments/fg-ui-lifecycle-private/` in the
parent workspace. Full native validation passed in 310.47 seconds; actual
cold-scene recovery remains pending.

2026-11-16 readiness candidate: focused production-path CPU fixtures pass:
gaze 105 policy checks plus 43 adapter checks, VRS 53 checks, and temporal
owner 1466 checks using offscreen WARP. The temporal cases verify real output
publication, refusal of a foreign COM device, skipped/layer-only outputs,
revocation after an already-ready pair, and close/invalidate serialization.
Exact eafe startup code compiled against the new fixture fails only its new
readiness assertion (53 checks, one failure). Ignoring SDK token changes fails
three intended assertions; publishing readiness at begin instead of successful
outputs fails five. These are fixture results, not a measured game-startup or
performance improvement. Private logs and pins are in
`work/native-vrs-readiness-private/freeze-v1.json` in the parent workspace.

2026-11-16 attribution preparation: the verified old gfx log is
`edvr_gfx_20261116_044958.log` from PID 40028, build 7a. Native windows mapped
through their saved QPC/UTC anchors to approximately 04:53:45.160-04:54:45.160
and 04:57:15.914-04:58:15.914 local. Fully contained old draw-clock reports:

| Mode | Ending frame | Emitted local | Mean ms/sample frame | Max ms | Samples |
| --- | ---: | --- | ---: | ---: | ---: |
| On | 25200 | 04:54:20.478 | 1.638 | 14.016 | 112 |
| On | 27000 | 04:54:40.502 | 1.463 | 7.514 | 113 |
| Off | 43200 | 04:57:40.721 | 1.432 | 10.477 | 112 |
| Off | 45000 | 04:58:00.722 | 1.335 | 4.237 | 113 |

These are whole-minus-forwarded estimates, with native preparation incorrectly
subtracted in that old build. No forwarded-real subtotal was exported, and the
driver's raw draw cost may itself change with VRS. They do not bound or isolate
native preparation. The corrected fixture guards must fail against that old
placement before a new result is used; at that preparation checkpoint no such
execution had run yet.

Reuse the existing `draw hook CPU:` line: ending frame, sampled-frame mean,
maximum, sample count, stride, AA state and current eye-draw count. It summarizes
1800 frames, roughly 20 seconds at 90 Hz, not 30 seconds. For a future single
30-second cost observation, begin just after a fresh report boundary so the next
whole report window is contained. Otherwise a complete window can take nearly
40 seconds to arrive. One frame in 16 samples every 64th direct draw and scales
by 64; it is an estimate, not an exact per-stage profiler. Keep coverage,
work-reference, app-GPU and pass-profile diagnostics Off. None of this changes
the native VRS startup limitation or establishes a performance fix.

2026-11-16: ruled out: a missing shading-rate resource view alone explains the
Off arm's unknown state. `token.known` accepts known disabled rates without a
binding. Command-list invalidation clears that knowledge but retains historical
epochs. All 80 selected Off rows had `before_epoch = rate_epoch + 4`, consistent
with two before/after command-list invalidation pairs; the exact event sequence
was not logged. Unknown remains unknown.

Ruled out: the UI guard never allows Magic to restore VRS for the recognized
world subset. Earlier current-build observations and this On arm found accepted
rate maps bound on those draws. This does not establish whole-scene coverage.

Saved artifacts, relative to the workspace rather than the source checkout:

- `outputs/Elite-Beyond-foveation-experiments/vrs-counter-calibration-01/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/assessment-pipeline-v2-off-01/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/assessment-pipeline-v2-on-01/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/assessment-pipeline-v2-comparison-01.json`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/world-work-candidate-da9db38/`

The current game diagnostic was restored off, with runtime acknowledgement at
2026-11-16 09:57:03.312 UTC. No result above establishes lower GPU time, native
frame-rate improvement, or headset visual quality.

## 2026-11-16 native ownership candidate

The superseded preselected-offset experiment on installed `82c426bb` saw zero
selector hits after restart, so it produced no matched triplets or shader-work
conclusion. Prior-launch packed-buffer offsets are not portable; selector
absence alone cannot distinguish a hook-entry miss from a signature mismatch.

Ruled out: redundant per-draw provider queries explain the legacy slowdown.
Coordinate-free telemetry observed 406-421 finite queries per five seconds,
roughly once per native frame. Cost per query remains unknown. Ignoring gaze
also failed to remove the approximately 83 fps average limit, so a query-cache
change alone is unsupported. These findings and the clean three-arm native-only
controls were reconciled in the main tree's documentation at `9509a1c`.

Concurrent SteamVR/native captures had mismatched windows and shutdown overlap;
the attempted 80-second envelope still missed containment by 1.715 seconds.
Their descriptive subsets do not replace the clean native-only controls or
establish a CPU bottleneck.

Private evidence, relative to the workspace:

- `outputs/Elite-Beyond-foveation-experiments/native-vrs-private/provider-01/`
- `outputs/Elite-Beyond-foveation-experiments/native-vrs-private/gpu-01/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/assessment-work-reference-v1-01/`
- `outputs/Elite-Beyond-foveation-experiments/world-diagnostics/matched-performance-82c426b/`
- `work/native-vrs-port-private/freeze-v1.json` and the subsequent v2 addendum.

The root-run private GPU proof completed at 12:08:18 UTC. It exercises the real
native VRS owner and driver on a private offscreen target, with simulated gaze,
no game scene, no swapchain and no performance claim. The separate real-provider
probe supplies source-compatibility evidence. Neither substitutes for game
shader-work measurements, native cadence/true lows or visual assessment.

The official OpenVR 2.15.6 header remains byte-pinned and isolated from the
legacy ABI namespace. Its complete BSD notice is retained beside the header,
in root LICENSE and as RCDATA in the graphics DLL/standalone-installer payload.
No live config, external module or installed binary was changed by the port.

Full build 01 stopped at the game-address audit after 42 passing Python gates.
Ruled out: its four reported literals are new hardcoded game RVAs. They are
the public NvAPI_Initialize interface ID and the official OpenVR overlay action
priority bounds. Exact path-and-value exceptions now cover only those SDK
constants; adjacent values and other files remain rejected. The new regression
tests fail twice without these exceptions and pass with them; the actual source
audit passes. The official header and production C++ remain unchanged. A full
build rerun is required; this gate result is not rendering evidence.

Full build 02 found one production integration error: the new Present callback
was outside a BoundaryTick. It now has its own native_vrs_clock fault budget at
the same position, after the sole frame ordinal advances and before graphics
standdown; normal vendor failures remain inside the owner's existing retry
policy. The gate pins that order. Three UI bridge rigs now use their shared
stub owner for the default-off native symbols. The runtime eye-reader census
classifies the source SteamVR orientation-only read and pins its 3x3 bounds;
no game-scaled translation is consumed. Full build 03 must validate these fixes
and the complete rig suite. No private GPU proof needs repeating for them.

Full build 03 reached the FSS source contract, which still expected the reissue
clock immediately next to the raw draw. The assertion now requires the added
full-rate boundary followed by the unchanged raw draw; FSS's other raw entry
points are unchanged. The related SRV turret negative mutation now targets the
current conditional original-issue line, so its duplicate-count control remains
effective. The UI-quality whole-body contract now includes native world
qualification and dispatch while retaining its exact refusal/strip/observer
ordering. These are test-contract updates only; production and private GPU
fixture bytes are unchanged. The next full build remains the validation gate.

Full build 04 reached 190 of 214 rigs before the sharp-weapons fixture's two
formatter expectations still named 29 gate holders. NativeVrs is the 30th; the
iteration, uniqueness and holder behavior checks already use the dynamic count.
Only those two expected strings change. The synchronous formatter has no race
in this result; historical log fixtures stay unchanged. Full build 05 remains
required, with no production or private GPU fixture change.

## Fork publication — 2026-11-16 (Pacific)

This experimental publication consolidates the tested candidate 8fe2295 onto
base accc120. All production source/header blobs match that candidate. The two
Windows diagnostic launchers now resolve their own checkout and check directory
changes, instead of pointing at a personal, older worktree.

Both corrected launchers passed when invoked from a different working directory.
The host module compiled under /WX; the native FG, OpenXR frame and owner CPU
rigs passed 143, 322 and 64 checks respectively, with 0 failures. The full native
build results above belong to 8fe2295; the publication commit has no fresh full
DLL build or headset trial. The deployment remains the previously tested build.

The original experimental commit history, raw logs/images and configuration
backups remain in the private NAS archive. This branch contains source and
tracked reports, including the original SDK notices.

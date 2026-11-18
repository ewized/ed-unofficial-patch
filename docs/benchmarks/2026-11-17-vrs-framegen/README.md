# Parked Pilot Assessment benchmark — draft

## Status

```text
Report status: DRAFT — FINAL RESULTS PENDING
Experiment: parked Pilot Assessment, Mawson Dock
Requested matrix: 40 rows
Configurable rows: 32; measurements and qualification PENDING
Unsupported rows: 8 requested FG 3x rows; never apply
Runtime used for benchmark: virtual HMD, nominal 90 Hz
Requested virtual eye target: 3892 × 3892 pixels per eye
Physical display scanout qualified: NO
Physical gaze qualified: NO
Gaze used in benchmark: synthetic, forward, 64 mm eye separation
Flight operation: NEVER TAKE OFF; no flight controls
Final native/generated/repeated submission rates: PENDING
Final CPU/GPU timing statistics and coverage: PENDING
Final feature/mode qualification: PENDING
Final byte/timestamp/input-profile restoration proof: PENDING
Performance summary or recommendation: PENDING FINAL RAW STATISTICS
```

Measured code commit: `cfe5e8ca4241b7169eea524f6d50fc4c5f64c620`. Its full native Windows build passed 211 job steps and 79 Python gates.

This is a method and original-settings draft prepared while the sweep is in progress. It contains no final benchmark result. The results placeholders must be replaced only after the final raw logs, row qualifications, source identities and cleanup evidence have been reviewed. Earlier smoke runs do not populate the final matrix.

## Hardware and runtime context

The captured system inventory reports an AMD Ryzen 9 9950X3D with 16 cores and 32 logical processors, 32 GiB installed RAM in two 16 GiB modules, and 33,407,524,864 bytes of OS-visible RAM (about 31.11 GiB). The GPU is NVIDIA GeForce RTX 5090, driver 621.24, with 32,607 MiB of reported GPU memory. Windows 11 Pro is build 26300 (64-bit). The installed DLSS SR file version is 310.9.1.0; the NVIDIA frame-generation file version is 310.9.16.0.

A separate background probe identified a Bigscreen Beyond through SteamVR 2.19.3. It reported a 90 Hz headset property and a recommended 3892 × 3892 eye target, but the pose was invalid, with tracking result 101. This establishes limited device/runtime metadata. It does not qualify physical rendering, valid physical gaze, benchmark workload or delivered physical scanout.

The saved physical-runtime preferences were global supersampling 2.1 (210%), compositor supersampling 2.5 (250%), application resolution 100%, application preferred refresh 90 Hz and global preferred refresh 144 Hz. A saved preference is not measured cadence. Supersampling percentages must not be treated as linear per-axis pixel multipliers. The runtime probe also recorded a GPU-speed render-target scale of 1.5; use actual observed dimensions when describing an active render path.

The actual benchmark uses a virtual HMD at a nominal 90 Hz, with a requested 3892 × 3892 target. Its gaze is synthetic and forward-facing, with a 64 mm eye separation. The recorded workload FOV is approximately ±0.78540 radians on both axes, or about 90 degrees across each full frustum. Final row reports must preserve their actual per-eye input/output dimensions and observed runtime metadata. Neither the virtual cadence nor a submission-counter rate proves physical headset scanout.

The baseline desktop display configuration is 1280 × 720, windowed, VSync off, with a 240/1 desktop refresh preference. The frame limiter is disabled even though its saved maximum is 144. These are desktop settings, not the headset refresh or benchmark delivery rate.

## Original settings and controlled changes

The value-only exports are [original-settings.json](original-settings.json) and [original-settings.csv](original-settings.csv). They contain 328 portable scalar settings/properties. They are readable evidence, not deployable configuration files or a substitute for the private byte-exact preimages.

| Setting | Original value | Measurement value |
| --- | --- | --- |
| ED-UP temporal AA | `dlss` | `off` for native; `dlss` for P/B/Q |
| DLSS reconstruction model | `k` | `k`, fixed |
| Game HMD multiplier | `0.580000` | 1 / 0.5 / 0.58 / 0.666667 by group |
| Game SSAA multiplier | `1.000000` | unchanged |
| Game AA mode | `0` | unchanged |
| Eye-tracked foveation | `on` | `off` or `on` by row |
| VRS focus | `normal` | unchanged |
| Frame generation | `auto` | `off`, `auto`, `2x` or `4x` by supported row |
| Application GPU timing | `off` | `on` uniformly |
| FPS overlay | `on` | `off` uniformly |
| VRS coverage diagnostic | `off` | unchanged, off |
| VRS work reference / pixel-shader measurement | absent; source defaults off | remain off |
| Graphics preset file | `High`; settings XML names `Custom` | preserve both files; accept actual mode/dimensions from the log |

The saved `fix.native_input_scale=0.5` is legacy/inert in this candidate. It is preserved, and must not be used as evidence of active resolution. The saved OpenXR resolution override list is likewise an original preference, not proof of the virtual benchmark's actual target. Use observed dimensions and NGX feature-created logs.

The original game and mirror INIs are byte-equal, SHA256 `c5ea958581209d075718b6feb2e2e9fdf25a1c5182d13e7ea249f28eead91b6f`. Supported measurement payloads change only five existing keys: `fix.temporal_aa`, `fix.frame_generation`, `experimental.eye_tracked_foveation`, `advanced.app_gpu_timing` and `menu.fps_overlay`. All other INI bytes, model K, focus Normal and disabled diagnostics remain preserved. Game graphics payloads change only `HMDRenderTargetMultiplier`; the other graphics XML files remain unchanged. The saved graphics override has no overrides.

The FPS overlay is disabled because a smoke trial showed that the overlay itself asserts the runtime Menu admission gate and blocks FG even with F8 closed. This is a known product limitation and a controlled measurement condition. Original overlay-on and GPU-timing-off settings must be restored after the sweep.

## Requested matrix and pending results

P/B/Q mean requested Performance/Balanced/Quality upscaling modes. Model K is a reconstruction model, not a P/B/Q choice. The HMD multipliers below request ratios; both-eye dimensions and actual NGX mode must confirm the classification. NVIDIA's queried mode ranges and recommended dimensions can affect the selected mode.

| Group | Temporal AA | HMD multiplier | VRS | FG off | FG Auto | FG 2x | FG 3x | FG 4x |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Native | off | 1.000000 | off | PENDING | PENDING | PENDING | unsupported | PENDING |
| Native | off | 1.000000 | on | PENDING | PENDING | PENDING | unsupported | PENDING |
| Performance | dlss | 0.500000 | off | PENDING | PENDING | PENDING | unsupported | PENDING |
| Performance | dlss | 0.500000 | on | PENDING | PENDING | PENDING | unsupported | PENDING |
| Balanced | dlss | 0.580000 | off | PENDING | PENDING | PENDING | unsupported | PENDING |
| Balanced | dlss | 0.580000 | on | PENDING | PENDING | PENDING | unsupported | PENDING |
| Quality | dlss | 0.666667 | off | PENDING | PENDING | PENDING | unsupported | PENDING |
| Quality | dlss | 0.666667 | on | PENDING | PENDING | PENDING | unsupported | PENDING |

The [40-row results skeleton](matrix-results-pending.csv) deliberately leaves every supported measurement and qualification blank/PENDING. Eight FG 3x requests are unsupported and have no applicable payload. Native/AA-off lacks the successful native temporal producer needed by VRS/FG; seven supported native rows therefore carry explicit expected-inactive feature annotations. Requested settings must never be presented as proof those features ran.

Final per-row fields remain PENDING: applied-file identity, observed AA/NGX mode, both-eye input/output sizes, accepted CPU/GPU windows, p50/p95/p99, valid/invalid/missing sample counts, native/generated/repeated submission rates and coverage, VRS readiness/activity/error deltas, FG admission/effective mode, Auto state durations/transitions, and scene/ownership qualification. Partial timing coverage, fallback, expected inactivity and blocked features need separate labels. A collected row is not automatically a qualified feature comparison.

## Scene, ownership and timing method

Each resolution group uses one owned game lifetime, with graphics XML changes only while stopped. Existing finite Training navigation reaches Pilot Assessment at Mawson Dock. The ship stays docked on the pad, waiting for Select Auto Launch. The controller never selects Auto Launch, sends flight controls, undocks or takes off. Fixed head-pose and calibrated cockpit checks guard the scene outside the timed interval. Parked tutorial lighting, NPC activity and other scene changes can still vary the workload.

The initial gaze configuration is set once. A five-second tracking lease found during smoke testing is maintained using a 60,000 ms tracked-only head replay, renewed after at least 25 seconds. Replay steps contain only time, head device and tracked validity; they change no head position or orientation. Binocular validity, fixed neutral pose, dashboard state and exact owned process identity remain guarded. Replay is tracking maintenance, not a camera or ship movement. An uncertain replay completion fails the operation rather than authorizing a blind retry.

The native group is initialized through native-resolution DLAA before its measured AA-off rows. One direct AA-off cold launch failed the compositor scene-ownership check; that attempt is retained as a startup failure, not benchmark data. Successful AA-off handoff remains to be proved.

Every group is primed for 10 seconds with AA on, VRS on and FG off outside measurement. At the native group's 1.0 multiplier this is a DLAA setup phase, not the native/AA-off measurement. A further 60-second settle accommodates the tutorial elevator/scene transition. The actual requested row INI is restored before its 20-second row warmup and measurement. Priming activity must not be attributed to the measured row.

The target is three completed, nonoverlapping 30-second collector windows in the same game session per row, with the collector's two-second completion drain. The controller polls every 15 seconds and bounds each row to 160 seconds after applying that row. Its own 20-second row warmup is distinct from the collector's internal warmup. Windows crossing configuration/scope boundaries, incomplete windows and invalid/missing GPU coverage must not become qualified results. A timeout with fewer windows is partial coverage, not a fabricated three-window result.

Application GPU timing stays on and the FPS overlay stays off for every measurement. F8, screenshots and other menu/capture activity stay outside the measurement windows. VRS coverage, work-reference and pixel-shader diagnostics stay off; no new workload claim is inferred from a diagnostic placeholder.

## Interpretation and known limits

CPU timing is producer/pre-submit wall time, including the work span defined by the collector; it is not exclusive CPU execution time. GPU timing is an independent application-render GPU elapsed-time distribution. Do not sum them or invert their medians into a claimed frame rate. Final aggregate percentiles are medians of accepted per-window percentiles, not pooled percentiles. No native 1% low is invented from p99 or from the inverse of another timing statistic.

Native, generated and repeated submission rates come from actual fresh, same-context/generation counter deltas over their captured monotonic interval. Empty, failed and background submissions remain separate. Do not multiply native FPS by a selected FG multiplier. Submission rates describe application XR delivery, not compositor acceptance at a physical display or physical scanout. Counter freshness, coverage and identity changes must remain visible.

VRS on needs valid binocular gaze, actual driver/world admission and increasing relevant counters without hidden errors. A configured flag or priming success is insufficient. FG similarly needs actual camera/backend admission and observed delivery. Auto may remain native or change effective mode; report its observed modes and durations.

A corrected smoke run observed active VRS but FG camera refusal reason 5, mask `0x31`, with zero generated delivery. This is a known parked-scene limitation to examine in final data, not a final matrix result. The experiment never takes off to rescue FG admission. Actual full-sweep rows may be blocked, expected-inactive, partial or qualified; only their raw evidence decides.

## Restoration and provenance

Normal Stop must finish and authoritative stopped state must be proved before original settings are restored. Restoration uses exact last-applied byte guards and fresh receipts; configuration conflicts must be retained for inspection. Restore game/mirror INIs, game graphics, runtime preferences, original input profile and original selection-relevant timestamps. Final restoration verification is PENDING and must be joined to the completed report.

[Source provenance](source-provenance.json) gives SHA256 and byte size for all 11 source inputs: the normalized original ED-UP settings snapshot, game/mirror INI snapshots, four graphics/display XML files, input preset, physical probe, initial environment and v2 matrix manifest. These hashes identify private immutable inputs; raw snapshots are not bundled. The v2 matrix manifest SHA256 is `b3ad41abc4127f1da801debf83de343257ae2ae865af12f1ec73bb823ebb9763`.

The candidate build identity supplied by the coordinator is `cfe5e8ca4241b7169eea524f6d50fc4c5f64c620`. Final reports still require their actual installed/runtime build verification and captured raw-file hashes. This offline draft does not establish the identity or completion of a live process.

The [privacy check](privacy-check.json) records export checks. The exports omit personal filesystem configuration, device identities, tracking coordinates, network addresses and account/game identity. The optional [offline export generator](prepare_exports.py) reads local snapshots only and refuses existing export filenames. Re-run it into a fresh directory when deliberate regeneration is required. No live machine access or tracked repository changes were used to prepare this draft.

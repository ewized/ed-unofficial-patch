# Parked Pilot Assessment benchmark

## Status

```text
Report status: PARTIAL — FULL SWEEP STILL RUNNING
Measured code: cfe5e8ca4241b7169eea524f6d50fc4c5f64c620
Experiment: parked Pilot Assessment, Mawson Dock; never take off
Requested matrix: 40 rows; 32 configurable; 8 FG 3x unsupported
Completed: Balanced 8/8; 24 accepted 30-second CPU/GPU windows
Pending: Performance 8; Quality 8; Native/all-off 8
Balanced native submissions: 32.59–32.62/s; generated: 0/s
Balanced VRS: active in all four requested-on rows
Balanced FG: Off verified; Auto/2x/4x inactive, camera refusal observed
Runtime: virtual HMD, nominal 90 Hz, observed 3892 × 3892 per eye
Physical headset metadata: Bigscreen Beyond, reported 90 Hz
Physical scanout and physical gaze qualified: NO
Gaze: synthetic forward, 64 mm eye separation; fixed head pose
Balanced normal Stop, original bytes/timestamps/input restoration: PASS
Final complete-matrix conclusions: PENDING
```

Measured code commit: `cfe5e8ca4241b7169eea524f6d50fc4c5f64c620`. Its full native Windows build passed 211 job steps and 79 Python gates.

This checkpoint contains the eight completed Balanced combinations and their raw measurements. Performance, Quality and Native/all-off remain pending. Earlier smoke runs and failed startups do not populate the result cells. Results describe this unattended virtual workload, rather than physical headset delivery.

## Hardware and runtime context

The captured system inventory reports an AMD Ryzen 9 9950X3D with 16 cores and 32 logical processors, 32 GiB installed RAM in two 16 GiB modules, and 33,407,524,864 bytes of OS-visible RAM (about 31.11 GiB). The GPU is NVIDIA GeForce RTX 5090, driver 621.24, with 32,607 MiB of reported GPU memory. Windows 11 Pro is build 26300 (64-bit). The installed DLSS SR file version is 310.9.1.0; the NVIDIA frame-generation file version is 310.9.16.0.

A separate background probe identified a Bigscreen Beyond through SteamVR 2.19.3. It reported a 90 Hz headset property and a recommended 3892 × 3892 eye target, but the pose was invalid, with tracking result 101. This establishes limited device/runtime metadata. It does not qualify physical rendering, valid physical gaze, benchmark workload or delivered physical scanout.

The saved physical-runtime preferences were global supersampling 2.1 (210%), compositor supersampling 2.5 (250%), application resolution 100%, application preferred refresh 90 Hz and global preferred refresh 144 Hz. A saved preference is not measured cadence. Supersampling percentages must not be treated as linear per-axis pixel multipliers. The runtime probe also recorded a GPU-speed render-target scale of 1.5; use actual observed dimensions when describing an active render path.

The actual benchmark uses a virtual HMD at a nominal 90 Hz, with a requested 3892 × 3892 target. Its gaze is synthetic and forward-facing, with a 64 mm eye separation. The recorded workload FOV is approximately ±0.78540 radians on both axes, or about 90 degrees across each full frustum. Final row reports must preserve their actual per-eye input/output dimensions and observed runtime metadata. Neither the virtual cadence nor a submission-counter rate proves physical headset scanout.

The baseline desktop display configuration is 1280 × 720, windowed, VSync off, with a 240/1 desktop refresh preference. The frame limiter is disabled even though its saved maximum is 144. These are desktop settings, not the headset refresh or benchmark delivery rate. The observed benchmark client window is 2032 × 2032.

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

## Requested matrix and current results

P/B/Q mean requested Performance/Balanced/Quality upscaling modes. Model K is a reconstruction model, not a P/B/Q choice. The HMD multipliers below request ratios; both-eye dimensions and actual NGX mode must confirm the classification. NVIDIA's queried mode ranges and recommended dimensions can affect the selected mode.

| Group | Temporal AA | HMD multiplier | VRS | FG off | FG Auto | FG 2x | FG 3x | FG 4x |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Native | off | 1.000000 | off | PENDING | PENDING | PENDING | unsupported | PENDING |
| Native | off | 1.000000 | on | PENDING | PENDING | PENDING | unsupported | PENDING |
| Performance | dlss | 0.500000 | off | PENDING | PENDING | PENDING | unsupported | PENDING |
| Performance | dlss | 0.500000 | on | PENDING | PENDING | PENDING | unsupported | PENDING |
| Balanced | dlss | 0.580000 | off | 32.62 | 32.59 | 32.61 | unsupported | 32.61 |
| Balanced | dlss | 0.580000 | on | 32.61 | 32.62 | 32.61 | unsupported | 32.61 |
| Quality | dlss | 0.666667 | off | PENDING | PENDING | PENDING | unsupported | PENDING |
| Quality | dlss | 0.666667 | on | PENDING | PENDING | PENDING | unsupported | PENDING |

Balanced table cells give native XR submissions per second; all eight completed rows have zero generated delivery. The [40-row results CSV](rows.csv) records actual metrics and qualification for eight rows and leaves 24 pending. Eight FG 3x requests are unsupported and have no applicable payload. Native/AA-off lacks the successful native temporal producer needed by VRS/FG; seven supported native rows therefore carry explicit expected-inactive feature annotations. Requested settings must never be presented as proof those features ran.

Completed rows include both-eye mode/dimension checks, CPU/GPU p50/p95/p99, accepted and rejected sample counts, actual native/generated/repeated submissions and coverage, VRS activity/error deltas, and FG/Auto admission state. Pending rows contain no invented measurements. A collected timing row does not prove requested FG activation.

| VRS | FG request | Native/s | Generated/s | CPU p50 ms | GPU p50 ms | GPU p95 ms | Qualification |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| off | off | 32.615 | 0.000 | 6.803 | 12.56 | 13.73 | measured |
| off | auto | 32.589 | 0.000 | 6.787 | 12.634 | 13.833 | measured_inactive |
| off | 2x | 32.607 | 0.000 | 6.786 | 12.626 | 13.725 | measured_inactive |
| off | 4x | 32.611 | 0.000 | 6.69 | 12.572 | 13.68 | measured_inactive |
| on | off | 32.609 | 0.000 | 6.991 | 12.754 | 13.925 | measured |
| on | auto | 32.621 | 0.000 | 7.003 | 12.75 | 13.921 | measured_inactive |
| on | 2x | 32.608 | 0.000 | 6.883 | 12.747 | 13.972 | measured_inactive |
| on | 4x | 32.607 | 0.000 | 6.916 | 12.729 | 13.875 | measured_inactive |


## Scene, ownership and timing method

Each resolution group uses one owned game lifetime, with graphics XML changes only while stopped. Existing finite Training navigation reaches Pilot Assessment at Mawson Dock. The ship stays docked on the pad, waiting for Select Auto Launch. The controller never selects Auto Launch, sends flight controls, undocks or takes off. Fixed head-pose and calibrated cockpit checks guard the scene outside the timed interval. Parked tutorial lighting, NPC activity and other scene changes can still vary the workload.

Synthetic binocular gaze is set once. A tracked-only 60-second head replay, renewed after at least 25 seconds, keeps its five-second lease valid without position or orientation changes. Exact process identity, neutral head pose, binocular validity and closed dashboard remain guarded. Uncertain requests are not blindly replayed.

The native group is initialized through native-resolution DLAA before its measured AA-off rows. One direct AA-off cold launch failed the compositor scene-ownership check; that attempt is retained as a startup failure, not benchmark data. Successful AA-off handoff remains to be proved. In two failed starts, the first graphics log appeared about 181 seconds after game creation, just after the 180-second observation budget. The relationship to observer polling is unproven; longer and less frequent startup observations are being prepared without weakening readiness predicates.

Every group receives 10 seconds of AA-on/VRS-on/FG-off priming, then 60 seconds of scene settling, outside measurement. Native 1.0 priming uses DLAA. Each requested row is applied before its 20-second warmup; priming activity never counts toward the row.

The target is three completed, nonoverlapping 30-second collector windows in the same game session per row, with the collector's two-second completion drain. The controller polls every 15 seconds and uses a 185-second polling deadline after applying that row; one in-flight poll may finish after that deadline. Its own 20-second row warmup is distinct from the collector's internal warmup. Windows crossing configuration/scope boundaries, incomplete windows and invalid/missing GPU coverage must not become qualified results. A timeout with fewer windows is partial coverage, not a fabricated three-window result.

GPU timing stays on and the FPS overlay stays off. F8/captures stay outside timed intervals; VRS coverage/work-reference/pixel-shader diagnostics remain off.

## Interpretation and known limits

CPU timing is producer/pre-submit wall time, including the work span defined by the collector; it is not exclusive CPU execution time. GPU timing is an independent application-render GPU elapsed-time distribution. Do not sum them or invert their medians into a claimed frame rate. Final aggregate percentiles are medians of accepted per-window percentiles, not pooled percentiles. Rows run in the fixed order Off, Auto, 2x, 4x with VRS off, then the same order with VRS on. They share process/cache history and are not randomized independent trials. No native 1% low is invented from p99 or from the inverse of another timing statistic.

Native/generated/repeated rates use fresh same-context/generation counter deltas over their captured monotonic intervals, including explicit coverage. Empty/failed/background submissions remain separate. Selected multipliers are never multiplied into FPS. These are application XR submissions, not physical scanout.

VRS activation requires valid binocular gaze, driver/world admissions and increasing counters without errors. FG requires actual admitted sources and generated delivery. Auto selections, durations and transitions remain separate from observed output.

The completed Balanced rows observe active VRS but zero generated delivery for Auto/2x/4x, with actual producer camera refusal reason 5 and mask `0x31`. This limits conclusions for this specific captured configuration; it does not establish that every parked scene refuses FG. The experiment never takes off to rescue FG admission. Actual full-sweep rows may be blocked, expected-inactive, partial or qualified; only their raw evidence decides.

## Restoration and provenance

Normal Stop must finish and authoritative stopped state must be proved before original settings are restored. Restoration uses exact last-applied byte guards and fresh receipts; configuration conflicts must be retained for inspection. Restore game/mirror INIs, game graphics, runtime preferences, original input profile and original selection-relevant timestamps. Balanced restoration passed for original game/mirror INIs, graphics selection, virtual-driver file and their exact timestamps, plus the original input preset. Later groups and the final broader preference audit remain pending. See [restoration evidence](restoration.json).

[Source provenance](source-provenance.json) identifies the 11 private original inputs and v2 profile manifest by size and SHA256; raw personal snapshots are excluded. The manifest hash is `b3ad41abc4127f1da801debf83de343257ae2ae865af12f1ec73bb823ebb9763`.

Both logs for every measured row are selected by verified process birth and PID where present, then checked through the project log reader against the installed candidate. Earlier same-build logs cannot substitute for a new process that has not initialized. The full build's receipt SHA256 is `48cf115d51940a194eedad0c0f9c49010178849800a61ab03b643d241ebe4a73`.

A diagnostic startup also loaded `CheekyOpenXRLayer.dll`; its effect is not isolated by this sweep. No third-party layer setting is changed for a treatment.

## Raw data

- [All requested rows and aggregates](rows.csv), [individual CPU/GPU windows](windows.csv), [GPU/process samples](system_samples.csv).
- [Presentation counter timeline](presentation_timeline.csv.gz) and [actual counter deltas](presentation_deltas.csv.gz).
- [FG/VRS admission observations](admissions.csv.gz) and [complete structured export](results.json.gz).
- [Integrity and privacy checks](integrity.json) identify stored and uncompressed SHA256 values. Gzip uses a zero timestamp for reproducibility; `gzip -dc FILE.gz` reads the raw data without changing it.

All percentiles remain per-window observations or explicitly labelled medians of those percentiles. Missing values stay empty/null. Raw source hashes identify retained private logs; account details, personal hosts/paths, device serials, screenshots and tracking coordinates are excluded from the published data.

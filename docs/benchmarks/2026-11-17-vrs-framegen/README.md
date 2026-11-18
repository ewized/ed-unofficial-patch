# Parked Pilot Assessment benchmark

## Status

```text
Report: MATRIX ACCOUNTED — QUALIFICATION LIMITS APPLY
Measured code: cfe5e8ca4241b7169eea524f6d50fc4c5f64c620
Documentation baseline: 832f421420ab294d93dda40249f2f4bcf4a5f223
Requests: 40; 32 configurable; 8 FG 3x unsupported
Current labels: 7 measured; 25 measured inactive; 0 partial; 0 blocked; 0 pending
Native AA-off 3892-to-3892 input/output verified: 8/8 configurable rows
Raw CSV tables available: 10/10
Restoration: PASS: all four groups, normal Stop, original bytes/captured timestamps/input, and broader preferences
Physical gaze / physical scanout qualified: NO / NO
```

All 40 requests are accounted for. The labels and raw coverage determine which comparisons are supported; inactive, partial and blocked cells do not establish active-feature performance. Measured code and the later documentation commit are separate identities. This README displays recorded statistics and qualification, without choosing a performance winner.

## Machine and workload

The captured inventory is AMD Ryzen 9 9950X3D (16 cores/32 logical processors), 32 GiB installed RAM in two 16 GiB modules, and 33,407,524,864 bytes OS-visible RAM. GPU: NVIDIA GeForce RTX 5090, driver 621.24, 32,607 MiB reported memory. OS: Windows 11 Pro build 26300, 64-bit. Installed DLSS SR/FG versions: 310.9.1.0 / 310.9.16.0. The CFE native build passed 211 job steps and 79 Python gates; documentation changes are not a new measured build.

A separate SteamVR 2.19.3 background probe reported Bigscreen Beyond, 90 Hz and recommended 3892 × 3892 per eye, but pose invalid/tracking result 101. This is physical-device metadata only. The actual benchmark is a **virtual HMD**, 3892 × 3892 per eye, nominal 90 Hz, fixed approximately 90° FOV on both axes (±0.78540 rad), and synthetic forward gaze with 64 mm eye separation. It does not qualify physical gaze, display cadence or compositor reprojection.

Original desktop preference: 1280 × 720, windowed/VSync off, desktop refresh 240/1, limiter disabled (saved maximum 144). Actual benchmark client: 2032 × 2032. Desktop, render-target and headset dimensions are distinct.

## Original preferences and test controls

| Preference | Original | Test |
| --- | --- | --- |
| SteamVR manual supersampleScale | 2.1 | 1.0 |
| Compositor supersampleScale | 2.5 | unchanged |
| Application resolution | 100% | unchanged |
| Application/global preferred refresh | 90 / 144 Hz | unchanged |
| GPU-speed render-target scale | 1.5 | unchanged |
| DLSS reconstruction / VRS focus | K / Normal | unchanged |
| ED-UP AA / VRS / FG | DLSS / on / Auto | row request |
| Game HMD multiplier / SSAA / AA mode | 0.58 / 1 / 0 | 1, 0.5, 0.58 or 0.666667 / 1 / 0 |
| Application GPU timing / FPS overlay | off / on | on / off, uniformly |

The virtual driver forces the virtual HMD with `requireHmd=false`, `multipleDrivers=true`, `pauseCompositorOnStandby=false`, and ten virtual tracker roles. No Home setting or compositor-scale-1 override is introduced. Saved percentages are not linear per-axis dimensions; observed dimensions and actual NGX mode determine the treatment.

`High` is the graphics preset file; Settings XML names `Custom`. It is **not a bindings preset**. Both graphics files are preserved. A temporary keyboard profile is restored through its own receipt. Legacy `fix.native_input_scale=0.5` is inert and preserved; it does not establish active resolution. Original values are exported separately, rather than as deployable configs.

INI payloads change only temporal AA, frame generation, eye-tracked foveation, application GPU timing and FPS overlay. Model K, Normal focus, SSAA 1, game AA 0 and disabled VRS coverage/work-reference/pixel-shader diagnostics stay fixed. The overlay is off because FPS-overlay rendering can assert FG's Menu gate even with F8 closed. Original overlay-on/timing-off and broader preferences must be restored afterward.

## Actual native XR submission rates

These are actual native-counter deltas per second, **not** total display FPS. `I` means measured inactive features; `P` partial coverage; `U` unverified native 1:1 metadata. Blocked/pending/unsupported cells are not measurements of the requested active combination.

| Mode | VRS | FG off | Auto | 2x | 3x | 4x |
| --- | --- | --- | --- | --- | --- | --- |
| Native | off | 32.62 | 16.99 I | 16.31 I | unsupported | 8.16 I |
| Native | on | 32.62 I | 16.31 I | 16.31 I | unsupported | 8.16 I |
| Performance | off | 32.59 | 32.61 I | 32.59 I | unsupported | 32.61 I |
| Performance | on | 32.61 | 32.62 I | 32.59 I | unsupported | 32.60 I |
| Balanced | off | 32.62 | 32.59 I | 32.61 I | unsupported | 32.61 I |
| Balanced | on | 32.61 | 32.62 I | 32.61 I | unsupported | 32.61 I |
| Quality | off | 32.60 | 32.59 I | 32.60 I | unsupported | 32.60 I |
| Quality | on | 32.60 | 32.61 I | 32.59 I | unsupported | 32.59 I |

The [40-row CSV](rows.csv) records every permutation and qualification. Native is literal AA-off at HMD 1; valid native measurements require actual 3892 × 3892 input **and** output per eye. P/B/Q request multipliers 0.5/0.58/0.666667 with DLSS; both-eye NGX mode/dimensions must confirm them. K is a reconstruction model, not a quality level. FG 3x is unsupported and never applied. Without a successful temporal producer, native VRS/FG are expected inactive. Auto selecting 2x is not proof of generated delivery.

## Generated delivery

Counts cover the recorded settled-row intervals only. Selected `0` means off/native. A selected multiplier does not multiply the observed native rate; missing counters remain unavailable. Repeated submissions are distinct from generated frames. Repeated coverage is known/eligible rows; repeated totals and ranges cover only that known subset. In AA-off native rows, a roughly unchanged total submission rate can contain fewer native submissions and more repeats; it does not establish generated-frame delivery.

| FG request | Rows with counters | Selected multipliers | Generated frames | Generated/s range | Repeated coverage | Repeated frames | Repeated/s range |
| --- | ---: | --- | ---: | --- | --- | ---: | --- |
| off | 8 | 0 | 0 | 0.000–0.000 | 8/8 | 0 | 0.000–0.000 |
| auto | 8 | 2 | 0 | 0.000–0.000 | 8/8 | 4306 | 0.000–16.323 |
| 2x | 8 | 2 | 0 | 0.000–0.000 | 8/8 | 4129 | 0.000–16.320 |
| 4x | 8 | 4 | 0 | 0.000–0.000 | 8/8 | 6180 | 0.000–24.484 |

## FG-off timing summary

Triples are p50/p95/p99 milliseconds; the cycle mean is the sample-weighted reported phase mean. CPU/GPU and cycle populations have distinct boundaries and coverage. Empty/unavailable values are not zero.

| Mode | VRS | Native XR/s | CPU wall p50/95/99 ms | GPU p50/95/99 ms | Cycle mean; p50/95/99 ms | RealPresent mean ms | Status |
| --- | --- | ---: | --- | --- | --- | ---: | --- |
| Native | off | 32.622 | 6.433/7.996/8.783 | 12.080/13.153/13.705 | 30.655; 30.633/31.462/31.793 | 0.072 | measured |
| Native | on | 32.624 | 6.458/8.036/9.130 | 12.044/13.171/13.774 | 30.649; 30.623/31.455/31.834 | 0.072 | measured_inactive |
| Performance | off | 32.588 | 6.829/8.406/9.209 | 11.974/13.156/13.542 | 30.688; 30.699/31.474/31.963 | 0.070 | measured |
| Performance | on | 32.609 | 7.029/8.658/9.511 | 12.208/13.321/13.835 | 30.664; 30.642/31.488/31.857 | 0.071 | measured |
| Balanced | off | 32.615 | 6.803/8.480/9.252 | 12.560/13.730/14.280 | 30.659; 30.614/31.472/31.891 | 0.069 | measured |
| Balanced | on | 32.609 | 6.991/8.546/9.482 | 12.754/13.925/14.360 | 30.667; 30.653/31.459/31.819 | 0.069 | measured |
| Quality | off | 32.599 | 6.824/8.352/9.548 | 13.883/15.041/15.627 | 30.680; 30.653/31.526/32.007 | 0.070 | measured |
| Quality | on | 32.603 | 7.059/8.575/9.483 | 14.031/15.222/15.674 | 30.671; 30.659/31.504/31.901 | 0.070 | measured |

CPU is producer/pre-submit wall time, not exclusive CPU execution. GPU is independent application-render elapsed time; do not add them or invert either median into FPS. Benchmark percentiles remain per-window values; row percentiles are medians of accepted window percentiles, not pooled percentiles. No inverse-p99 “1% low” is invented.

Runtime frame cycles span host wait-return to next wait-return. Zero runtime input dimensions mean FOV-trim metadata unavailable, **not failed timing or zero-size rendering**; null dimensions/raw zeros remain in the runtime CSV. Output dimensions and clock/thread/boundary guards stay strict. Row phase means weight each reported window mean by its sample count; percentiles remain per-window or medians of window percentiles. Nested phases are not additive. The raw runtime `late_frames` field is retained as reported; it is not a physical dropped-frame count.

True RealPresent is the post-submit `raw_dxgi_present` phase, measured around the DXGI Present call. The containing post-submit window covers second-submit-return to next-wait-entry; exported time bounds use parent-cycle coverage for row qualification, rather than one continuous Present span. Call-count phases retain `calls` units. Missing post-submit evidence stays unavailable, never inferred from submit roundtrips or runtime-cycle time.

## Scene and sampling

Pilot Assessment at Mawson Dock remains docked. The tutorial elevator automatically raises the ship from hangar to the docked launch pad. **Never select Auto Launch; never take off.** No flight controls are sent. Fixed neutral pose, valid binocular gaze, hidden dashboard and owned process/session checks guard sampling; capture/UI checks occur outside measurement. Tutorial lighting/NPC work can still vary.

Each resolution group has its own game lifetime. Ten seconds of AA-on/VRS-on/FG-off priming precede measurement; HMD-1 native priming is DLAA setup. A further 60-second settle accommodates the elevator. Apply the actual row before its 20-second warmup. Target: three completed nonoverlapping 30-second CPU/GPU windows plus two-second drain. Polling is every 15 seconds with a 185-second row deadline; one in-flight poll may finish afterward. Rows share process/cache history and fixed order Off, Auto, 2x, 4x with VRS off, then on; this is not a randomized independent-trials design.

A 60-second tracked-only head replay, renewed after at least 25 seconds, maintains the five-second synthetic gaze lease without position/orientation changes. Unknown command completion is not blindly replayed. Priming, scene settling and capture activity are excluded from row measurements.

Native/generated/repeated XR rates use fresh same-identity counter deltas over captured intervals, not selected multipliers, physical scanout or compositor reprojection. FG producer camera mask `0x31` and backend failure code `5` are separate diagnostic namespaces; one must not be decoded as the other. VRS on requires actual valid gaze/world/map activity, not just configuration. Admission, invalid/missing coverage and inactive features remain explicit in the data. Row invalid/missing diagnostics count windows wholly inside the settled interval; unsplittable boundary-crossing windows have separate fields. A diagnostic startup also loaded `CheekyOpenXRLayer.dll`; its effect is not isolated. No third-party layer setting changes between treatments.

## Data, restoration and integrity

- [Readable timings for every permutation](permutations.md); [all 40 raw rows](rows.csv), [eligible CPU/GPU windows](windows.csv), [GPU/process samples](system_samples.csv).
- [XR counter timeline](presentation_timeline.csv.gz), [actual deltas](presentation_deltas.csv.gz), [FG/VRS admission evidence](admissions.csv.gz).
- [raw runtime windows](runtime_windows.csv), [raw runtime phases](runtime_phases.csv.gz).
- [raw post-submit windows](postsubmit_windows.csv), [raw post-submit phases](postsubmit_phases.csv.gz).
- [Combined structured results](results.json.gz); [original settings JSON](original-settings.json)/[CSV](original-settings.csv), [hardware/software](hardware-software.json), [original-source provenance](source-provenance.json).

[Restoration proof](restoration.json): **PASS: all four groups, normal Stop, original bytes/captured timestamps/input, and broader preferences**. Provided proof must cover normal Stop, empty game/runtime census, original game/mirror INIs, graphics selection, virtual-driver bytes/timestamps, keyboard profile and final broader preferences. The final audit covers 13 file identities/existence checks and six captured timestamps. Five existing-file timestamps were never captured, so their restoration is not claimed. The SteamVR-settings timestamp baseline was captured after physical-probe byte restoration; its earlier timestamp is unknown. A checkpoint does not certify later groups. This offline generator does not inspect a live machine.

[Integrity](integrity.json) identifies every stored artifact by SHA256 and, where compressed, its uncompressed name/hash/size. Source logs remain private; public records retain source hashes without personal paths, identities, tracking coordinates or account data. Gzip links reflect the chosen publication encoding; `gzip -dc FILE.gz` reads them. No raw table is replaced by a screenshot or a summary statistic.

Generation inputs (SHA256): canonical README `706f073343a2c4f62d3776307b90f0ae982c8b54908be576d059ed60b7f56435`; results JSON `6a531bb01112835e5247b5181c57ee556b83fcfb85f7f71fdc2e7073eeffba4a`; restoration proof `5c7521b84d0cc1c882e3d73c57ad5dacb48d05e9293ef19c0a351d9e227e5b7a`. CSV/JSON agreement and all declared artifact hashes were checked. Documentation baseline is `832f421420ab294d93dda40249f2f4bcf4a5f223`; measured candidate remains `cfe5e8ca4241b7169eea524f6d50fc4c5f64c620`.

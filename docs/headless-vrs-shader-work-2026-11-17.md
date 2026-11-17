# Headless VRS sampled shader work — 2026-11-17

## Status

- Existing sealed `8fe2295` met the diagnostic criterion of at least five matched
  full/native-map/full triples per eye: **left 11, right 5**, docked at Mawson Dock
  in Pilot Assessment. No C++ or instrumentation changes accompanied this run.
- For these selected opaque original draws, median native-map PSInvocations
  were **29.8626% left / 29.9289% right** of average full-rate controls: roughly
  70% fewer invocations. This establishes sampled shading reduction, not
  whole-frame GPU savings, image quality, physical-headset behavior or 90 Hz.
- All 506 sample rows were valid; 16 of 30 completed triples matched. The other
  14 failed full-control stability and are excluded. Finished/drained pending=0.
- Recognized HUD was excluded. Generic world_ps_reduction_verified=0 and
  gpu_benefit_verified=0 fields are hard-coded placeholders, not calculated results.
- Saved pre-probe INI/mirror were restored exactly; simulator stopped. Normal
  controller Stop passed stopped_clean/exit 0, no cleanup errors, inactive
  receipt and no remaining game/runtime processes.
- A later focused, queries-disabled VRS On/Off/On comparison qualified all three
  SDK windows, but GPU-span medians 8.6920/8.8723/8.9147 ms establish no repeatable
  timing benefit. Return-On drift exceeds the initial On/Off difference.
- Earlier physical-headset misses do not transfer to this scene. Successful
  discovery here does not fix known selector starvation. Whole-frame benefit
  and visual qualification remain open.

## Configuration and provenance

The session owner verified 2320x2320 world input, 4000x4000 output per eye, TAA,
FG Off, Turbo Off, VRS mode 1 and very-narrow foveation with the virtual HMD and
synthetic eye input described in [headless testing](headless-vr-testing.md).
Every sample independently records size=2320x2320; the filtered artifact alone
cannot prove output size, every setting or physical gaze calibration. Earlier
cadence and GPU measurements remain in the
[performance report](headless-vr-performance-2026-11-17.md).

Raw artifact: `vrs-ps-5d56b066.txt`, UTF-16LE with BOM, 758736 bytes, SHA-256
`7d6c53c1b9c91facc946dd0662634c64a00a9c64f58383bdb9b5ac463e0cab8c`.
This filtered `edvr_log.py` output records version v0.22.1-21-g8fe2295,
build 6AFB9A5B and the expected-build result “build matches: 8fe2295”. Installed
candidate: `8fe229513ac23ab6964f6cbda1fbac700e539299`.

Its 955 diagnostic lines comprise 1 armed, 416 candidate, 506 sample, 30 triple,
1 finished and 1 drained. Log times span 00:05:46.007–00:06:01.004; sampled
frames span 6839–7315, below the 600-frame limit. Finished counters are
matched_left=11, matched_right=5, pending=0; drained has pending=0/new_queries=0.
The latter's performance_window=eligible_after_settle does not qualify the
instrumented interval itself as a performance window.

## Method and independent validation

The existing [recorder](../src/d3d11/native_vrs_ps_measure.h) and
[VRS integration](../src/d3d11/native_vrs.cpp) alternate full rate, native map,
then full rate across recurring draws, without replay. The
[draw scope](../src/d3d11/vscreen.cpp) brackets the first forwarded original API
draw; HUD, mirrors and later reissues stay outside. Budget: one query per eye
per frame, DONOTFLUSH polling and at least 4096 PSInvocations for a useful first
full control.

Independent parsing joined every triple to its three sample rows by
candidate_instance, eye, frame and phase. It reproduced acceptance for all
30 triples, checking:

- Valid samples/query success and exact agreement with the triple's PS counts.
- Identical logged VS/PS/state identities, draw arguments and target size;
  identical seven non-PS pipeline statistics, including IA/VS/GS/clipping work.
- Same eye/config/gaze_epoch/map, increasing frame/sequence and span <=12 frames.
- Nonzero full controls differing by at most floor(min(full_before,full_after)/100).

All 14 rejected triples fail only the last check among these reconstructed tests.
Every sample also satisfies IAVertices=count*instances and
IAPrimitives=(count*instances)/3. Individual valid rows rely on internal
before/after receipt and restoration checks; both receipts are not exposed in
text. Invalidation serials may advance between separately qualified draws.

## Results

For each accepted triple, F=(full_before+full_after)/2 and R=native_map/F.
The median/range describe per-triple ratios. Pooled R=sum(native_map)/sum(F)
weights these selected observations by control work; it is not a frame metric.

| Eye | Samples | Matched / completed triples | Median R | R range | Mean R | Pooled R |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Left | 259 | 11 / 20 | 29.8626% | 22.8604–40.1298% | 33.5864% | 32.6466% |
| Right | 247 | 5 / 10 | 29.9289% | 29.6011–40.1998% | 33.6950% | 32.6053% |

Median reductions are 70.1374% left and 70.0711% right **for selected draws**.
Accepted F ranges are 5654–9263.5 left and 5589–9236 right; native-map counts
are 1628–2759 left and 2172–2760 right. Observations reuse three candidate
identities left (5/1/5 triples) and two right (2/3), not 16 independent scenes.

Across all 506 samples, PSInvocations range 0–193274, including 274 zero-work
rows. That maximum is not a matched-triple size. Samples report hud_excluded=1
and scope=first_forwarded_opaque_world_draw; triples report
scope=sampled_world_only/performance_claim=0.

## Limits and next qualification

This rules out “the native map never reduces PSInvocations in this headless
scene.” It does not establish the fraction of frame work covered, elapsed GPU
savings, visual acceptability or a 90-fps result. Equal logged identities/work
and stable full controls do not freeze resource contents, visibility or every
shader input. Repeated selected observations do not supply a whole-frame
confidence interval or speedup.

Generic world_ps_reduction_verified=0 and gpu_benefit_verified=0 are literal
placeholders in [vscreen.cpp](../src/d3d11/vscreen.cpp). They neither negate this
sampled evidence nor establish frame benefit. HUD composition is unchanged;
projection mapping and physical calibration remain separate qualifications.

The [earlier physical-headset query](beyond-vrs-verification.md) found negligible
work and no matched triples; this different scene does not explain that miss.
The [source audit](performance-audit-beyond-2026-11-16.md) reproduces selector
starvation with a synthetic scheduling transcription. That issue remains open.

The subsequent timing comparison below measures a different scope from these
draw queries. Sampled PS reduction does not predict its elapsed-time result.

## Queries-disabled VRS On/Off/On comparison

A fresh session of the same sealed build reached the docked Mawson scene after
instructor readiness and open dock doors. Owner-verified settings: TAA, FG Off,
Turbo Off, 2320 input to 4000 output, very-narrow VRS; PS diagnostics and native
application GPU timing remained Off for consistent overhead. Only
experimental.eye_tracked_foveation changed On/Off/On through saved INI/mirror
preimages. Native benchmark records independently confirm AA on and these sizes.

Each window renewed synthetic eyes and a 60-second neutral-head replay before
collection, with no helper actions during collection. Return-On renewed its
replay through Stop/Start before timing; an earlier rejected eye update before
Off was retried outside its timing interval. Those setup details are the
session owner's record, not something inferred from GPU samples.

The private dataset `vrs-ab-benchmarks` retains raw frames, history batches,
rejections, summaries, control/focus records and the expected-build-filtered
UTF-16LE `vrs-ab-gfx-evidence.txt` / `vrs-ab-cadence-evidence.txt`. Independent
parsing exactly reproduced all three raw-frame median/p95/p99 values. All SDK
windows qualified with no rejected samples, invalid records, history overruns
or clock discontinuities. Selected SDK spans were 29.9273/29.9353/29.8956 seconds
for the requested 30-second windows.

Metric: SteamVR m_flTotalRenderGpuMs, the GPU span from work submitted immediately
after present through the end of compositor work. This includes compositor work;
it is not isolated application GPU time or native frame-time percentiles.

| VRS window | UTC collection start | Samples | Median ms | p95 ms | p99 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| On A | 08:15:50 | 318 | 8.6919675 | 9.5077858 | 9.9628484 |
| Off B | 08:17:20 | 317 | 8.8722572 | 9.9845633 | 10.3681487 |
| On C | 08:18:33 | 316 | 8.9146557 | 9.7787678 | 10.0713051 |

All three controls passed unchanged game identity, unchanged final INI hash and
collector exit 0. Every focus record matched the game: 302 observations per
window, strictly increasing monotonic spans 30.1571/30.1735/30.1734 seconds and
maximum gaps 0.103313/0.101267/0.101209 seconds. This covers the monitored interval
at approximately 100 ms cadence; transitions between observations remain unseen.
All selected SDK HMD poses were valid with the same neutral matrix across runs.
That does not freeze the game's scene animation or resource state.

Actual native VRS reports, before and throughout each interval, show configured
1/0/1. Bracketing log rows give the following cumulative counters; their wider
boundaries are not the exact collector boundaries:

| Window | Valid pairs before → after | Accepted world draws before → after |
| --- | ---: | ---: |
| On A | 116 → 1230 | 144710 → 1573622 |
| Off B | 1855 → 1855 | 2385012 → 2385012 |
| On C | 1934 → 3041 | 2483260 → 3910600 |

Every intermediate On report increases both counters; Off reports remain
constant. Stale-refusal counters do not increase within any bracket. This
supports functioning gaze/map admission in both On windows and actual Off in
the control, rather than relying on INI bytes alone.

Return-On's median is 0.2226882 ms (2.5620%) above initial On, exceeding the
initial Off-minus-On difference of 0.1802897 ms. Return-On is 0.0423985 ms slower
than Off. Thus this sequence establishes **no repeatable whole-frame timing
benefit**, while retaining the earlier positive sampled-draw PS evidence.
Outside-window GPU snapshots also vary: core clocks 1890→1890, 1890→1860 and
1875→1875 MHz; temperatures 76→77, 79→79 and 79→79 C; used memory
10428→10799, 11083→11117 and 11107→11894 MiB, respectively. Thermal, memory,
clock and scene variation remain confounds; these endpoints do not prove a
specific cause or a VRS resource leak.

Separate complete native windows near these measurements report about
31.55–31.77 caller cycles/s, not 90. Their boundaries differ from the SDK
collections. Native application GPU samples were missing by design with its
optional timing disabled, so these SDK results cannot substitute for them.
No generated cadence, image-quality acceptance or new rendering change follows.
The owner restored the exact saved pre-probe INI/mirror, stopped the simulator
and completed normal stopped_clean cleanup with no active receipt or remaining
game/runtime processes.

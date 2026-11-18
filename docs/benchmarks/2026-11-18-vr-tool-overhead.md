# VR tool overhead in Pilot Assessment

## Status

The exact Windows build `2bbac50c` retained the independent FPS layer and
verified generated-frame counts in 3× and Auto. In the mirrored tools-off
comparison, Compact with GPU timing still cost 1.25–1.69% in application
cadence. Detailed with GPU timing cost 1.88–1.94%. Both increased the p99 frame
cycle. The investigation continues with GPU timing and Compact tested separately.

The first performance candidate `3f0a7c2e` delivered more application frame
cycles per second and lower frame-cycle p95 in each corresponding control
block than `40eb7bcc`. Its combined tools still reduced cadence within both
candidate pairs. The four-treatment diagnostic found a consistent cadence
reduction when the Detailed FPS overlay was enabled.

## Workload

Pilot Assessment remained docked at Mawson Dock on an RTX 5090. SteamVR/OpenXR
used a simulated 90 Hz display at 3892 × 3892 per eye with DLSS Balanced.
VRS, frame generation, and experimental Reflex were off. Each build used one
game launch and four quiet observation blocks in order Off, On, On, Off.
On enabled both Detailed FPS overlay and application GPU timing. The harness
verified settings and ownership at the boundaries and analyzed existing
OpenXR frame-cycle reports after the blocks.

Cadence counts application wait returns per second. The reported p95 is
the median of complete windows' wait-return-to-wait-return p95s. Confidence
about physical presentation and motion-to-photon latency requires real headset
measurements.

| Block | Tools | Baseline cycles/s | Candidate cycles/s | Baseline p95 | Candidate p95 |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | Off | 56.37 | 58.54 | 24.254 ms | 22.638 ms |
| 2 | On | 54.35 | 55.30 | 24.943 ms | 23.344 ms |
| 3 | On | 54.41 | 55.47 | 24.645 ms | 23.166 ms |
| 4 | Off | 52.82 | 57.33 | 25.640 ms | 22.686 ms |

All 9,756 baseline and 6,800 candidate application cycle observations
qualified. The baseline tools-on cadence effects were −3.58% and +3.03%
across opposite orders. Candidate effects were −5.53% and −3.26%, with p95
increases of 0.706 ms and 0.480 ms. Separate game launches can vary. The
corresponding-block difference describes this comparison, with no
independent estimate of session-to-session noise.

## Separate tool costs

One `3f0a7c2e` diagnostic session used the same display profile and visibly
docked cockpit. It reached the matching view with no navigation input. The
journal scan found no normal-game load inside the owned startup interval.
The eight blocks establish within-session tool costs. The game did not record
the explicit Pilot Assessment menu route for this launch.

| Block | Enabled tools | Cycles/s | Cycle p95 | Cycle p99 |
| --- | --- | ---: | ---: | ---: |
| 1 | None | 57.67 | 22.847 ms | 24.393 ms |
| 2 | GPU timing | 56.77 | 22.768 ms | 24.454 ms |
| 3 | Detailed overlay | 55.60 | 22.851 ms | 24.888 ms |
| 4 | Both | 56.47 | 22.936 ms | 24.643 ms |
| 5 | Both | 56.50 | 22.811 ms | 24.676 ms |
| 6 | Detailed overlay | 56.60 | 22.958 ms | 24.760 ms |
| 7 | GPU timing | 57.30 | 22.810 ms | 24.254 ms |
| 8 | None | 57.13 | 22.817 ms | 24.373 ms |

The 13,622 qualified cycles showed an overlay effect of −3.58% and −0.93%
with GPU timing off, and −0.53% and −1.40% with GPU timing on. Effects of
GPU timing changed sign across the mirrored blocks. Enabling both tools
reduced cadence by 2.08% and 1.11%. The corresponding cycle p95 changes were
+0.089 ms and −0.006 ms. Exact application GPU completion counters recorded
10,195 valid samples and no invalid counter increase between contained markers.

The harness scheduled 398.2 seconds of quiet waits against 512 seconds for
eight fixed waits. It inspected the complete timing windows after the run.
The virtual display clock ranged from 89.982 to 90.009 Hz at the boundaries.

## Startup and scene entry

Six alternating idle status checks per method measured a 2.599-second median
and 328,134 response bytes for full status, compared with 1.008 seconds and
170 bytes for the launch status hint. The hint observes an acknowledged
launch. The harness still reads full status to verify the fresh game owner
before sending input.

The 17-capture scene-recognition replay produced identical decisions and
reduced median corpus time from 15.514 to 3.764 seconds. Three subsequent
production sessions reached the verified docked Pilot Assessment view
157.6–161.4 seconds after the harness began, including preflight. Each
session recorded all four menu inputs and confirmed their destination. The
earlier report of a ten-minute load has no equivalent instrumented baseline.

A fourth launch reached the same view in 184.7 seconds. The image following
the final menu input stayed byte-identical for about 37 seconds, compared
with about 16 seconds in the previous launch of the same build. Once that
image changed, dock confirmation took about 22–23 seconds in both launches.
The verifier waited for the cockpit labels and the docked Auto Launch control
before admitting timing. Both scene entry and timing preserved the docked ship.

An offline prototype cached negative classifications of identical screenshots
and reduced median classifier CPU time from 10.125 to 6.908 seconds over the
recorded 55-check sequence. The replay preserved the input sequence and
decisions. It used retained checkpoint images and synthetic stand-ins for the
unretained negative images, preserving the observed duplicate pattern.
The live runs used the uncached classifier.

## Two updates per second

`bae4fd1a8f4d25cc8ea464bc89c46a4e8070d7e6` used the same independent
OpenXR quad and changed native Detailed raster refresh from 250 ms to 500 ms.
The package passed 211 native Windows jobs and 79 Python gates. This run
verified all four Pilot Assessment menu inputs and the docked cockpit before
timing any treatment.

| Block | Enabled tools | Cycles/s | Cycle p95 | Cycle p99 |
| --- | --- | ---: | ---: | ---: |
| 1 | None | 57.17 | 22.779 ms | 24.723 ms |
| 2 | GPU timing | 56.57 | 22.905 ms | 25.541 ms |
| 3 | Detailed overlay | 55.44 | 23.377 ms | 25.362 ms |
| 4 | Both | 55.27 | 23.524 ms | 25.795 ms |
| 5 | Both | 56.60 | 23.114 ms | 24.556 ms |
| 6 | Detailed overlay | 55.20 | 23.783 ms | 25.755 ms |
| 7 | GPU timing | 57.23 | 22.768 ms | 24.374 ms |
| 8 | None | 57.03 | 22.723 ms | 24.496 ms |

Overlay-only cost remained 3.03% and 3.21% in opposite orders. Cycle p95
rose by 0.598 ms and 1.060 ms. With GPU timing on, the overlay cost was 2.30%
and 1.11%. Both tools together still reduced cadence by 3.32% and 0.76%.
The two-update experiment did not resolve the performance regression.

Separate checks in the same docked training session recorded 918 generated
submissions in fixed 3× and 821 in Auto at its selected 2×. Left-eye
compositor captures showed generated FPS and cumulative counts on the
independent quad in both modes. Each check used bounded head-only motion.
The headset captured the cumulative session total while the measured
submission count covers only the qualified interval.

## Overlay submission and raster updates

Temporary diagnostic `df88866f53a1188cb65e458bccc52a1a64cecf5d` measured
13,588 valid app cycles in eight docked Pilot Assessment blocks. It held
application GPU timing off. The order was Off, Detailed Live, Detailed Frozen,
Compact, Compact, Detailed Frozen, Detailed Live, Off.

| Overlay | Forward cycles/s | Reverse cycles/s | Forward p95 | Reverse p95 |
| --- | ---: | ---: | ---: | ---: |
| Off | 57.17 | 57.23 | 22.829 ms | 22.860 ms |
| Detailed Live | 55.90 | 56.47 | 23.471 ms | 22.951 ms |
| Detailed Frozen | 56.57 | 56.23 | 23.176 ms | 22.952 ms |
| Compact | 56.63 | 56.73 | 22.961 ms | 23.016 ms |

Both Detailed modes used the same 1280 × 480 image crop, quad extent, and
pose. They shared the 2600 × 1024 swapchain allocation with Compact. The
measured live modes uploaded about two images per second. Frozen kept
submitting the same released image with zero uploads during its timing blocks.
It still reduced cadence by 1.05% and 1.75% versus Off. The cadence effect of
live updates changed direction between the two halves.

The sampled Compact crop was 1718 × 158. Compact improved cadence by 1.31%
and 0.47% versus Detailed Live and reduced cycle p99 by 0.905 ms and 0.449 ms.
The remaining cadence differences versus Off were −0.93% and −0.87%, with
p95 changes of +0.132 ms and +0.156 ms. The diagnostic establishes a smaller
cost for Compact in this setup.

## Production Compact comparison

The original `bae4fd1a` package was restored and independently verified
before this docked Pilot Assessment run. It used the same four mirrored
treatments as the Detailed comparison, with only the overlay style changing.
All eight timing blocks qualified. The four visible blocks reported a matching
OpenXR attachment of the Compact raster with rates, mode, and cumulative count.

| Block | Enabled tools | Cycles/s | Cycle p95 | Cycle p99 |
| --- | --- | ---: | ---: | ---: |
| 1 | None | 57.70 | 22.703 ms | 24.390 ms |
| 2 | GPU timing | 56.13 | 22.883 ms | 25.249 ms |
| 3 | Compact overlay | 55.30 | 24.003 ms | 25.472 ms |
| 4 | Both | 55.43 | 23.451 ms | 25.508 ms |
| 5 | Both | 56.43 | 23.183 ms | 25.153 ms |
| 6 | Compact overlay | 57.13 | 22.717 ms | 24.497 ms |
| 7 | GPU timing | 57.57 | 22.719 ms | 24.166 ms |
| 8 | None | 57.20 | 22.844 ms | 24.580 ms |

Compact reduced cadence by 1.25% and 1.97% with GPU timing enabled. Both
tools together reduced it by 3.93% and 1.34% relative to no tools. With GPU
timing off, the Compact effects were −4.16% and −0.12%. The two Compact-only
blocks differed by 3.32%, making the size of the Compact-only effect uncertain.
This production comparison did not reproduce the diagnostic advantage with
both tools active.

The Compact frame-generation check stopped before timing because its harness
omitted a required output directory. The verified 3× and Auto results above
use Detailed.

## Local cost checks

An extracted Linux C++ replay of the hidden FPS overlay path removed three
uncontended locks and reduced median execution time from 37.60 ns to
9.42 ns per read and pose-update pair. The benchmark collector replay
produced identical reports and reduced CPU time per admitted frame by
81–85% across ordered, orphaned, and out-of-order inputs. These isolate
specific CPU paths. The live test measures their combined workload separately.

## Same-session feature checks

With Reflex untouched, fixed 3× submitted 902 generated frames and Auto
submitted 761 at its selected 2× multiplier. The Detailed FPS quad was
included on generated submissions after the overlay had been re-enabled.
Both samples used bounded simulated head yaw while docked.

Reflex On and Boost requests produced enabled readbacks after 4, 16, and
64 successful game Presents. Off returned success immediately but still
reported enabled at all three delayed observations. Keep Reflex experimental
and disabled by default while its live reversal remains unverified.

## Timing scope observation

The virtual display stayed at approximately 90 Hz. During application
pacing changes, OpenXR also returned predicted display periods of about
11.111, 22.222, and 33.333 ms. The CPU/GPU benchmark treated the reciprocal
of each prediction as the display mode. All observed application CPU/GPU
windows aborted with a scope-change record. Their printed workload
dimensions, FOV, and treatment stayed the same. The independent frame-cycle
windows above remained valid.

Using the reported 90 Hz display rate, `bae4fd1a` completed a contained
30-second CPU/GPU benchmark with both tools enabled. All 1,695 samples were
valid in each series, with zero invalid or missing entries. The CPU median,
p95, and p99 were 6.478, 8.422, and 9.564 ms. The GPU values were 12.814,
14.154, and 14.687 ms. The frame used 2257 × 2257 per eye as DLSS input and
3892 × 3892 per eye as OpenXR output.

## Smaller swapchains and manual timing readback

The Windows build of `2bbac50c6977671928bc09ede5547be1b1dcf2ca` passed 211
native jobs and 79 Python gates. The native build took 313.2 seconds. The
harness verified the two installed VR binaries against that
build before starting Pilot Assessment.

The separate OpenXR FPS quad allocates 2048 × 256 for Compact and 1536 × 512
for Detailed when the runtime can supply the smaller swapchains. The original
2600 × 1024 swapchain remains the fallback. All three allocations succeeded
in this session. The reported crop and pose stayed the same. Manual app GPU
timing reads pending queries at the producer's first admitted command and at
Present, with a bounded 50 ms fallback for long frames. A scripted delayed
query replay reduced stalled `GetData` calls from 336 to 56 and produced the
same 56 timing markers. The timing policy for Auto frame generation was
preserved.

An exploratory six-treatment session completed nine 30-second windows. A
foreground change at the start of block ten caused the harness to stop and
restore settings. The completed windows stayed docked and have verified
settings, application cycles, and compositor traces. Their first half shares
one Off control. The session did not complete its mirrored control.

| Enabled tools | Cycles/s | Cycle p95 | Cycle p99 | Cadence change vs Off |
| --- | ---: | ---: | ---: | ---: |
| None | 53.67 | 26.134 ms | 27.856 ms | |
| GPU timing | 52.87 | 26.312 ms | 28.346 ms | −1.49% |
| Compact overlay | 52.83 | 26.614 ms | 28.632 ms | −1.55% |
| Detailed overlay | 52.50 | 26.363 ms | 28.417 ms | −2.17% |
| Compact and GPU timing | 53.30 | 26.554 ms | 28.437 ms | −0.68% |
| Detailed and GPU timing | 52.90 | 26.448 ms | 28.617 ms | −1.43% |

The three repeated treatments varied 0.13–1.00% in cadence. With GPU timing
enabled, adding the Compact overlay changed cadence by +0.82% and adding
Detailed changed it by +0.06% in the first half. Their cycle p95 increases
were 0.242 ms and 0.136 ms. Timing-enabled windows reported 52.76–53.24
valid GPU completions per second and zero invalid completions. These results
guide the follow-up comparison with Off at both ends of the session.

The exploratory Off control was slower than the six previous Off windows
at 57.03–57.70 cycles/s and 22.703–22.847 ms p95. Against the first
`bae4fd1a` Compact-session control, mean cycle time rose by 1.298 ms and
1.037 ms of that increase appeared in the next-wait queue. The scene and
settings matched in the receipts, and the overlay and manual GPU queries
were inactive. Cross-launch cadence therefore includes a frame-end and
runtime queue shift whose cause remains unresolved. The tool deltas above
use the Off control from the same game process.

### Mirrored repeat

The exact build then completed a fresh six-window run at the same docked
view. All 9,495 application cycles qualified. Both overlay modes ran with
manual app GPU timing enabled. The simulator used the 90 Hz clock throughout.

| Block | Enabled tools | Cycles/s | Cycle p50 | Cycle p95 | Cycle p99 |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | None | 53.37 | 20.488 ms | 26.211 ms | 27.786 ms |
| 2 | Detailed and GPU timing | 52.33 | 20.507 ms | 26.771 ms | 29.068 ms |
| 3 | Compact and GPU timing | 52.70 | 19.944 ms | 26.838 ms | 28.850 ms |
| 4 | Compact and GPU timing | 52.43 | 20.163 ms | 27.093 ms | 29.226 ms |
| 5 | Detailed and GPU timing | 52.33 | 20.602 ms | 26.873 ms | 28.722 ms |
| 6 | None | 53.33 | 20.943 ms | 25.354 ms | 27.855 ms |

Compact with GPU timing reduced cadence by 1.25% and 1.69%, and Detailed
with GPU timing reduced it by 1.94% and 1.88%. Cycle p95 rose by
0.627/1.739 ms for Compact and 0.560/1.519 ms for Detailed. The Off controls
differed by 0.06% in cadence. The timing tails varied more than cadence even
between those controls, including a 0.857 ms p95 difference. Both tool sets
also increased p99 in both time directions. The smaller Compact allocation
improved cadence by 0.70% and 0.19% relative to Detailed at the same GPU
timing setting, with mixed p99 changes. The remaining tool cost was
repeatable in this workload.

Mean wall time before the first eye submission rose 0.32–0.57 ms with the
two tool sets. Mean next-wait roundtrip time shortened in this repeat.
The exploratory session shifted cost into the next-wait interval instead,
so the two sessions do not isolate one stable source of the overhead.

After the comparison, fixed 3× yielded 1,448 generated XR submissions and
Auto yielded 1,166 with 2× active. Successful OpenXR frame-end and raster
traces confirmed that the Compact quad remained attached to generated
submissions with the cumulative count advancing. The left-eye compositor
captures showed 22 native and 43 generated submissions per second at 3×,
and 31 native, 32 generated, and one repeated per second in Auto. These are
readout snapshots from the sampled intervals. All checks used bounded head
motion and stayed docked. Physical scanout was outside this simulator test.

# VR tool overhead in Pilot Assessment

## Status

The performance candidate `3f0a7c2eb7405352697840de56fc776a22718092`
passed the full Windows build. It delivered more application frame cycles per
second and lower frame-cycle p95 in each corresponding control block than
`40eb7bccb8077569ed2ea5c0f1bc9c0d8a2919ed`. The combined tools still reduced
cadence within both candidate pairs. The four-treatment diagnostic found a
consistent cadence reduction when the Detailed FPS overlay was enabled.

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

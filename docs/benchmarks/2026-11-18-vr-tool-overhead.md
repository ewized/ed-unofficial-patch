# VR tool overhead in Pilot Assessment

## Status

The performance candidate `3f0a7c2eb7405352697840de56fc776a22718092`
passed the full Windows build. It delivered more application frame cycles per
second and lower frame-cycle p95 in each corresponding control block than
`40eb7bccb8077569ed2ea5c0f1bc9c0d8a2919ed`. The combined tools still reduced
cadence within both candidate pairs. The next comparison separates Detailed
FPS overlay from application GPU timing.

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

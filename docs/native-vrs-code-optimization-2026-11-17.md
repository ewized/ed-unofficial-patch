# Native VRS code optimization — 2026-11-17

## Status

- Native VRS full-rate fast path skips three redundant atomic status stores
  when the owner already has acknowledged full-rate state. Real transitions,
  invalidation and driver-error recovery retain their publication paths.
- A separate sampler fix prevents diagnostic ordinal starvation and pauses
  metadata reads while query slots are pending. It improves measurement
  reliability, rather than normal world shading or HUD composition.
- Full native MSVC attempt 03 **PASSED**, exit 0 in 654.257 seconds, with a
  full-pass receipt: 46 initial and 34 inner Python gates passed, plus all
  206 pooled and 8 quiet jobs (214 total). Native VRS passed **115 checks/0
  failures**, and the focus guard passed. Core/installer checks completed.
- The validated input tree contained the uncommitted rendering changes and
  narrow edvr_log literal-build fix. Attempt 02's failure and the corrected before-version's
  **115/19** control remain recorded below; the successful build supersedes
  their incomplete-validation status.
- The native MSVC real-owner GPU probe separately passed six offscreen cases
  with simulated gaze. This is component correctness evidence, not game FPS.
- Earlier development artifacts identify themselves as **64d04505-dirty** and
  remain unsuitable for installation. A fresh destination build of clean
  **52ca7b0f** subsequently passed all 214 native jobs and the separate NVIDIA
  GPU probe, then passed installed-build verification and a docked game test.
- The new sampler produced **5 left / 10 right matched triples** with no pending
  queries. All 132 sample queries were valid; independent joins reproduced
  every acceptance decision. See [destination validation](#clean-destination-validation).
  This qualifies the tested diagnostic, not a whole-frame speedup or 90 Hz.
- Scope is native NVIDIA rendering optimization; FG optimization is cancelled.
  No measured FPS or elapsed GPU-time gain is claimed.

## Acknowledged full-rate fast path

In [native_vrs.cpp](../src/d3d11/native_vrs.cpp), fullRate previously called
publish even when VendorStateCache already proved disabled rates and a null
resource binding. Driver setters were already skipped in that branch, but each
call still stored vendorKnown, fullKnown and coarseKnown into shared atomics.
This path is reached repeatedly while recognized HUD and other full-rate work
are protected, including fallback/disabled-VRS processing after initialization.

The acknowledged no-op now returns immediately, removing those three relaxed
atomic stores per call. This is a source-level operation count, not a measured
CPU-time improvement. The cache belongs to the rendering owner. The production
snapshot atomics have one writer, publish, and cache-changing paths continue to
publish. Full/coarse/full transitions still publish acknowledgement; invalidation
still forces the reset; failed resets still publish unknown state and retry on
the existing next-frame cadence. No driver command, drawing decision or ownership
check was removed.

The [driver fixture](../tools/native_vrs_test/native_vrs_test.cpp) now captures
publications under EDVR_NATIVE_VRS_TEST only, through test structures in
[native_vrs.h](../src/d3d11/native_vrs.h). It checks 256 acknowledged full calls
publish once, full/coarse/full publishes all three states, invalidation republishes
a successful reset and failure/retry remains unknown until acknowledgement.

Limited CPU validation extracted the exact publish, fullRate, syncInvalidation
and activateMap bodies, using the production cache policy with CPU driver
callbacks and small State/Shared test substitutes. It passed; restoring the old
full-rate publication made the regression fail. This exercises neither the
complete Windows adapter nor actual NVAPI/D3D behavior. The later complete
native fixture result below supersedes this limited CPU check for its tested paths.

## Diagnostic sampler problem and bounded fix

The [source audit](performance-audit-beyond-2026-11-16.md) found that discovery
advanced its starting draw ordinal by 16 each frame. With 40 stable eligible
identities and a query issued at the first metadata admission, the schedule
revisited only {0,8,16,24,32}. Useful ordinal 25 remained unseen over 600 frames.
The prior WARP fixture happened to place useful work at aligned ordinal 24.

Discovery now starts after the last ordinal whose metadata was actually
examined. If a first tiny query consumes the frame's query allowance, the next
frame starts at its adjacent ordinal. If metadata checks fail to produce a
query, discovery can still advance through its 16-entry window. Per-eye cursors
remain independent, and useful-candidate priority can bypass the ordinal window
while retaining the metadata budget.

The recorder also checks query-slot availability before admitting metadata.
When all six slots are pending, discovery keeps counting world calls but spends
no metadata reads and preserves its cursor. This prevents query completion
cadence from introducing another fixed traversal pattern. Slots remain owned
until normal collection resolves them; this adds no query reuse or forced flush.

The existing 16-metadata/one-query-per-eye/frame budgets, 600-frame stop,
full/native-map/full matching, useful-work floor, HUD exclusion and GPU query
ownership rules remain. Complete traversal assumes stable eligible draw order,
available query capacity and no active useful-candidate priority track; it does
not promise all possible draws can be sampled within a finite 600-frame run.

## Sampler regressions and validation limits

The CPU fixture directly exercises the production Discovery class with draw
counts 1, 15, 16, 17, 32, 40, 48, 64, 96 and 256. It checks every ordinal is
visited under one-query-per-frame scheduling, explicitly including ordinal 25;
paused pending frames preserve the next ordinal; metadata-invalid prefixes
continue rotating; and priority, per-eye independence and the 16-entry cap hold.
The before/after translation units contain verbatim production class text and
the same discoveryCases function. There are no D3D stubs in this CPU proof.

The existing real WARP fixture now places useful work at ordinal 25 behind
stable tiny identities. Its changing startInstance epochs allow expiry plus a
complete discovery traversal, and it retains checks against stale partial
triples. A new case fills all six real query slots, checks that pending ownership
pauses metadata discovery, then drains and resumes. Fixture-only event queries
wait for GPU completion on a two-second wall-clock deadline without advancing
simulated frames on each CPU poll, then collect once on the next frame. A failed
wait stops further issues; unresolved events and statistics slots are not reused.
This keeps query latency from consuming the 600-frame/12-frame budgets. These
WARP cases passed in the final native development run below.

## Complete native development results

Clang 14 targeting Windows-GNU compiled the exact production native_vrs.cpp and
complete native_vrs_test.cpp; MinGW-posix linked static executables. An earlier
plain-GCC attempt stopped at [guard.h](../src/common/guard.h)'s Windows SEH syntax.
The initial Clang executable crashed with 0xC0000005 at its first thread_local
capture assignment. External -femulated-tls resolved that toolchain TLS path,
without modifying production guards or inserting a production workaround.

The final binaries contain no temporary progress diagnostics. Both retain the
same current tests and ordinary test capture instrumentation; the corrected
before-version restores the old publication/discovery paths. Saved native run
proofs and stdout independently agree:

| Complete fixture | Checks | Failures | Exit | Elapsed seconds |
| --- | ---: | ---: | ---: | ---: |
| Fixed | 115 | 0 | 0 | 0.571946 |
| Before-version control | 115 | 19 | 1 | 0.574270 |

The 19 expected control failures are four publication checks, nine pure
Discovery checks and six WARP discovery/rollover/pending/resume checks. Thus the
new tests fail with the old code and pass with the fixes. Stdout explicitly
records PS_query=WARP, NVAPI=mock and OpenVR=0. This runs real D3D11 statistics
queries and the production recorder, rather than only the selector transcription.

Artifact SHA-256 values, recorded in the private `vrs-development-clean-results`
proof files:

- Fixed fixture: `24b5310c7460c0783280234a111627ec5e0ea408c789a000768dea5f1db493d5`.
- Before control: `60451cf83128acaa6906c3e6460234ad337b1c02a435b6d4353ba32ec50208b1`.
- GPU fixture: `9749d463fb2be133c848a72c0ea6970c71d4790b57ac4ce4fada4d8192598e9d`.

The unchanged [hardware fixture](../tools/native_vrs_test/native_vrs_gpu_test.cpp)
separately ran the production owner path on the selected GPU. Dry-run exited 0
with zero GPU calls/dynamic DLL loads; actual execution exited 0 in 0.218366
seconds. All six offscreen 1024x1024 R32_FLOAT cases retained matching topology;
coarse shading reduced PSInvocations, the per-eye and moved-eye probes differed
as expected, and full-after/Off restored full-rate results. Final proof fields
are topology_valid=true, coarse_replication_verified=true,
PSInvocations_reduced=true and full_after_and_off_restored=true.

That GPU fixture uses a simulated gaze provider, reports game_world_claim=false,
physical_gaze_claim=false and performance_claim=false, and does not benchmark
4000-square game rendering. Those Clang/MinGW fixtures are separate from the
full repository MSVC result below. Installation and flight qualification remain
separate from both development-test routes.

## Native MSVC full-build attempts

Native Windows development attempts use the installed MSVC 19.44 and Windows
SDK 26100; no additional host tools were installed for them. Attempt 02 passed
all 46 pre-gates and compiled the production DLLs. Its native VRS fixture
reported checks=115, failures=0, PS_query=WARP, NVAPI=mock and OpenVR=0.
The focus guard passed as well. These are completed component checks, not a
successful full repository build.

Attempt 02 ended with exit 1 after 200 of 214 jobs. The failing existing
edvr_log.py self-test supplied a complete describe label, which Git resolved
and re-described as a differently abbreviated bare hash. The same fixture
failure reproduced independently. No full-build receipt was produced; the
remaining jobs and aggregate full-build result were not qualified.

The narrow [edvr_log.py](../tools/edvr_log.py) change preserves complete literal
N-gHASH describe labels before invoking Git. HEAD, branches, bare hashes and
bare tags retain Git resolution; version_matches is unchanged. New regressions,
the Linux full self-test and independent literal/ref compatibility review passed,
including rejection of a different commit. This tooling gate fix is separate
from the rendering changes and does not weaken version matching.

Full native attempt 03 passed with that fix: exit 0, elapsed 654.257 seconds
(the build's internal phase total is 652.3 seconds). The saved terminal state is
passed with full_build_receipt_exists=true. Main-log evidence confirms:

- 46 initial gates and 34 inner Python gates passed.
- The pool of 206 jobs and 8 quiet jobs completed: 214 total, with no aggregate
  failure. The native VRS fixture again reported 115 checks and zero failures.
- The focus watcher reported no shown window, opened console or foreground move
  to the build tree throughout its guarded run.
- Core package/PE and installer checks completed; actual installer resources
  matched the release files. The log ended “Native OpenXR build and all gates
  passed.” These are validated development artifacts, not a release promotion.

The saved full_build_receipt.json has status=full-pass, compiler
19.44.35230, commit=64d045050c7b7bdccc8a9f9deee35fe739400e73 and
describe=64d04505-dirty. Its validated input fingerprint is
`6e0126980a31b7ae76db963e781eeb9e0b183aa5d4d705ab1953df7d98bfde6e`.
The isolated build covered the current five changed C++ source/test headers and
translation units plus the narrow literal-build tooling fix. It did not install
those artifacts into the game. Independently hashed copied development outputs:

| Artifact | SHA-256 |
| --- | --- |
| d3d11.dll | ef33da4b87bf163b7ec7046e3435b61fc0e0156dcf3683eaa477b53743c5fd44 |
| openvr_api.dll | a07cfa5891d90edea9a510d6ed57c81233edcc9945c5cf6087e254ec4f84a5c0 |
| edvr-installer.exe | 5b58b83fa84d179aa63343b0869985045426f39e081897e9793a27f8e5310a18 |

The dirty build required receipt-based clean promotion or destination validation
before use. At that point the handoff supplied no destination test, installation,
flight or new FPS evidence. The subsequent clean destination qualification below
supersedes that installation/test gap; the 4000-square/90-Hz performance goal
remains unachieved.

A separate native MSVC build of the unchanged GPU fixture also passed actual
hardware execution. Its private proof `msvc-native-vrs-gpu-proof.txt` reports
success=true, topology_valid=true, coarse_replication_verified=true,
PSInvocations_reduced=true and full_after_and_off_restored=true across the same
six offscreen cases. Executable SHA-256:
`bb0c24a48a6b4c3f8fd53b66ba214f9a673260c5f61507bdcae5826daebe1403`.
Its provider remains simulated, with game_world_claim=false,
physical_gaze_claim=false and performance_claim=false. This is component GPU
correctness evidence, not a game benchmark or full-build receipt.

Earlier [sampled PS evidence](headless-vrs-shader-work-2026-11-17.md) remains
scoped to the sealed build that produced it: selected-draw reductions were real,
while the separate VRS On/Off/On timing comparison established no repeatable
whole-frame benefit. That runtime evidence neither validates this new sampler
nor supplies a measured performance gain for either code change.

## Clean destination validation

A fresh native Windows build of clean rendering commit **52ca7b0f** passed in
459.361 seconds using MSVC 19.44.35228: 46 initial and 34 inner Python gates,
206 pooled plus 8 quiet native jobs, the focus guard and installer/PE checks.
The new full-pass receipt retains the same validated input fingerprint above.
Its SHA-256 is `9458b6849793f0a23afaa32e0654a9a7dcaf45401e33e4ee2aed86a69b84023c`.
Installed graphics/runtime hashes matched those fresh outputs. The separate
RTX 5090 offscreen probe passed coarse replication, reduced PSInvocations and
full-rate restoration; its provider/readiness remain simulated.

The installed game logs matched **52ca7b0f**. At docked Mawson Dock in Pilot
Assessment, the test used SteamVR/OpenXR, a neutral virtual HMD, synthetic
binocular gaze, FG Off, very-narrow native foveation and configured DLSS K.
Sampled targets were 2320x2320; native benchmark rows recorded 4000x4000 output
per eye. During the valid-gaze window, accepted world work increased by
2,258,220 and map updates increased by 1,800 for each eye, with no new invalid
pairs or driver errors. Stopping gaze restored acknowledged full rate.

The bounded diagnostic armed and finished/drained in **3.704 seconds**, with
pending=0. All **132 queries** returned valid samples. Independent joins of all
29 completed triples reproduced **15 accepted** (left 5, right 10) and **14
rejected** solely for full-control drift beyond 1%. Accepted triples preserved
their identities, dimensions, draw arguments, non-PS statistics, useful floor,
phase order and frame-span limits. Median native-map PSInvocations divided by
mean full controls were **0.4092805 left / 0.2842389 right**: 59.07% and 71.58%
fewer invocations on those selected draws. This is sampled shading reduction,
not an FPS gain attributable to the new code or whole-frame GPU savings.

Private filtered artifact `vrs-ps-complete.txt` contains 333 lines / 144336
bytes, SHA-256 `00b54676562d6b5837ed3cf08c905c6f23a9012e36ea441cd2814e3eb97f9843`.
The progress collector falsely reported an observation timeout after its short
tail dropped the earlier armed line. Complete-log analysis proves the single
generation finished and drained; it was not rerun. Future collectors must
retain phase observations across polls or read only the bounded phase summary.
Original INI/mirror and controls were restored exactly. Normal Stop left no
held input, simulator lease or game/runtime processes. Physical calibration,
whole-frame improvement and the 4000-square/90-Hz goal remain unproved.

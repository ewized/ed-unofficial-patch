# Benchmark capture qualification

## Status

- **Purpose:** qualify captured EDVR benchmark rows without confusing collection, feature activity and physical display delivery.
- **Inputs:** normalized records from the acquisition parser, its qualification, the exact supplied graphics/OpenXR parser-stream bytes, and a live capture receipt.
- **Scope:** read-only validation and optional creation of a fresh result file. This does not launch the game or change its settings.
- **Limit:** submission counts describe application `xrEndFrame` delivery; they do not establish physical scanout or successful hardware frame generation.

Run `python tools/edvr_benchmark.py --self-test` for the regression fixtures. The same gate runs in `build.bat`.

The controller calls `qualify_capture` with `run`, `row`, `records`, `qualification`, `receipt`, `sources` and `collection_status`. The tool keeps wire parsing in the existing acquisition parser. A receipt binds before/after PID, process creation ticks, image and Windows session to an owned capture session, full source commit, configuration SHA-256 and exact raw source hashes/lengths. Acquire those live identities around the capture; copying an old receipt is not a fresh observation. Hashes bind data, not trust in an arbitrary producer. When the sanctioned reader stitches rotated logs, retain its logical UTF-8 byte streams as parser sources and export the original closed on-disk segments separately with their own hashes. These are distinct source populations and their hashes can differ.

For file-based use, put those arguments except `sources` in a capture JSON object and pass `--input capture.json --gfx graphics.log --openxr openxr.log`. Add `--output result.json` to create a new file; an existing output is refused. `--dry-run` prints the qualified result and creates nothing, including when `--output` is supplied.

The row declares `expected_output_dimensions` as left width, left height, right width, right height. Native AA-off requires input and output to match that target. A runtime timing record with unavailable 0×0 input metadata cannot substitute for application dimension proof.

Results keep collection outcome, clean timing-window coverage, requested settings and actual feature qualification separate. Native, generated, repeated and total rates each carry their own known-counter coverage; missing counters remain unavailable and observed zero stays zero. Total uses intervals where all component counters are known. An active multiplier alone is not generated-delivery evidence.

Diagnostics include only benchmark windows contained in the settled row through drain. Boundary-crossing windows remain whole and separate. CPU producer wall time, application GPU time, runtime cycle phases and raw DXGI Present remain separate populations; nested phases are not additive and runtime late frames are not physical dropped frames.

For desktop navigation, `python tools/edvr_screen.py --capture capture.png --profile profile.json --context context.json --expected-state state` returns success only when one calibrated state matches and it is the requested state. The tool reads files and prints a decision; it never sends input. The caller owns capture freshness, process identity, focus, and input safety.

The explicit profile (schema 2, `screen_checks` schema 1) declares the actual source client width and height, allowed output edges, expected neutral HMD matrix, and the states and their templates. Each variant applies only at its exact output edge. Context carries the observed source and PNG dimensions, profile ID, and actual observed pose and simulator serial. PNG dimensions must agree with both the context and the scaled client dimensions, including nearest-pixel rounding. ROIs use a normalized 1024-unit grid on both axes. Every ROI must meet at least 0.90 intersection over union; multiple matching states fail. Validate calibration against independent manually labeled captures before using it to guard input. The screen check does not qualify render timing, per-eye output, or physical scanout.

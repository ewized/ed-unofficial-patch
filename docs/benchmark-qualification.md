# Benchmark capture qualification

## Status

- **Purpose:** qualify captured EDVR benchmark rows without confusing collection, feature activity and physical display delivery.
- **Inputs:** normalized records from the acquisition parser, its qualification, exact raw graphics/OpenXR logs, and a live capture receipt.
- **Scope:** read-only validation and optional creation of a fresh result file. This does not launch the game or change its settings.
- **Limit:** submission counts describe application `xrEndFrame` delivery; they do not establish physical scanout or successful hardware frame generation.

Run `python tools/edvr_benchmark.py --self-test` for the regression fixtures. The same gate runs in `build.bat`.

The controller calls `qualify_capture` with `run`, `row`, `records`, `qualification`, `receipt`, `sources` and `collection_status`. The tool keeps wire parsing in the existing acquisition parser. A receipt binds before/after PID, process creation ticks, image and Windows session to an owned capture session, full source commit, configuration SHA-256 and exact raw source hashes/lengths. Acquire those live identities around the capture; copying an old receipt is not a fresh observation. Hashes bind data, not trust in an arbitrary producer.

For file-based use, put those arguments except `sources` in a capture JSON object and pass `--input capture.json --gfx graphics.log --openxr openxr.log`. Add `--output result.json` to create a new file; an existing output is refused. `--dry-run` prints the qualified result and creates nothing, including when `--output` is supplied.

The row declares `expected_output_dimensions` as left width, left height, right width, right height. Native AA-off requires input and output to match that target. A runtime timing record with unavailable 0×0 input metadata cannot substitute for application dimension proof.

Results keep collection outcome, clean timing-window coverage, requested settings and actual feature qualification separate. Native, generated, repeated and total rates each carry their own known-counter coverage; missing counters remain unavailable and observed zero stays zero. Total uses intervals where all component counters are known. An active multiplier alone is not generated-delivery evidence.

Diagnostics include only benchmark windows contained in the settled row through drain. Boundary-crossing windows remain whole and separate. CPU producer wall time, application GPU time, runtime cycle phases and raw DXGI Present remain separate populations; nested phases are not additive and runtime late frames are not physical dropped frames.

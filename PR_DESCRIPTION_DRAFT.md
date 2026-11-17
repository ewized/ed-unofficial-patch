# Draft pull request: Experimental Beyond VR upscaling, VRS, and Auto frame generation

## Summary

This experimental change adds a higher-resolution VR rendering path for Bigscreen Beyond, eye-tracked variable-rate shading for world draws, and native-first automatic frame generation. It also protects recognized HUD/menu draws by rendering them in a separate full-resolution layer and compositing them after world reconstruction and generation. The single-projection HUD path addresses the doubled orange cockpit UI observed during head movement.

The integration uses standard full-frame DLSS Super Resolution. DLSS reconstruction itself is not foveated; eye-tracked VRS changes shading rates on eligible world draws. HUD protection is implemented by ED-UP's draw extraction and compositing, not by assuming DLSS recognizes UI. Unsupported or unrecognized UI/effects can remain in the world image.

The Beyond controls page exposes reconstruction/model hints, input scale, sharpening, texture LOD bias, desktop window, frame generation, and VRS options. Auto frame generation uses the headset's physical refresh budget and can remain at native 1x when the source keeps up. Turbo is left off in the tested user configuration.

The change also includes a 0.26 OpenVR header with its source and BSD notices, and improves the desktop-window treatment. Two Windows native frame-generation test launchers now locate the checkout relative to their own files instead of using a personal absolute path.

## Implementation details

- Adds native gaze snapshots and readiness checks, VRS rate-map publication/binding, world/UI draw guards, and a bounded pipeline-statistics diagnostic for matched full-rate/native-map/full-rate world draws.
- Adds NGX frame-generation scheduling, interop, output and lifecycle code, including Auto admission/recovery and native fallbacks.
- Adds input-scale negotiation between the graphics proxy and OpenXR runtime. The independent input-scale override has a 3200-square output qualification guard; at larger output sizes the tested setup keeps that override at 1.0 and uses Elite's HMD render-target scale.
- Preserves recognized HUD/menu content at output resolution and composites it into one XR projection after generated world frames. No UI depth or positional reprojection is inferred.
- Adds focused CPU/GPU fixtures and records investigation results and limitations in `docs/beyond-vrs-verification.md`.

## Internal validation already recorded

No tests were run while preparing this draft. The following results were recorded for tested source commit `8fe229513ac23ab6964f6cbda1fbac700e539299` and the two path-only helper updates on the publication candidate:

- Full native Windows build and all configured gates: passed in 176.2 seconds.
- Clean DLL promotion after validation: passed in 45.9 seconds.
- Stereo WARP HUD/composition fixture: 33,039 checks, 0 failures.
- Native frame-generation CPU fixture: 143 checks, 0 failures under `/WX`.
- Omitting inline HUD coverage as a negative control: 8 expected image-check failures.
- Corrected native FG test launchers were invoked from a different working directory. The host compile passed; native FG, OpenXR frame, and OpenXR owner CPU rigs passed 143, 322, and 64 checks respectively, with 0 failures.
- The publication diff was checked against the tested tree: all 2,236 tracked paths and modes match except these intended files: this report, `tools/native_fg_test/compile-host.cmd`, and `tools/native_fg_test/targeted-cpu.cmd`.

## In-game observations and limitations

- The user reported the doubled orange HUD was gone during a short headset check with 2x generation active and later described gameplay as very playable. This does not establish long-session comfort or correct positional parallax.
- On the later Beyond configuration, runtime logs confirmed 4000x4000 XR output per eye and 2320x2320 DLSS input per eye at 90 Hz, with DLSS Balanced-sized input, very-narrow eye-tracked VRS, Auto frame generation, and Turbo off.
- A historical passive 3200-output sample measured about 89.98 native fps over three 30-second game windows; application GPU window medians were 7.740–8.605 ms. Auto remained native 1x during that sample. It is not a 4000-output benchmark or an all-features-off comparison.
- Earlier matched VRS on/off samples were about 90 fps average and 85 fps true 1% low, with one qualified 30-second application-GPU median of 8.48 ms on and 8.39 ms off. This does not establish a repeatable VRS benefit.
- The latest bounded 4000 cockpit shader-work diagnostic selected draws with negligible pixel work and produced no matched full/map/full triple in either eye. It cannot establish visible-world shading reduction. Driver/map binding activity confirms integration activity, not performance gain.
- The strict 90 fps native 1% low target remains unmet or unverified. Native and generated frame cadence must be reported separately. The internal DLSS model actually evaluated by the driver was not independently verified; a model hint is not proof.

## Review and CI notes

This branch consolidates the tested source commit onto the fork's `main` base and adds this draft. The original experimental commit graph was not imported. Full DLL validation belongs to the recorded `8fe2295` candidate; no fresh full DLL build or headset test was performed on the consolidated branch. The repository's Windows workflow should validate the exact PR head before merge.

The repository uses `AGENTS.md` and `CODING_STANDARDS.md`; no additional nested agent/rule files or PR template were found under `src/`, `tools/`, or `docs/`.

## Suggested reviewer focus

1. Verify the OpenVR header provenance and bundled BSD notices.
2. Review world/UI classification and the single-projection composition under head motion.
3. Review Auto frame-generation cadence and fallback behavior, including native-vs-generated accounting.
4. Treat VRS shader-work and performance gains as open until a useful matched in-world measurement supports them.
5. Validate the exact PR head with the repository's full Windows workflow.

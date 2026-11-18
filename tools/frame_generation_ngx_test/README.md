# Native D3D12 NGX frame-generation backend

Original narrow backend using the official NVIDIA NGX FG helper API. The held
GPL worker was inspected for context; its focus/periphery engine, fallback,
presenter, scheduler and project identifier were not copied. Dependency files
and their licences remain unchanged.

`cpu_test.cpp` links CPU NGX entry-point stubs instead of the NGX library.
Bootstrap stubs abort if reached; allocation/scratch/create are enabled only
inside explicitly scripted feature-preparation fixtures. It drives the production official
evaluation helper and persistent metadata storage, checking count/index keys,
resource bindings, signed motion scales, jitter, failure propagation, matrix
storage lifetime, capability policy and fence/consumer reuse policy. The actual
production preparation method is also exercised with scratch-query failure,
successful zero-sized scratch followed by create failure/success, and nonzero
scratch that cannot be allocated. Stage evidence never turns a query error into
a successful zero scratch result.

`build.cmd` compiles/runs that fixture, compiles `probe.exe` against the SDK,
and runs only dry paths for both creation methods. The probe performs no DXGI/device/NGX work
unless explicitly given `--run`. It has not been run with `--run` by this work.
Real probe arguments require a multiplier of exactly 2, 3 or 4, absolute
`--feature-dir` and `--data-dir`, with optional `--width` and `--height`.
`--guide-width`/`--guide-height` independently select paired depth/MV dimensions;
omitting them uses the color dimensions. The explicit `--official-fg-helper`
diagnostic invokes the official FG Create helper without issuing a generic
scratch query. Without that flag, the original query route remains selected
and every query error remains fatal. There is no automatic transition between
routes and an uncalled query is never reported as successful size zero.
Root review is required before that explicitly requested GPU evaluation.

Default MFG diagnosis leaves the original private per-eye/per-index suppression buffers
and strict zero-first-byte eligibility intact. Every fenced completion logs all
four bytes and resource identity per eye/index. An all-FF sentinel pattern is
reported as a pattern, not reinterpreted as permission or proof of a driver
write. `--native-frames` (1..64) and `--warmup-frames` permit a longer controlled
warmup. `--diagnostic-suppressed-output` additionally reads centre pixels after
the fence for suppressed batches, always labelled diagnostic-only/presentable0;
matching pixels never turn a suppressed batch into a ready result.

The explicit trial `--shared-native-pair-suppression` selects one suppression
resource per eye/native pair. It initializes FF once before index1, binds that
same resource to later indexed calls, transitions COPY_SOURCE back to UAV
without reinitializing, and retains separate fenced readbacks for every index.
Any nonzero/unknown byte still rejects the batch. This is a separate selectable
layout, not a fallback. The same production command-plan helper and official
NGX binder are exercised by the CPU recorder for identity, one initialization,
state transitions and nonzero/untouched rejection. Exact build8 diagnostics are
preserved separately in `restore-build8.zip` with a hash manifest.

Source history reset mask values are 1=first source, 2=sequence gap,
4=history generation changed, 8=caller requested, 16=unknown generation.
They describe the supplied reset policy, not an unobservable internal NGX reset.
The exact pre-diagnostic, hardware-proven 2x build and source are preserved in
`restore-build6.zip` with a per-file hash manifest. No restore was installed.

The real probe uses a separate process and synthetic stationary, full-eye
color/depth/motion with explicit perspective camera metadata. It runs four
native intervals and reports each stereo batch's NGX result and suppression
readback. Each ready eye/index also copies its output's centre pixel to a fenced
readback and checks it against the constant gray input; this is a small output
sanity check, not whole-image validation. GPU timestamps are not measured.
Producer and downstream copy work have a separate bounded fence guard; timeout
retains their allocators, descriptors, input resources and consumer output lease.
It does not attach to Elite, start XR, present frames, alter a driver
setting, or prove VR image quality, latency or pacing. It uses hardware NVIDIA
D3D12 only; no WARP or shader FG fallback.

## Integration

1. The generation owner calls `initialize` with a D3D12 device, exact full-eye
   color and guide dimensions, typed color format, multiplier, existing writable
   data directory and feature directory. The context mode is an explicit
   caller assertion: D3D12CreateDevice alone cannot prove unique ownership.
2. Submit one immutable stereo guide snapshot from a positive native frame
   ordinal, including camera matrices without jitter, separate clip-space
   jitter, motion normalization, reset/history generation, producer fence/value,
   and shared snapshot lease. Depth must be typed R32_FLOAT; motion must be
   R16G16_FLOAT or R32G32_FLOAT. No camera defaults or guide conversion is made.
3. Inputs are transitioned from supplied states to shader-readable states,
   then restored before the private NORMAL queue's completion signal. The
   interop bridge can supply COMMON and attach its retirement to that signal.
4. A nonnull returned ticket lease means GPU ownership was accepted, even on
   evaluation failure. Attach a valid completion fence/value regardless of
   `evaluationSucceeded`. Never cancel a nonnull accepted lease. Signal failure
   quarantines the lease and returns no valid retirement point.
5. Poll the ticket without a backend lock. Only `Ready` is presentable.
   `Suppressed` means at least one eye/index did not explicitly report zero in
   the SDK's disable-interpolation output. API success alone is insufficient.
6. Retain tickets through downstream GPU use. Three slots are bounded by both
   inference completion and external ticket lifetime; busy returns promptly.
   Release stale/consumed tickets promptly. Drain on the generation owner with
   a bounded timeout. A timed-out or untracked submission is never force-freed.

The backend never calls global/device NGX Shutdown. Parameters, matrices,
features, allocators and inputs stay alive until the completion fence; failed
signal/unfinished destructor paths retain the entire implementation for process
lifetime. NGX common path descriptors also remain stable for that context.
No Turbo mode or display pacing policy is implemented here.

Official API references: [FG helper](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_helpers_dlssg_d3d.h),
[count/index/capability and suppression definitions](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs_dlssg.h),
[camera metadata](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_params_dlssg.h).

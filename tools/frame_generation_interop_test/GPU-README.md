# Actual interop adapter correctness rig

New `gpu_test.cpp` links the production `frame_generation_interop.cpp`; the
existing CPU fixture and its runner are unchanged. `build-gpu.cmd` compiles
the isolated binary and executes only its CPU `--self-test`. There are no
D3D imports: the default path asserts D3D11/D3D12/DXGI are not loaded before
or after calling the actual adapter with OFF and poison COM pointers.

Only explicit `--gpu` or `--run` loads verified System32 D3D11/DXGI modules,
selects NVIDIA hardware, and creates a private D3D11 device. Production code
creates/imports D3D12 objects on that same adapter. No NGX, game, VR runtime,
settings or presentation functions are called. This is not a performance test.

The GPU path captures both eyes and all five roles using the actual staged
begin/capture/publish/abort API. It checks RGBA8/sRGB/BGRA8/sRGB color and UI,
F16 UI, R32_FLOAT depth and R16G16_FLOAT motion. Source updates are enqueued
after each immutable copy and before publication. A D3D12 consumer queue waits
on the actual producer fence, copies every pixel with real row footprints,
returns imports to COMMON, and signals a consumer completion fence. Bitwise
readback must match the original image rather than the modified source.

Additional cases cover owner/context/sequence/history/device/duplicate and
incomplete-publication refusals, partial-copy abort, exact required masks,
the three-slot ring under retained external leases, unsignaled consumer
retirement, immutable cancellation/retirement, a deliberately gated private
consumer queue, and a lease surviving adapter destruction. That final case
uses production's deliberate bounded-owner retention policy; it does not
claim all objects are freed after destruction with an outstanding lease.

Every exit after queue use attempts CPU `gate->Signal(1)` before any bounded
drain. If completion cannot be proved, the complete GPU owner is quarantined
until process exit. A root-controlled external process timeout remains useful
against driver calls that themselves stop returning. Source-only review is
not execution evidence; no GPU result is claimed by authoring this fixture.

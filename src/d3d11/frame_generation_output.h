#pragma once
#include "frame_generation_interop.h"
#include "frame_generation_ngx.h"
#include "../common/native_frame_generation.h"
#include <memory>

namespace edvr {
struct FrameGenerationOutputUi {
    bool empty = false, useMult = false;
    // The finalized SMAA result is already in the immutable captured plane.
    float uv[4] = {0, 0, 1, 1};
};
struct FrameGenerationOutputSource {
    EdvrFgSource previous{}, current{};
    uint64_t historyGeneration = 0;
    uint32_t multiplier = 0;
};
enum class FrameGenerationOutputFailure : uint32_t {
    None, Off, NotInitialized, WrongThread, InvalidArgument, InvalidMetadata,
    InvalidResource, WrongDevice, Pending, Busy, Allocation, Shader, Queue,
    DeviceRemoved, Timeout, OutstandingLease, UnsafeBackend, Stale
};
struct FrameGenerationOutputStatus {
    bool initialized = false, failed = false, quarantined = false;
    FrameGenerationOutputFailure failure = FrameGenerationOutputFailure::NotInitialized;
    HRESULT lastHresult = S_OK;
    uint64_t submitted = 0, completed = 0, busy = 0, nativeOnly = 0;
    // Preparation retires after its own fence and all downstream owners.
    // These counters let the worker collect pins even if NGX never submits.
    uint64_t recompositionSubmitted = 0, recompositionRetired = 0;
};
struct FrameGenerationOutputLease;
struct FrameGenerationRecompositionEye {
    ID3D12Resource* color = nullptr;
    ID3D12Resource* hudless = nullptr;
    ID3D12Resource* ui = nullptr;
};
struct FrameGenerationRecompositionTicket {
    // Accepted work may read the original interop lease. Its caller retires
    // that source exactly once behind preparation and every downstream read.
    bool accepted = false, unsafe = false;
    ID3D12Fence* readyFence = nullptr;
    uint64_t readyValue = 0;
    // Borrowed COMMON textures. Any consumer waits on the ready fence and
    // retains this lease until its own completion, including raw HUD-less reads.
    FrameGenerationRecompositionEye eyes[2];
    std::shared_ptr<FrameGenerationOutputLease> lease;
};
struct FrameGenerationOutputTicket {
    // accepted means this publisher retained the inputs for submitted or
    // unprovably completed GPU work. Never cancel the interop lease then.
    bool accepted = false, unsafe = false;
    ID3D12Fence* finalFence = nullptr;
    uint64_t finalValue = 0;
    std::shared_ptr<FrameGenerationOutputLease> lease;
    unsigned generatedCount = 0; // actual submitted phases, including nativeOnly suppression
    // Non-consuming snapshot. S_OK means acquireBatch can export a fresh,
    // fenced batch. S_FALSE means pending or already transferred.
    HRESULT exportStatus() const noexcept;
    // CPU-only/nonblocking. Exactly one successful call transfers one ABI
    // token; S_FALSE means pending/already transferred. Handles and phaseMask
    // are exposed only after the publisher's completion fence actually passes.
    HRESULT acquireBatch(EdvrFgBatch*) const noexcept;
};

class FrameGenerationOutput {
public:
    FrameGenerationOutput() noexcept = default;
    ~FrameGenerationOutput();
    FrameGenerationOutput(const FrameGenerationOutput&) = delete;
    FrameGenerationOutput& operator=(const FrameGenerationOutput&) = delete;
    // NGX-worker ownership. Creates its own NORMAL direct queue. OFF returns
    // before loading graphics modules, querying devices or allocating resources.
    bool initialize(bool enabled, ID3D12Device*, unsigned width, unsigned height,
                    uint32_t timeoutMs = 2000) noexcept;
    // A fenced Ready NGX ticket enables generated phases unless nativeOnly
    // seeds history. Real backend fences/leases remain required and retained.
    // Fenced suppression
    // or evaluation failure yields an explicitly native-only phaseMask=1.
    // Pending is unaccepted; accepted backend work without a completion fence
    // is quarantined. Every published phase uses the exact production HUD
    // shader/parameters over finalized immutable UI planes on this private queue.
    FrameGenerationOutputTicket submit(const std::shared_ptr<FrameGenerationInteropLease>&,
        const FrameGenerationNgxTicket&, const FrameGenerationOutputUi (&)[2],
        const FrameGenerationOutputSource&, bool nativeOnly = false) noexcept;
    // Worker-only preparation for UI recomposition, with no XR publication.
    // A covered UI texel contains the exact native composite with alpha 1.
    // Uncovered pixels are transparent; HUD-less aliases the immutable source.
    FrameGenerationRecompositionTicket prepareRecomposition(const std::shared_ptr<FrameGenerationInteropLease>&,
        const FrameGenerationOutputUi (&)[2], const FrameGenerationOutputSource&) noexcept;
    // Worker loop calls collect even when capture has no free source slot:
    // input/NGX leases release as soon as final GPU work completes, independently
    // of the XR consumer's later output-lease retirement.
    bool collect() noexcept;
    bool drain(uint32_t timeoutMs = 0) noexcept;
    FrameGenerationOutputStatus status() const noexcept;
#ifdef EDVR_FRAME_GENERATION_COORDINATOR_TEST
    // Seed the three pending outputs on the test worker. Fence values 1..3
    // drive the production collector and readiness checks without a GPU.
    bool testSeedRetirement(ID3D12Fence*, const std::shared_ptr<FrameGenerationInteropLease> (&pins)[3],
        const EdvrFgBatch (&batches)[3], FrameGenerationOutputTicket (&tickets)[3]) noexcept;
#endif
private:
    struct Impl;
    Impl* impl_ = nullptr;
    FrameGenerationOutputFailure initial_ = FrameGenerationOutputFailure::NotInitialized;
};

// Exact frozen C ABI callbacks. Retire attaches one consumer completion point
// covering ALL phases; Discard proves no consumer GPU read was queued.
HRESULT WINAPI frameGenerationOutputRetain(void*) noexcept;
void WINAPI frameGenerationOutputRelease(void*) noexcept;
HRESULT WINAPI frameGenerationOutputRetire(void*, HANDLE, uint64_t) noexcept;
HRESULT WINAPI frameGenerationOutputDiscard(void*) noexcept;

#ifdef EDVR_FRAME_GENERATION_OUTPUT_TEST
// Drives the production ABI token callbacks and retirement state, without COM.
unsigned frameGenerationOutputTestProtocol() noexcept;
#endif
} // namespace edvr

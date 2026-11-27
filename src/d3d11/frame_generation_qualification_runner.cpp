#include "frame_generation_qualification_runner.h"
#include "frame_generation_qualification_fixture.h"

#include <windows.h>

#include <memory>
#include <new>

namespace edvr {
namespace {
using Microsoft::WRL::ComPtr;
using Verdict = FrameGenerationQualificationVerdict;
using Reason = FrameGenerationQualificationReason;

constexpr unsigned kEyes = 2;
constexpr unsigned kPlanes = 5;
constexpr unsigned kRows = 2;
constexpr unsigned kSourceFrames = 12;
constexpr unsigned kWarmupFrames = 4;
constexpr unsigned kRequiredMidpoints = 2;
constexpr auto kColor = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kFormats[kPlanes] = {
    kColor, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, kColor, kColor
};

struct Deadline {
    uint64_t start = GetTickCount64();
    uint32_t limit;
    bool spent() const noexcept { return GetTickCount64() - start >= limit; }
};

struct Inputs {
    ComPtr<ID3D12Resource> textures[kEyes][kPlanes];
};

struct FixtureGpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> producerFence, readbackFence;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> rtvs;
    ComPtr<ID3D12Resource> readback;
    std::shared_ptr<Inputs> inputs;
    FrameGenerationNgxTicket ticket;
    unsigned descriptorStride = 0, rowPitch = 0;
    uint64_t rowStride = 0, readbackBytes = 0;

    uint64_t offset(unsigned eye, unsigned row) const noexcept {
        return (eye * kRows + row) * rowStride;
    }
};

FrameGenerationQualificationResult result(Verdict verdict, Reason reason,
    unsigned passingMidpoints = 0) noexcept {
    FrameGenerationQualificationResult out;
    out.verdict = verdict;
    out.reason = reason;
    out.passingMidpoints = passingMidpoints;
    return out;
}

FrameGenerationQualificationResult quarantine(std::unique_ptr<FixtureGpu>& gpu,
    unsigned passingMidpoints) noexcept {
    // A possibly queued read or write still references these exact objects.
    // Retain the preallocated owner even if a new allocation would fail now.
    static_cast<void>(gpu.release());
    return result(Verdict::Unsafe, Reason::Quarantined, passingMidpoints);
}

bool waitFence(ID3D12Fence& fence, uint64_t value, const Deadline& deadline,
    uint64_t& completed) noexcept {
    if (!value || value == UINT64_MAX) return false;
    do {
        completed = fence.GetCompletedValue();
        if (completed == UINT64_MAX) return false;
        if (completed >= value) return true;
        if (deadline.spent()) return false;
        // Poll only on the generation worker. A timeout leaves no registered
        // event that would need independent lifetime tracking.
        Sleep(1);
    } while (true);
}

bool supported(const FrameGenerationNgxStatus& status, const FrameGenerationNgxInit& init,
    const FrameGenerationQualificationLimits& limits) noexcept {
    return status.initialized && status.available && !status.failed && !status.quarantined &&
        status.failure == FrameGenerationNgxFailure::None && !status.submitted && status.maximumGenerated >= 1 &&
        status.requestedMultiplier == 2 && init.multiplier == 2 && init.uiRecomposition &&
        status.creationMode == FrameGenerationNgxCreationMode::OfficialFgHelper &&
        init.creationMode == status.creationMode &&
        status.suppressionLayout == FrameGenerationNgxSuppressionLayout::SharedNativePair &&
        init.suppressionLayout == status.suppressionLayout &&
        init.device && init.colorFormat == kColor &&
        init.width >= 256 && init.width <= 16384 && init.height >= 64 && init.height <= 16384 &&
        init.guideWidth >= 64 && init.guideWidth <= 16384 &&
        init.guideHeight >= 32 && init.guideHeight <= 16384 &&
        limits.timeoutMs && limits.timeoutMs <= fg_ngx::kMaxTimeoutMs && limits.fixtureBytes;
}

D3D12_RESOURCE_DESC textureDesc(const FrameGenerationNgxInit& init, unsigned plane) noexcept {
    const bool guide = plane == 1 || plane == 2;
    D3D12_RESOURCE_DESC out{};
    out.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    out.Width = guide ? init.guideWidth : init.width;
    out.Height = guide ? init.guideHeight : init.height;
    out.DepthOrArraySize = out.MipLevels = 1;
    out.Format = kFormats[plane];
    out.SampleDesc.Count = 1;
    out.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return out;
}

bool chargeAllocation(ID3D12Device& device, const D3D12_RESOURCE_DESC& desc,
    uint64_t& budget) noexcept {
    const auto allocation = device.GetResourceAllocationInfo(0, 1, &desc);
    if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX ||
        allocation.SizeInBytes > budget) return false;
    budget -= allocation.SizeInBytes;
    return true;
}

bool allocate(FixtureGpu& gpu, const FrameGenerationNgxInit& init,
    uint64_t budget, const Deadline& deadline) noexcept {
    gpu.device = init.device;
    gpu.rowPitch = (init.width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    gpu.rowStride = (uint64_t(gpu.rowPitch) + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1u) &
        ~(uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1u);
    gpu.readbackBytes = kEyes * kRows * gpu.rowStride;
    D3D12_RESOURCE_DESC readback{};
    readback.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback.Width = gpu.readbackBytes;
    readback.Height = 1;
    readback.DepthOrArraySize = readback.MipLevels = 1;
    readback.SampleDesc.Count = 1;
    readback.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (!chargeAllocation(*init.device, readback, budget)) return false;
    for (unsigned eye = 0; eye < kEyes; ++eye) for (unsigned plane = 0; plane < kPlanes; ++plane) {
        const auto desc = textureDesc(init, plane);
        if (!chargeAllocation(*init.device, desc, budget)) return false;
    }
    if (deadline.spent()) return false;

    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D12_DESCRIPTOR_HEAP_DESC targets{};
    targets.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    targets.NumDescriptors = kEyes * kPlanes;
    auto& device = *init.device;
    if (FAILED(device.CreateCommandQueue(&queue, IID_PPV_ARGS(&gpu.queue))) ||
        FAILED(device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.producerFence))) ||
        FAILED(device.CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.readbackFence))) ||
        FAILED(device.CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator))) ||
        FAILED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(),
            nullptr, IID_PPV_ARGS(&gpu.list))) ||
        FAILED(gpu.list->Close()) ||
        FAILED(device.CreateDescriptorHeap(&targets, IID_PPV_ARGS(&gpu.rtvs)))) return false;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    if (FAILED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpu.readback)))) return false;
    gpu.descriptorStride = device.GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    for (unsigned eye = 0; eye < kEyes; ++eye) for (unsigned plane = 0; plane < kPlanes; ++plane) {
        if (deadline.spent()) return false;
        auto& texture = gpu.inputs->textures[eye][plane];
        const auto desc = textureDesc(init, plane);
        if (FAILED(device.CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)))) return false;
        auto target = gpu.rtvs->GetCPUDescriptorHandleForHeapStart();
        target.ptr += SIZE_T(eye * kPlanes + plane) * gpu.descriptorStride;
        device.CreateRenderTargetView(texture.Get(), nullptr, target);
    }
    return gpu.descriptorStride != 0;
}

void transition(ID3D12GraphicsCommandList& list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    list.ResourceBarrier(1, &barrier);
}

bool begin(FixtureGpu& gpu) noexcept {
    return SUCCEEDED(gpu.allocator->Reset()) && SUCCEEDED(gpu.list->Reset(gpu.allocator.Get(), nullptr));
}

bool recordSource(FixtureGpu& gpu, const FrameGenerationNgxInit& init,
    fg_fixture::HudFixture fixture, unsigned frame, const fg_fixture::ProbeMotion (&views)[kEyes]) noexcept {
    if (!begin(gpu)) return false;
    for (unsigned eye = 0; eye < kEyes; ++eye) for (unsigned plane = 0; plane < kPlanes; ++plane) {
        auto* input = gpu.inputs->textures[eye][plane].Get();
        transition(*gpu.list.Get(), input, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto target = gpu.rtvs->GetCPUDescriptorHandleForHeapStart();
        target.ptr += SIZE_T(eye * kPlanes + plane) * gpu.descriptorStride;
        fg_fixture::paintInput(plane, init.width, init.height, init.guideWidth, init.guideHeight,
            frame, eye, fixture, views[eye].distantMotion, [&](const auto& color, const fg_fixture::Rect* rect) {
                if (!rect) gpu.list->ClearRenderTargetView(target, color.data(), 0, nullptr);
                else {
                    const D3D12_RECT region{rect->left, rect->top, rect->right, rect->bottom};
                    gpu.list->ClearRenderTargetView(target, color.data(), 1, &region);
                }
            });
        transition(*gpu.list.Get(), input, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    }
    return SUCCEEDED(gpu.list->Close());
}

FrameGenerationNgxInput describeSource(const FixtureGpu& gpu, unsigned frame,
    const fg_fixture::ProbeMotion (&views)[kEyes]) noexcept {
    FrameGenerationNgxInput input;
    input.sequence = frame;
    input.historyGeneration = 1;
    input.producerFence = gpu.producerFence.Get();
    input.producerValue = frame;
    input.inputLease = gpu.inputs;
    input.pairedFullEyeGuides = true;
    for (unsigned eye = 0; eye < kEyes; ++eye) {
        const auto& planes = gpu.inputs->textures[eye];
        auto& out = input.eyes[eye];
        out.color = planes[0].Get();
        out.depth = planes[1].Get();
        out.motion = planes[2].Get();
        out.hudless = planes[3].Get();
        out.ui = planes[4].Get();
        out.camera = views[eye].camera;
    }
    return input;
}

bool recordReadback(FixtureGpu& gpu, const FrameGenerationNgxInit& init,
    fg_fixture::HudFixture fixture) noexcept {
    if (!gpu.ticket.evaluationSucceeded || !gpu.ticket.uiRecomposed || gpu.ticket.generatedCount != 1 ||
        !gpu.ticket.generated[0][0] || !gpu.ticket.generated[1][0] ||
        gpu.ticket.generated[0][0] == gpu.ticket.generated[1][0] || !begin(gpu)) return false;
    for (unsigned eye = 0; eye < kEyes; ++eye) {
        auto* generated = gpu.ticket.generated[eye][0];
        const auto desc = generated->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != init.width ||
            desc.Height != init.height || desc.Format != kColor || desc.SampleDesc.Count != 1) return false;
        transition(*gpu.list.Get(), generated, gpu.ticket.outputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = generated;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        for (unsigned row = 0; row < kRows; ++row) {
            const unsigned y = fg_fixture::scanlineY(init.height, fixture, row);
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = gpu.readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint.Offset = gpu.offset(eye, row);
            destination.PlacedFootprint.Footprint = {kColor, init.width, 1, 1, gpu.rowPitch};
            const D3D12_BOX box{0, y, 0, init.width, y + 1, 1};
            gpu.list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
        }
        transition(*gpu.list.Get(), generated, D3D12_RESOURCE_STATE_COPY_SOURCE, gpu.ticket.outputState);
    }
    return SUCCEEDED(gpu.list->Close());
}

// Once ExecuteCommandLists accepts the list, a failed Signal is not a
// completed submission. Callers quarantine this owner in that case.
bool execute(FixtureGpu& gpu, ID3D12Fence& fence, uint64_t value) noexcept {
    ID3D12CommandList* const commands[] = {gpu.list.Get()};
    gpu.queue->ExecuteCommandLists(1, commands);
    return SUCCEEDED(gpu.queue->Signal(&fence, value));
}

bool observe(FixtureGpu& gpu, const FrameGenerationNgxInit& init, unsigned frame,
    fg_fixture::HudFixture fixture, const fg_fixture::ProbeMotion (&views)[kEyes],
    fg_recomposition::StereoReadback& pixels) noexcept {
    void* mapped = nullptr;
    const D3D12_RANGE read{0, SIZE_T(gpu.readbackBytes)};
    if (FAILED(gpu.readback->Map(0, &read, &mapped)) || !mapped) return false;
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    const fg_fixture::EyeReadbackRows rows[kEyes] = {
        {bytes + gpu.offset(0, 0), bytes + gpu.offset(0, 1)},
        {bytes + gpu.offset(1, 0), bytes + gpu.offset(1, 1)}
    };
    pixels = fg_fixture::observeStereoMidpoint(rows, init.width, frame, fixture, views);
    const D3D12_RANGE written{0, 0};
    gpu.readback->Unmap(0, &written);
    return true;
}

bool bilateral(const fg_recomposition::StereoReadback& pixels, unsigned covered) noexcept {
    return pixels.eligibleEyes == 3 && pixels.translatedEyes == 3 && pixels.worldEyes == 3 &&
        pixels.hudEyes == covered && pixels.throughHudEyes == covered && pixels.neutralEyes == (3u ^ covered);
}
} // namespace

FrameGenerationQualificationResult qualifyFrameGeneration(FrameGenerationNgx& backend,
    const FrameGenerationNgxInit& init, const FrameGenerationQualificationLimits& limits) noexcept {
    const auto status = backend.status();
    if (status.quarantined) return result(Verdict::Unsafe, Reason::Quarantined);
    if (!supported(status, init, limits)) return result(Verdict::Unverified, Reason::UnsupportedBackend);

    const Deadline deadline{GetTickCount64(), limits.timeoutMs};
    auto gpu = std::unique_ptr<FixtureGpu>(new (std::nothrow) FixtureGpu);
    if (!gpu) return result(Verdict::Unverified, Reason::Allocation);
    try { gpu->inputs = std::make_shared<Inputs>(); }
    catch (...) { return result(Verdict::Unverified, Reason::Allocation); }
    if (!allocate(*gpu, init, limits.fixtureBytes, deadline))
        return result(Verdict::Unverified, deadline.spent() ? Reason::Budget : Reason::Allocation);

    const auto fixture = limits.neutralRightEye ? fg_fixture::HudFixture::Neutral : fg_fixture::HudFixture::Opaque;
    const unsigned covered = limits.neutralRightEye ? 1u : 3u;
    unsigned verified = 0;
    for (unsigned frame = 1; frame <= kSourceFrames; ++frame) {
        if (deadline.spent()) return result(Verdict::Unverified, Reason::Budget, verified);
        const fg_fixture::ProbeMotion views[kEyes] = {
            fg_fixture::probeMotion(init.width, init.height, init.guideWidth, frame, 0, fixture),
            fg_fixture::probeMotion(init.width, init.height, init.guideWidth, frame, 1, fixture)
        };
        if (!recordSource(*gpu, init, fixture, frame, views))
            return result(Verdict::Unverified, Reason::Producer, verified);
        uint64_t completed = 0;
        if (!execute(*gpu, *gpu->producerFence.Get(), frame) ||
            !waitFence(*gpu->producerFence.Get(), frame, deadline, completed))
            return quarantine(gpu, verified);
        if (deadline.spent()) return result(Verdict::Unverified, Reason::Budget, verified);

        try { gpu->ticket = backend.submit(describeSource(*gpu, frame, views)); }
        catch (...) { return quarantine(gpu, verified); }
        if (!gpu->ticket.lease) {
            if (backend.status().quarantined) return quarantine(gpu, verified);
            return result(Verdict::Unverified, Reason::Evaluation, verified);
        }
        if (!gpu->ticket.completionFence || !waitFence(*gpu->ticket.completionFence,
            gpu->ticket.completionValue, deadline, completed)) return quarantine(gpu, verified);

        const auto completion = gpu->ticket.poll();
        if (completion == FrameGenerationNgxCompletion::DeviceRemoved) return quarantine(gpu, verified);
        if (completion != FrameGenerationNgxCompletion::Ready &&
            completion != FrameGenerationNgxCompletion::Suppressed)
            return result(Verdict::Unverified, Reason::Evaluation, verified);

        if (completion == FrameGenerationNgxCompletion::Suppressed || frame <= kWarmupFrames) {
            gpu->ticket = {};
            if (!backend.collect()) return quarantine(gpu, verified);
            continue;
        }
        if (deadline.spent()) return result(Verdict::Unverified, Reason::Budget, verified);
        if (!recordReadback(*gpu, init, fixture))
            return result(Verdict::Unverified, Reason::Readback, verified);
        if (FAILED(gpu->queue->Wait(gpu->ticket.completionFence, gpu->ticket.completionValue)))
            return quarantine(gpu, verified);
        if (!execute(*gpu, *gpu->readbackFence.Get(), frame)) return quarantine(gpu, verified);

        auto receipt = backend.recordQualificationReadback(gpu->ticket, gpu->readbackFence.Get(), frame);
        if (!waitFence(*gpu->readbackFence.Get(), frame, deadline, completed)) return quarantine(gpu, verified);
        if (deadline.spent()) return result(Verdict::Unverified, Reason::Budget, verified);
        fg_recomposition::StereoReadback pixels;
        const bool mapped = observe(*gpu, init, frame, fixture, views, pixels);
        gpu->ticket = {};
        if (!backend.collect()) return quarantine(gpu, verified);
        if (deadline.spent()) return result(Verdict::Unverified, Reason::Budget, verified);
        if (!receipt) return result(Verdict::Unverified, Reason::Registration, verified);
        if (!mapped) return result(Verdict::Unverified, Reason::Readback, verified);

        const bool passed = bilateral(pixels, covered);
        if (passed && ++verified < kRequiredMidpoints) continue;
        auto out = result(passed ? Verdict::Passed : Verdict::Unverified,
            passed ? Reason::Verified : Reason::Pixels, verified);
        out.pixels = pixels;
        out.coveredEyes = covered;
        out.submitted = frame;
        out.completed = completed;
        out.readbackFence = gpu->readbackFence;
        out.resetReceipt = std::move(receipt);
        return out;
    }
    return result(Verdict::Unverified, Reason::InsufficientReady, verified);
}
} // namespace edvr

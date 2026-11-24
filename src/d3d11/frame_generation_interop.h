#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <limits>

namespace edvr {
enum class FrameGenerationPlane : unsigned { Color, Depth, Motion, UiColorTransmittance, UiMultiply, Count };
inline constexpr unsigned frameGenerationPlaneBit(FrameGenerationPlane role) noexcept { return 1u << unsigned(role); }
inline constexpr unsigned frameGenerationRequiredPlanes = 7;
enum class FrameGenerationInteropFailure : unsigned {
    None, Off, NotInitialized, WrongThreadOrContext, InvalidSequence, InvalidDescriptor,
    InvalidFormat, WrongDevice, WrongAdapter, Busy, Device, Allocation, Fence, Incomplete, DeviceRemoved
};
struct FrameGenerationInteropInit {
    bool enabled{};
    ID3D11Device* device11{};
    ID3D11DeviceContext* context{};
    DWORD renderThreadId{};
    unsigned colorWidth{}, colorHeight{}, guideWidth{}, guideHeight{};
};
struct FrameGenerationInteropPlane {
    ID3D11Texture2D* alias11{};
    ID3D12Resource* resource12{};
    // Captured image descriptor; Color TYPELESS input becomes typed UNORM.
    // Original object provenance remains in sourceIdentity/sourceTextureIdentity.
    D3D11_TEXTURE2D_DESC sourceDesc{};
    std::uint64_t sourceIdentity{}, sourceTextureIdentity{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
};
class FrameGenerationInteropLease {
public:
    struct Impl;
    ~FrameGenerationInteropLease();
    FrameGenerationInteropLease(const FrameGenerationInteropLease&) = delete;
    FrameGenerationInteropLease& operator=(const FrameGenerationInteropLease&) = delete;
    std::uint64_t sequence() const noexcept;
    std::uint64_t historyGeneration() const noexcept;
    const FrameGenerationInteropPlane* plane(unsigned eye, FrameGenerationPlane) const noexcept;
    ID3D12Fence* producerFence() const noexcept;
    std::uint64_t producerValue() const noexcept;
    // Generation-thread metadata handoff. Completion must cover ALL uses and
    // return every imported texture to COMMON before this fence value signals.
    bool retire(ID3D12Fence* consumerFence, std::uint64_t value) noexcept;
    // Only when the caller proves no consumer GPU work was submitted. An empty
    // backend ticket after an evaluation error is insufficient proof.
    bool cancel() noexcept;
private:
    explicit FrameGenerationInteropLease(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl_;
    friend class FrameGenerationInterop;
};
struct FrameGenerationInteropStatus {
    bool initialized{}, quarantined{};
    FrameGenerationInteropFailure failure{FrameGenerationInteropFailure::NotInitialized};
    HRESULT lastHresult{S_OK};
    std::uint64_t published{}, busy{}, rejected{};
};
class FrameGenerationInterop {
public:
    FrameGenerationInterop() noexcept = default;
    ~FrameGenerationInterop();
    FrameGenerationInterop(const FrameGenerationInterop&) = delete;
    FrameGenerationInterop& operator=(const FrameGenerationInterop&) = delete;
    // Explicit known game render-thread ownership; no DllMain/deferred context.
    // OFF returns before driver loading, allocation or device/context queries.
    bool initialize(const FrameGenerationInteropInit&);
    ID3D12Device* device12() const noexcept;
    bool begin(ID3D11DeviceContext*, std::uint64_t sequence, std::uint64_t historyGeneration) noexcept;
    // Copy immediately at the authoritative temporal/sharp-eye callback, before
    // mutable source reuse. sourceIdentity is an audit tag, not content proof.
    // A pooled source must supply its immutable owner. The transport retains it
    // until this slot's producer/consumer GPU work and external leases retire,
    // including partial captures sealed by abort(). An empty owner preserves
    // the ordinary independently owned COM-texture contract.
    bool capture(ID3D11DeviceContext*, std::uint64_t sequence, std::uint64_t historyGeneration,
        unsigned eye, FrameGenerationPlane, ID3D11Texture2D*, std::uint64_t sourceIdentity,
        std::shared_ptr<const void> sourceOwner = {}) noexcept;
    // Both eyes must contain exactly the coordinator's explicit required mask.
    // One Signal+Flush follows all copies. Null UI never implicitly means empty.
    std::shared_ptr<FrameGenerationInteropLease> publish(ID3D11DeviceContext*,
        std::uint64_t sequence, std::uint64_t historyGeneration, unsigned requiredPlaneMask) noexcept;
    // Seal partial copies with a producer fence, cancel consumer use, never wait.
    bool abort(ID3D11DeviceContext*, std::uint64_t sequence, std::uint64_t historyGeneration) noexcept;
    // After producers and workers quiesce, all slots must have both fence proofs
    // and no remaining consumers before the coordinator releases this ring.
    bool retired() const noexcept;
    FrameGenerationInteropStatus status() const noexcept;
private:
    struct Impl;
    Impl* impl_{};
    FrameGenerationInteropFailure initialFailure_{FrameGenerationInteropFailure::NotInitialized};
};

// CPU policy shared by the adapter and synthetic fixtures. No COM calls here.
namespace fg_interop {
constexpr unsigned slots = 3, eyes = 2, planes = unsigned(FrameGenerationPlane::Count);
constexpr unsigned allPlanes = (1u << planes) - 1;
inline bool owner(DWORD expected, DWORD current, const void* context, const void* provided) noexcept {
    return expected && expected == current && context && context == provided;
}
inline bool complete(std::uint64_t actual, std::uint64_t target) noexcept {
    return target && actual != std::numeric_limits<std::uint64_t>::max() && actual >= target;
}
inline bool reusable(unsigned owners, std::uint64_t producerActual, std::uint64_t producerTarget,
    bool canceled, bool consumerAttached, std::uint64_t consumerActual, std::uint64_t consumerTarget) noexcept {
    return owners == 1 && producerActual != std::numeric_limits<std::uint64_t>::max() &&
        (!producerTarget || complete(producerActual, producerTarget)) &&
        (canceled || (consumerAttached && complete(consumerActual, consumerTarget)));
}
// Only Color can type a compatible RGBA8 source during its immutable copy.
// Other roles and typed formats retain their existing descriptor interpretation.
inline DXGI_FORMAT captureFormat(FrameGenerationPlane role, DXGI_FORMAT value) noexcept {
    return role == FrameGenerationPlane::Color && value == DXGI_FORMAT_R8G8B8A8_TYPELESS
        ? DXGI_FORMAT_R8G8B8A8_UNORM : value;
}
inline bool format(FrameGenerationPlane role, DXGI_FORMAT value) noexcept {
    const bool color = value == DXGI_FORMAT_R8G8B8A8_UNORM || value == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        value == DXGI_FORMAT_B8G8R8A8_UNORM || value == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    switch (role) {
    case FrameGenerationPlane::Color: return color || value == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case FrameGenerationPlane::Depth: return value == DXGI_FORMAT_R32_FLOAT;
    case FrameGenerationPlane::Motion: return value == DXGI_FORMAT_R16G16_FLOAT;
    case FrameGenerationPlane::UiColorTransmittance:
    case FrameGenerationPlane::UiMultiply: return color || value == DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return false;
    }
}
inline bool descriptor(const D3D11_TEXTURE2D_DESC& desc, unsigned width, unsigned height) noexcept {
    return width && height && desc.Width == width && desc.Height == height && desc.MipLevels == 1 &&
        desc.ArraySize == 1 && desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
        desc.Usage == D3D11_USAGE_DEFAULT && desc.CPUAccessFlags == 0;
}
struct Transaction {
    std::uint64_t sequence{}, generation{};
    unsigned captured[2]{};
    bool matches(std::uint64_t seq, std::uint64_t gen) const noexcept {
        return seq && gen && seq == sequence && gen == generation;
    }
    bool accepts(std::uint64_t seq, std::uint64_t gen, unsigned eye, FrameGenerationPlane role) const noexcept {
        return matches(seq, gen) && eye < eyes && unsigned(role) < planes &&
            !(captured[eye] & frameGenerationPlaneBit(role));
    }
    bool paired(std::uint64_t seq, std::uint64_t gen, unsigned mask) const noexcept {
        return matches(seq, gen) && (mask & frameGenerationRequiredPlanes) == frameGenerationRequiredPlanes &&
            !(mask & ~allPlanes) && captured[0] == mask && captured[1] == mask;
    }
};
}  // namespace fg_interop
}  // namespace edvr

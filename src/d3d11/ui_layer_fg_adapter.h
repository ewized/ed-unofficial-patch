#pragma once
// Private production adapter shared by ui_layer.cpp and its focused WARP rig.
#include "ui_layer.h"
#include "ui_layer_smaa.h"
#include "ui_layer_shaders.h"
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstring>
#include <utility>

namespace edvr::ui_layer_fg {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;

class ComputeStateGuard {
    ID3D11DeviceContext* context_;
    Ptr<ID3D11ComputeShader> shader_;
    Ptr<ID3D11Buffer> constants_;
    Ptr<ID3D11UnorderedAccessView> target_;
    std::array<Ptr<ID3D11ShaderResourceView>, 3> resources_;
    std::array<Ptr<ID3D11SamplerState>, 2> samplers_;
    std::array<Ptr<ID3D11ClassInstance>, D3D11_SHADER_MAX_INTERFACES> classes_;
    UINT classCount_ = D3D11_SHADER_MAX_INTERFACES;
public:
    explicit ComputeStateGuard(ID3D11DeviceContext* context) : context_(context) {
        ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
        context_->CSGetShader(&shader_, instances, &classCount_);
        for (UINT i = 0; i < classCount_; ++i) classes_[i].Attach(instances[i]);
        context_->CSGetConstantBuffers(0, 1, &constants_);
        context_->CSGetUnorderedAccessViews(0, 1, &target_);
        ID3D11ShaderResourceView* resources[3]{};
        context_->CSGetShaderResources(0, 3, resources);
        for (unsigned i = 0; i < 3; ++i) resources_[i].Attach(resources[i]);
        ID3D11SamplerState* samplers[2]{};
        context_->CSGetSamplers(0, 2, samplers);
        for (unsigned i = 0; i < 2; ++i) samplers_[i].Attach(samplers[i]);
    }
    ComputeStateGuard(const ComputeStateGuard&) = delete;
    ComputeStateGuard& operator=(const ComputeStateGuard&) = delete;
    ~ComputeStateGuard() {
        ID3D11ShaderResourceView* empty[3]{};
        ID3D11UnorderedAccessView* nullTarget = nullptr;
        context_->CSSetShaderResources(0, 3, empty);
        context_->CSSetUnorderedAccessViews(0, 1, &nullTarget, nullptr);
        ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
        for (UINT i = 0; i < classCount_; ++i) instances[i] = classes_[i].Get();
        context_->CSSetShader(shader_.Get(), instances, classCount_);
        ID3D11Buffer* cb = constants_.Get();
        context_->CSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* resources[3] = {resources_[0].Get(), resources_[1].Get(), resources_[2].Get()};
        context_->CSSetShaderResources(0, 3, resources);
        ID3D11UnorderedAccessView* target = target_.Get();
        context_->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        ID3D11SamplerState* samplers[2] = {samplers_[0].Get(), samplers_[1].Get()};
        context_->CSSetSamplers(0, 2, samplers);
    }
};

struct Plane {
    Ptr<ID3D11Texture2D> texture;
    Ptr<ID3D11ShaderResourceView> view;
    D3D11_TEXTURE2D_DESC description{};
    D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
};
struct PlaneStorage { Plane layer, multiply; };
} // namespace edvr::ui_layer_fg

namespace edvr {
class UiLayerFgSnapshot {
public:
    UiLayerFgInfo info;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> layer, multiply;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> layerSrv, multiplySrv;
    // Separate publication metadata from reusable pixels. Retaining this
    // snapshot keeps its slot immutable through every consumer's GPU fence.
    std::shared_ptr<ui_layer_fg::PlaneStorage> storage;
    float uv[4] = {0, 0, 1, 1};
    bool useMult = false;
};
} // namespace edvr

namespace edvr::ui_layer_fg {
// Exactly one producer eye owns each cache. Only the producer thread touches
// its COM/SMAA fields. A foreign caller is rejected before reading those fields.
// Retained snapshots copy the mutable SMAA output. Direct capture only borrows
// it on this context while the sink queues its copy.
class CaptureCache {
    std::atomic<DWORD> thread_{0};
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    unsigned eye_ = 2;
    std::array<std::shared_ptr<PlaneStorage>, 3> planes_;
public:
    UiLayerSmaa smaa;
    UiLayerFxaa fxaa;
    uint64_t builderCalls = 0, reused = 0, resets = 0;
    uint64_t textureAllocations = 0, viewAllocations = 0, planeReuses = 0, poolMisses = 0;
    CaptureCache() = default;
    CaptureCache(const CaptureCache&) = delete;
    CaptureCache& operator=(const CaptureCache&) = delete;
    bool bind(ID3D11DeviceContext* context, ID3D11Device* device, unsigned eye) {
        if (!context || !device || eye > 1) return false;
        const DWORD caller = GetCurrentThreadId();
        DWORD owner = thread_.load(std::memory_order_acquire);
        if (!owner && !thread_.compare_exchange_strong(owner, caller, std::memory_order_acq_rel)) {
            if (owner != caller) return false;
        } else if (owner && owner != caller) return false;
        if (!context_ && !device_) { context_ = context; device_ = device; eye_ = eye; }
        return context_.Get() == context && device_.Get() == device && eye_ == eye;
    }
    // Only cache-exclusive storage may be rewritten. Every GPU reader must
    // retain its snapshot until completion; interop capture carries that owner
    // through its existing fence retirement. Exhaustion keeps native fallback.
    std::shared_ptr<PlaneStorage> acquire() {
        if (thread_.load(std::memory_order_acquire) != GetCurrentThreadId()) return {};
        for (auto& plane : planes_) {
            if (!plane) plane = std::make_shared<PlaneStorage>();
            if (plane.use_count() == 1) return plane;
        }
        ++poolMisses;
        return {};
    }
    // Preflight without claiming an unbound cache. A producer handoff checks
    // both eyes before either cache can release its storage references.
    bool canRetireStorage(ID3D11DeviceContext* context, ID3D11Device* device, unsigned eye) const noexcept {
        if (!context || !device || eye > 1) return false;
        const DWORD owner = thread_.load(std::memory_order_acquire);
        if (!owner) return true;
        if (owner != GetCurrentThreadId()) return false;
        return context_.Get() == context && device_.Get() == device && eye_ == eye;
    }
    // Drop only the cache references to leased pixels. Existing snapshots and
    // interop fences retain the old storage independently. Unleased planes and
    // SMAA remain reusable on this same producer context/device/thread.
    bool retireSharedStorage() noexcept {
        const DWORD owner = thread_.load(std::memory_order_acquire);
        if (!owner) return true;
        if (owner != GetCurrentThreadId()) return false;
        for (auto& plane : planes_) if (plane && plane.use_count() > 1) plane.reset();
        return true;
    }
    bool ensure(unsigned width, unsigned height, UiLayerSmaaFail* failure) {
        if (thread_.load(std::memory_order_acquire) != GetCurrentThreadId()) return false;
        if (smaa.ready() && smaa.w == width && smaa.h == height) { ++reused; return true; }
        if (smaa.failedW == width && smaa.failedH == height) return false;
        ++builderCalls;
        const auto state = uiLayerSmaaEnsure(smaa, device_.Get(), width, height, failure);
        return state == UiLayerSmaaState::kReady || state == UiLayerSmaaState::kMade;
    }
    bool ensureFxaa(unsigned width, unsigned height, UiLayerSmaaFail* failure) {
        if (thread_.load(std::memory_order_acquire) != GetCurrentThreadId()) return false;
        const auto state = uiLayerFxaaEnsure(fxaa, device_.Get(), width, height, failure);
        return state == UiLayerSmaaState::kReady || state == UiLayerSmaaState::kMade;
    }
    // Called only at the existing owned frame boundary on a context lifecycle
    // change. A thread migration without an old-owner handoff refuses FG.
    bool resetOwned() noexcept {
        const DWORD owner = thread_.load(std::memory_order_acquire);
        if (owner && owner != GetCurrentThreadId()) return false;
        smaa.releaseAll(); fxaa.releaseAll(); planes_ = {}; context_.Reset(); device_.Reset(); eye_ = 2; ++resets;
        thread_.store(0, std::memory_order_release);
        return true;
    }
};

// Successful new-producer handoff only: preflight both eyes without claiming
// unbound caches, then detach old leased storage without waiting on its GPU.
inline bool retireStorage(CaptureCache (&caches)[2], ID3D11DeviceContext* context, ID3D11Device* device) noexcept {
    for (unsigned eye = 0; eye < 2; ++eye)
        if (!caches[eye].canRetireStorage(context, device, eye)) return false;
    for (auto& cache : caches) if (!cache.retireSharedStorage()) return false;
    return true;
}

struct Source {
    UiLayerFgFacts facts;
    UiLayerFgInfo info;
    // Authoritative pre-RCAS temporal output for this eye/sequence, supplied by
    // ui_layer.cpp. Size/device equality is deliberately insufficient.
    const void* expectedFrame = nullptr;
    ID3D11Texture2D* layer = nullptr;
    ID3D11ShaderResourceView* layerView = nullptr;
    ID3D11Texture2D* multiply = nullptr;
    ID3D11ShaderResourceView* multiplyView = nullptr;
    float uv[4] = {0, 0, 1, 1};
    bool useMult = false, smooth = false, fxaa = false;
};

inline bool planeMatches(ID3D11Device* device, ID3D11Texture2D* source,
    ID3D11ShaderResourceView* sourceView, unsigned width, unsigned height) {
    if (!source || !sourceView) return false;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if (desc.Width != width || desc.Height != height || desc.SampleDesc.Count != 1 ||
        desc.ArraySize != 1 || desc.MipLevels != 1) return false;
    Ptr<ID3D11Device> sourceDevice; source->GetDevice(&sourceDevice);
    Ptr<ID3D11Resource> viewed; sourceView->GetResource(&viewed);
    return sourceDevice.Get() == device && viewed.Get() == static_cast<ID3D11Resource*>(source);
}

// Deep-copy into a cache slot acquired exclusively by this publication. Reuse
// its texture/view only for matching descriptors; allocation failure never
// replaces the old complete pair. All GPU work stays on the producer context.
inline bool copyPlane(ID3D11DeviceContext* context, ID3D11Device* device,
    ID3D11Texture2D* source, ID3D11ShaderResourceView* sourceView,
    Plane& retained, Ptr<ID3D11Texture2D>& texture, Ptr<ID3D11ShaderResourceView>& view, CaptureCache& cache) {
    if (!source) return false;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if (!planeMatches(device, source, sourceView, desc.Width, desc.Height)) return false;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = desc.MiscFlags = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sourceView->GetDesc(&sd);
    const auto& old = retained.description;
    const auto& oldView = retained.viewDescription;
    // Admission fixes one mip, slice and sample; all other output flags above
    // are canonical. Compare the legal single-slice SRV interpretations too.
    bool sameView = oldView.Format == sd.Format && oldView.ViewDimension == sd.ViewDimension;
    if (sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
        sameView = sameView && oldView.Texture2D.MostDetailedMip == sd.Texture2D.MostDetailedMip &&
            oldView.Texture2D.MipLevels == sd.Texture2D.MipLevels;
    } else if (sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY) {
        sameView = sameView && oldView.Texture2DArray.MostDetailedMip == sd.Texture2DArray.MostDetailedMip &&
            oldView.Texture2DArray.MipLevels == sd.Texture2DArray.MipLevels &&
            oldView.Texture2DArray.FirstArraySlice == sd.Texture2DArray.FirstArraySlice &&
            oldView.Texture2DArray.ArraySize == sd.Texture2DArray.ArraySize;
    } else sameView = false;
    if (!retained.texture || !retained.view || old.Width != desc.Width || old.Height != desc.Height ||
        old.Format != desc.Format || old.SampleDesc.Quality != desc.SampleDesc.Quality || !sameView) {
        Ptr<ID3D11Texture2D> created; Ptr<ID3D11ShaderResourceView> createdView;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &created))) return false;
        ++cache.textureAllocations;
        if (FAILED(device->CreateShaderResourceView(created.Get(), &sd, &createdView))) return false;
        ++cache.viewAllocations;
        retained.texture = std::move(created); retained.view = std::move(createdView);
        retained.description = desc; retained.viewDescription = sd;
    } else ++cache.planeReuses;
    texture = retained.texture; view = retained.view;
    context->CopyResource(texture.Get(), source);
    return SUCCEEDED(device->GetDeviceRemovedReason());
}

using Smooth = ID3D11ShaderResourceView*(*)(UiLayerSmaa&, ID3D11DeviceContext*, ID3D11ShaderResourceView*);
// Validate and prepare the final producer planes before handing them to a
// synchronous consumer. All later producer writes on this immediate context
// are ordered after the consumer's copy commands.
template<class Consume>
bool withPlanes(ID3D11DeviceContext* context,
    ID3D11Texture2D* frame, const Source& source, CaptureCache& cache, UiLayerFgInfo* info,
    Smooth runSmooth, Consume&& consume) noexcept {
    UiLayerFgPlanes planes;
    auto& result = planes.info;
    result = source.info;
    result.fullSupported = result.empty = result.smoothingApplied = false;
    const auto fail = [&](UiLayerFgRefusal refusal) {
        result.fullSupported = result.empty = result.smoothingApplied = false;
        result.refusal = refusal;
        if (info) *info = result;
        return false;
    };
    try {
        auto facts = source.facts;
        facts.arguments = facts.arguments && context && frame && result.sequence && result.eye < 2;
        facts.owner = facts.owner && context && context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
        facts.eye = facts.eye && frame && frame == source.expectedFrame;
        const auto refusal = uiLayerFgAdmission(facts);
        if (refusal != UiLayerFgRefusal::None) return fail(refusal);
        Ptr<ID3D11Device> device, contextDevice;
        frame->GetDevice(&device); context->GetDevice(&contextDevice);
        if (device.Get() != contextDevice.Get()) return fail(UiLayerFgRefusal::ForeignOwner);
        if (!cache.bind(context, device.Get(), result.eye)) return fail(UiLayerFgRefusal::ForeignOwner);
        D3D11_TEXTURE2D_DESC fd{}; frame->GetDesc(&fd);
        if (!fd.Width || !fd.Height || fd.SampleDesc.Count != 1 || fd.ArraySize != 1 || fd.MipLevels != 1 ||
            uiLayerFrameView(fd.Format) == DXGI_FORMAT_UNKNOWN) return fail(UiLayerFgRefusal::SizeMismatch);
        result.width = fd.Width; result.height = fd.Height; result.frameFormat = fd.Format;
        result.sourceDevice = reinterpret_cast<uintptr_t>(device.Get()); result.ownerThread = GetCurrentThreadId();
        if (facts.hasUi) {
            if (!source.layer || !source.layerView) return fail(UiLayerFgRefusal::IncompletePublication);
            if (!planeMatches(device.Get(), source.layer, source.layerView, fd.Width, fd.Height))
                return fail(UiLayerFgRefusal::SizeMismatch);
            if (source.useMult) {
                if (!source.multiply || !source.multiplyView) return fail(UiLayerFgRefusal::IncompletePublication);
                if (!planeMatches(device.Get(), source.multiply, source.multiplyView, fd.Width, fd.Height))
                    return fail(UiLayerFgRefusal::SizeMismatch);
            }
        } else if (source.useMult) return fail(UiLayerFgRefusal::IncompletePublication);
        planes.layer = facts.hasUi ? source.layer : nullptr;
        planes.useMult = source.useMult;
        planes.multiply = source.useMult ? source.multiply : nullptr;
        std::memcpy(planes.layerUv, source.uv, sizeof(planes.layerUv));
        auto* layerView = facts.hasUi ? source.layerView : nullptr;
        Ptr<ID3D11Texture2D> smoothedTexture;
        if (facts.hasUi && (source.smooth || source.fxaa)) {
            UiLayerSmaaFail failure;
            const bool ready = source.fxaa ? cache.ensureFxaa(fd.Width, fd.Height, &failure)
                                           : cache.ensure(fd.Width, fd.Height, &failure);
            if (!ready) return fail(UiLayerFgRefusal::CopyFailed);
            // Restore compute state before giving the final texture to the sink.
            ComputeStateGuard restoreCompute(context);
            layerView = source.fxaa ? uiLayerFxaaRun(cache.fxaa, context, source.layerView) :
                runSmooth ? runSmooth(cache.smaa, context, source.layerView) : nullptr;
            Ptr<ID3D11Resource> resource;
            if (layerView) layerView->GetResource(&resource);
            if (!resource || FAILED(resource.As(&smoothedTexture)) ||
                !planeMatches(device.Get(), smoothedTexture.Get(), layerView, fd.Width, fd.Height))
                return fail(UiLayerFgRefusal::CopyFailed);
            planes.layer = smoothedTexture.Get();
            result.smoothingApplied = true;
        }
        result.empty = !facts.hasUi; result.fullSupported = true; result.refusal = UiLayerFgRefusal::None;
        if (!consume(device.Get(), planes, layerView)) return fail(UiLayerFgRefusal::CopyFailed);
        if (info) *info = result;
        return true;
    } catch (...) { return fail(UiLayerFgRefusal::CopyFailed); }
}

inline bool captureTo(ID3D11DeviceContext* context, ID3D11Texture2D* frame,
    const Source& source, CaptureCache& cache, UiLayerFgCaptureSink sink, void* cookie,
    UiLayerFgInfo* info, Smooth runSmooth = uiLayerSmaaRun) noexcept {
    if (!sink) {
        if (info) {
            *info = source.info;
            info->fullSupported = info->empty = info->smoothingApplied = false;
            info->refusal = UiLayerFgRefusal::InvalidArgument;
        }
        return false;
    }
    return withPlanes(context, frame, source, cache, info, runSmooth,
        [&](ID3D11Device*, const UiLayerFgPlanes& planes, ID3D11ShaderResourceView*) {
            return sink(cookie, planes);
        });
}

inline std::shared_ptr<const UiLayerFgSnapshot> capture(ID3D11DeviceContext* context,
    ID3D11Texture2D* frame, const Source& source, CaptureCache& cache, UiLayerFgInfo* info,
    Smooth runSmooth = uiLayerSmaaRun) noexcept {
    std::shared_ptr<UiLayerFgSnapshot> snapshot;
    withPlanes(context, frame, source, cache, info, runSmooth,
        [&](ID3D11Device* device, const UiLayerFgPlanes& planes, ID3D11ShaderResourceView* layerView) {
            auto retained = std::make_shared<UiLayerFgSnapshot>();
            retained->info = planes.info; retained->device = device; retained->context = context;
            retained->useMult = planes.useMult;
            std::memcpy(retained->uv, planes.layerUv, sizeof(retained->uv));
            if (!planes.info.empty) {
                retained->storage = cache.acquire();
                if (!retained->storage || !copyPlane(context, device, planes.layer, layerView,
                        retained->storage->layer, retained->layer, retained->layerSrv, cache))
                    return false;
                if (planes.useMult && !copyPlane(context, device, planes.multiply, source.multiplyView,
                        retained->storage->multiply, retained->multiply, retained->multiplySrv, cache))
                    return false;
            }
            snapshot = std::move(retained);
            return true;
        });
    return snapshot;
}

} // namespace edvr::ui_layer_fg

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
} // namespace edvr::ui_layer_fg

namespace edvr {
class UiLayerFgSnapshot {
public:
    UiLayerFgInfo info;
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> layer, multiply;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> layerSrv, multiplySrv;
    float uv[4] = {0, 0, 1, 1};
    bool useMult = false, smooth = false;
};
} // namespace edvr

namespace edvr::ui_layer_fg {
// Exactly one producer eye owns each cache. Only the producer thread touches
// its COM/SMAA fields. A foreign caller is rejected before reading those fields.
// Snapshots never retain the mutable SMAA output: each publication copies it.
class CaptureCache {
    std::atomic<DWORD> thread_{0};
    Ptr<ID3D11Device> device_;
    Ptr<ID3D11DeviceContext> context_;
    unsigned eye_ = 2;
public:
    UiLayerSmaa smaa;
    uint64_t builderCalls = 0, reused = 0, resets = 0;
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
    bool ensure(unsigned width, unsigned height, UiLayerSmaaFail* failure) {
        if (thread_.load(std::memory_order_acquire) != GetCurrentThreadId()) return false;
        if (smaa.ready() && smaa.w == width && smaa.h == height) { ++reused; return true; }
        if (smaa.failedW == width && smaa.failedH == height) return false;
        ++builderCalls;
        const auto state = uiLayerSmaaEnsure(smaa, device_.Get(), width, height, failure);
        return state == UiLayerSmaaState::kReady || state == UiLayerSmaaState::kMade;
    }
    // Called only at the existing owned frame boundary on a context lifecycle
    // change. A thread migration without an old-owner handoff refuses FG.
    bool resetOwned() noexcept {
        const DWORD owner = thread_.load(std::memory_order_acquire);
        if (owner && owner != GetCurrentThreadId()) return false;
        smaa.releaseAll(); context_.Reset(); device_.Reset(); eye_ = 2; ++resets;
        thread_.store(0, std::memory_order_release);
        return true;
    }
};

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
    bool useMult = false, smooth = false;
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

inline bool copyPlane(ID3D11DeviceContext* context, ID3D11Device* device,
    ID3D11Texture2D* source, ID3D11ShaderResourceView* sourceView,
    Ptr<ID3D11Texture2D>& texture, Ptr<ID3D11ShaderResourceView>& view) {
    if (!source) return false;
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if (!planeMatches(device, source, sourceView, desc.Width, desc.Height)) return false;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = desc.MiscFlags = 0;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sourceView->GetDesc(&sd);
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)) ||
        FAILED(device->CreateShaderResourceView(texture.Get(), &sd, &view))) return false;
    // The cross-API fence is signalled by the caller AFTER capture returns.
    context->CopyResource(texture.Get(), source);
    return SUCCEEDED(device->GetDeviceRemovedReason());
}

using Smooth = ID3D11ShaderResourceView*(*)(UiLayerSmaa&, ID3D11DeviceContext*, ID3D11ShaderResourceView*);
inline std::shared_ptr<const UiLayerFgSnapshot> capture(ID3D11DeviceContext* context,
    ID3D11Texture2D* frame, const Source& source, CaptureCache& cache, UiLayerFgInfo* info,
    Smooth runSmooth = uiLayerSmaaRun) noexcept {
    UiLayerFgInfo result = source.info;
    result.fullSupported = result.empty = result.smoothingApplied = false;
    const auto fail = [&](UiLayerFgRefusal refusal) -> std::shared_ptr<const UiLayerFgSnapshot> {
        result.refusal = refusal; if (info) *info = result; return {};
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
        // Includes success, ordinary failure and exception exits of SMAA. Its
        // caller contract leaves CS/CB/SRV/UAV changed; it restores samplers only.
        ComputeStateGuard restoreCompute(context);
        auto snapshot = std::make_shared<UiLayerFgSnapshot>();
        snapshot->device = device; snapshot->context = context; snapshot->useMult = source.useMult;
        std::memcpy(snapshot->uv, source.uv, sizeof(snapshot->uv));
        if (facts.hasUi && source.useMult &&
            !copyPlane(context, device.Get(), source.multiply, source.multiplyView, snapshot->multiply, snapshot->multiplySrv))
            return fail(UiLayerFgRefusal::CopyFailed);
        if (facts.hasUi && source.smooth) {
            UiLayerSmaaFail failure;
            if (!cache.ensure(fd.Width, fd.Height, &failure)) return fail(UiLayerFgRefusal::CopyFailed);
            // The live source is read before its next ordered clear/write on
            // this same producer context. Only the FINAL smoothed plane needs
            // a deep copy; no redundant raw-layer snapshot is allocated.
            ID3D11ShaderResourceView* smoothed = runSmooth ? runSmooth(cache.smaa, context, source.layerView) : nullptr;
            Ptr<ID3D11Resource> resource; Ptr<ID3D11Texture2D> texture;
            if (smoothed) smoothed->GetResource(&resource);
            if (!resource || FAILED(resource.As(&texture)) ||
                !copyPlane(context, device.Get(), texture.Get(), smoothed, snapshot->layer, snapshot->layerSrv))
                return fail(UiLayerFgRefusal::CopyFailed);
            result.smoothingApplied = true;
        } else if (facts.hasUi &&
            !copyPlane(context, device.Get(), source.layer, source.layerView, snapshot->layer, snapshot->layerSrv)) {
            return fail(UiLayerFgRefusal::CopyFailed);
        }
        result.empty = !facts.hasUi; result.fullSupported = true; result.refusal = UiLayerFgRefusal::None;
        snapshot->info = result; if (info) *info = result; return snapshot;
    } catch (...) { return fail(UiLayerFgRefusal::CopyFailed); }
}

// Shared production dispatch: the normal composite and replay fixture use the
// same bindings, shader bytecode and group layout. Caller owns a state guard.
inline void issueComposite(ID3D11DeviceContext* context, ID3D11ComputeShader* shader,
    ID3D11Buffer* constants, ID3D11ShaderResourceView* frame, ID3D11ShaderResourceView* layer,
    ID3D11ShaderResourceView* multiply, ID3D11UnorderedAccessView* target, unsigned width, unsigned height) {
    ID3D11ShaderResourceView* empty[3]{}; ID3D11UnorderedAccessView* nullTarget = nullptr;
    context->CSSetShaderResources(0, 3, empty); context->CSSetUnorderedAccessViews(0, 1, &nullTarget, nullptr);
    context->CSSetShader(shader, nullptr, 0); context->CSSetConstantBuffers(0, 1, &constants);
    ID3D11ShaderResourceView* resources[3] = {frame, layer, multiply};
    context->CSSetShaderResources(0, 3, resources); context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
    context->Dispatch((width + kUiLayerCompositeGroupX - 1) / kUiLayerCompositeGroupX,
        (height + kUiLayerCompositeGroupY - 1) / kUiLayerCompositeGroupY, 1);
}

template<class Composite>
bool replay(ID3D11DeviceContext* context, const std::shared_ptr<const UiLayerFgSnapshot>& snapshot,
    ID3D11Texture2D* world, ID3D11Texture2D** output, bool owner, Composite&& composite, UiLayerFgInfo* info) noexcept {
    if (output) *output = nullptr;
    UiLayerFgInfo result = snapshot ? snapshot->info : UiLayerFgInfo{};
    const auto fail = [&](UiLayerFgRefusal why) {
        result.fullSupported = false; result.refusal = why; if (info) *info = result; return false;
    };
    try {
        if (!snapshot || !snapshot->info.fullSupported || !world || !output) return fail(UiLayerFgRefusal::InvalidArgument);
        if (!owner || context != snapshot->context.Get() || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            GetCurrentThreadId() != snapshot->info.ownerThread) return fail(UiLayerFgRefusal::ForeignOwner);
        Ptr<ID3D11Device> device; world->GetDevice(&device);
        if (device.Get() != snapshot->device.Get()) return fail(UiLayerFgRefusal::ForeignOwner);
        D3D11_TEXTURE2D_DESC fd{}; world->GetDesc(&fd);
        if (fd.Width != result.width || fd.Height != result.height || fd.SampleDesc.Count != 1 || fd.ArraySize != 1 ||
            fd.MipLevels != 1 || uiLayerFrameView(fd.Format) == DXGI_FORMAT_UNKNOWN ||
            uiLayerFrameView(fd.Format) != uiLayerFrameView(static_cast<DXGI_FORMAT>(result.frameFormat)))
            return fail(UiLayerFgRefusal::SizeMismatch);
        ComputeStateGuard restoreCompute(context);
        Ptr<ID3D11Texture2D> rendered;
        if (snapshot->info.empty) {
            fd.Usage = D3D11_USAGE_DEFAULT; fd.CPUAccessFlags = fd.MiscFlags = 0; fd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&fd, nullptr, &rendered))) return fail(UiLayerFgRefusal::CopyFailed);
            context->CopyResource(rendered.Get(), world);
        } else rendered.Attach(composite(*snapshot, world));
        if (!rendered || FAILED(device->GetDeviceRemovedReason())) return fail(UiLayerFgRefusal::CompositeFailed);
        *output = rendered.Detach(); if (info) *info = result; return true;
    } catch (...) { return fail(UiLayerFgRefusal::CompositeFailed); }
}
} // namespace edvr::ui_layer_fg

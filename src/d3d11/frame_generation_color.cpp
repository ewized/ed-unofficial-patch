#include "frame_generation_color.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>

namespace edvr {
using Microsoft::WRL::ComPtr;
namespace {
// Only the slots this pass writes. Class instances are retained as well as the
// shader; restoring just the shader would change a dynamically linked CS.
struct ComputeState {
    ID3D11DeviceContext* context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11ShaderResourceView> source;
    ComPtr<ID3D11UnorderedAccessView> output;
    std::array<ComPtr<ID3D11ClassInstance>, D3D11_SHADER_MAX_INTERFACES> classes;
    UINT count = D3D11_SHADER_MAX_INTERFACES;
    explicit ComputeState(ID3D11DeviceContext* ctx) : context(ctx) {
        ID3D11ClassInstance* raw[D3D11_SHADER_MAX_INTERFACES]{};
        context->CSGetShader(&shader, raw, &count);
        for (UINT i = 0; i < count; ++i) classes[i].Attach(raw[i]);
        context->CSGetShaderResources(0, 1, &source);
        context->CSGetUnorderedAccessViews(0, 1, &output);
    }
    ~ComputeState() {
        ID3D11ShaderResourceView* noSource = nullptr;
        ID3D11UnorderedAccessView* noOutput = nullptr;
        const UINT keep = UINT(-1);
        context->CSSetShaderResources(0, 1, &noSource);
        context->CSSetUnorderedAccessViews(0, 1, &noOutput, &keep);
        ID3D11ClassInstance* raw[D3D11_SHADER_MAX_INTERFACES]{};
        for (UINT i = 0; i < count; ++i) raw[i] = classes[i].Get();
        context->CSSetShader(shader.Get(), raw, count);
        auto* srv = source.Get(); auto* uav = output.Get();
        context->CSSetShaderResources(0, 1, &srv);
        context->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    }
};
bool sameDevice(ID3D11Device* expected, ID3D11DeviceChild* child) {
    ComPtr<ID3D11Device> actual;
    if (child) child->GetDevice(&actual);
    return expected && actual.Get() == expected;
}
}
struct FrameGenerationColor::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    DWORD thread = 0;
    struct Eye {
        ComPtr<ID3D11Texture2D> color, bgra, empty[2];
        ComPtr<ID3D11ShaderResourceView> bgraView;
        ComPtr<ID3D11UnorderedAccessView> colorView;
        ComPtr<ID3D11RenderTargetView> emptyView[2];
    } eyes[2];
    ComPtr<ID3D11ComputeShader> convert;
    bool owner(ID3D11DeviceContext* supplied) const noexcept {
        return context.Get() == supplied && GetCurrentThreadId() == thread;
    }
    bool texture(ComPtr<ID3D11Texture2D>& target, unsigned w, unsigned h,
                 DXGI_FORMAT format, UINT bind, bool* changed = nullptr) {
        if (changed) *changed = false;
        if (target) {
            D3D11_TEXTURE2D_DESC d{}; target->GetDesc(&d);
            if (d.Width == w && d.Height == h && d.Format == format && d.BindFlags == bind) return true;
        }
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = format; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
        ComPtr<ID3D11Texture2D> made;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &made))) return false;
        target = made; if (changed) *changed = true; return true;
    }
    bool shader() {
        if (convert) return true;
        // shader_swap has no runtime compute form. Use the existing probe
        // compiler pattern: resolve on first opt-in use and keep the module
        // resident because returned blob vtables belong to it. No PE import.
        using Compile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
            LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        static const Compile compile = []() noexcept -> Compile {
            HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
            return module ? reinterpret_cast<Compile>(GetProcAddress(module, "D3DCompile")) : nullptr;
        }();
        if (!compile) return false;
        static const char code[] =
            "Texture2D<float4> Source:register(t0);RWTexture2D<float4> Target:register(u0);"
            "[numthreads(8,8,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h;"
            "Target.GetDimensions(w,h);if(p.x<w&&p.y<h)Target[p.xy]=Source.Load(int3(p.xy,0));}";
        ComPtr<ID3DBlob> blob, errors;
        return SUCCEEDED(compile(code, sizeof(code) - 1, "EDUP_FG_color", nullptr, nullptr,
            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)) &&
            SUCCEEDED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &convert));
    }
};
FrameGenerationColor::~FrameGenerationColor() { delete impl_; }
bool FrameGenerationColor::initialize(bool enabled, ID3D11Device* device,
                                      ID3D11DeviceContext* context, DWORD thread) {
    // This is a one-time initializer, not an enable/disable control surface.
    if (impl_) { failure_ = FrameGenerationColorFailure::Owner; return false; }
    if (!enabled) { failure_ = FrameGenerationColorFailure::Off; return false; }
    if (!device || !context || !thread || thread != GetCurrentThreadId() ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !sameDevice(device, context)) {
        failure_ = FrameGenerationColorFailure::Owner; return false;
    }
    impl_ = new Impl; impl_->device = device; impl_->context = context; impl_->thread = thread;
    failure_ = FrameGenerationColorFailure::None; return true;
}
bool FrameGenerationColor::normalize(ID3D11DeviceContext* context, unsigned eye,
                                     ID3D11Texture2D* source, ID3D11Texture2D** output) {
    if (output) *output = nullptr;
    if (!impl_) { failure_ = FrameGenerationColorFailure::NotInitialized; return false; }
    if (!output || !source || eye > 1 || !impl_->owner(context) || !sameDevice(impl_->device.Get(), source)) {
        failure_ = FrameGenerationColorFailure::Owner; return false;
    }
    for (const auto& scratch : impl_->eyes) {
        if (source == scratch.color.Get() || source == scratch.bgra.Get() ||
            source == scratch.empty[0].Get() || source == scratch.empty[1].Get()) {
            failure_ = FrameGenerationColorFailure::Descriptor; return false;
        }
    }
    D3D11_TEXTURE2D_DESC d{}; source->GetDesc(&d);
    if (!fg_color::descriptor(d)) { failure_ = FrameGenerationColorFailure::Descriptor; return false; }
    const auto format = fg_color::canonical(d.Format);
    if (format == DXGI_FORMAT_UNKNOWN) { failure_ = FrameGenerationColorFailure::Format; return false; }
    // The caller immediately copies this borrowed image into immutable interop
    // storage. Exact canonical input needs no intermediate image or GPU copy.
    if (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        if (FAILED(impl_->device->GetDeviceRemovedReason())) { failure_ = FrameGenerationColorFailure::Device; return false; }
        *output = source; failure_ = FrameGenerationColorFailure::None; return true;
    }
    auto& e = impl_->eyes[eye];
    bool colorChanged = false;
    if (!impl_->texture(e.color, d.Width, d.Height, format, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &colorChanged)) {
        failure_ = FrameGenerationColorFailure::Allocation; return false;
    }
    if (colorChanged) e.colorView.Reset();
    if (!fg_color::bgra(d.Format)) context->CopyResource(e.color.Get(), source);
    else {
        bool bgraChanged = false;
        if (!impl_->texture(e.bgra, d.Width, d.Height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, &bgraChanged) || !impl_->shader()) {
            failure_ = FrameGenerationColorFailure::Allocation; return false;
        }
        if (bgraChanged) e.bgraView.Reset();
        if ((!e.bgraView && FAILED(impl_->device->CreateShaderResourceView(e.bgra.Get(), nullptr, &e.bgraView))) ||
            (!e.colorView && FAILED(impl_->device->CreateUnorderedAccessView(e.color.Get(), nullptr, &e.colorView)))) {
            failure_ = FrameGenerationColorFailure::Allocation; return false;
        }
        context->CopyResource(e.bgra.Get(), source);
        ComputeState restore(context);
        auto* srv = e.bgraView.Get(); auto* uav = e.colorView.Get(); const UINT keep = UINT(-1);
        context->CSSetShader(impl_->convert.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, &srv);
        context->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
        context->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
    }
    if (FAILED(impl_->device->GetDeviceRemovedReason())) { failure_ = FrameGenerationColorFailure::Device; return false; }
    *output = e.color.Get(); failure_ = FrameGenerationColorFailure::None; return true;
}
bool FrameGenerationColor::emptyUi(ID3D11DeviceContext* context, unsigned eye, unsigned width,
                                   unsigned height, bool multiply, bool authoritativeEmpty,
                                   ID3D11Texture2D** output) {
    if (output) *output = nullptr;
    if (!impl_) { failure_ = FrameGenerationColorFailure::NotInitialized; return false; }
    if (!authoritativeEmpty || !output || eye > 1 || !width || !height || width > 16384 || height > 16384 || !impl_->owner(context)) {
        failure_ = FrameGenerationColorFailure::Owner; return false;
    }
    auto& e = impl_->eyes[eye]; const unsigned index = multiply ? 1 : 0;
    bool changed = false;
    if (!impl_->texture(e.empty[index], width, height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &changed)) {
        failure_ = FrameGenerationColorFailure::Allocation; return false;
    }
    if (changed) e.emptyView[index].Reset();
    if (!e.emptyView[index]) {
        if (FAILED(impl_->device->CreateRenderTargetView(e.empty[index].Get(), nullptr, &e.emptyView[index]))) {
            failure_ = FrameGenerationColorFailure::Allocation; return false;
        }
        // These private identity images are read-only after initialization.
        // A missing view also covers retry after failed view creation.
        const float clear[4] = {multiply ? 1.f : 0.f, multiply ? 1.f : 0.f, multiply ? 1.f : 0.f, 1.f};
        context->ClearRenderTargetView(e.emptyView[index].Get(), clear);
    }
    if (FAILED(impl_->device->GetDeviceRemovedReason())) { failure_ = FrameGenerationColorFailure::Device; return false; }
    *output = e.empty[index].Get(); failure_ = FrameGenerationColorFailure::None; return true;
}
}

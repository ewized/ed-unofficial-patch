// The default path calls production refusal gates without loading D3D11 or
// the shader compiler. --gpu opts into WARP and the actual production adapter.
#include "../../src/d3d11/frame_generation_color.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using edvr::FrameGenerationColor;
using Failure = edvr::FrameGenerationColorFailure;

namespace {
unsigned checks = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) throw std::runtime_error(label);
}
void hr(HRESULT value, const char* label) {
    if (FAILED(value)) std::printf("HRESULT %s = 0x%08lx\n", label, static_cast<unsigned long>(value));
    check(SUCCEEDED(value), label);
}
template<class T> T* poison(std::uintptr_t value = 1) {
    return reinterpret_cast<T*>(value);
}

void cpu() {
    check(!GetModuleHandleW(L"d3d11.dll") && !GetModuleHandleW(L"d3dcompiler_47.dll"),
          "CPU entry has not loaded D3D11/compiler (link with delay imports)");
    check(!std::is_copy_constructible<FrameGenerationColor>::value &&
          !std::is_copy_assignable<FrameGenerationColor>::value, "adapter ownership cannot be copied");
    {
        FrameGenerationColor off;
        check(off.failure() == Failure::NotInitialized, "new adapter starts uninitialized");
        check(!off.initialize(false, poison<ID3D11Device>(), poison<ID3D11DeviceContext>(), 1) &&
              off.failure() == Failure::Off, "OFF does not dereference poison COM inputs");
        check(!off.initialize(false, poison<ID3D11Device>(), poison<ID3D11DeviceContext>(), 1),
              "repeated initial OFF remains side-effect free");
        ID3D11Texture2D* out = poison<ID3D11Texture2D>();
        check(!off.normalize(poison<ID3D11DeviceContext>(), 0, poison<ID3D11Texture2D>(), &out) && !out,
              "uninitialized normalization clears output without querying COM");
        out = poison<ID3D11Texture2D>();
        check(!off.emptyUi(poison<ID3D11DeviceContext>(), 0, 13, 9, false, true, &out) && !out,
              "uninitialized UI clear refuses before context access");
        check(!off.normalize(nullptr, 0, nullptr, nullptr), "null output is safely refused before initialization");
    }
    {
        FrameGenerationColor adapter;
        check(!adapter.initialize(true, nullptr, poison<ID3D11DeviceContext>(), GetCurrentThreadId()) &&
              adapter.failure() == Failure::Owner, "missing device refuses before context COM query");
        check(!adapter.initialize(true, poison<ID3D11Device>(), nullptr, GetCurrentThreadId()),
              "missing context refuses before device COM query");
        check(!adapter.initialize(true, poison<ID3D11Device>(), poison<ID3D11DeviceContext>(), 0),
              "absent owner refuses before COM query");
        DWORD other = GetCurrentThreadId() == 1 ? 2 : 1;
        check(!adapter.initialize(true, poison<ID3D11Device>(), poison<ID3D11DeviceContext>(), other),
              "foreign owner refuses before COM query");
    }
    const DXGI_FORMAT rgba[] = {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    const DXGI_FORMAT bgra[] = {DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    for (auto format : rgba) check(edvr::fg_color::canonical(format) == DXGI_FORMAT_R8G8B8A8_UNORM &&
        !edvr::fg_color::bgra(format), "RGBA variants retain encoded byte interpretation");
    for (auto format : bgra) check(edvr::fg_color::canonical(format) == DXGI_FORMAT_R8G8B8A8_UNORM &&
        edvr::fg_color::bgra(format), "BGRA variants require explicit channel conversion");
    check(edvr::fg_color::canonical(DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_FLOAT,
          "F16 remains F16");
    for (auto format : {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SNORM,
            DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
            DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R32G32B32A32_FLOAT})
        check(edvr::fg_color::canonical(format) == DXGI_FORMAT_UNKNOWN && !edvr::fg_color::bgra(format),
              "unsupported formats are not silently reinterpreted");
    D3D11_TEXTURE2D_DESC good{};
    good.Width = good.Height = 3200;
    good.MipLevels = good.ArraySize = good.SampleDesc.Count = 1;
    good.Usage = D3D11_USAGE_DEFAULT;
    check(edvr::fg_color::descriptor(good), "full-eye descriptor admitted");
    for (unsigned kind = 0; kind < 11; ++kind) {
        auto bad = good;
        switch (kind) {
        case 0: bad.Width = 0; break;
        case 1: bad.Height = 0; break;
        case 2: bad.Width = 16385; break;
        case 3: bad.Height = 16385; break;
        case 4: bad.MipLevels = 2; break;
        case 5: bad.ArraySize = 2; break;
        case 6: bad.SampleDesc.Count = 2; break;
        case 7: bad.SampleDesc.Quality = 1; break;
        case 8: bad.Usage = D3D11_USAGE_DYNAMIC; break;
        case 9: bad.Usage = D3D11_USAGE_STAGING; break;
        case 10: bad.CPUAccessFlags = D3D11_CPU_ACCESS_READ; break;
        }
        check(!edvr::fg_color::descriptor(bad), "unsafe descriptor falsifier refused");
    }
    good.Width = good.Height = 16384;
    check(edvr::fg_color::descriptor(good), "maximum D3D11 dimension admitted");
    check(!GetModuleHandleW(L"d3d11.dll") && !GetModuleHandleW(L"d3dcompiler_47.dll"),
          "CPU OFF/refusal path loaded neither D3D11 nor shader compiler");
}

// Observe the real production GPU calls on this private fixture context.
// The per-object table is restored before its storage leaves scope.
struct ColorCommandSpy {
    using Copy = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
    using Clear = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
    inline static ColorCommandSpy* active = nullptr;
    ID3D11DeviceContext* context;
    void** original;
    std::array<void*, 115> table{};
    unsigned copies = 0, clears = 0;
    static void STDMETHODCALLTYPE copy(ID3D11DeviceContext* c, ID3D11Resource* target, ID3D11Resource* source) {
        ++active->copies;
        reinterpret_cast<Copy>(active->original[47])(c, target, source);
    }
    static void STDMETHODCALLTYPE clear(ID3D11DeviceContext* c, ID3D11RenderTargetView* view, const FLOAT* color) {
        ++active->clears;
        reinterpret_cast<Clear>(active->original[50])(c, view, color);
    }
    void install(void** replacement) {
        DWORD previous = 0, ignored = 0;
        check(VirtualProtect(context, sizeof(void*), PAGE_READWRITE, &previous) != 0, "color spy context writable");
        *reinterpret_cast<void***>(context) = replacement;
        check(VirtualProtect(context, sizeof(void*), previous, &ignored) != 0, "color spy context protection restored");
    }
    explicit ColorCommandSpy(ID3D11DeviceContext* c) : context(c), original(*reinterpret_cast<void***>(c)) {
        check(!active, "one color command spy owner");
        std::memcpy(table.data(), original, sizeof(table));
        table[47] = reinterpret_cast<void*>(&copy); table[50] = reinterpret_cast<void*>(&clear);
        active = this; install(table.data());
    }
    ~ColorCommandSpy() { install(original); active = nullptr; }
};

// Fail one real view creation, leaving allocation successful. A subsequent
// call must initialize the retained texture instead of publishing undefined pixels.
struct ViewFailure {
    using Create = HRESULT (STDMETHODCALLTYPE*)(ID3D11Device*, ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**);
    inline static ViewFailure* active = nullptr;
    ID3D11Device* device;
    void** slot;
    Create original;
    bool fail = true;
    static HRESULT STDMETHODCALLTYPE create(ID3D11Device* d, ID3D11Resource* resource,
        const D3D11_RENDER_TARGET_VIEW_DESC* desc, ID3D11RenderTargetView** output) {
        if (d == active->device && active->fail) {
            active->fail = false; if (output) *output = nullptr; return E_OUTOFMEMORY;
        }
        return active->original(d, resource, desc, output);
    }
    void install(void* replacement) {
        DWORD previous = 0, ignored = 0;
        check(VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous) != 0, "view failure slot writable");
        *slot = replacement;
        check(VirtualProtect(slot, sizeof(void*), previous, &ignored) != 0, "view failure protection restored");
    }
    explicit ViewFailure(ID3D11Device* d) : device(d), slot(*reinterpret_cast<void***>(d) + 9),
        original(reinterpret_cast<Create>(*slot)) {
        check(!active, "one view failure owner");
        // Keep the runtime's complete table: CreateTexture2D can use entries
        // beyond the public base interface while this failure is installed.
        // Other devices sharing the table always forward to the saved method.
        active = this; install(reinterpret_cast<void*>(&create));
    }
    ~ViewFailure() { install(reinterpret_cast<void*>(original)); active = nullptr; }
};

struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> debug;
    Device() {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;
        auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 1,
            D3D11_SDK_VERSION, &device, nullptr, &context);
        if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 1,
                D3D11_SDK_VERSION, &device, nullptr, &context);
        }
        hr(result, "WARP FL11 device");
        device.As(&debug);
        if (!debug) std::puts("SKIP D3D11 debug queue: SDK debug layer unavailable");
    }
    void noErrors() {
        if (!debug) return;
        const auto count = debug->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T bytes = 0;
            hr(debug->GetMessage(i, nullptr, &bytes), "debug message size");
            std::vector<unsigned char> memory(bytes);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(memory.data());
            hr(debug->GetMessage(i, message, &bytes), "debug message");
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                std::printf("D3D11 error: %s\n", message->pDescription);
                check(false, "no D3D11 validation errors from production adapter");
            }
        }
        check(true, "no D3D11 validation errors from production adapter");
    }
};

unsigned pixelBytes(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
}
ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, DXGI_FORMAT format, unsigned width,
    unsigned height, UINT bind, const std::vector<unsigned char>* pixels = nullptr) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA data{};
    if (pixels) { data.pSysMem = pixels->data(); data.SysMemPitch = width * pixelBytes(format); }
    ComPtr<ID3D11Texture2D> made;
    hr(device->CreateTexture2D(&desc, pixels ? &data : nullptr, &made), "fixture texture");
    return made;
}
std::vector<unsigned char> read(Device& device, ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    const auto rowBytes = desc.Width * pixelBytes(desc.Format);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    hr(device.device->CreateTexture2D(&desc, nullptr, &staging), "readback texture");
    device.context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr(device.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "readback map");
    std::vector<unsigned char> pixels(static_cast<size_t>(rowBytes) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(pixels.data() + static_cast<size_t>(rowBytes) * y,
            static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(mapped.RowPitch) * y, rowBytes);
    device.context->Unmap(staging.Get(), 0);
    return pixels;
}
std::vector<unsigned char> pattern(DXGI_FORMAT format, unsigned width, unsigned height) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * pixelBytes(format));
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        // Include signed, HDR, subnormal and finite extreme values; F16 is a
        // bit-compatible copy, with no float sampling or SDR clamp involved.
        const std::uint16_t values[] = {0x0000, 0x3c00, 0x3800, 0x4000, 0xbc00, 0x0001, 0x7bff, 0x3555};
        for (size_t i = 0; i < pixels.size() / 2; ++i) {
            const auto value = values[i % 8];
            std::memcpy(pixels.data() + i * 2, &value, sizeof(value));
        }
    } else {
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = static_cast<unsigned char>((i * 73 + (i / 4) * 19 + 7) & 255);
    }
    return pixels;
}

struct ComputeSnapshot {
    ComPtr<ID3D11ComputeShader> shader;
    std::array<ComPtr<ID3D11ClassInstance>, D3D11_SHADER_MAX_INTERFACES> classes;
    UINT count = D3D11_SHADER_MAX_INTERFACES;
    std::array<ComPtr<ID3D11ShaderResourceView>, 2> srvs;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 2> uavs;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11ShaderResourceView> pixelSource;
    explicit ComputeSnapshot(ID3D11DeviceContext* context) {
        ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
        context->CSGetShader(&shader, instances, &count);
        for (UINT i = 0; i < count; ++i) classes[i].Attach(instances[i]);
        ID3D11ShaderResourceView* rawSrvs[2]{};
        ID3D11UnorderedAccessView* rawUavs[2]{};
        context->CSGetShaderResources(0, 2, rawSrvs);
        context->CSGetUnorderedAccessViews(0, 2, rawUavs);
        for (unsigned i = 0; i < 2; ++i) { srvs[i].Attach(rawSrvs[i]); uavs[i].Attach(rawUavs[i]); }
        context->CSGetConstantBuffers(0, 1, &constants);
        context->CSGetSamplers(0, 1, &sampler);
        context->PSGetShaderResources(0, 1, &pixelSource);
    }
    void equal(const ComputeSnapshot& other) const {
        check(shader == other.shader && count == other.count, "production adapter restores CS and interface count");
        for (UINT i = 0; i < count; ++i)
            check(classes[i] == other.classes[i], "production adapter restores exact dynamic class instances");
        for (unsigned i = 0; i < 2; ++i)
            check(srvs[i] == other.srvs[i] && uavs[i] == other.uavs[i], "CS source/UAV sentinels survive conversion");
        check(constants == other.constants && sampler == other.sampler && pixelSource == other.pixelSource,
              "untouched CB/sampler/pixel-stage sentinels survive conversion");
    }
};

struct Sentinels {
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11ClassLinkage> linkage;
    ComPtr<ID3D11ClassInstance> instance;
    ComPtr<ID3D11Texture2D> input[2], output[2];
    ComPtr<ID3D11ShaderResourceView> srvs[2];
    ComPtr<ID3D11UnorderedAccessView> uavs[2];
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    explicit Sentinels(Device& device) {
        const char code[] = "[numthreads(1,1,1)]void main(uint3 p:SV_DispatchThreadID){}";
        ComPtr<ID3DBlob> blob, errors;
        hr(D3DCompile(code, sizeof(code) - 1, "FG_sentinel", nullptr, nullptr, "main", "cs_5_0", 0, 0,
            &blob, &errors), "sentinel shader compile");
        hr(device.device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader),
            "sentinel shader");
        for (unsigned i = 0; i < 2; ++i) {
            input[i] = texture(device.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 13, 9, D3D11_BIND_SHADER_RESOURCE);
            output[i] = texture(device.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 13, 9, D3D11_BIND_UNORDERED_ACCESS);
            hr(device.device->CreateShaderResourceView(input[i].Get(), nullptr, &srvs[i]), "sentinel SRV");
            hr(device.device->CreateUnorderedAccessView(output[i].Get(), nullptr, &uavs[i]), "sentinel UAV");
        }
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 16; cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr(device.device->CreateBuffer(&cb, nullptr, &constants), "sentinel constant buffer");
        D3D11_SAMPLER_DESC sample{}; sample.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxLOD = D3D11_FLOAT32_MAX;
        hr(device.device->CreateSamplerState(&sample, &sampler), "sentinel sampler");
    }
    void bind(Device& device) {
        auto* classInstance = instance.Get();
        device.context->CSSetShader(shader.Get(), instance ? &classInstance : nullptr, instance ? 1 : 0);
        ID3D11ShaderResourceView* sources[] = {srvs[0].Get(), srvs[1].Get()};
        ID3D11UnorderedAccessView* targets[] = {uavs[0].Get(), uavs[1].Get()};
        const UINT keep[] = {UINT(-1), UINT(-1)};
        device.context->CSSetShaderResources(0, 2, sources);
        device.context->CSSetUnorderedAccessViews(0, 2, targets, keep);
        auto* cb = constants.Get(); auto* sample = sampler.Get();
        device.context->CSSetConstantBuffers(0, 1, &cb);
        device.context->CSSetSamplers(0, 1, &sample);
        device.context->PSSetShaderResources(0, 1, sources);
    }
    void dynamicClasses(Device& device) {
        const char code[] =
            "interface IColor{float4 apply(float4 value);};"
            "class Offset:IColor{float4 bias;float4 apply(float4 value){return value+bias;}};"
            "Offset baseColor;IColor selected;RWTexture2D<float4> target:register(u1);"
            "[numthreads(1,1,1)]void main(uint3 p:SV_DispatchThreadID){"
            "target[p.xy]=selected.apply(float4(p.xyz,1))+baseColor.apply(float4(0,0,0,0));}";
        ComPtr<ID3DBlob> blob, errors;
        hr(D3DCompile(code, sizeof(code) - 1, "FG_dynamic_sentinel", nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_SKIP_OPTIMIZATION, 0, &blob, &errors), "dynamic sentinel compile");
        hr(device.device->CreateClassLinkage(&linkage), "sentinel class linkage");
        hr(device.device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), linkage.Get(), &shader),
            "dynamic sentinel shader");
        hr(linkage->GetClassInstance("baseColor", 0, &instance), "named sentinel dynamic class instance");
        std::puts("dynamic_class_fixture=enabled");
    }
};

void pixels(Device& device, FrameGenerationColor& adapter, Sentinels& sentinels) {
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_TYPELESS,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_FLOAT};
    for (auto format : formats) for (unsigned eye = 0; eye < 2; ++eye) {
        std::printf("color_pixels format=%u eye=%u phase=begin\n", unsigned(format), eye);
        // Non-multiples of the 8x8 conversion group exercise edge coverage.
        const unsigned width = 13, height = 9;
        const bool borrowed = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        const auto original = pattern(format, width, height);
        auto expected = original;
        if (edvr::fg_color::bgra(format))
            for (size_t i = 0; i < expected.size(); i += 4) std::swap(expected[i], expected[i + 2]);
        auto source = texture(device.device.Get(), format, width, height, 0, &original);
        sentinels.bind(device);
        ComputeSnapshot before(device.context.Get());
        ID3D11Texture2D* output = nullptr;
        {
            ColorCommandSpy calls(device.context.Get());
            std::puts("color_pixels phase=spy-installed");
            check(adapter.normalize(device.context.Get(), eye, source.Get(), &output) && output,
                  "production adapter prepares a color image");
            std::printf("color_pixels phase=normalized copies=%u\n", calls.copies);
            check(calls.copies == (borrowed ? 0u : 1u),
                  "compatible RGBA8 skips normalization copy; fallback preserves one copy");
        }
        std::puts("color_pixels phase=spy-restored");
        check((output == source.Get()) == borrowed,
              "only UNORM and typeless RGBA8 borrow the original source");
        before.equal(ComputeSnapshot(device.context.Get()));
        D3D11_TEXTURE2D_DESC desc{}; output->GetDesc(&desc);
        check(desc.Width == width && desc.Height == height && desc.Format == (borrowed ? format : edvr::fg_color::canonical(format)),
              "prepared format is borrowed input or converted canonical output");
        check(read(device, output) == expected, "production conversion preserves every encoded channel or F16 bit");
        check(read(device, source.Get()) == original, "production conversion leaves source image unchanged");
        auto* firstOutput = output;
        check(adapter.normalize(device.context.Get(), eye, source.Get(), &output) && output == firstOutput,
              "same eye and descriptor reuses the canonical source or private output");
        before.equal(ComputeSnapshot(device.context.Get()));
    }
    auto original = pattern(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 7, 3);
    auto source = texture(device.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 7, 3, 0, &original);
    ID3D11Texture2D* output = nullptr;
    check(adapter.normalize(device.context.Get(), 0, source.Get(), &output) && read(device, output) == original,
          "descriptor change recreates correctly sized output without stale conversion views");
    sentinels.dynamicClasses(device);
    sentinels.bind(device);
    ComputeSnapshot before(device.context.Get());
    check(before.shader == sentinels.shader && before.count == 1 && before.classes[0] == sentinels.instance,
          "dynamic-class sentinel is truly bound before production conversion");
    auto bgraPixels = pattern(DXGI_FORMAT_B8G8R8A8_UNORM, 13, 9);
    auto bgra = texture(device.device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 13, 9, 0, &bgraPixels);
    check(adapter.normalize(device.context.Get(), 0, bgra.Get(), &output), "BGRA conversion with dynamic CS linkage bound");
    before.equal(ComputeSnapshot(device.context.Get()));
}

void refusals(Device& device, FrameGenerationColor& adapter) {
    auto source = texture(device.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 13, 9, 0);
    ID3D11Texture2D* output = poison<ID3D11Texture2D>();
    check(!adapter.normalize(device.context.Get(), 2, poison<ID3D11Texture2D>(), &output) && !output &&
        adapter.failure() == Failure::Owner, "invalid eye refuses before touching source");
    check(!adapter.normalize(device.context.Get(), 0, source.Get(), nullptr), "missing output refuses safely");
    check(!adapter.normalize(device.context.Get(), 0, nullptr, &output) && !output, "missing source refuses safely");
    bool accepted = true;
    std::thread foreign([&] {
        ID3D11Texture2D* denied = poison<ID3D11Texture2D>();
        accepted = adapter.normalize(device.context.Get(), 0, source.Get(), &denied) || denied;
    });
    foreign.join();
    check(!accepted && adapter.failure() == Failure::Owner, "foreign thread cannot enter immediate-context conversion");
    ComPtr<ID3D11DeviceContext> deferred;
    hr(device.device->CreateDeferredContext(0, &deferred), "deferred owner falsifier");
    FrameGenerationColor invalid;
    check(!invalid.initialize(true, device.device.Get(), deferred.Get(), GetCurrentThreadId()),
          "deferred context cannot initialize adapter");
    check(!adapter.normalize(deferred.Get(), 0, source.Get(), &output) && !output,
          "deferred/foreign context cannot use initialized adapter");
    Device another;
    auto foreignSource = texture(another.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 13, 9, 0);
    check(!adapter.normalize(device.context.Get(), 0, foreignSource.Get(), &output) && !output,
          "foreign-device source cannot enter copy/conversion");
    check(!adapter.normalize(another.context.Get(), 0, source.Get(), &output) && !output,
          "foreign immediate context cannot enter copy/conversion");
    auto unsupported = texture(device.device.Get(), DXGI_FORMAT_R32_FLOAT, 13, 9, 0);
    check(!adapter.normalize(device.context.Get(), 0, unsupported.Get(), &output) && !output &&
          adapter.failure() == Failure::Format, "actual unsupported source refuses before allocation/copy");
    auto desc = D3D11_TEXTURE2D_DESC{};
    source->GetDesc(&desc); desc.ArraySize = 2;
    ComPtr<ID3D11Texture2D> array;
    hr(device.device->CreateTexture2D(&desc, nullptr, &array), "array descriptor falsifier");
    check(!adapter.normalize(device.context.Get(), 0, array.Get(), &output) && !output &&
          adapter.failure() == Failure::Descriptor, "actual array source is refused");
    check(!adapter.initialize(false, poison<ID3D11Device>(), poison<ID3D11DeviceContext>(), 0) &&
          adapter.failure() == Failure::Owner, "reinitializing live adapter OFF cannot alter its ownership");
    check(adapter.normalize(device.context.Get(), 0, source.Get(), &output), "live adapter remains usable after rejected reinitialization");
    auto scratchSource = texture(device.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 13, 9, 0);
    check(adapter.normalize(device.context.Get(), 0, scratchSource.Get(), &output), "noncanonical input obtains private scratch");
    ID3D11Texture2D* ownColor = output;
    check(!adapter.normalize(device.context.Get(), 0, ownColor, &output) && !output,
          "same-eye output cannot alias a normalization source");
    check(!adapter.normalize(device.context.Get(), 1, ownColor, &output) && !output,
          "opposite-eye adapter scratch cannot become an input");
    another.noErrors();
}

void emptyUi(Device& device, FrameGenerationColor& adapter) {
    for (unsigned eye = 0; eye < 2; ++eye) for (bool multiply : {false, true}) {
        ID3D11Texture2D* output = poison<ID3D11Texture2D>();
        ComputeSnapshot before(device.context.Get());
        check(!adapter.emptyUi(device.context.Get(), eye, 13, 9, multiply, false, &output) && !output,
              "missing authoritative empty-UI proof cannot invent an identity layer");
        before.equal(ComputeSnapshot(device.context.Get()));
        check(adapter.emptyUi(device.context.Get(), eye, 13, 9, multiply, true, &output) && output,
              "authoritative empty UI produces a private identity plane");
        before.equal(ComputeSnapshot(device.context.Get()));
        std::vector<unsigned char> expected(13 * 9 * 4, multiply ? 255 : 0);
        for (size_t i = 3; i < expected.size(); i += 4) expected[i] = 255;
        check(read(device, output) == expected, "empty UI is exact RGB0/transmittance1 or multiply RGB1");
        auto* first = output;
        check(adapter.emptyUi(device.context.Get(), eye, 13, 9, multiply, true, &output) && output == first,
              "unchanged empty UI reuses private plane");
        check(!adapter.normalize(device.context.Get(), eye, first, &output) && !output,
              "empty UI scratch cannot alias a color normalization input");
        check(read(device, first) == expected, "alias refusal leaves identity source unchanged");
    }
    ID3D11Texture2D* output = poison<ID3D11Texture2D>();
    check(!adapter.emptyUi(device.context.Get(), 0, 0, 9, false, true, &output) && !output,
          "zero-sized empty UI refused");
    check(!adapter.emptyUi(device.context.Get(), 0, 16385, 9, false, true, &output) && !output,
          "oversized empty UI refused");
    check(!adapter.emptyUi(device.context.Get(), 2, 13, 9, false, true, &output) && !output,
          "invalid-eye empty UI refused");
}

// Alternate both identities and eyes through reuse, resize, and failed view
// creation. Pixels and observed clear calls qualify each cache transition.
void emptyUiReuse(Device& device) {
    FrameGenerationColor adapter;
    check(adapter.initialize(true, device.device.Get(), device.context.Get(), GetCurrentThreadId()), "empty UI cache owner");
    ColorCommandSpy calls(device.context.Get());
    std::array<ComPtr<ID3D11Texture2D>, 4> originals;
    for (unsigned pass = 0; pass < 3; ++pass) for (unsigned eye = 0; eye < 2; ++eye) for (bool multiply : {false, true}) {
        const unsigned index = eye * 2 + unsigned(multiply);
        ID3D11Texture2D* output = nullptr;
        const auto previous = calls.clears;
        check(adapter.emptyUi(device.context.Get(), eye, 13, 9, multiply, true, &output), "cached empty identity admitted");
        check(calls.clears - previous == (pass == 0 ? 1u : 0u), "empty UI clears once per eye and identity, never on reuse");
        if (!pass) originals[index] = output;
        check(output == originals[index].Get(), "alternating empty identities retain their own immutable image");
        std::vector<unsigned char> expected(13 * 9 * 4, multiply ? 255 : 0);
        for (size_t i = 3; i < expected.size(); i += 4) expected[i] = 255;
        check(read(device, output) == expected, "reused empty identity preserves every pixel");
    }
    for (unsigned eye = 0; eye < 2; ++eye) for (bool multiply : {false, true}) {
        std::printf("empty_ui_cache eye=%u multiply=%u phase=resize\n", eye, unsigned(multiply));
        ID3D11Texture2D* output = poison<ID3D11Texture2D>();
        const auto previous = calls.clears;
        {
            ViewFailure failure(device.device.Get());
            std::puts("empty_ui_cache phase=failure-spy-installed");
            check(!adapter.emptyUi(device.context.Get(), eye, 7, 3, multiply, true, &output) && !output &&
                  adapter.failure() == Failure::Allocation, "failed resized view creation exposes no uninitialized plane");
            check(calls.clears == previous, "failed view creation does not issue a clear");
            std::puts("empty_ui_cache phase=view-failed-retry");
            check(adapter.emptyUi(device.context.Get(), eye, 7, 3, multiply, true, &output) && output,
                  "retry initializes allocated empty plane after view creation failure");
        }
        check(calls.clears == previous + 1, "resized identity clears exactly once after successful retry");
        check(output != originals[eye * 2 + unsigned(multiply)].Get(), "resize replaces only selected cached identity");
        std::vector<unsigned char> expected(7 * 3 * 4, multiply ? 255 : 0);
        for (size_t i = 3; i < expected.size(); i += 4) expected[i] = 255;
        check(read(device, output) == expected, "resized identity has correct initialized pixels");
        check(adapter.emptyUi(device.context.Get(), eye, 7, 3, multiply, true, &output) && calls.clears == previous + 1,
              "resized identity remains read-only on reuse");
        std::vector<unsigned char> oldExpected(13 * 9 * 4, multiply ? 255 : 0);
        for (size_t i = 3; i < oldExpected.size(); i += 4) oldExpected[i] = 255;
        check(read(device, originals[eye * 2 + unsigned(multiply)].Get()) == oldExpected,
              "retained former identity survives resize and failure unchanged");
    }
    std::printf("empty_ui_cache initialized_planes=8 production_clears=%u repeated_clears=0 failed_views=4\n", calls.clears);
}

void gpu(bool emptyOnly) {
    Device device;
    if (emptyOnly) { emptyUiReuse(device); device.noErrors(); return; }
    {
        FrameGenerationColor adapter;
        check(adapter.initialize(true, device.device.Get(), device.context.Get(), GetCurrentThreadId()),
              "production adapter accepts authoritative immediate owner");
        Sentinels sentinels(device);
        std::puts("color_gpu phase=pixels");
        pixels(device, adapter, sentinels);
        std::puts("color_gpu phase=refusals");
        refusals(device, adapter);
        std::puts("color_gpu phase=empty-ui");
        emptyUi(device, adapter);
        std::puts("color_gpu phase=empty-ui-reuse");
        emptyUiReuse(device);
        std::puts("color_gpu phase=cleanup");
        device.context->ClearState();
        device.context->Flush();
        device.noErrors();
    }
    check(SUCCEEDED(device.device->GetDeviceRemovedReason()), "WARP device remains healthy");
}
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool runGpu = false, emptyOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--gpu")) runGpu = true;
        else if (!std::strcmp(argv[i], "--gpu-empty-ui")) { runGpu = true; emptyOnly = true; }
        else if (std::strcmp(argv[i], "--self-test")) {
            std::fprintf(stderr, "Usage: frame_generation_color_test [--self-test] [--gpu | --gpu-empty-ui]\n");
            return 2;
        }
    }
    try {
        cpu();
        if (runGpu) gpu(emptyOnly);
        std::printf("PASS FG production color adapter checks=%u mode=%s game_calls=0 ngx_calls=0\n",
            checks, runGpu ? "WARP" : "CPU-no-D3D11-or-compiler-load");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL FG production color adapter after checks=%u: %s\n", checks, error.what());
        return 1;
    }
}

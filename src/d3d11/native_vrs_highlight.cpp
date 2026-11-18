#include "native_vrs.h"
#include "native_vrs_policy.h"
#include "shader_swap.h"
#include "temporal_shader_bytecode.h"
#include "vscreen.h"
#include "ui_layer_shaders.h"
#include "../common/guard.h"
#include "../common/log.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace edvr {
namespace {
using Microsoft::WRL::ComPtr;
static_assert(native_vrs::kTileSize == 16, "The highlight shader addresses 16-pixel VRS tiles.");

struct Geometry {
    uint32_t outputSize[2], inputSize[2];
    float renderedJitter[2], padding[2];
};
static_assert(sizeof(Geometry) == 32, "VRS highlight shader constant layout.");

struct Eye {
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11UnorderedAccessView> target;
    ComPtr<ID3D11ShaderResourceView> world, map;
    ID3D11Texture2D* worldIdentity = nullptr;
    ID3D11Texture2D* mapIdentity = nullptr;
    bool noted = false;
};
struct Highlight {
    ID3D11Device* device = nullptr;
    ComPtr<ID3D11DeviceContext> recorder;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> geometry;
    Eye eyes[2];
    bool attempted = false;
};
// Only the VRS owner uses this cache. Lazy construction avoids any allocation
// while the highlight is off. The cache has no COM destructor under DllMain.
thread_local Highlight* cache = nullptr;

bool prepare(Highlight& state, ID3D11Device* device, ID3D11DeviceContext* playback) {
    if (state.device != device) {
        state = {};
        state.device = device;
    }
    if (!state.attempted) {
        state.attempted = true;
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(Geometry);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateDeferredContext(0, &state.recorder)) ||
            FAILED(device->CreateBuffer(&desc, nullptr, &state.geometry))) return false;
        state.shader.Attach(shaderSwapCreateCs(playback, kNativeVrsHighlightBytecode,
            sizeof(kNativeVrsHighlightBytecode), "native_vrs_highlight_cs", "VRS zone highlight"));
    }
    return state.recorder && state.geometry && state.shader;
}

// Cache the views by resource identity. ComPtr views retain their backing
// textures, so pointer reuse cannot make an old cached view look current.
bool prepareEye(Eye& eye, ID3D11Device* device, ID3D11Texture2D* frame,
                ID3D11Texture2D* rateMap, D3D11_TEXTURE2D_DESC desc, DXGI_FORMAT format) {
    if (!eye.world || eye.worldIdentity != frame) {
        if (eye.output) {
            D3D11_TEXTURE2D_DESC old{};
            eye.output->GetDesc(&old);
            if (old.Width != desc.Width || old.Height != desc.Height || old.Format != desc.Format) {
                eye.target.Reset();
                eye.output.Reset();
            }
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(frame, &view, eye.world.ReleaseAndGetAddressOf())))
            return false;
        eye.worldIdentity = frame;
    }
    if (!eye.map || eye.mapIdentity != rateMap) {
        if (FAILED(device->CreateShaderResourceView(rateMap, nullptr, eye.map.ReleaseAndGetAddressOf())))
            return false;
        eye.mapIdentity = rateMap;
    }
    if (!eye.output) {
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = 0;
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11Texture2D> output;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &output)) ||
            FAILED(device->CreateUnorderedAccessView(output.Get(), &view, &eye.target))) return false;
        eye.output = output;
    }
    return true;
}
}

ID3D11Texture2D* nativeVrsHighlightEye(uint64_t sequence,unsigned eye,
                                      ID3D11Texture2D* frame,const uint32_t region[4]) noexcept {
    if (!nativeVrsHighlightRequested()) {
        if (cache) *cache = {};
        return nullptr;
    }
    try {
        ID3D11Texture2D* output = nullptr;
        guarded("nativeVrs/highlight", [&] {
            Geometry geometry{};
            if (!nativeTemporalVrsInput(sequence, eye, geometry.inputSize[0], geometry.inputSize[1],
                                        geometry.renderedJitter)) return;
            D3D11_TEXTURE2D_DESC desc{};
            frame->GetDesc(&desc);
            const auto format = uiLayerFrameView(desc.Format);
            if (format == DXGI_FORMAT_UNKNOWN || !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
                region[0] || region[1] || desc.Width != region[2] || desc.Height != region[3]) return;
            geometry.outputSize[0] = desc.Width;
            geometry.outputSize[1] = desc.Height;
            ComPtr<ID3D11Device> device;
            frame->GetDevice(&device);
            ID3D11DeviceContext* playback = nullptr;
            ID3D11Texture2D* map = nullptr;
            if (!nativeVrsHighlightMap(device.Get(), sequence, eye, geometry.inputSize[0], geometry.inputSize[1],
                                       playback, map)) return;
            if (!cache) cache = new Highlight;
            auto& state = *cache;
            if (!prepare(state, device.Get(), playback)) return;
            auto& target = state.eyes[eye];
            if (!prepareEye(target, device.Get(), frame, map, desc, format)) return;

            // Record on a private context. Playback restores the game's D3D
            // state and the raw playback path owns the VRS reset boundary.
            auto* recorder = state.recorder.Get();
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(recorder->Map(state.geometry.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
            *static_cast<Geometry*>(mapped.pData) = geometry;
            recorder->Unmap(state.geometry.Get(), 0);
            ID3D11Buffer* constants = state.geometry.Get();
            ID3D11ShaderResourceView* resources[] = {target.world.Get(), target.map.Get()};
            ID3D11UnorderedAccessView* destination = target.target.Get();
            recorder->CSSetShader(state.shader.Get(), nullptr, 0);
            recorder->CSSetConstantBuffers(0, 1, &constants);
            recorder->CSSetShaderResources(0, 2, resources);
            recorder->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
            recorder->Dispatch((desc.Width + 15) / 16, (desc.Height + 15) / 16, 1);
            ComPtr<ID3D11CommandList> commands;
            if (FAILED(recorder->FinishCommandList(FALSE, &commands))) {
                state.recorder.Reset();
                return;
            }
            vScreenExecuteCommandListRaw(playback, commands.Get(), 1);
            if (!target.noted) {
                target.noted = true;
                Log::get().note("VRS zone highlight: eye %u, input %ux%u, output %ux%u, blue reduced-rate tiles.",
                    eye, geometry.inputSize[0], geometry.inputSize[1], desc.Width, desc.Height);
            }
            output = target.output.Get();
            output->AddRef();
        });
        return output;
    } catch (...) {
        return nullptr;
    }
}
}

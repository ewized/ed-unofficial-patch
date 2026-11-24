#pragma once
#include <d3d11.h>
#include <cstdint>

namespace edvr {
enum class FrameGenerationColorFailure : uint32_t {
    None, Off, NotInitialized, Owner, Descriptor, Format, Device, Allocation, Shader
};
namespace fg_color {
inline DXGI_FORMAT canonical(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return format;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}
inline bool bgra(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_B8G8R8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
inline bool descriptor(const D3D11_TEXTURE2D_DESC& desc) noexcept {
    return desc.Width && desc.Height && desc.Width <= 16384 && desc.Height <= 16384 &&
        desc.MipLevels == 1 && desc.ArraySize == 1 && desc.SampleDesc.Count == 1 &&
        desc.SampleDesc.Quality == 0 && desc.Usage == D3D11_USAGE_DEFAULT && !desc.CPUAccessFlags;
}
}
// SDR formats preserve encoded color values. No sharpening, transfer function,
// interpolation, global settings or calls to the vendor API are involved.
class FrameGenerationColor {
public:
    FrameGenerationColor() noexcept = default;
    ~FrameGenerationColor();
    FrameGenerationColor(const FrameGenerationColor&) = delete;
    FrameGenerationColor& operator=(const FrameGenerationColor&) = delete;
    // OFF has no allocation, COM, compiler or GPU side effects.
    bool initialize(bool enabled, ID3D11Device*, ID3D11DeviceContext*, DWORD ownerThread);
    // Owner thread only. RGBA8 UNORM/TYPELESS return the borrowed source;
    // interop capture supplies the canonical typed UNORM image. Other formats
    // return private scratch valid until the next operation for that eye.
    // Immediately enqueue the immutable interop copy before source/scratch reuse.
    bool normalize(ID3D11DeviceContext*, unsigned eye, ID3D11Texture2D*, ID3D11Texture2D** output);
    // The coordinator must first prove an authoritative empty UI snapshot.
    // This produces either RGB0/transmittance1 or multiply identity RGB1.
    // Borrowed planes are read-only and initialized once per eye/size/identity;
    // consumers may copy them but must never bind them for writing.
    bool emptyUi(ID3D11DeviceContext*, unsigned eye, unsigned width, unsigned height,
                 bool multiply, bool authoritativeEmpty, ID3D11Texture2D** output);
    FrameGenerationColorFailure failure() const noexcept { return failure_; }
private:
    struct Impl;
    Impl* impl_ = nullptr;
    FrameGenerationColorFailure failure_ = FrameGenerationColorFailure::NotInitialized;
};
}

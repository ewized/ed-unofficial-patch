#pragma once
#include <d3d11.h>
#include "native_vrs_material.h"

namespace edvr::native_vrs {

inline constexpr GUID kMaterialPsReceipt{0x028950d5,0x4715,0x4f43,{0xb5,0xa2,0x63,0xdf,0xba,0xb4,0x41,0xf0}};

inline void rememberMaterialPs(ID3D11PixelShader* shader, uint64_t hash, const void* bytes, size_t size) noexcept {
    if (shader && hash && compatibleScenePixelShader(bytes, size)) shader->SetPrivateData(kMaterialPsReceipt, sizeof(hash), &hash);
}

inline bool hasMaterialPsReceipt(ID3D11PixelShader* shader, uint64_t expected) noexcept {
    uint64_t hash = 0;
    UINT size = sizeof(hash);
    return shader && expected && SUCCEEDED(shader->GetPrivateData(kMaterialPsReceipt, &size, &hash)) && size == sizeof(hash) && hash == expected;
}

} // namespace edvr::native_vrs

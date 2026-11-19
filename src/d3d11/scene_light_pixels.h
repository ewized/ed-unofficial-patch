#pragma once
#include <cstdint>

namespace edvr {
// Flight 32: the sun quad and instanced light volumes, isolated one PS at a time
// by the concourse light test. A light volume may be a large indexed mesh.
inline constexpr uint64_t kSceneLightingPixels[] = {
    0x7CECABDE34FFBE9Eull, 0xC6E6E419DA9F6FADull, 0x50364C9D994141D5ull, 0x3D8442D2FC1DCADDull,
    0xAC3CE66FBAF0FA9Aull, 0x81812EF97FB4A361ull, 0x70E6FCA6CF692D2Aull, 0x2DB678B6B558B604ull,
    0xB98B140FC83B4448ull, 0x1F64463B15189104ull, 0x37649F54AD53319Cull};

inline bool sceneLightingPixelShader(uint64_t ps) noexcept {
    for (auto light : kSceneLightingPixels) if (ps == light) return true;
    return false;
}
}

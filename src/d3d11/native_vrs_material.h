#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace edvr::native_vrs {

// Read-only subset of the retail SM5 DXBC format in dxbc_container.h. This
// runs on successful game PS creation, including when VRS is toggled on later.
// Keep only the receipt, not the bytecode; unknown chunk layouts fail closed.
inline bool materialPixelShader(const void* bytecode, size_t size) noexcept {
    if (!bytecode || size < 32 || size > 1024 * 1024) return false;
    const auto* bytes = static_cast<const uint8_t*>(bytecode);
    const auto word = [&](size_t at) { uint32_t value; std::memcpy(&value, bytes + at, 4); return value; };
    const uint32_t chunks = word(28);
    if (std::memcmp(bytes, "DXBC", 4) || word(24) != size || chunks > 128 || chunks > (size - 32) / 4) return false;
    bool normal = false, tangent = false, surfaceUv = false, terrainUv = false, lightingPosition = false;
    bool input = false, output = false, program = false, color = false;
    for (uint32_t index = 0; index < chunks; ++index) {
        const size_t at = word(32 + 4 * index);
        if (at > size - 8) return false;
        const size_t base = at + 8, length = word(at + 4);
        if (length > size - base) return false;
        const auto tag = std::string_view(reinterpret_cast<const char*>(bytes + at), 4);
        if (tag == "ISGN" || tag == "OSGN") {
            bool& seen = tag == "ISGN" ? input : output;
            if (seen || length < 8 || word(base + 4) != 8) return false;
            seen = true;
            const uint32_t elements = word(base);
            if (elements > 128 || elements > (length - 8) / 24) return false;
            for (uint32_t element = 0; element < elements; ++element) {
                const size_t row = base + 8 + element * 24, offset = word(row);
                if (offset >= length) return false;
                const char* name = reinterpret_cast<const char*>(bytes + base + offset);
                const auto* end = static_cast<const char*>(std::memchr(name, 0, length - offset));
                if (!end) return false;
                const std::string_view semantic(name, static_cast<size_t>(end - name));
                if (tag == "OSGN") {
                    // No explicit depth, sample mask, or motion/stencil ownership outputs.
                    if (semantic != "SV_TARGET" || word(row + 4) > 3) return false;
                    color |= word(row + 4) == 0;
                } else if (word(row + 20) & 0x0f00) {
                    normal |= semantic == "__USER_VERTEX_M_LIGHTINGNORMAL";
                    tangent |= semantic == "__USER_VERTEX_M_LIGHTINGTANGENT";
                    surfaceUv |= semantic == "__USER_VERTEX_M_TEXCOORD";
                    terrainUv |= semantic == "__USER_PLANETSURFACE_TEXCOORD";
                    lightingPosition |= semantic == "__USER_VERTEX_M_LIGHTINGPOSITION";
                }
            }
        } else if (tag == "SHEX" || tag == "SHDR") {
            if (program || length < 12 || length % 4 || word(base) != 0x50 || word(base + 4) != length / 4) return false;
            program = true;
            for (size_t cursor = 8; cursor < length;) {
                const uint32_t token = word(base + cursor), op = token & 0x7ff;
                // Like dxbc_flat_overlay.h, allow read-only SM5 buffer operations;
                // UAV declarations, stores and atomics cannot pass. Discard would
                // broadcast a cutout decision over multiple independent depth pixels.
                if (op == 13 || op == 120 || (op >= 143 && op != 161 && op != 162 && op != 165 && op != 167)) return false;
                uint32_t words = (token >> 24) & 127;
                if (op == 53) {
                    if ((token >> 11) != 3 || length - cursor < 8) return false;
                    words = word(base + cursor + 4);
                    if (words < 2 || (words - 2) % 4) return false;
                }
                if (!words || words > (length - cursor) / 4) return false;
                cursor += words * 4;
            }
        }
    }
    return input && output && color && program && ((normal && (tangent || surfaceUv)) || (terrainUv && lightingPosition));
}

} // namespace edvr::native_vrs

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace edvr {

enum class DlssQuality : uint32_t { Custom, Dlaa, Quality, Balanced, Performance, Invalid };

inline const char* dlssQualityName(DlssQuality quality) noexcept {
    switch (quality) {
    case DlssQuality::Custom: return "custom";
    case DlssQuality::Dlaa: return "dlaa";
    case DlssQuality::Quality: return "quality";
    case DlssQuality::Balanced: return "balanced";
    case DlssQuality::Performance: return "performance";
    default: return "invalid";
    }
}

inline DlssQuality parseDlssQuality(std::string_view value) noexcept {
    for (const DlssQuality quality : {DlssQuality::Custom, DlssQuality::Dlaa,
                                     DlssQuality::Quality, DlssQuality::Balanced,
                                     DlssQuality::Performance}) {
        const std::string_view name = dlssQualityName(quality);
        if (value.size() != name.size()) continue;
        std::size_t index = 0;
        for (; index < name.size(); ++index) {
            char letter = value[index];
            if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter + 'a' - 'A');
            if (letter != name[index]) break;
        }
        if (index == name.size()) return quality;
    }
    return DlssQuality::Invalid;
}

inline double dlssQualityRatio(DlssQuality quality) noexcept {
    switch (quality) {
    case DlssQuality::Dlaa: return 1.0;
    case DlssQuality::Quality: return 2.0 / 3.0;
    case DlssQuality::Balanced: return 0.58;
    case DlssQuality::Performance: return 0.50;
    default: return 0.0;
    }
}

inline bool dlssEliteMultiplierKnown(float multiplier) noexcept {
    return multiplier > 0.0f && multiplier < 8.0f;
}

// Find the nearest even OpenVR recommendation that Elite's float multiply
// and truncation convert to the requested render size (or a few pixels
// above). Rounding up prevents Performance undershooting NGX's half-size floor.
// Return 0 if there is no valid ask within D3D11's per-axis limit.
inline uint32_t dlssGameDimension(uint32_t output, DlssQuality quality, float eliteMultiplier) noexcept {
    constexpr uint32_t limit = 16384;
    const double ratio = dlssQualityRatio(quality);
    if (!output || output > limit || !ratio || !dlssEliteMultiplierKnown(eliteMultiplier)) return 0;
    const uint32_t target = static_cast<uint32_t>(std::ceil(output * ratio));
    const double unrounded = target / static_cast<double>(eliteMultiplier);
    if (unrounded > limit && static_cast<float>(limit) * eliteMultiplier < static_cast<float>(target)) return 0;
    uint32_t ask = static_cast<uint32_t>(unrounded) & ~1u;
    if (ask < 2) ask = 2;
    while (ask <= limit) {
        const float rendered = static_cast<float>(ask) * eliteMultiplier;
        if (rendered > static_cast<float>(limit)) return 0;
        if (rendered >= static_cast<float>(target)) return ask;
        ask += 2;
    }
    return 0;
}

}

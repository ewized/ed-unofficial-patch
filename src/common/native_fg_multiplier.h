#pragma once

#include <string_view>

namespace edvr {
// Fixed native VR modes represent the native endpoint plus its intermediate
// frames. The NGX runtime must support at least multiplier minus one
// generated frames for the requested mode.
constexpr bool nativeFgFixedMultiplier(unsigned multiplier) noexcept {
    return multiplier >= 2 && multiplier <= 4;
}

struct NativeFgSelection {
    bool automatic = false;
    unsigned multiplier = 0;

    constexpr bool enabled() const noexcept { return automatic || nativeFgFixedMultiplier(multiplier); }
};

namespace native_fg_detail {
constexpr bool equalSetting(std::string_view value, std::string_view lower) noexcept {
    if (value.size() != lower.size()) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if ((ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch) != lower[i]) return false;
    }
    return true;
}
}

// A saved selection, independent of current cadence, backend availability,
// or temporary suspension. Unknown values keep native presentation.
constexpr NativeFgSelection nativeFgSelection(std::string_view value) noexcept {
    if (native_fg_detail::equalSetting(value, "auto")) return {true, 0};
    if (value.size() == 2 && (value[1] == 'x' || value[1] == 'X') &&
        value[0] >= '2' && value[0] <= '4') return {false, unsigned(value[0] - '0')};
    return {};
}
// A zero maximum means the session has not reported its capability yet.
// Auto may request 2x, and Off must remain available even without support.
constexpr bool nativeFgOptionEnabled(std::string_view value, unsigned maximum) noexcept {
    if (native_fg_detail::equalSetting(value, "off")) return true;
    const auto selection = nativeFgSelection(value);
    if (!selection.enabled()) return false;
    return maximum == 0 || (selection.automatic ? 2u : selection.multiplier) <= maximum;
}
} // namespace edvr

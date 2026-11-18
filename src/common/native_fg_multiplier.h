#pragma once

namespace edvr {
// Fixed native VR modes represent the native endpoint plus its intermediate
// frames. The NGX runtime must support at least multiplier minus one
// generated frames for the requested mode.
constexpr bool nativeFgFixedMultiplier(unsigned multiplier) noexcept {
    return multiplier >= 2 && multiplier <= 4;
}
} // namespace edvr

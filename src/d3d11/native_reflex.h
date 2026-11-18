#pragma once

#include <cstdint>

struct ID3D11Device;

namespace edvr::native_reflex {

enum class Mode : uint8_t { Off, On, Boost };

// A successful enable is owned until a successful disable. Driver refusal
// keeps the previous ownership so a failed live disable can be retried at close.
class ModeHistory {
public:
    constexpr bool change(Mode mode) noexcept {
        if (noted_ && requested_ == mode) return false;
        requested_ = mode;
        noted_ = true;
        return true;
    }

    constexpr bool requiresDriver() const noexcept { return requested_ != Mode::Off || needsReset_; }
    constexpr void accepted() noexcept { needsReset_ = requested_ != Mode::Off; }
    constexpr bool needsReset() const noexcept { return needsReset_; }

private:
    Mode requested_ = Mode::Off;
    bool noted_ = false;
    bool needsReset_ = false;
};

// Called at the validated game Present boundary after config/profile updates.
void updateOnGamePresent(ID3D11Device* device);

} // namespace edvr::native_reflex

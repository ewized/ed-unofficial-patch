#pragma once

#include <cstdint>

struct ID3D11Device;

namespace edvr::native_reflex {

enum class Mode : uint8_t { Off, On, Boost };

// Keep ownership of a successful enable until a later Off request is verified.
class ModeHistory {
public:
    constexpr bool change(Mode mode) noexcept {
        if (noted_ && requested_ == mode) return false;
        requested_ = mode;
        noted_ = true;
        observing_ = false;
        return true;
    }

    constexpr Mode requested() const noexcept { return requested_; }
    constexpr bool requiresDriver() const noexcept { return requested_ != Mode::Off || needsReset_; }
    constexpr void acceptRequest() noexcept {
        if (requested_ != Mode::Off) needsReset_ = true;
        observing_ = true;
    }
    constexpr bool observing() const noexcept { return observing_; }
    constexpr bool needsReset() const noexcept { return needsReset_; }

    static constexpr unsigned kLastObservation = 64;
    static constexpr bool observationDue(unsigned presents) noexcept {
        return presents == 4 || presents == 16 || presents == kLastObservation;
    }

    // The last delayed read decides if the live disable has taken effect.
    // A failed read or enabled readback keeps the close-time Off attempt armed.
    constexpr void finishObservation(bool driverDisabled) noexcept {
        if (observing_ && requested_ == Mode::Off && driverDisabled) needsReset_ = false;
        observing_ = false;
    }

private:
    Mode requested_ = Mode::Off;
    bool noted_ = false;
    bool needsReset_ = false;
    bool observing_ = false;
};

// Called at the validated game Present boundary after config/profile updates.
void updateOnGamePresent(ID3D11Device* device);

} // namespace edvr::native_reflex

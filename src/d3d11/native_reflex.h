#pragma once

#include <cstdint>
#include <atomic>

struct ID3D11Device;

namespace edvr::native_reflex {

enum class Mode : uint8_t { Off, On, Boost };

enum class RequestState : uint8_t { NoSession, Untouched, Accepted, ExternalPolicy, Refused };
enum class DriverMode : uint8_t { Unknown, Off, On };
struct Snapshot {
    Mode requested;
    RequestState request;
    DriverMode driver;
    bool apiAvailable;
    bool gameSleepObserved;
    bool markerTransportReady;
    // Never inferred from an accepted mode request or a DXGI Present.
    bool fullBoundaryAvailable;
};

// A menu read sees one coherent request and device observation. On describes
// the driver's low-latency bit; NVAPI exposes no effective Boost bit.
inline std::atomic<uint32_t> statusWord_{0};
inline void publishStatus(Snapshot s) noexcept {
    statusWord_.store(uint32_t(s.requested) | (uint32_t(s.request) << 8) | (uint32_t(s.driver) << 16) |
        (uint32_t(s.apiAvailable) << 24) | (uint32_t(s.gameSleepObserved) << 25) |
        (uint32_t(s.markerTransportReady) << 26) | (uint32_t(s.fullBoundaryAvailable) << 27), std::memory_order_relaxed);
}
inline Snapshot status() noexcept {
    const auto word = statusWord_.load(std::memory_order_relaxed);
    return {Mode(word & 0xff), RequestState((word >> 8) & 0xff), DriverMode((word >> 16) & 0xff),
        bool(word & (1u << 24)), bool(word & (1u << 25)), bool(word & (1u << 26)), bool(word & (1u << 27))};
}

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

#pragma once

#include "native_dlss_quality.h"
#include <atomic>

namespace edvr {

// The native caller clears this before treating one eye. Only a successful
// NGX evaluation fills it, using the feature's creation-time quality.
struct NativeDlssEvaluation {
    int eye = -1;
    const char* feature = "none";
    uint32_t width = 0, height = 0, outputWidth = 0, outputHeight = 0;
};
inline thread_local NativeDlssEvaluation nativeDlssEvaluation;

class NativeDlssEvidence {
    using Status = NativeDlssQualityStatus;
public:
    void publish(DlssQuality selected, float multiplier, bool declined = false) noexcept {
        multiplier_ = multiplier;
        const auto initial = declined ? Status::Unavailable : selected == DlssQuality::Custom ? Status::Custom : Status::Pending;
        for (auto& eye : eyes_) eye.store(initial, std::memory_order_relaxed);
    }

    // Door dimensions describe the frame actually being treated, including
    // trim adoption. NGX's actual output can differ on a served-floor cut,
    // or match its input at DLAA after Elite changed its HMD multiplier.
    bool observe(unsigned eye, DlssQuality selected, uint32_t width, uint32_t height,
                 uint32_t doorWidth, uint32_t doorHeight, const NativeDlssEvaluation& result, bool attempted = true) noexcept {
        if (eye >= 2 || selected == DlssQuality::Custom) return false;
        const float multiplier = multiplier_.load(std::memory_order_relaxed);
        const auto near = [](uint32_t first, uint32_t second) { return first > second ? first - second <= 1 : second - first <= 1; };
        const auto matchesInput = [&](uint32_t input, uint32_t door) {
            const auto ask = dlssGameDimension(door, selected, multiplier);
            return ask && near(input, static_cast<uint32_t>(float(ask) * multiplier));
        };
        Status next = Status::Stale;
        if (matchesInput(width, doorWidth) && matchesInput(height, doorHeight)) {
            if (!attempted) next = Status::Pending;
            else if (result.eye != static_cast<int>(eye)) next = Status::Unavailable;
            else if (result.width == width && result.height == height &&
                     (selected == DlssQuality::Dlaa ? result.outputWidth == width && result.outputHeight == height :
                      near(result.outputWidth, doorWidth) && near(result.outputHeight, doorHeight))) {
                next = std::string_view(result.feature) == dlssQualityName(selected) ? Status::Applied : Status::Unavailable;
            }
        }
        if (eyes_[eye].load(std::memory_order_relaxed) == next) return false;
        eyes_[eye].store(next, std::memory_order_relaxed);
        return true;
    }

    Status status() const noexcept {
        const auto left = eyes_[0].load(std::memory_order_relaxed), right = eyes_[1].load(std::memory_order_relaxed);
        if (left == Status::Unavailable || right == Status::Unavailable) return Status::Unavailable;
        if (left == Status::Stale || right == Status::Stale) return Status::Stale;
        return left == right ? left : Status::Pending;
    }

private:
    std::atomic<float> multiplier_{0.f};
    std::atomic<Status> eyes_[2]{{Status::Custom}, {Status::Custom}};
};

inline NativeDlssEvidence nativeDlssEvidence;

}

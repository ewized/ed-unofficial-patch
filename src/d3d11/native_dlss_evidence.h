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
    float launchMultiplier() const noexcept { return multiplier_.load(std::memory_order_relaxed); }

    void publish(DlssQuality selected, float multiplier, bool declined = false) noexcept {
        invalidate();
        multiplier_ = multiplier;
        const auto initial = declined ? Status::Unavailable : selected == DlssQuality::Custom ? Status::Custom : Status::Pending;
        for (auto& eye : eyes_) eye.store(initial, std::memory_order_relaxed);
    }

    // A skipped/incomplete pair or a temporal reset cannot attest to stereo.
    // Keep the last measured failure (and startup refusal) until fresh evidence.
    void invalidate() noexcept {
        lastPair_.store(0, std::memory_order_relaxed);
        for (auto& receipt : sequences_) receipt.store(0, std::memory_order_relaxed);
    }

    // Door dimensions describe the frame actually being treated, including
    // trim adoption. NGX's actual output can differ on a served-floor cut,
    // or match its input at DLAA after Elite changed its HMD multiplier.
    bool observe(uint64_t sequence, unsigned eye, DlssQuality selected, uint32_t width, uint32_t height,
                 uint32_t doorWidth, uint32_t doorHeight, const NativeDlssEvaluation& result, bool attempted = true) noexcept {
        if (!sequence || eye >= 2 || selected == DlssQuality::Custom) return false;
        const float multiplier = multiplier_.load(std::memory_order_relaxed);
        const auto withinOnePixel = [](uint32_t first, uint32_t second) { return first > second ? first - second <= 1 : second - first <= 1; };
        const auto matchesInput = [&](uint32_t input, uint32_t door) {
            const auto ask = dlssGameDimension(door, selected, multiplier);
            return ask && withinOnePixel(input, static_cast<uint32_t>(float(ask) * multiplier));
        };
        Status next = Status::Stale;
        if (matchesInput(width, doorWidth) && matchesInput(height, doorHeight)) {
            if (!attempted) next = Status::Pending;
            else if (result.eye != static_cast<int>(eye)) next = Status::Unavailable;
            else if (result.width == width && result.height == height &&
                     (selected == DlssQuality::Dlaa ? result.outputWidth == width && result.outputHeight == height :
                      withinOnePixel(result.outputWidth, doorWidth) && withinOnePixel(result.outputHeight, doorHeight))) {
                next = std::string_view(result.feature) == dlssQualityName(selected) ? Status::Applied : Status::Unavailable;
            }
        }
        const uint64_t previousPair = lastPair_.load(std::memory_order_relaxed);
        sequences_[eye].store(next == Status::Applied ? sequence : 0, std::memory_order_relaxed);
        const bool stereo = next == Status::Applied && sequences_[1-eye].load(std::memory_order_relaxed) == sequence;
        // Keep the previous complete verdict between the two eyes of the next
        // frame. A gap, a failed eye or invalidate() withdraws that verdict.
        if (stereo) lastPair_.store(sequence, std::memory_order_relaxed);
        else if (next != Status::Applied || sequence - previousPair > 1) lastPair_.store(0, std::memory_order_relaxed);
        const Status previous = eyes_[eye].load(std::memory_order_relaxed);
        if (next != previous) eyes_[eye].store(next, std::memory_order_relaxed);
        return previous != next || (stereo && !previousPair);
    }

    Status status() const noexcept {
        const auto left = eyes_[0].load(std::memory_order_relaxed), right = eyes_[1].load(std::memory_order_relaxed);
        if (left == Status::Unavailable || right == Status::Unavailable) return Status::Unavailable;
        if (left == Status::Stale || right == Status::Stale) return Status::Stale;
        if (left == Status::Applied && right == Status::Applied && !lastPair_.load(std::memory_order_relaxed)) return Status::Pending;
        return left == right ? left : Status::Pending;
    }

private:
    std::atomic<float> multiplier_{0.f};
    std::atomic<uint64_t> sequences_[2]{{0}, {0}}, lastPair_{0};
    std::atomic<Status> eyes_[2]{{Status::Custom}, {Status::Custom}};
};

inline NativeDlssEvidence nativeDlssEvidence;

}

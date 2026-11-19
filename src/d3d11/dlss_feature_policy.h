#pragma once

#include "../common/dlss_quality.h"

#include <array>

namespace edvr {

struct DlssFeatureKey {
    uint32_t width = 0, height = 0, outWidth = 0, outHeight = 0;
    uint64_t presetGeneration = 0;
    uint32_t createFlags = 0;

    bool operator==(const DlssFeatureKey& b) const noexcept {
        return width == b.width && height == b.height && outWidth == b.outWidth &&
               outHeight == b.outHeight && presetGeneration == b.presetGeneration &&
               createFlags == b.createFlags;
    }
};

enum class DlssFailure { None, Transient, Permanent };

// Failed contracts are independent within one feature slot/model generation.
// An incoming history change can probe each transient streak early once.
class DlssRetryPolicy {
public:
    static constexpr unsigned kFailureSlots = 8;
    static constexpr uint64_t kMinRetryMs = 250;

    // successfulLiveFeature belongs to this exact key and has previously
    // evaluated successfully. It only bypasses the unseen-contract throttle.
    bool ready(const DlssFeatureKey& key, uint64_t nowMs, bool historyChanged,
               bool successfulLiveFeature = false) const noexcept {
        if (presetGeneration_ != key.presetGeneration) return true;
        if (const auto* failure = find(key)) return failure->ready(nowMs, historyChanged);
        return successfulLiveFeature || count_ < kFailureSlots || nowMs >= nextUnknownProbe_;
    }

    // Called only once the caller actually has inputs and will ask NGX.
    void attempting(const DlssFeatureKey& key, uint64_t nowMs, bool successfulLiveFeature = false) noexcept {
        selectPreset(key.presetGeneration);
        if (auto* failure = find(key)) {
            if (failure->kind == DlssFailure::Transient && nowMs < failure->retryAt_)
                failure->usedEarlyRetry_ = true;
        } else if (!successfulLiveFeature && count_ == kFailureSlots) {
            nextUnknownProbe_ = nowMs + kMinRetryMs;
        }
    }

    void failed(const DlssFeatureKey& key, uint64_t nowMs, DlssFailure kind) noexcept {
        selectPreset(key.presetGeneration);
        auto* failure = find(key);
        if (!failure) {
            // Never discard a permanent refusal or an active cooldown. If
            // capacity is exhausted, throttled probes cover untracked keys.
            for (auto& entry : entries_) {
                if (entry.kind == DlssFailure::None) { failure = &entry; break; }
                if (entry.kind == DlssFailure::Transient && nowMs >= entry.retryAt_ &&
                    (!failure || entry.retryAt_ < failure->retryAt_)) failure = &entry;
            }
            if (!failure) {
                nextUnknownProbe_ = nowMs + kMinRetryMs;
                return;
            }
            if (failure->kind == DlssFailure::None && ++count_ == kFailureSlots)
                nextUnknownProbe_ = nowMs + kMinRetryMs;
            *failure = {key};
        }
        failure->kind = kind;
        failure->retryAt_ = nowMs + (kMinRetryMs << failure->failures);
        if (failure->failures < 4) ++failure->failures;
    }

    // Successful evaluation retires only the contract actually evaluated.
    void succeeded(const DlssFeatureKey& key) noexcept {
        if (auto* failure = find(key)) {
            *failure = {};
            --count_;
        }
    }

    void clear() noexcept { *this = {}; }
    bool hasFailure() const noexcept { return count_ != 0; }

private:
    struct Failure {
        DlssFeatureKey key{};
        DlssFailure kind = DlssFailure::None;
        uint64_t retryAt_ = 0;
        unsigned failures = 0;
        bool usedEarlyRetry_ = false;

        bool ready(uint64_t nowMs, bool historyChanged) const noexcept {
            return kind == DlssFailure::Transient &&
                   (nowMs >= retryAt_ || (historyChanged && !usedEarlyRetry_));
        }
    };

    template<class Entries>
    static auto findIn(Entries& entries, const DlssFeatureKey& key) noexcept
        -> decltype(entries.data()) {
        for (auto& failure : entries)
            if (failure.kind != DlssFailure::None && failure.key == key) return &failure;
        return nullptr;
    }
    Failure* find(const DlssFeatureKey& key) noexcept { return findIn(entries_, key); }
    const Failure* find(const DlssFeatureKey& key) const noexcept { return findIn(entries_, key); }
    void selectPreset(uint64_t generation) noexcept {
        if (presetGeneration_ == generation) return;
        clear();
        presetGeneration_ = generation;
    }

    std::array<Failure, kFailureSlots> entries_{};
    uint64_t presetGeneration_ = 0;
    uint64_t nextUnknownProbe_ = 0;
    unsigned count_ = 0;
};

// A named quality uses the same recommendation, float multiplication and
// truncation as Elite's startup sizing. Unknown sizing means initialize NGX
// without features; creating a speculative unity pair would waste that work.
inline DlssFeatureKey dlssWarmSize(DlssQuality quality, float multiplier,
                                  uint32_t outputWidth, uint32_t outputHeight) noexcept {
    const auto askW = dlssGameDimension(outputWidth, quality, multiplier);
    const auto askH = dlssGameDimension(outputHeight, quality, multiplier);
    if (!askW || !askH) return {};
    const auto width = static_cast<uint32_t>(static_cast<float>(askW) * multiplier);
    const auto height = static_cast<uint32_t>(static_cast<float>(askH) * multiplier);
    if (quality == DlssQuality::Dlaa) return {width, height, width, height};
    // Match the native temporal door: both axes must merit an upscale.
    if (width * 50u >= outputWidth * 49u || height * 50u >= outputHeight * 49u)
        return {};
    return {width, height, outputWidth, outputHeight};
}

} // namespace edvr

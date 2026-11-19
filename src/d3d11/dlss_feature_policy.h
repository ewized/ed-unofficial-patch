#pragma once

#include "../common/dlss_quality.h"

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

// One retry state per feature slot, scoped to the initialized device. A genuine
// history change can probe early once per failure streak; NGX's own reset bit
// is deliberately excluded because the fallback itself raises it.
class DlssRetryPolicy {
public:
    bool ready(const DlssFeatureKey& key, uint64_t nowMs, bool historyChanged) const noexcept {
        return failure_ == DlssFailure::None || !(key_ == key) ||
               (failure_ == DlssFailure::Transient &&
                (nowMs >= retryAt_ || (historyChanged && !usedEarlyRetry_)));
    }

    // Called only once the caller actually has inputs and will ask NGX.
    void attempting(const DlssFeatureKey& key, uint64_t nowMs) noexcept {
        if (failure_ == DlssFailure::Transient && key_ == key && nowMs < retryAt_)
            usedEarlyRetry_ = true;
    }

    void failed(const DlssFeatureKey& key, uint64_t nowMs, DlssFailure failure) noexcept {
        if (failure_ == DlssFailure::None || !(key_ == key)) clear();
        key_ = key;
        failure_ = failure;
        // 250 ms, 500 ms, 1 s, 2 s, then 4 s. A persistent driver or memory
        // problem stays observable without trying expensive creation every frame.
        retryAt_ = nowMs + (uint64_t{250} << failures_);
        if (failures_ < 4) ++failures_;
    }

    void clear() noexcept { *this = {}; }
    bool hasFailure() const noexcept { return failure_ != DlssFailure::None; }

private:
    DlssFeatureKey key_{};
    DlssFailure failure_ = DlssFailure::None;
    uint64_t retryAt_ = 0;
    unsigned failures_ = 0;
    bool usedEarlyRetry_ = false;
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

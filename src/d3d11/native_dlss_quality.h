#pragma once

#include "../common/dlss_quality.h"

namespace edvr {

// The quality used for the game's published size, held until restart even
// when a later frame uses another ratio or falls back to the own pass.
DlssQuality nativeDlssQualityActive() noexcept;

enum class NativeDlssQualityStatus { Custom, Pending, Applied, Stale, Unavailable };

// Applied requires both eyes of one frame to have evaluated the requested NGX feature
// with matching input and output dimensions. Stale requires a restart to
// recapture Elite's settings. Unavailable includes an NGX refusal at startup
// (Custom sizing) or a later fallback (the named sizing remains published).
NativeDlssQualityStatus nativeDlssQualityStatus() noexcept;

// New samplers can use the captured ratio before the first eye is treated.
// Stop applying or reporting that bias if the measured frame contradicts it.
inline float nativeDlssQualityBiasRatio(DlssQuality published) noexcept {
    if (published == DlssQuality::Custom) return 0.f;
    const auto state = nativeDlssQualityStatus();
    return state == NativeDlssQualityStatus::Pending || state == NativeDlssQualityStatus::Applied
         ? static_cast<float>(dlssQualityRatio(published)) : 0.f;
}

}

#pragma once

#include "native_gaze.h"

#include <cmath>

namespace edvr::native_gaze_detail {

inline bool isClientImageName(const wchar_t* actual) noexcept {
    auto* expected = L"vrclient_x64.dll";
    for (; *actual && *expected; ++actual, ++expected) {
        const auto lower = [](wchar_t c) noexcept {
            return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c;
        };
        if (lower(*actual) != *expected) return false;
    }
    return *actual == *expected;
}

// The SDK returns one validity bit for the two eye centers. Keep each point
// in its original NDC and require a real receipt before treating the pair as usable.
inline NativeGazeSample sampleGaze(bool returnedValid, const float (&ndc)[2][2],
    std::uint64_t sequence, std::uint64_t receipt, std::uint64_t frequency) noexcept {
    NativeGazeSample sample{};
    sample.providerQueried = true;
    sample.providerReturnedValid = returnedValid;
    sample.querySequence = sequence;
    sample.receiptQpc = receipt;
    sample.receiptQpcFrequency = frequency;
    bool allFinite = true;
    bool allInRange = true;
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& value = sample.eyes[eye];
        value.xNdc = ndc[eye][0];
        value.yNdc = ndc[eye][1];
        const bool finite = std::isfinite(value.xNdc) && std::isfinite(value.yNdc);
        const bool inRange = finite && value.xNdc >= -1.f && value.xNdc <= 1.f &&
            value.yNdc >= -1.f && value.yNdc <= 1.f;
        allFinite = allFinite && finite;
        allInRange = allInRange && inRange;
        value.valid = returnedValid && inRange;
    }
    sample.failure = !returnedValid ? NativeGazeFailure::ProviderInvalid :
        !allFinite ? NativeGazeFailure::NonFinite :
        !allInRange ? NativeGazeFailure::OutOfRange :
        !receipt || !frequency ? NativeGazeFailure::ReceiptClockUnavailable : NativeGazeFailure::None;
    sample.usable = sample.failure == NativeGazeFailure::None;
    return sample;
}

}  // namespace edvr::native_gaze_detail

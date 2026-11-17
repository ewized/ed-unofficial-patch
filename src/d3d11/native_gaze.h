#pragma once

#include <cstdint>

namespace edvr {

enum class NativeGazeFailure : std::uint8_t {
    None,
    Disabled,
    ModuleLoadFailed,
    ModulePathMismatch,
    ContextUnavailable,
    ProviderInvalid,
    NonFinite,
    OutOfRange,
    ReceiptClockUnavailable,
    InitTokenUnavailable,
    ContextChanged,
};

struct NativeGazeEye {
    // OpenVR per-eye NDC, copied without averaging or a tangent conversion.
    float xNdc{};
    float yNdc{};
    // The API has one combined validity bit. This also checks this eye's bounds.
    bool valid{};
    // Official source projection: m00,m11,m02,m12; NDC = scale*tangent-offset.
    float projection[4]{};
    bool projectionValid{};
};

struct NativeGazeSample {
    NativeGazeEye eyes[2]{};
    bool providerQueried{};
    bool providerReturnedValid{};
    bool usable{};
    // IVRSystem026 supplies no sensor timestamp. QPC measures callback receipt.
    bool sensorTimestampUnavailable{true};
    std::uint64_t receiptQpc{};
    std::uint64_t receiptQpcFrequency{};
    std::uint64_t querySequence{};
    NativeGazeFailure failure{NativeGazeFailure::Disabled};
};

// Called once per game frame on the renderer thread. OFF has no API, loader,
// context, clock or sequence side effects. Invalid samples require full rate.
NativeGazeSample nativeGazePoll(bool enabled) noexcept;
// Pure renderer-owned cache invalidation; no SDK, loader or shutdown calls.
void nativeGazeSetLifetime(std::uint64_t epoch) noexcept;
void nativeGazeReset() noexcept;

}  // namespace edvr

#pragma once

#include <cstdint>

namespace edvr {

inline constexpr char kNativeGazeOpenXrRuntime[] = "SteamVR/OpenXR";

enum class NativeGazeFailure : std::uint8_t {
    None,
    Disabled,
    ModuleNotLoaded,
    ModulePathMismatch,
    ContextUnavailable,
    ProviderInvalid,
    NonFinite,
    OutOfRange,
    ReceiptClockUnavailable,
    ModuleCensusIncomplete,
    ModuleAmbiguous,
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
    bool instanceFunctionInClient{}; // Optional probe witness; false is inconclusive.
    // IVRSystem026 supplies no sensor timestamp. QPC measures callback receipt.
    bool sensorTimestampUnavailable{true};
    std::uint64_t receiptQpc{};
    std::uint64_t receiptQpcFrequency{};
    std::uint64_t querySequence{};
    NativeGazeFailure failure{NativeGazeFailure::Disabled};
};

// The serialized caller holds a live SteamVR OpenXR instance through the call.
// In game, nativeTemporalWithGazeReady fences instance destruction. A stopped XR
// session can return invalid gaze; no interface survives the poll. OFF performs
// no lookup or clock work. The admitted epoch owns one complete module census,
// and each poll uses only that image. An unavailable sample requires full rate.
// The optional probe address is informational and cannot grant admission.
NativeGazeSample nativeGazePoll(std::uint64_t admittedEpoch, const void* instanceFunction = nullptr) noexcept;

}  // namespace edvr

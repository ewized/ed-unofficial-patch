#pragma once
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <algorithm>

// Separate optional CPU capability. Existing render-settings/sizing ABIs and
// their output dimensions are unchanged. Both files belong to a paired build.
#define EDVR_NATIVE_INPUT_SCALE_VERSION_1 1u
#define EDVR_NATIVE_INPUT_SCALE_CAPABILITY_EXACT_ASK 2u
struct EdvrNativeInputScaleSettings {
    uint32_t size, version;
    float requestedScale, activeScale;
    uint32_t policy, reserved;
};
static_assert(sizeof(EdvrNativeInputScaleSettings) == 24, "native input settings ABI");
using EdvrQueryNativeInputScale = BOOL (WINAPI*)(uint32_t, uint32_t, void*);
using EdvrNativeInputScaleCapability = uint32_t (WINAPI*)(uint32_t);
extern "C" BOOL WINAPI edvrQueryNativeInputScale(uint32_t, uint32_t, void*);

namespace edvr::native_input {
constexpr float kDefault = 1.0f, kMinimum = 1.0f / 3.0f;
constexpr uint32_t kQualifiedOutput = 3200;
enum class Policy : uint32_t {
    NotCaptured, Default, Active, InvalidScale, RequiresHmdOne, RequiresSsaaOne,
    TrimConfigured, RequiresDlss, PairUnavailable
};
inline bool validScale(float value) noexcept {
    return std::isfinite(value) && value >= kMinimum - 1.0e-6f && value <= 1.0f;
}
inline const char* reason(Policy policy) noexcept {
    switch (policy) {
        case Policy::Default: return "unchanged";
        case Policy::Active: return "accepted";
        case Policy::InvalidScale: return "scale outside 1/3..1";
        case Policy::RequiresHmdOne: return "requires saved HMD=1";
        case Policy::RequiresSsaaOne: return "requires saved Supersampling=1";
        case Policy::TrimConfigured: return "requires empty FOV trim lists";
        case Policy::RequiresDlss: return "requires DLSS at startup";
        case Policy::PairUnavailable: return "paired native bridge unavailable";
        default: return "not captured";
    }
}
inline Policy choose(float scale, bool hmdKnown, float hmd, bool trimEmpty,
                     bool dlss, bool paired, float ssaa=1.0f) noexcept {
    if (!validScale(scale)) return Policy::InvalidScale;
    if (scale == kDefault) return Policy::Default;
    if (!hmdKnown || !std::isfinite(hmd) || std::fabs(hmd - 1.0f) > 1.0e-5f)
        return Policy::RequiresHmdOne;
    if (!std::isfinite(ssaa) || std::fabs(ssaa-1.0f)>1.0e-5f) return Policy::RequiresSsaaOne;
    if (!trimEmpty) return Policy::TrimConfigured;
    if (!dlss) return Policy::RequiresDlss;
    return paired ? Policy::Active : Policy::PairUnavailable;
}
inline bool accepted(Policy policy) noexcept {
    return policy == Policy::Default || policy == Policy::Active;
}
// Default1 preserves the old recommendation exactly. An explicitly scaled
// game input may be odd: NGX's 3200 Ultra range is the singleton1067, so
// rounding it to an even1068 would force a smaller DLSS output/spatial tail.
inline uint32_t recommendation(uint32_t base, float scale) noexcept {
    if (scale == kDefault) return base;
    if (!base || base > 16384 || !validScale(scale)) return 0;
    const double value = std::floor(double(base) * double(scale) + .5);
    return static_cast<uint32_t>((std::min)(double(base), (std::max)(2.0, value)));
}
inline float activeScale(float requested) noexcept {
    return requested==1.0f?1.0f:float(recommendation(kQualifiedOutput,requested))/float(kQualifiedOutput);
}
inline float mipBias(float hmd, float activeScale) noexcept {
    // Retain the exact old expression when the feature is disabled/default.
    return activeScale == kDefault ? std::log2(hmd) : std::log2(hmd * activeScale);
}
inline bool validSettings(const EdvrNativeInputScaleSettings& info) noexcept {
    if (info.size != sizeof(info) || info.version != EDVR_NATIVE_INPUT_SCALE_VERSION_1 || info.reserved ||
        !std::isfinite(info.requestedScale) || !validScale(info.activeScale)) return false;
    const auto policy = static_cast<Policy>(info.policy);
    if(policy==Policy::Default)return info.requestedScale==1 && info.activeScale==1;
    if(policy==Policy::Active)return validScale(info.requestedScale)&&info.requestedScale<1 &&
        info.activeScale==activeScale(info.requestedScale);
    // A well-formed refusal keeps the unscaled legacy path alive and visible.
    return policy>=Policy::InvalidScale && policy<=Policy::PairUnavailable && info.activeScale==1;
}
// The exact production startup latch; protected by the provider's mutex.
// Reloads can change desired config, never this accepted request or mip scale.
struct StartupLatch {
    bool captured=false;
    EdvrNativeInputScaleSettings info{sizeof(info),EDVR_NATIVE_INPUT_SCALE_VERSION_1,1,1,
        static_cast<uint32_t>(Policy::NotCaptured),0};
    bool capture(float requested,Policy policy) noexcept {
        if(captured)return false;
        captured=true;info.requestedScale=requested;
        info.activeScale=policy==Policy::Active?activeScale(requested):1.0f;
        info.policy=static_cast<uint32_t>(policy);
        return true;
    }
};
} // namespace edvr::native_input

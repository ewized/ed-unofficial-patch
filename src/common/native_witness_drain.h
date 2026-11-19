#pragma once

#include <atomic>

struct ID3D11DeviceContext;

namespace edvr {

using NativeWitnessDrain = bool (*)(ID3D11DeviceContext*) noexcept;

// Optional lifecycle barrier. Providers call before taking their own locks:
// recovery can invalidate the currently produced eye. Null context delegates
// owner/thread validation to the registered renderer.
inline std::atomic<NativeWitnessDrain> g_nativeWitnessDrain{nullptr};

inline void setNativeWitnessDrain(NativeWitnessDrain drain) noexcept {
    g_nativeWitnessDrain.store(drain, std::memory_order_release);
}

inline bool nativeWitnessDrainInstalled() noexcept {
    return g_nativeWitnessDrain.load(std::memory_order_acquire) != nullptr;
}

inline bool nativeWitnessDrain(ID3D11DeviceContext* context = nullptr) noexcept {
    const auto drain = g_nativeWitnessDrain.load(std::memory_order_acquire);
    return !drain || drain(context);
}

} // namespace edvr

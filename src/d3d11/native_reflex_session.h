#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include "../common/native_reflex_frame.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace edvr::native_reflex {
class FrameDriver;
struct FrameSession;

// UI reads the last verified session capability without querying COM/NVAPI.
inline std::atomic<bool> fullFrameAvailable{false};
inline std::atomic<uint64_t> fullFrameQualificationEpoch{0};
// The active transport serializes its capability writes. Each edge invalidates
// evidence retained by consumers, even when they miss the intervening state.
inline void publishFrameAvailability(bool qualified) noexcept {
    if (fullFrameAvailable.load(std::memory_order_relaxed) == qualified) return;
    fullFrameQualificationEpoch.fetch_add(1, std::memory_order_acq_rel);
    fullFrameAvailable.store(qualified, std::memory_order_release);
}
// A runtime transport fault is distinct from initial unavailability. The mode
// owner observes this latch at its next safe Present and withdraws its request.
inline std::atomic<bool> fullFrameFaulted{false};
inline bool frameSessionFaulted() noexcept { return fullFrameFaulted.load(std::memory_order_acquire); }
inline bool availableForUser() noexcept {
    return fullFrameAvailable.load(std::memory_order_acquire) && !frameSessionFaulted();
}

// A successful begin retains the exact session/device through the real
// Present. Ending a stale receipt cannot address a replacement session.
struct PresentFrameReceipt {
    std::shared_ptr<FrameSession> owner;
    uint64_t frameId = 0;
    uint64_t attempt = 0;
};

// Armed only in a qualified LateRender worker. Entry to the first tracked
// immediate-context command after pose consumption begins its CPU interval.
void noteFrameCommand(ID3D11DeviceContext*) noexcept;
PresentFrameReceipt beginFramePresent(ID3D11Device*, const EdvrNativeReflexSource&) noexcept;
// Return true only when this exact attempt submitted the image (S_OK).
bool endFramePresent(const PresentFrameReceipt&, HRESULT result) noexcept;

// The input owner validates an actual removed game-window left-button message.
// Queue its press edge for the next qualified InputManager frame, independently
// of the optional diagnostic probe and the selected low-latency mode.
void mouseMessageRemoved(bool down) noexcept;

// Normal native-session lifecycle only. The returned transport is borrowed
// until stopFrameSession for this generation. Engine callbacks take their own
// strong snapshot before calling the driver without renderer locks held.
FrameDriver* startFrameSession(ID3D11Device*, uint64_t generation);
void stopFrameSession(uint64_t generation) noexcept;
void reportFrameSession(uint64_t generation) noexcept;

// Called from the game's removed WM_QUIT, outside DllMain/loader teardown.
void shutdownFrameTelemetry() noexcept;
} // namespace edvr::native_reflex

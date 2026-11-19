// Optional passive frame-ownership recording. Each observation retains its own
// identity; an app epoch, a reused texture, or a mirror Present alone cannot
// identify the simulation that produced a real stereo frame.
#pragma once

#include <atomic>
#include <cstdint>

struct ID3D11CommandList;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Resource;
struct IDXGISwapChain;

namespace edvr::native_reflex {

enum class TraceEvent {
    TickBegin, TickEnd, InputBegin, InputEnd, EarlyWorker, LateWorker, JobIssue,
    RenderBegin, RenderEnd, CommandList, RenderTarget, NativePose,
    EyeInput, PresentBegin, PresentEnd, HmdRequest, HmdReturn, GameEyeAccepted, GameEyeRejected,
    GpuDraw, GpuCopy, GpuClear, ViewCopyBegin, ViewCopyEnd, ViewConsumeBegin, ViewConsumeEnd, ViewRefresh, ViewWatchFull, ViewCopyPrior, ViewMailbox
};
struct TraceSample {
    TraceEvent event;
    uintptr_t actor = 0;
    uintptr_t resource = 0;
    uint64_t sequence = 0;
    uint64_t detail = 0;
    uint32_t caller = 0;
};
struct TraceObserver {
    void (*sample)(TraceSample) noexcept;
    void (*targets)(ID3D11DeviceContext*, unsigned, ID3D11RenderTargetView* const*) noexcept;
    void (*draw)(ID3D11DeviceContext*, const void*, unsigned, unsigned) noexcept;
    void (*copy)(ID3D11DeviceContext*, const void*, ID3D11Resource*, ID3D11Resource*) noexcept;
    void (*clear)(ID3D11DeviceContext*, const void*, ID3D11RenderTargetView*) noexcept;
    uint64_t (*viewConsume)(uintptr_t, int32_t, const void*) noexcept;
    void (*viewRefresh)(uintptr_t, uintptr_t, uintptr_t) noexcept;
};
inline std::atomic<const TraceObserver*> traceObserver{nullptr};
inline std::atomic<uint64_t> traceNativeSession{0};

inline bool traceEnabled() noexcept { return traceObserver.load(std::memory_order_acquire) != nullptr; }
inline void traceSample(TraceSample sample) noexcept {
    if (auto* observer = traceObserver.load(std::memory_order_acquire)) observer->sample(sample);
}
inline void traceCommandList(ID3D11DeviceContext* context, ID3D11CommandList* list) noexcept {
    traceSample({TraceEvent::CommandList, reinterpret_cast<uintptr_t>(context), reinterpret_cast<uintptr_t>(list)});
}
inline void traceRenderTargets(ID3D11DeviceContext* context, unsigned count, ID3D11RenderTargetView* const* views) noexcept {
    if (auto* observer = traceObserver.load(std::memory_order_acquire)) observer->targets(context, count, views);
}

// Call at the actual original D3D issue with the game thunk's caller. Disabled
// captures and zero-work draws do not query the D3D context or acquire a view.
inline void traceDrawIssued(ID3D11DeviceContext* ctx, const void* caller, unsigned count, unsigned instances) noexcept {
    if (auto* observer = traceObserver.load(std::memory_order_acquire); observer && count && instances)
        observer->draw(ctx, caller, count, instances);
}
inline void traceCopyIssued(ID3D11DeviceContext* ctx, const void* caller, ID3D11Resource* dst, ID3D11Resource* src) noexcept {
    if (auto* observer = traceObserver.load(std::memory_order_acquire)) observer->copy(ctx, caller, dst, src);
}
inline void traceClearIssued(ID3D11DeviceContext* ctx, const void* caller, ID3D11RenderTargetView* view) noexcept {
    if (auto* observer = traceObserver.load(std::memory_order_acquire)) observer->clear(ctx, caller, view);
}

// Only the validated game Present owner reads configuration and ends a window.
// Begin returns an observation ticket that pairs the actual backbuffer before the
// call with its result after return, including rejected, test or proxy Presents.
void updateFrameTrace();
uint64_t tracePresentBegin(IDXGISwapChain* chain, const void* caller, unsigned flags, bool proxy);
void tracePresentEnd(IDXGISwapChain* chain, uint64_t ticket, long result, bool owner);

} // namespace edvr::native_reflex

#pragma once

// Temporary passive measurement of contiguous draws that might share one
// fine-motion replay. No D3D calls, resource retention or validation claim.
#include "../common/log.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <windows.h>

struct ID3D11DeviceContext;

namespace edvr {

enum class VrsCohortBreak : unsigned {
    Frame, OtherDraw, PairIdentity, Guard, Target, InputBuffer, TextureWrite,
    Pipeline, Query, CommandList, MotionRead, MotionWrite, Unmap, Uncertain, Count
};

struct VrsCohortPair {
    unsigned eye;
    const void *color, *depth, *slots, *skin, *vertex, *fine, *blend;
    bool guarded, skinBound, copied;

    bool sameTarget(const VrsCohortPair& b) const noexcept {
        return eye == b.eye && color == b.color && depth == b.depth && slots == b.slots && skin == b.skin;
    }
    bool samePipeline(const VrsCohortPair& b) const noexcept {
        return vertex == b.vertex && fine == b.fine && blend == b.blend && skinBound == b.skinBound;
    }
};

namespace vrs_cohort_probe {
constexpr uint64_t kFrames = 300;
constexpr ULONGLONG kMaxWaitMs = 30000;
constexpr unsigned kBuckets = 7;

struct Histogram {
    std::array<uint64_t, kBuckets> runs{}, draws{};
    uint64_t largest = 0;

    void note(uint64_t length) noexcept {
        if (!length) return;
        const unsigned bin = length == 1 ? 0 : length == 2 ? 1 : length <= 4 ? 2 :
                             length <= 8 ? 3 : length <= 16 ? 4 : length <= 32 ? 5 : 6;
        ++runs[bin];
        draws[bin] += length;
        if (length > largest) largest = length;
    }
};

// The owner thread updates the run. Hooks on another context or thread only
// set uncertainty, consumed on the next owned event. No identities are logged.
struct Probe {
    std::atomic<ID3D11DeviceContext*> owner{nullptr};
    std::atomic<DWORD> thread{0};
    std::atomic<bool> requested{false}, live{false}, uncertain{false}, finished{false};
    VrsCohortPair last{};
    Histogram upper[2], observed[2];
    std::array<uint64_t, static_cast<unsigned>(VrsCohortBreak::Count)> breaks{};
    uint64_t upperLength = 0, observedLength = 0;
    uint64_t frame = 0, firstFrame = 0, frames = 0, pairs = 0, copies = 0, guards = 0, skin = 0;
    ULONGLONG deadline = 0;
    bool drawPending = false;

    bool owns(ID3D11DeviceContext* ctx) const noexcept {
        return owner.load(std::memory_order_relaxed) == ctx &&
               thread.load(std::memory_order_relaxed) == GetCurrentThreadId();
    }
    void closeObserved(VrsCohortBreak reason) noexcept {
        if (!observedLength) return;
        observed[last.eye].note(observedLength);
        observedLength = 0;
        ++breaks[static_cast<unsigned>(reason)];
    }
    void closeRun(VrsCohortBreak reason) noexcept {
        closeObserved(reason);
        if (upperLength) upper[last.eye].note(upperLength);
        upperLength = 0;
    }
    void closeForeign() noexcept {
        if (uncertain.exchange(false, std::memory_order_relaxed)) closeRun(VrsCohortBreak::Uncertain);
    }
    void report(const char* stop) {
        constexpr const char* reasons[] = {"frame", "other_draw", "pair_identity", "guard",
            "target", "input_buffer", "texture_write", "pipeline", "query", "command_list",
            "motion_read", "motion_write", "unmap", "uncertain"};
        static_assert(std::size(reasons) == static_cast<unsigned>(VrsCohortBreak::Count), "probe reason names");
        auto& log = Log::get();
        log.noteReserved("VRS cohort probe: stop=%s frames=%llu first=%llu last=%llu pairs=%llu copies=%llu guards=%llu skin_target_bound=%llu gpu_samples=0 replay_certified=0",
            stop, frames, firstFrame, frame, pairs, copies, guards, skin);
        for (unsigned eye = 0; eye < 2; ++eye) for (unsigned observedOnly = 0; observedOnly < 2; ++observedOnly) {
            const auto& h = observedOnly ? observed[eye] : upper[eye];
            log.noteReserved("VRS cohort histogram: eye=%u kind=%s bins=1,2,3-4,5-8,9-16,17-32,33+ "
                "runs=%llu,%llu,%llu,%llu,%llu,%llu,%llu draws=%llu,%llu,%llu,%llu,%llu,%llu,%llu max=%llu",
                eye, observedOnly ? "observed_boundaries" : "adjacent_pairs",
                h.runs[0], h.runs[1], h.runs[2], h.runs[3], h.runs[4], h.runs[5], h.runs[6],
                h.draws[0], h.draws[1], h.draws[2], h.draws[3], h.draws[4], h.draws[5], h.draws[6], h.largest);
        }
        for (unsigned i = 0; i < std::size(reasons); ++i)
            if (breaks[i]) log.noteReserved("VRS cohort break: reason=%s runs=%llu", reasons[i], breaks[i]);
    }
};

inline Probe probe;
} // namespace vrs_cohort_probe

inline void vrsCohortConfigure(bool enabled) noexcept {
    vrs_cohort_probe::probe.requested.store(enabled, std::memory_order_relaxed);
}

inline bool vrsCohortActive(ID3D11DeviceContext* ctx) noexcept {
    const auto& p = vrs_cohort_probe::probe;
    return p.live.load(std::memory_order_acquire) && p.owns(ctx);
}

inline bool vrsCohortTracking() noexcept {
    return vrs_cohort_probe::probe.live.load(std::memory_order_relaxed);
}

inline void vrsCohortNoteBreak(ID3D11DeviceContext* ctx, VrsCohortBreak reason) noexcept {
    auto& p = vrs_cohort_probe::probe;
    if (!p.live.load(std::memory_order_acquire)) return;
    if (!p.owns(ctx)) {
        p.uncertain.store(true, std::memory_order_relaxed);
        return;
    }
    p.closeForeign();
    if (reason == VrsCohortBreak::OtherDraw || reason == VrsCohortBreak::Frame ||
        reason == VrsCohortBreak::CommandList)
        p.closeRun(reason);
    else
        p.closeObserved(reason);
}

inline void vrsCohortNoteDraw(ID3D11DeviceContext* ctx) noexcept {
    auto& p = vrs_cohort_probe::probe;
    if (!p.live.load(std::memory_order_acquire)) return;
    if (!p.owns(ctx)) {
        p.uncertain.store(true, std::memory_order_relaxed);
        return;
    }
    p.closeForeign();
    if (p.drawPending) p.closeRun(VrsCohortBreak::OtherDraw);
    p.drawPending = true;
}

inline void vrsCohortNotePair(ID3D11DeviceContext* ctx, const VrsCohortPair& pair) noexcept {
    auto& p = vrs_cohort_probe::probe;
    if (pair.eye >= 2 || !vrsCohortActive(ctx)) return;
    p.closeForeign();
    if (!p.drawPending) p.closeRun(VrsCohortBreak::Uncertain);
    p.drawPending = false;
    if (!p.firstFrame) {
        p.firstFrame = p.frame;
        p.deadline = GetTickCount64() + vrs_cohort_probe::kMaxWaitMs;
    }
    if (p.upperLength && !pair.sameTarget(p.last)) p.closeRun(VrsCohortBreak::PairIdentity);
    if (pair.guarded) p.closeRun(VrsCohortBreak::Guard);
    if (p.observedLength && (!pair.samePipeline(p.last) || pair.copied))
        p.closeObserved(VrsCohortBreak::PairIdentity);
    p.last = pair;
    ++p.upperLength;
    ++p.observedLength;
    ++p.pairs;
    p.copies += pair.copied;
    p.guards += pair.guarded;
    p.skin += pair.skinBound;
    if (pair.guarded) p.closeRun(VrsCohortBreak::Guard);
}

// One session window. Wait for actual paired draws before spending its frame
// budget, and report only at a frame boundary (never inside an issue hook).
inline void vrsCohortFrame(ID3D11DeviceContext* ctx, uint64_t nextFrame, bool vrsLive) {
    auto& p = vrs_cohort_probe::probe;
    if (!p.live.load(std::memory_order_acquire)) {
        if (!ctx || !nextFrame || !vrsLive || p.finished.load(std::memory_order_relaxed) ||
            !p.requested.load(std::memory_order_relaxed)) return;
        p.owner.store(ctx, std::memory_order_relaxed);
        p.thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
        p.frame = nextFrame;
        p.deadline = GetTickCount64() + vrs_cohort_probe::kMaxWaitMs;
        p.live.store(true, std::memory_order_release);
        return;
    }
    if (!p.owns(ctx) || p.frame == nextFrame) return;
    p.closeForeign();
    p.closeRun(p.drawPending ? VrsCohortBreak::OtherDraw : VrsCohortBreak::Frame);
    p.drawPending = false;
    if (p.firstFrame) ++p.frames;
    const char* stop = !vrsLive || !p.requested.load(std::memory_order_relaxed) ? "disabled" :
                       p.frames >= vrs_cohort_probe::kFrames ? "complete" :
                       GetTickCount64() >= p.deadline ? "timeout" : nullptr;
    if (stop) {
        p.live.store(false, std::memory_order_release);
        if (p.firstFrame || vrsLive) {
            p.finished.store(true, std::memory_order_relaxed);
            p.report(stop);
        }
        return;
    }
    p.frame = nextFrame;
}

} // namespace edvr

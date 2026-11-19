// Bounded F213 scene-row capture. The Present owner opens and closes a frame.
// Render publications reserve a slot for that exact frame. They never wait for
// the recorder or consume the next frame's allowance.
#pragma once
#include <atomic>
#include <cstdint>

namespace edvr::onfootrec {

class SceneEyeWindow {
public:
    static constexpr uint32_t frameCount = 32;
    static constexpr unsigned perEye = 2; // first publication and F130 replacement
    struct Receipt {
        uint32_t windowFirst = 0;
        unsigned ordinal = 0; // one-based; zero declines the observation
    };
    struct End {
        uint32_t first, frame;
        unsigned left, right, omitted;
        bool skippedBoundary;
    };

    // Call after the recorder updates its existing detail/on-foot gate. Arm
    // once per rising edge; the next 32 consecutive frame IDs bound the cost.
    void open(uint32_t frame, bool active) noexcept {
        if (!active) { enabled_ = false; return; }
        if (!enabled_) { first_ = frame; enabled_ = true; spent_ = false; }
        if (spent_ || slot_.load(std::memory_order_relaxed)) return;
        const uint32_t index = frame - first_;
        if (index >= frameCount) { spent_ = true; return; }
        const uint64_t value = (uint64_t(frame) << 32) | openBit | (uint64_t(index) << 16);
        slot_.store(value, std::memory_order_release);
    }

    // Called before publishing the incremented Present counter or disabling
    // the recorder. Missing boundary IDs within this window are marked
    // separately from an observed frame with no scene publication.
    template<class Emit> void closeBefore(uint32_t nextFrame, Emit emit) {
        const auto observed = slot_.load(std::memory_order_relaxed);
        if (!observed || uint32_t(observed >> 32) == nextFrame) return;
        const auto value = slot_.exchange(0, std::memory_order_acq_rel);
        const uint32_t frame = uint32_t(value >> 32);
        emit(End{first_, frame, unsigned(value & 3u), unsigned((value >> 2) & 3u),
                 unsigned((value >> 4) & 3u), false});
        const uint32_t limit = nextFrame - first_;
        for (uint32_t index = frame - first_ + 1; index < frameCount && index < limit; ++index)
            emit(End{first_, first_ + index, 0, 0, 0, true});
    }

    Receipt reserve(uint32_t frame, unsigned eye) noexcept {
        if (eye > 1) return {};
        auto value = slot_.load(std::memory_order_acquire);
        while ((value & openBit) && uint32_t(value >> 32) == frame) {
            const unsigned shift = 2 * eye;
            const unsigned count = unsigned((value >> shift) & 3u);
            const uint64_t next = count < perEye ? value + (uint64_t(1) << shift) : value | (uint64_t(1) << (4 + eye));
            if (next == value) return {};
            if (slot_.compare_exchange_weak(value, next, std::memory_order_acq_rel)) {
                if (count == perEye) return {};
                return {frame - uint32_t((value >> 16) & 31u), count + 1};
            }
        }
        return {};
    }

private:
    static constexpr uint64_t openBit = uint64_t(1) << 31;
    std::atomic<uint64_t> slot_{0};
    uint32_t first_ = 0; // Present owner only
    bool enabled_ = false, spent_ = false;
};
} // namespace edvr::onfootrec

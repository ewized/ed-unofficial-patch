#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace edvr::native_vrs {

struct WitnessDepthStencilTuple {
    uint32_t depthEnable, depthWriteMask, depthFunc;
    uint32_t stencilEnable, stencilReadMask, stencilWriteMask;
    std::array<uint32_t, 4> front, back; // comparison, fail, depth fail, pass
    uint32_t stencilRef;

    bool operator==(const WitnessDepthStencilTuple& b) const noexcept {
        return depthEnable == b.depthEnable && depthWriteMask == b.depthWriteMask && depthFunc == b.depthFunc &&
               stencilEnable == b.stencilEnable && stencilReadMask == b.stencilReadMask && stencilWriteMask == b.stencilWriteMask &&
               front == b.front && back == b.back && stencilRef == b.stencilRef;
    }
};

// One window per process. Only request() and reportReady() may run off the
// render owner. Off seals a started window even if On arrives before reporting.
class WitnessDepthCopyCensus {
public:
    static constexpr unsigned copyLimit = 8192, rowLimit = 32;
    struct Row { WitnessDepthStencilTuple key; unsigned copies; };

    void request(bool enabled) noexcept {
        if (enabled) {
            auto state = State::Off;
            request_.compare_exchange_strong(state, State::Armed, std::memory_order_relaxed);
            return;
        }
        auto state = request_.load(std::memory_order_relaxed);
        while (state == State::Armed || state == State::Collecting) {
            const auto next = state == State::Armed ? State::Off : State::Stopped;
            if (request_.compare_exchange_weak(state, next, std::memory_order_relaxed)) return;
        }
    }

    bool captureDescriptor() const noexcept {
        const auto state = request_.load(std::memory_order_relaxed);
        return state == State::Armed || state == State::Collecting;
    }

    // Record at the real copy site, even if the later material issue refuses.
    // An absent tuple is null/default if observed as such, unknown otherwise.
    void record(const WitnessDepthStencilTuple* key, bool nullState) noexcept {
        auto state = request_.load(std::memory_order_relaxed);
        if (state == State::Armed && request_.compare_exchange_strong(state, State::Collecting, std::memory_order_relaxed))
            state = State::Collecting;
        if (state != State::Collecting) return;
        if (++sampled == copyLimit) request_.store(State::Complete, std::memory_order_relaxed);
        if (!key) {
            if (nullState) ++defaultNull;
            else ++unknown;
            return;
        }
        for (unsigned i = 0; i < rowCount; ++i) {
            if (*key == rows[i].key) { ++rows[i].copies; return; }
        }
        if (rowCount < rowLimit) rows[rowCount++] = {*key, 1};
        else ++overflow;
    }

    bool reportReady() const noexcept {
        const auto state = request_.load(std::memory_order_relaxed);
        return state == State::Complete || state == State::Stopped;
    }
    bool complete() const noexcept { return sampled == copyLimit; }
    void markReported() noexcept { request_.store(State::Reported, std::memory_order_relaxed); }

    unsigned sampled = 0, rowCount = 0, defaultNull = 0, unknown = 0, overflow = 0;
    std::array<Row, rowLimit> rows{};

private:
    enum class State : uint8_t { Off, Armed, Collecting, Stopped, Complete, Reported };
    std::atomic<State> request_{State::Off};
};

} // namespace edvr::native_vrs

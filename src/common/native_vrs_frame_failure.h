#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace edvr {

// A single-use temporal acquisition has one record. Pack the source sequence
// and both eye bits together so a delayed draw cannot mark a newer sequence
// between the identity check and the write. 0 is closed/unpublished.
class NativeVrsFrameFailure {
public:
    void begin(uint64_t sequence) noexcept {word_.store(key(sequence),std::memory_order_release);}
    void end() noexcept {word_.store(0,std::memory_order_release);}
    uint32_t retire(uint64_t sequence) noexcept {
        // Withdraw the old frame and collect its bits together. A callback
        // cannot land after this history decision but before withdrawal.
        const auto observed=word_.exchange(0,std::memory_order_acq_rel);
        return eyesFor(sequence,observed);
    }

    bool clean(uint64_t sequence) const noexcept {
        const auto wanted=key(sequence);
        return wanted&&word_.load(std::memory_order_acquire)==wanted;
    }

    bool mark(uint64_t sequence,unsigned eye) noexcept {
        const auto wanted=key(sequence);
        if(!wanted||eye>1)return false;
        const uint64_t bit=uint64_t{1}<<eye;
        auto observed=word_.load(std::memory_order_acquire);
        while((observed&~eyeMask)==wanted) {
            if((observed&bit)||word_.compare_exchange_strong(observed,observed|bit,
                std::memory_order_acq_rel,std::memory_order_acquire))return true;
        }
        return false;
    }

    uint32_t failedEyes(uint64_t sequence) const noexcept {
        return eyesFor(sequence,word_.load(std::memory_order_acquire));
    }

private:
    static constexpr uint64_t eyeMask=3;
    static constexpr uint64_t key(uint64_t sequence) noexcept {
        return sequence<=((std::numeric_limits<uint64_t>::max)()>>2)?sequence<<2:0;
    }
    static constexpr uint32_t eyesFor(uint64_t sequence,uint64_t observed) noexcept {
        const auto wanted=key(sequence);
        return wanted&&(observed&~eyeMask)==wanted?uint32_t(observed&eyeMask):0;
    }
    static_assert(std::atomic<uint64_t>::is_always_lock_free,"VRS failure is published under the engine lock");
    std::atomic<uint64_t> word_{0};
};
}

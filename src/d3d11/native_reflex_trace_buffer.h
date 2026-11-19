// A single bounded, non-recycling capture. Sealing freezes the exact reservation
// count while earlier writers finish their own slots. The reader never blocks
// the game thread waiting for another producer and never sees a partial row.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace edvr::native_reflex {

template<class Row, uint32_t Capacity>
class TraceBuffer {
    static constexpr uint64_t sealed = uint64_t(1) << 63;
    struct Slot { Row row{}; std::atomic<bool> ready{false}; };
    std::array<Slot, Capacity> slots_{};
    std::atomic<uint64_t> next_{0};
public:
    // Zero means the row was outside the window or beyond its capacity.
    uint64_t append(Row row) noexcept {
        auto index = next_.load(std::memory_order_relaxed);
        do {
            if (index & sealed) return 0;
        } while (!next_.compare_exchange_weak(index, index + 1, std::memory_order_relaxed));
        if (index >= Capacity) return 0;
        slots_[index].row = row;
        slots_[index].ready.store(true, std::memory_order_release);
        return index + 1;
    }
    uint64_t seal() noexcept { return next_.fetch_or(sealed, std::memory_order_relaxed) & ~sealed; }
    uint64_t count() const noexcept { return next_.load(std::memory_order_relaxed) & ~sealed; }
    bool readable(uint32_t index) const noexcept {
        return index < Capacity && slots_[index].ready.load(std::memory_order_acquire);
    }
    const Row& row(uint32_t index) const noexcept { return slots_[index].row; }
};

} // namespace edvr::native_reflex

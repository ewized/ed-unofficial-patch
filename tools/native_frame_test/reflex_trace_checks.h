#pragma once
#include "../../src/d3d11/native_reflex_trace_buffer.h"
#include "../../src/d3d11/native_reflex_engine_hook.h"
#include <thread>

namespace reflex_trace_checks {
inline unsigned issuedDraws = 0;
inline void noteDraw(ID3D11DeviceContext*, const void*, unsigned, unsigned) noexcept { ++issuedDraws; }

template<class Check> void run(Check check) {
    using namespace edvr::native_reflex;
    TraceObserver fake{};
    fake.draw = noteDraw;
    issuedDraws = 0;
    traceObserver.store(&fake, std::memory_order_release);
    traceDrawIssued(nullptr, nullptr, 0, 1);
    traceDrawIssued(nullptr, nullptr, 3, 0);
    check(issuedDraws == 0, "empty geometry or an empty instance range creates no draw witness");
    traceDrawIssued(nullptr, nullptr, 3, 1);
    check(issuedDraws == 1, "a nonempty forwarded draw reaches the capture observer");
    traceObserver.store(nullptr, std::memory_order_release);
    traceDrawIssued(nullptr, nullptr, 3, 1);
    check(issuedDraws == 1, "a sealed or disabled capture does not query draw targets");
    TraceAddresses<2> watched;
    check(!watched.find(0, true) && !watched.find(7), "unknown camera addresses stay unregistered on a lookup");
    std::thread registration([&] { watched.find(7, true); });
    const bool registered = watched.find(7, true);
    registration.join();
    check(registered && watched.find(7) && watched.find(8, true), "duplicate concurrent registrations preserve capacity");
    check(!watched.find(9, true) && watched.find(8) && watched.find(7), "a full capture preserves prior camera addresses");
    TraceBuffer<int, 2> limited;
    check(!limited.readable(0), "an unreserved trace slot is unreadable");
    check(limited.append(17) == 1 && limited.append(42) == 2, "trace tickets name distinct rows");
    check(limited.append(99) == 0 && limited.seal() == 3, "overflow preserves the exact attempted row count");
    check(limited.append(77) == 0 && limited.seal() == 3 && limited.row(1) == 42,
          "sealing prevents later callbacks changing the capture");

    // Hold a producer inside the actual slot assignment. Sealing must return
    // immediately; the final row can only be read after the producer publishes.
    struct PausedRow {
        int value = 0;
        std::atomic<unsigned>* phase = nullptr;
        PausedRow& operator=(const PausedRow& other) noexcept {
            if (other.phase) {
                other.phase->store(1, std::memory_order_release);
                while (other.phase->load(std::memory_order_acquire) == 1) std::this_thread::yield();
            }
            value = other.value;
            return *this;
        }
    };
    std::atomic<unsigned> phase{0};
    TraceBuffer<PausedRow, 1> slow;
    std::thread writer([&] { slow.append({37, &phase}); });
    while (!phase.load(std::memory_order_acquire)) std::this_thread::yield();
    check(slow.seal() == 1 && !slow.readable(0), "a reserved row at seal waits for publication without blocking");
    phase.store(2, std::memory_order_release);
    writer.join();
    check(slow.readable(0) && slow.row(0).value == 37, "the completed row remains readable after sealing");
}
}

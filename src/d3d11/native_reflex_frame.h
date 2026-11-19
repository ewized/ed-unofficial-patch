#pragma once
#include <cstdint>

namespace edvr::native_reflex {

// NV_LATENCY_MARKER_TYPE and marker placement:
// https://github.com/NVIDIA/nvapi/blob/main/nvapi.h (NvAPI_D3D_SetLatencyMarker).
// Callers serialize
// access. Simulation may start while an older native frame is still being
// rendered; an XR tick or Present can never start its own source frame.
enum class Marker : uint32_t { SimulationStart, SimulationEnd, RenderStart, RenderEnd, PresentStart, PresentEnd, InputSample };

class FrameEvents {
public:
    // sourceId identifies one native simulation, monotonically for the
    // device's lifetime. A failure consumes the ID: never sleep twice on
    // an ambiguous or partially recorded frame.
    template<class Sleep, class Mark>
    bool beforeInput(uint64_t sourceId, Sleep sleep, Mark mark) {
        if (!sourceId || sourceId <= newest_) return false;
        newest_ = sourceId;
        Slot* slot = nullptr;
        for (auto& candidate : slots_) if (candidate.phase == Phase::Idle) { slot = &candidate; break; }
        if (!slot || !sleep() || !mark(sourceId, Marker::SimulationStart)) return false;
        *slot = {sourceId, Phase::Simulation};
        return true;
    }

    template<class Mark>
    bool event(uint64_t sourceId, Marker marker, Mark mark) {
        Slot* slot = nullptr;
        for (auto& candidate : slots_) if (candidate.id == sourceId && candidate.phase != Phase::Idle) { slot = &candidate; break; }
        if (!slot) return false;
        const Phase phase = slot->phase;
        Phase next = Phase::Idle;
        switch (marker) {
        case Marker::InputSample: if (phase != Phase::Simulation) return false; next = Phase::Sampled; break;
        case Marker::SimulationEnd: if (phase != Phase::Simulation && phase != Phase::Sampled) return false; next = Phase::Simulated; break;
        case Marker::RenderStart: if (phase != Phase::Simulated) return false; next = Phase::Rendering; break;
        case Marker::RenderEnd: if (phase != Phase::Rendering) return false; next = Phase::Rendered; break;
        case Marker::PresentStart: if (phase != Phase::Rendered) return false; next = Phase::Presenting; break;
        case Marker::PresentEnd: if (phase != Phase::Presenting) return false; break;
        default: return false;
        }
        if (!mark(sourceId, marker)) { slot->phase = Phase::Idle; return false; }
        slot->phase = next;
        return true;
    }

    // Drop incomplete receipts without ever recycling a source ID.
    void invalidate() noexcept { for (auto& slot : slots_) slot = {}; }
private:
    enum class Phase { Idle, Simulation, Sampled, Simulated, Rendering, Rendered, Presenting };
    struct Slot { uint64_t id = 0; Phase phase = Phase::Idle; };
    Slot slots_[8]{};
    uint64_t newest_ = 0;
};

}

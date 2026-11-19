#pragma once

#include <array>
#include <cstdint>

namespace edvr::native_vrs {

// Direct draw arguments own their values and can outlive the intercepted
// call. DrawAuto and indirect draws depend on GPU state and are never queued.
struct WitnessDraw {
    enum class Kind : uint8_t { Unknown, Vertices, Indexed, Instanced, IndexedInstanced };

    Kind kind = Kind::Unknown;
    uint32_t count = 0, instances = 0, start = 0;
    int32_t baseVertex = 0;
    uint32_t startInstance = 0;

    static constexpr WitnessDraw vertices(uint32_t count, uint32_t start) noexcept {
        return {Kind::Vertices, count, 1, start, 0, 0};
    }
    static constexpr WitnessDraw indexed(uint32_t count, uint32_t start, int32_t base) noexcept {
        return {Kind::Indexed, count, 1, start, base, 0};
    }
    static constexpr WitnessDraw instanced(uint32_t count, uint32_t instances, uint32_t start,
                                           uint32_t firstInstance) noexcept {
        return {Kind::Instanced, count, instances, start, 0, firstInstance};
    }
    static constexpr WitnessDraw indexedInstanced(uint32_t count, uint32_t instances, uint32_t start,
                                                  int32_t base, uint32_t firstInstance) noexcept {
        return {Kind::IndexedInstanced, count, instances, start, base, firstInstance};
    }
    constexpr bool valid() const noexcept {
        if (!count || !instances) return false;
        switch (kind) {
            case Kind::Vertices:
            case Kind::Indexed: return instances == 1;
            case Kind::Instanced:
            case Kind::IndexedInstanced: return true;
            default: return false;
        }
    }
};

// Call the adapter's original D3D entry points. The replay adapter owns the
// state and interception guard; no nested engine bookkeeping belongs here.
template<class Api>
void issueWitnessDraw(Api& api, const WitnessDraw& draw) {
    switch (draw.kind) {
        case WitnessDraw::Kind::Vertices: api.Draw(draw.count, draw.start); break;
        case WitnessDraw::Kind::Indexed: api.DrawIndexed(draw.count, draw.start, draw.baseVertex); break;
        case WitnessDraw::Kind::Instanced:
            api.DrawInstanced(draw.count, draw.instances, draw.start, draw.startInstance); break;
        case WitnessDraw::Kind::IndexedInstanced:
            api.DrawIndexedInstanced(draw.count, draw.instances, draw.start, draw.baseVertex,
                                     draw.startInstance); break;
        default: break; // A cohort never accepts an unknown draw.
    }
}

// The native owner supplies an immutable state receipt with retained graphics
// resources. Generations change before writes and setters; the queue does not
// prove that a complete set of barriers has been installed.
struct WitnessCohortKey {
    const void *context = nullptr, *eyeTarget = nullptr, *depthTarget = nullptr, *state = nullptr;
    uint64_t frame = 0, inputs = 0, pipeline = 0;
    unsigned eye = 2;

    bool valid() const noexcept {
        return context && eyeTarget && depthTarget && state && frame && inputs && pipeline && eye < 2;
    }
    bool operator==(const WitnessCohortKey& b) const noexcept {
        return context == b.context && eyeTarget == b.eyeTarget && depthTarget == b.depthTarget &&
               state == b.state && frame == b.frame && inputs == b.inputs && pipeline == b.pipeline && eye == b.eye;
    }
};

enum class WitnessCohortAppend { Appended, Drain, Refused };

// Holds one proven contiguous run. On Drain, replay the old run before
// retrying or forwarding the new call; neither failure result mutates it.
class WitnessCohort {
public:
    static constexpr unsigned capacity = 64;

    WitnessCohortAppend append(const WitnessCohortKey& key, const WitnessDraw& draw) noexcept {
        if (!key.valid() || !draw.valid())
            return empty() ? WitnessCohortAppend::Refused : WitnessCohortAppend::Drain;
        if (!empty() && (!(key_ == key) || count_ == capacity)) return WitnessCohortAppend::Drain;
        if (empty()) key_ = key;
        draws_[count_++] = draw;
        return WitnessCohortAppend::Appended;
    }

    bool empty() const noexcept { return count_ == 0; }
    unsigned size() const noexcept { return count_; }
    const WitnessDraw* data() const noexcept { return draws_.data(); }
    const WitnessCohortKey& key() const noexcept { return key_; }

    // Owner calls after all original material draws and their exact witness
    // have issued, or after verified combined full-rate fallback.
    void retire() noexcept {
        count_ = 0;
        key_ = {};
    }

private:
    WitnessCohortKey key_{};
    std::array<WitnessDraw, capacity> draws_{};
    unsigned count_ = 0;
};

} // namespace edvr::native_vrs

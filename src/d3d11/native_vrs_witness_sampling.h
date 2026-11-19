#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace edvr::native_vrs {

enum class WitnessRoute : unsigned { SyncGuard, SyncOther, Single, Small, Medium, Large, Count };
inline WitnessRoute witnessRoute(bool cohort,bool guarded,unsigned length) noexcept {
    if (!cohort) return guarded ? WitnessRoute::SyncGuard : WitnessRoute::SyncOther;
    if (length == 1) return WitnessRoute::Single;
    if (length <= 8) return WitnessRoute::Small;
    return length <= 32 ? WitnessRoute::Medium : WitnessRoute::Large;
}

// Each frame chooses three distinct strata, then one random occurrence in each.
// The preceding frame sets a padded upper bound for each stratum's rank. A
// growing scene can exceed it; those events are counted separately, never
// extrapolated. Every in-range event has known inclusion probability.
struct WitnessSamplePlan {
    static constexpr unsigned strata = 2 * unsigned(WitnessRoute::Count), perFrame = 3;
    struct Row { unsigned seen=0, extent=0, selected=0; };
    std::array<Row,strata> rows{};
    uint32_t random=0x92c21bd7;
    bool pilot=true;

    uint32_t next() noexcept {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        return random;
    }
    void advance() noexcept {
        const unsigned first=next()%strata;
        for(unsigned i=0;i<strata;++i) {
            auto& r=rows[i];
            // The D3D11 direct-draw index and frame counters are 32 bits.
            r.extent=unsigned((std::min)(uint64_t(0x7fffffffu),uint64_t(r.seen)+r.seen/2+8));
            r.seen=0;
            r.selected=((i+strata-first)%strata<perFrame) ? 1+next()%r.extent : 0;
        }
        pilot=false;
    }
    struct Pick { uint64_t weight=0; bool outside=false; };
    Pick event(unsigned stratum) noexcept {
        auto& r=rows[stratum];
        ++r.seen;
        if(pilot)return {};
        return {r.seen==r.selected ? uint64_t(r.extent)*(strata/perFrame) : 0,
                r.seen>r.extent};
    }
};

constexpr bool witnessDepthOnlyReads(bool depthEnabled,bool depthWrites,bool readOnlyDepth,
                                     bool stencilEnabled,bool stencilMask,bool changesStencil,
                                     bool readOnlyStencil,bool hasStencil) noexcept {
    return !(depthEnabled && depthWrites && !readOnlyDepth) &&
           !(hasStencil && stencilEnabled && stencilMask && changesStencil && !readOnlyStencil);
}

} // namespace edvr::native_vrs

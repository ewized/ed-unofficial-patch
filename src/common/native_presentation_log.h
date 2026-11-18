#pragma once
#include "native_presentation.h"

namespace edvr {
// Match the readout's receiver-clock freshness rule. Missing capability and
// invalid/future packet identity remain unavailable, never zero-rate samples.
inline bool nativePresentationPacketFresh(const NativePresentationSnapshot& sample,uint64_t now) noexcept {
    return sample.active&&sample.available&&sample.generation&&sample.context&&sample.sequence&&
        sample.capturedAtMs&&now>=sample.capturedAtMs&&now-sample.capturedAtMs<=2000;
}

// Render-thread log cadence, independent of menu visibility. First observation,
// capability changes and session/context changes are immediate; otherwise even
// unchanged or stale packets get one heartbeat per second. Disabled observation
// resets the cadence so re-enabling cannot hide its first packet.
class NativePresentationLogCadence {
    bool started_=false,active_=false,available_=false;
    uint64_t generation_=0,context_=0,lastMs_=0;
public:
    void reset() noexcept {*this={};}
    bool due(bool enabled,const NativePresentationSnapshot& sample,uint64_t now) noexcept {
        if(!enabled){reset();return false;}
        const bool changed=!started_||sample.active!=active_||sample.available!=available_||
            sample.generation!=generation_||sample.context!=context_;
        if(!changed&&now>=lastMs_&&now-lastMs_<1000)return false;
        started_=true;active_=sample.active;available_=sample.available;
        generation_=sample.generation;context_=sample.context;lastMs_=now;
        return true;
    }
};
}

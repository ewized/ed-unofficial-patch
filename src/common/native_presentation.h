#pragma once
#include <stdint.h>

#define EDVR_NATIVE_PRESENTATION_VERSION_1 1u

// Cumulative app xrEndFrame observations, once per stereo submission, never
// per eye or produced batch. They do not prove compositor scanout. All six
// categories partition sequence; a failed call cannot count generated pixels.
struct EdvrNativePresentation {
    uint32_t size,version;
    uint64_t generation,sequence;
    uint64_t nativeFrames,generatedFrames,repeatedFrames,emptyFrames,failedFrames,backgroundFrames;
    uint32_t fgEnabled,activeMultiplier; // 0=native cadence, otherwise 2 or 4
};
static_assert(sizeof(EdvrNativePresentation)==80,"native presentation v1 ABI");

namespace edvr {
enum class NativePresentationKind {Native,Generated,Repeated,Empty,Failed,Background};
// Owned by the XR thread. Rebind resets only for a new runtime generation;
// cumulative snapshots recover counts even if an optional publication is lost.
inline bool recordNativePresentation(EdvrNativePresentation& out,uint64_t generation,
    NativePresentationKind kind,bool enabled,uint32_t multiplier) noexcept {
    if(!generation||unsigned(kind)>unsigned(NativePresentationKind::Background))return false;
    if(out.generation!=generation){
        out={};out.size=sizeof(out);out.version=EDVR_NATIVE_PRESENTATION_VERSION_1;out.generation=generation;
    }
    if(out.sequence==UINT64_MAX)return false;
    ++out.sequence;
    switch(kind){
    case NativePresentationKind::Native:++out.nativeFrames;break;
    case NativePresentationKind::Generated:++out.generatedFrames;break;
    case NativePresentationKind::Repeated:++out.repeatedFrames;break;
    case NativePresentationKind::Empty:++out.emptyFrames;break;
    case NativePresentationKind::Failed:++out.failedFrames;break;
    case NativePresentationKind::Background:++out.backgroundFrames;break;
    }
    out.fgEnabled=enabled?1u:0u;
    out.activeMultiplier=enabled&&(multiplier==2||multiplier==4)?multiplier:0u;
    return true;
}

// Graphics-side observation. capturedAtMs and context are receiver-owned.
// available means a packet arrived; consumers age capturedAtMs and reset rate
// baselines on generation/context changes. Missing capability stays unavailable.
struct NativePresentationSnapshot {
    bool active=false,available=false;
    uint64_t generation=0,context=0,capturedAtMs=0,sequence=0;
    uint64_t nativeFrames=0,generatedFrames=0,repeatedFrames=0,emptyFrames=0,failedFrames=0,backgroundFrames=0;
    bool fgEnabled=false;
    uint32_t activeMultiplier=0;
};
}

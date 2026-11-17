#pragma once
#include <cstdint>

struct ID3D11Device;
namespace edvr {
// Owned by native_temporal's existing mutex. A treated/skipped eye is not a
// successful output; only two real outputs from the same frame open this gate.
struct NativeGazeReadiness {
    uint64_t epoch=0,frame=0,pairSequence=0;
    unsigned successfulEyes=0;
    bool pairReady=false;
    void reset(uint64_t next) noexcept {epoch=next;frame=pairSequence=0;successfulEyes=0;pairReady=false;}
    void begin(uint64_t sequence) noexcept {frame=sequence;successfulEyes=0;}
    void succeeded(uint64_t sequence,unsigned eye) noexcept {
        if(!epoch||!frame||sequence!=frame||eye>1)return;
        successfulEyes|=1u<<eye;
        if(successfulEyes==3){pairReady=true;pairSequence=sequence;}
    }
};
struct NativeGazeReadySnapshot {uint64_t epoch=0,pairSequence=0;bool ready=false;};
// Identity checks and publication belong to the native temporal channel.
NativeGazeReadySnapshot nativeTemporalGazeReadiness(ID3D11Device*,unsigned long);
// Zero whenever the successful stereo milestone is revoked. Lock-free draw
// check; this is not a frame sequence or a guessed readiness timeout.
uint64_t nativeTemporalGazeEpoch() noexcept;
// The callback runs synchronously under that owner's mutex, serializing our
// invalidate/close. It must not call nativeTemporal*, XR, D3D or context APIs.
// It must contain SEH internally so a provider fault cannot strand the mutex.
using NativeGazeReadyCallback=void(*)(void*,uint64_t);
bool nativeTemporalWithGazeReady(ID3D11Device*,unsigned long,uint64_t,
                                NativeGazeReadyCallback,void*);
}

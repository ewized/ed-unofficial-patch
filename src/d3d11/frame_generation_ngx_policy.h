#pragma once
#include "../common/native_fg_multiplier.h"
#include <cmath>
#include <cstdint>
#include <limits>

namespace edvr::fg_ngx {
constexpr unsigned kSlots=3,kEyes=2,kMaxGenerated=3;
constexpr uint32_t kMaxTimeoutMs=5000;
constexpr uint32_t kResetFirstSource=1,kResetSequenceGap=2,kResetGenerationChanged=4,kResetCaller=8,kResetUnknownGeneration=16;
inline uint32_t resetHistoryReasons(uint64_t previous,uint64_t current,uint64_t priorGeneration,uint64_t generation,bool requested)noexcept {
    uint32_t reasons=0;
    if(!previous)reasons|=kResetFirstSource;
    if(previous&&current!=previous+1)reasons|=kResetSequenceGap;
    if(generation!=priorGeneration)reasons|=kResetGenerationChanged;
    if(!generation)reasons|=kResetUnknownGeneration;
    if(requested)reasons|=kResetCaller;
    return reasons;
}
inline bool suppressionAllows(const uint8_t* raw)noexcept {return raw&&raw[0]==0;}
enum class SuppressionState {CopyDestination,UnorderedAccess,CopySource};
inline unsigned suppressionOwner(bool sharedNativePair,unsigned index)noexcept {return sharedNativePair?0:index;}
template<class Transition,class Initialize>
void prepareSuppressionFlag(bool sharedNativePair,unsigned index,bool reused,Transition&& transition,Initialize&& initialize){
    if(sharedNativePair&&index){
        transition(SuppressionState::CopySource,SuppressionState::UnorderedAccess);
        return;
    }
    if(reused)transition(SuppressionState::CopySource,SuppressionState::CopyDestination);
    initialize();
    transition(SuppressionState::CopyDestination,SuppressionState::UnorderedAccess);
}
constexpr unsigned generatedCount(unsigned multiplier) noexcept {
    return nativeFgFixedMultiplier(multiplier) ? multiplier - 1 : 0;
}
struct Capability {
    unsigned maximumGenerated,maximumMultiplier;
};
inline Capability capability(bool queried,bool available,bool needsDriver,bool hasMaximum,unsigned reported) noexcept {
    // NGX predating MultiFrameCountMax supports one generated frame.
    const unsigned maximum=hasMaximum?reported:1u;
    return {maximum,!queried?0u:(!available||needsDriver?1u:1u+(maximum<3u?maximum:3u))};
}
inline bool supported(unsigned multiplier,bool available,bool needsDriver,unsigned maximum) noexcept {
    const auto count=generatedCount(multiplier);return count&&available&&!needsDriver&&maximum>=count;
}
inline bool completed(uint64_t actual,uint64_t expected) noexcept {
    return expected&&actual!=std::numeric_limits<uint64_t>::max()&&actual>=expected;
}
inline bool reusable(uint64_t actual,uint64_t expected,unsigned owners) noexcept {
    return (!expected||completed(actual,expected))&&owners<=1;
}
inline bool timedOut(uint64_t now,uint64_t submitted,uint32_t limit) noexcept {
    return now>=submitted&&now-submitted>=limit;
}
inline bool resetHistory(uint64_t previous,uint64_t current,uint64_t priorGeneration,uint64_t generation,bool requested) noexcept {
    return resetHistoryReasons(previous,current,priorGeneration,generation,requested)!=0;
}
inline bool finiteMatrix(const float (&m)[4][4]) noexcept {
    for(const auto& row:m)for(float value:row)if(!std::isfinite(value))return false;return true;
}
inline bool inversePair(const float (&a)[4][4],const float (&b)[4][4]) noexcept {
    if(!finiteMatrix(a)||!finiteMatrix(b))return false;
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){
        double value=0;for(unsigned k=0;k<4;++k)value+=double(a[y][k])*b[k][x];
        if(std::abs(value-(x==y?1.0:0.0))>.02)return false;
    }
    return true;
}
} // namespace edvr::fg_ngx

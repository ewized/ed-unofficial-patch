#pragma once
#include "vrs_coverage.h"
#include "../common/pvr_gaze_snapshot.h"
#include <array>
#include <cstdint>

namespace edvr { namespace vrs_work_reference {
inline bool consumeInternal(bool& marker) noexcept {const bool own=marker;marker=false;return own;}
enum class Arm : unsigned { Natural, Full, Coarse };
inline const char* armName(Arm arm) noexcept {return arm==Arm::Natural?"natural":arm==Arm::Full?"uniform_1x1":"uniform_2x2_calibration";}
inline unsigned uniformRate(Arm arm) noexcept {return arm==Arm::Coarse?8u:5u;}
inline bool selected(const VrsCoverageSignature& s) noexcept {
    return s.vs==vrs_coverage::kWorldVs&&s.ps==vrs_coverage::kWorldPsE46&&s.kind=='X'&&
        s.count==288&&s.instances==4&&s.start==14674580&&s.base==2491586;
}
inline bool modeOne(const EdvrPvrGazeSnapshotV1& s,uint64_t now,uint64_t frequency) noexcept {
    return s.size==sizeof(s)&&s.version==1&&s.sequence&&s.mode==1&&s.trackerPresent==1&&s.providerQueried==1&&s.providerFiniteValid==1&&s.cachedFallback==0&&s.ignored==0&&frequency&&s.qpcFrequency==frequency&&
        s.callbackQpc&&s.callbackQpc<=now&&double(now-s.callbackQpc)*1000000.0/double(frequency)<=50000.0;
}
struct Rate {
    bool parsed=false,enabled=false;
    unsigned count=0;
    std::array<uint32_t,16> palette{};
    bool supported() const noexcept {return parsed&&count<=1;}
    bool uniform(unsigned value) const noexcept {
        if(!supported())return false;
        if(!count||!enabled)return value==5;
        for(auto entry:palette)if(entry!=0&&entry!=value)return false;
        return true;
    }
    bool same(const Rate& other) const noexcept {
        return supported()&&other.supported()&&count==other.count&&(!count||(enabled==other.enabled&&palette==other.palette));
    }
};
// Descriptor copies are values: the forwarding adapter owns ABI pointers.
// Unsupported/concurrent requests pass through unchanged and poison evidence.
struct Override {
    Arm arm=Arm::Natural;
    Rate original{},lastOriginal{},applied{};
    unsigned nestedRates=0,unexpected=0;
    bool forced=false,restoreRequired=false,restoring=false,surrendered=false;
    bool internalWriteAllowed(unsigned writers) const noexcept {return !surrendered&&writers==0;}
    void start(Arm a,const Rate& initial) noexcept {
        *this={};arm=a;original=lastOriginal=initial;forced=a!=Arm::Natural;restoreRequired=forced;
    }
    Rate request(const Rate& requested,bool internal,bool ownerThread) noexcept {
        if(!internal){++nestedRates;
            if(!ownerThread){surrendered=true;forced=false;restoreRequired=false;++unexpected;return requested;}
            if(!ownerThread||!requested.supported()){++unexpected;return requested;}
        }
        Rate result=requested;
        if(forced&&!restoring&&!surrendered&&result.supported()&&result.count&&result.enabled)
            for(auto& entry:result.palette)if(entry!=0)entry=uniformRate(arm);
        return result;
    }
    void result(const Rate& actual,int status,const Rate* requested=nullptr,bool ownerThread=true) noexcept {
        applied=actual;
        if(requested&&status==0&&!surrendered){
            if(!ownerThread||!requested->supported()){
                // The original call succeeded unchanged. Its newer intent owns
                // the context; never overwrite it with our obsolete snapshot.
                surrendered=true;forced=false;restoreRequired=false;++unexpected;
            }else lastOriginal=*requested;
        }
        if(status!=0||!actual.supported()||(forced&&!restoring&&!actual.uniform(uniformRate(arm))))++unexpected;
    }
};
struct Plan {bool ready=false,expired=false;Arm arm=Arm::Natural;uint64_t cycle=0;};
struct Schedule {
    Arm arm[2]{Arm::Natural,Arm::Natural};
    uint64_t cycle[2]{},next[2]{},expires[2]{};
    uint32_t startInstance[2]{};
    Plan peek(unsigned eye,uint32_t instance,uint64_t now) noexcept {
        if(eye>=2||now<next[eye])return {};
        if(arm[eye]!=Arm::Natural&&now>expires[eye]){arm[eye]=Arm::Natural;next[eye]=now+3000;return {false,true,Arm::Natural,cycle[eye]};}
        if(arm[eye]!=Arm::Natural&&instance!=startInstance[eye])return {};
        return {true,false,arm[eye],cycle[eye]+(arm[eye]==Arm::Natural?1u:0u)};
    }
    void issued(unsigned eye,uint32_t instance,uint64_t now) noexcept {
        if(eye>=2)return;
        if(arm[eye]==Arm::Natural){++cycle[eye];startInstance[eye]=instance;expires[eye]=now+1000;arm[eye]=Arm::Full;}
        else if(arm[eye]==Arm::Full)arm[eye]=Arm::Coarse;
        else arm[eye]=Arm::Natural;
        next[eye]=now+(arm[eye]==Arm::Natural?3000:0);
    }
};
inline bool workMatches(uint64_t vertices,uint64_t primitives,uint64_t ps) noexcept {return vertices==1152&&primitives==384&&ps>0;}
} }

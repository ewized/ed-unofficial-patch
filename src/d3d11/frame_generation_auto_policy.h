#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace edvr::fg_auto {
enum class Reason : unsigned { NativeFirst, Measuring, NativeHeadroom, SustainedLoad, RecoveredHeadroom, Unavailable, Fault };
inline const char* reasonName(Reason reason) noexcept {
    switch(reason){
    case Reason::NativeFirst:return "native first";
    case Reason::Measuring:return "measuring";
    case Reason::NativeHeadroom:return "native headroom";
    case Reason::SustainedLoad:return "sustained load";
    case Reason::RecoveredHeadroom:return "recovered headroom";
    case Reason::Unavailable:return "waiting for measurements";
    case Reason::Fault:return "generation fault";
    }return "unknown";
}
// Budget from the published physical display rate. Application prediction
// can be 22.222 ms at 2x on a 90 Hz panel and is not a refresh-rate source.
inline double panelPeriodMs(double hz) noexcept {
    return std::isfinite(hz)&&hz>=20.&&hz<=250.?1000./hz:0.;
}
struct Sample {
    uint64_t nowMs=0,session=0,cpuSequence=0,gpuSequence=0;
    double cpuMs=0,gpuMs=0,periodMs=0;
    bool valid=false,fault=false,fatal=false;
};
struct Status {
    unsigned multiplier=0;
    Reason reason=Reason::NativeFirst;
    double cpuMs=0,gpuMs=0,periodMs=0;
    uint64_t transitions=0;
};
// Independent, fresh CPU-work and application-render GPU estimates. Neither
// includes the pose-credit wait, and overlapping CPU/GPU intervals are not added.
// This is a load policy, not a measurement of total multi-queue GPU cost.
class Policy {
    Status out_{};
    uint64_t session_=0,lastCpu_=0,lastGpu_=0,lastValid_=0,changed_=0,cooldown_=0;
    unsigned pressured_=0,recovered_=0;
    bool initialized_=false,fatal_=false;
    void choose(unsigned multiplier,Reason reason,uint64_t now) noexcept {
        if(out_.multiplier!=multiplier){out_.multiplier=multiplier;++out_.transitions;changed_=now;}
        out_.reason=reason;pressured_=recovered_=0;
    }
 public:
    void reset() noexcept {*this=Policy{};}
    Status status() const noexcept {return out_;}
    unsigned observe(const Sample& s) noexcept {
        if(s.fatal)fatal_=true;
        if(fatal_){choose(0,Reason::Fault,s.nowMs);return 0;}
        if(s.fault){choose(0,Reason::Fault,s.nowMs);cooldown_=s.nowMs+5000;return 0;}
        const bool valid=s.valid&&s.nowMs&&s.session&&s.cpuSequence&&s.gpuSequence&&
            std::isfinite(s.cpuMs)&&std::isfinite(s.gpuMs)&&std::isfinite(s.periodMs)&&
            s.cpuMs>=0&&s.gpuMs>=0&&s.cpuMs<=1000&&s.gpuMs<=1000&&s.periodMs>=4&&s.periodMs<=50;
        if(!valid){
            if(!lastValid_||s.nowMs<lastValid_||s.nowMs-lastValid_>1500)choose(0,Reason::Unavailable,s.nowMs);
            return out_.multiplier;
        }
        if(session_!=s.session||s.nowMs<lastValid_||(out_.periodMs&&std::fabs(s.periodMs-out_.periodMs)>.1)){
            const auto transitions=out_.transitions;reset();out_.transitions=transitions;
            session_=s.session;changed_=s.nowMs;
        }
        if((lastCpu_&&s.cpuSequence<lastCpu_)||(lastGpu_&&s.gpuSequence<lastGpu_)){
            choose(0,Reason::Unavailable,s.nowMs);return 0;
        }
        const bool newCpu=s.cpuSequence!=lastCpu_,newGpu=s.gpuSequence!=lastGpu_;
        if(!newCpu&&!newGpu)return out_.multiplier;
        if(newCpu)out_.cpuMs=initialized_?out_.cpuMs*.9+s.cpuMs*.1:s.cpuMs;
        if(newGpu)out_.gpuMs=initialized_?out_.gpuMs*.9+s.gpuMs*.1:s.gpuMs;
        initialized_=true;lastCpu_=s.cpuSequence;lastGpu_=s.gpuSequence;lastValid_=s.nowMs;out_.periodMs=s.periodMs;
        if(s.nowMs<cooldown_){out_.reason=Reason::Measuring;return out_.multiplier;}
        const double work=(std::max)(out_.cpuMs,out_.gpuMs);
        if(!out_.multiplier){
            recovered_=0;pressured_=work>s.periodMs*.96?pressured_+1:0;
            out_.reason=pressured_?Reason::Measuring:Reason::NativeHeadroom;
            if(pressured_>=18&&s.nowMs-changed_>=1500)choose(2,Reason::SustainedLoad,s.nowMs);
        }else{
            pressured_=0;recovered_=work<s.periodMs*.70?recovered_+1:0;
            if(recovered_>=120&&s.nowMs-changed_>=5000)choose(0,Reason::RecoveredHeadroom,s.nowMs);
        }
        return out_.multiplier;
    }
};
} // namespace edvr::fg_auto

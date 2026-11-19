#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace edvr::openxr::fghandoff {

enum class Import { NotNeeded, RetirementWait, ProviderNoBatch, Future, FenceWait, Imported, Rejected };
enum class Images { None, Generated, Endpoint, WrongEpoch, BehindWatermark, PreviousMismatch, Late, NativeOnly, OutsideWindow };
enum class Slot { Empty, Generated, Native, Repeat, RefusedCredit };
// First decision that stops a trial, or the ready path that took its slot.
enum class Handoff : uint8_t {
    None, GeneratedReady, CommittedEndpoint, NoPendingNative, NativeEndpointReady,
    Scene, PollRejected, SourceIdentity, FallbackCoverage, NoSynthesisProof,
    ColdTrialSpent, AttemptAlreadySpent, Time, Deferred, RecoveryDeferred
};
inline bool recordHandoff(Handoff* observation,Handoff decision) noexcept {
    if(observation)*observation=decision;
    return decision==Handoff::Deferred||decision==Handoff::RecoveryDeferred;
}

struct Poll {
    Import result=Import::NotNeeded;
    uint64_t sequence=0; // candidate or pending token, if the provider supplied one
    int64_t poseTime=0;
};

struct Sample {
    bool completion=false,frameOpen=false,staged=false,shouldRender=false;
    uint64_t timeUs=0,tick=0,generation=0,config=0,history=0;
    uint64_t source=0,pendingNative=0,watermark=0,previous=0,current=0;
    uint64_t fallbackBefore=0,fallbackAfter=0,presented=0;
    uint64_t latestSource=0,latestReady=0;
    uint64_t pollBeginUs=0,pollEndUs=0,slotBeginUs=0;
    int64_t displayTime=0,sourceTime=0,runtimePeriod=0,admissionPeriod=0;
    Poll poll{};
    int32_t pollHr=0;
    Images images=Images::None;
    Slot slot=Slot::Empty;
    unsigned phase=0,multiplier=0;
    uint8_t importedGenerated=0,importedPhaseMask=0;
    Handoff handoff=Handoff::None;
};

// Owner-only fixed storage. Keep the first bounded block after the opt-in,
// and count overflow without logging or taking more clocks on the hot path.
template<std::size_t Capacity> class Recorder {
public:
    void arm() noexcept {active_=true;size_=lost_=0;}
    bool active() const noexcept {return active_;}
    bool sampleWanted() noexcept {
        if(!active_)return false;
        if(size_==Capacity){++lost_;return false;}
        return true;
    }
    void append(const Sample& sample) noexcept {
        if(active_&&size_<Capacity)records_[size_++]=sample;
    }
    template<class Sink> void finish(Sink sink) {
        if(!active_)return;
        active_=false;
        sink(records_.data(),size_,lost_);
    }

private:
    std::array<Sample,Capacity> records_{};
    std::size_t size_=0,lost_=0;
    bool active_=false;
};

} // namespace edvr::openxr::fghandoff

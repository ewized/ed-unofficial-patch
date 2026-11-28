#pragma once
#include "../common/native_fg_multiplier.h"
#include "native_fg_timeline.h"
#include <cstdint>
#include <limits>

namespace edvr::openxr::fghandoff {
enum class Images { None, Generated, Endpoint, WrongEpoch, BehindWatermark, PreviousMismatch, Late, NativeOnly, TranslatedCoverage, OutsideWindow };
enum class Handoff { None, SourceIdentity, FallbackCoverage, NoSynthesisProof, ColdTrialSpent, AttemptAlreadySpent, Time, Deferred, RecoveryDeferred };

inline bool recordHandoff(Handoff* observation,Handoff decision) noexcept {
  if(observation)*observation=decision;
  return decision==Handoff::Deferred||decision==Handoff::RecoveryDeferred;
}
} // namespace edvr::openxr::fghandoff

namespace edvr::openxr::fgbootstrap {
struct Epoch {
  uint64_t generation=0,config=0,reference=0,history=0;
  bool valid() const {return generation&&config&&reference&&history;}
  bool operator==(const Epoch& b) const {
    return generation==b.generation&&config==b.config&&reference==b.reference&&history==b.history;
  }
};

// Only spend a native-credit spacing slot on a phase already owned by the
// consumer and selected for this render tick. Never wait for a GPU result.
inline bool creditDue(uint64_t tick,uint64_t nextSpacedCredit,bool generatedPhase) {
  return !generatedPhase||tick>=nextSpacedCredit;
}

inline unsigned firstPhase(uint64_t previous,uint64_t current,unsigned count,uint64_t displayed) {
  return count&&displayed&&previous==displayed&&current>displayed?1u:0u;
}

// A source can spend up to one requested native interval rendering, plus
// one display period reaching the first interpolation slot. Subsequent
// phases get one additional period each, not a new deadline from import.
inline bool interpolationFits(int64_t now,int64_t sourceTime,int64_t period,
                              unsigned multiplier,unsigned phase) {
  if(sourceTime<=0||now<sourceTime||period<=0||!nativeFgFixedMultiplier(multiplier)||
     !phase||phase>=multiplier)return false;
  const auto periods=multiplier+phase;
  return period<=(std::numeric_limits<int64_t>::max)()/periods&&now-sourceTime<=period*periods;
}

// Each SDK image retains its fractional instant between the two original pose
// times. Divide the span first so long-running XrTime values stay representable.
inline int64_t phaseTime(int64_t previous,int64_t current,unsigned multiplier,unsigned phase) {
  if(!phase)return current;
  if(previous<=0||current<=previous||!nativeFgFixedMultiplier(multiplier)||phase>=multiplier)return 0;
  const auto span=current-previous;
  return previous+(span/multiplier)*phase+((span%multiplier)*phase)/multiplier;
}

class Bootstrap {
  Epoch epoch_{};
  Timeline timeline_{};
  bool generated_=false,attempted_=false;
  uint64_t completed_=0,firstPending_=0,displayed_=0,previousDisplayed_=0;
  uint64_t displayedTick_=0,previousDisplayedTick_=0,lastTrialSource_=0;
  uint64_t bootstrapEndpoint_=0,committed_=0;
  int64_t recoveryPoseTime_=0;

  template<class Batch> bool provesSynthesis(const Batch& batch) const {
    return epoch_.valid()&&nativeFgFixedMultiplier(batch.multiplier)&&
      batch.generatedCount==batch.multiplier-1&&batch.generation==epoch_.generation&&
      batch.configGeneration==epoch_.config&&batch.referenceGeneration==epoch_.reference&&
      batch.historyGeneration==epoch_.history;
  }
public:
  void clear(){*this={};}
  void sync(const Epoch& epoch){if(!(epoch==epoch_)){clear();epoch_=epoch;}}
  const Epoch& epoch() const {return epoch_;}
  void syncDisplayPeriod(int64_t panelPeriod){timeline_.syncPeriod(panelPeriod);}
  void noteSlot(uint64_t tick,int64_t displayTime,int64_t nextPeriod,int64_t panelPeriod) {
    if(epoch_.valid())timeline_.note(tick,displayTime,nextPeriod,panelPeriod);
  }
  bool phaseFits(int64_t displayTime,int64_t sourceTime,int64_t period,unsigned multiplier,unsigned phase,
                 int64_t nextPeriod=0) const {
    if(displayTime<sourceTime||sourceTime<=0||period<=0||!nativeFgFixedMultiplier(multiplier)||
       !phase||phase>=multiplier||nextPeriod<0||displayTime>(std::numeric_limits<int64_t>::max)()-nextPeriod)return false;
    // With a proven runtime timeline the horizon is measured in predicted
    // display opportunities. Dynamic predictions can straddle the nominal
    // time limit for the same opportunity. Other sources keep the wall cap.
    if(const auto slots=timeline_.opportunities(sourceTime,displayTime,period,nextPeriod))return *slots<=multiplier+phase;
    return interpolationFits(displayTime+nextPeriod,sourceTime,period,multiplier,phase);
  }
  // On regular cadence the index-p image precedes B by (m-p) panel
  // intervals. Its latest allowed presentation is (m+p) intervals after B,
  // hence the image itself can be at most two native intervals old. A hitch
  // between A and B spends that same budget, even when B is freshly imported.
  bool interpolatedPhaseFits(int64_t display,int64_t previous,int64_t current,int64_t period,
                             unsigned multiplier,unsigned phase,int64_t nextPeriod=0) const {
    const auto instant=phaseTime(previous,current,multiplier,phase);
    if(!instant||!phaseFits(display,current,period,multiplier,phase,nextPeriod))return false;
    const auto age=display+nextPeriod-instant;
    const auto intervals=age/period,remainder=age%period;
    const int64_t limit=2*multiplier;
    if(intervals<limit||(intervals==limit&&!remainder))return true;
    // Dynamic predictions can straddle this same final panel opportunity.
    // Only a proven B-to-display ledger permits unique nearest-slot rounding.
    return intervals==limit&&remainder<period-remainder&&
      timeline_.opportunities(current,display,period,nextPeriod).has_value();
  }
  uint64_t completedSequence() const {return completed_;}
  void completeNative(uint64_t sequence) {
    if(!epoch_.valid()||sequence<=completed_)return;
    if(!completed_)firstPending_=sequence;
    completed_=sequence;
  }
  void consumeNative(uint64_t sequence) {
    if(firstPending_&&sequence>=firstPending_) {
      if(sequence>=completed_)completed_=0;
      firstPending_=completed_;
      attempted_=false;
    }
  }
  void displayNative(uint64_t sequence,uint64_t tick=0) {
    if(sequence>displayed_) {
      previousDisplayed_=displayed_;displayed_=sequence;
      previousDisplayedTick_=displayedTick_;displayedTick_=tick;
    }
    if(sequence>=committed_)committed_=0;
    consumeNative(sequence);
  }

  // After polling, a proven A->B handoff may keep the displayed A until one
  // further real slot where phase one would still meet its original deadline.
  // Replacing B with another completion cannot renew this attempt.
  bool deferNative(uint64_t displayed,uint64_t latestQueued,uint64_t latestExported,
                   int64_t sourceTime,int64_t displayTime,int64_t nextPeriod,int64_t admissionPeriod,
                   unsigned multiplier,fghandoff::Handoff* observation=nullptr) {
    using fghandoff::Handoff;
    using fghandoff::recordHandoff;
    if(attempted_)return recordHandoff(observation,Handoff::AttemptAlreadySpent);
    if(!displayed)return recordHandoff(observation,Handoff::SourceIdentity);
    if(!generated_&&(bootstrapEndpoint_!=displayed||
       (lastTrialSource_&&!timeline_.opportunities(recoveryPoseTime_,displayTime,admissionPeriod,0))))
      return recordHandoff(observation,lastTrialSource_?Handoff::ColdTrialSpent:Handoff::NoSynthesisProof);
    if(completed_<=displayed||completed_-displayed!=1||firstPending_!=completed_||
       latestQueued!=completed_||latestExported>=completed_)return recordHandoff(observation,Handoff::SourceIdentity);
    if(nextPeriod<=0||!phaseFits(displayTime,sourceTime,admissionPeriod,multiplier,1,nextPeriod))
      return recordHandoff(observation,Handoff::Time);
    if(observation)*observation=lastTrialSource_&&!generated_?Handoff::RecoveryDeferred:Handoff::Deferred;
    attempted_=true;
    lastTrialSource_=completed_;
    bootstrapEndpoint_=0;recoveryPoseTime_=0;
    return true;
  }
  void miss(){generated_=false;bootstrapEndpoint_=0;recoveryPoseTime_=0;}
  void abandonNativeAttempt(){if(attempted_)miss();}
  bool acceptPhase(int64_t displayTime,int64_t sourceTime,int64_t period,unsigned multiplier,unsigned phase) {
    const bool timely=phaseFits(displayTime,sourceTime,period,multiplier,phase);
    // An unheld late result misses its own slot. A spent trial additionally
    // loses pacing proof when it fails to deliver within that same deadline.
    if(!timely)abandonNativeAttempt();
    return timely;
  }

  // After a miss, only a newer pair that actually arrived on the next
  // owner poll after its native endpoint can prove another one-slot trial.
  // Require the real runtime timeline for this stronger delivery proof.
  template<class Batch> void observeBehind(const Batch& batch,bool timely,uint64_t tick=0,int64_t displayTime=0,
                                           int64_t period=0) {
    if(!timely||!provesSynthesis(batch)||!previousDisplayed_||batch.previousSequence!=previousDisplayed_||
       batch.currentSequence!=displayed_)return;
    if(lastTrialSource_) {
      if(batch.currentSequence<=lastTrialSource_||!previousDisplayedTick_||previousDisplayedTick_>=displayedTick_||
         tick<=displayedTick_||tick-displayedTick_!=1||batch.phaseMask!=((1u<<batch.multiplier)-1u)||
         batch.previousPoseDisplayTime<=0||batch.previousPoseDisplayTime>=batch.currentPoseDisplayTime)return;
      const auto slots=timeline_.opportunities(batch.currentPoseDisplayTime,displayTime,period,0);
      if(!slots||!*slots||*slots>batch.multiplier+1)return;
      recoveryPoseTime_=batch.currentPoseDisplayTime;
    }
    bootstrapEndpoint_=displayed_;
  }
  template<class Batch> void displayGenerated(const Batch& batch) {
    if(provesSynthesis(batch)) {
      committed_=batch.currentSequence;
      attempted_=false;
      lastTrialSource_=0;
      bootstrapEndpoint_=0;recoveryPoseTime_=0;
      generated_=true;
    }
  }
  uint64_t committedSequence() const {return committed_;}
  bool needsEndpoint(uint64_t sequence) const {return committed_&&sequence==committed_;}
};
} // namespace edvr::openxr::fgbootstrap

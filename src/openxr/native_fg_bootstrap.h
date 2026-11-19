#pragma once
#include "../common/native_fg_multiplier.h"
#include <cstdint>
#include <limits>

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

class Bootstrap {
  Epoch epoch_{};
  bool generated_=false,trialSpent_=false,attempted_=false;
  uint64_t completed_=0,firstPending_=0,displayed_=0,previousDisplayed_=0;
  uint64_t bootstrapEndpoint_=0,committed_=0;

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
  void displayNative(uint64_t sequence) {
    if(sequence>displayed_){previousDisplayed_=displayed_;displayed_=sequence;}
    if(sequence>=committed_)committed_=0;
    consumeNative(sequence);
  }

  // After polling, a proven A->B handoff may keep the displayed A until one
  // further real slot where phase one would still meet its original deadline.
  // Replacing B with another completion cannot renew this attempt.
  bool deferNative(uint64_t displayed,uint64_t latestQueued,uint64_t latestExported,
                   int64_t sourceTime,int64_t displayTime,int64_t nextPeriod,int64_t admissionPeriod,
                   unsigned multiplier) {
    if(attempted_||!displayed||(!generated_&&(trialSpent_||bootstrapEndpoint_!=displayed))||
       completed_<=displayed||completed_-displayed!=1||
       firstPending_!=completed_||latestQueued!=completed_||latestExported>=completed_||
       nextPeriod<=0||displayTime<=0||displayTime>(std::numeric_limits<int64_t>::max)()-nextPeriod||
       !interpolationFits(displayTime+nextPeriod,sourceTime,admissionPeriod,multiplier,1))return false;
    attempted_=true;
    trialSpent_=true;
    return true;
  }
  void miss(){generated_=false;bootstrapEndpoint_=0;}
  void abandonNativeAttempt(){if(attempted_)miss();}
  bool acceptPhase(int64_t displayTime,int64_t sourceTime,int64_t period,unsigned multiplier,unsigned phase) {
    const bool timely=interpolationFits(displayTime,sourceTime,period,multiplier,phase);
    // An unheld late result misses its own slot. A spent trial additionally
    // loses pacing proof when it fails to deliver within that same deadline.
    if(!timely)abandonNativeAttempt();
    return timely;
  }

  // A timely result for the exact displayed A->B pair can prove one cold
  // trial. Only a submitted generated phase replenishes that allowance.
  template<class Batch> void observeBehind(const Batch& batch,bool timely) {
    if(!trialSpent_&&timely&&provesSynthesis(batch)&&previousDisplayed_&&
       batch.previousSequence==previousDisplayed_&&batch.currentSequence==displayed_)
      bootstrapEndpoint_=displayed_;
  }
  template<class Batch> void displayGenerated(const Batch& batch) {
    if(provesSynthesis(batch)) {
      committed_=batch.currentSequence;
      attempted_=false;
      trialSpent_=false;
      bootstrapEndpoint_=0;
      generated_=true;
    }
  }
  bool needsEndpoint(uint64_t sequence) const {return committed_&&sequence==committed_;}
};
} // namespace edvr::openxr::fgbootstrap

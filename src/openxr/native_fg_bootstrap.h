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
  bool generated_=false;
  uint64_t completed_=0,firstPendingTick_=0;
public:
  void clear(){*this={};}
  void sync(const Epoch& epoch){if(!(epoch==epoch_)){clear();epoch_=epoch;}}
  const Epoch& epoch() const {return epoch_;}
  uint64_t completedSequence() const {return completed_;}
  void completeNative(uint64_t sequence,uint64_t tick) {
    if(!epoch_.valid()||sequence<=completed_)return;
    if(!completed_)firstPendingTick_=tick;
    completed_=sequence;
  }
  void displayNative(uint64_t sequence) {
    if(sequence>=completed_)completed_=firstPendingTick_=0;
  }

  // Once a timely generated batch proves the current epoch, preserve A for
  // the rest of the tick in which B finishes. Poll first on the next tick:
  // display A->B if ready, or the complete native B if it missed that slot.
  // Newer source completions cannot extend the original handoff deadline.
  bool refreshDue(uint64_t tick) const {
    return epoch_.valid()&&completed_&&(!generated_||tick>firstPendingTick_);
  }
  // A missed handoff returns to immediate native presentation. Later timely
  // generated work may re-prime the epoch without rebuilding GPU resources.
  void miss(){generated_=false;}

  template<class Batch>
  void observe(const Batch& batch,bool timely) {
    if(timely&&epoch_.valid()&&nativeFgFixedMultiplier(batch.multiplier)&&
       batch.generatedCount==batch.multiplier-1&&batch.generation==epoch_.generation&&
       batch.configGeneration==epoch_.config&&batch.referenceGeneration==epoch_.reference&&
       batch.historyGeneration==epoch_.history)generated_=true;
  }
};
} // namespace edvr::openxr::fgbootstrap

#pragma once
#include "../common/native_frame_generation.h"

namespace edvr::openxr::fgbootstrap {
struct Epoch {
  uint64_t generation=0,config=0,reference=0,history=0;
  bool valid() const {return generation&&config&&reference&&history;}
  bool operator==(const Epoch& b) const {
    return generation==b.generation&&config==b.config&&reference==b.reference&&history==b.history;
  }
};
// Native fallback advances only until a real generated batch can take over.
// Otherwise keeping the newest native endpoint ahead of NGX would starve it.
class Bootstrap {
  Epoch epoch_{};
  bool generated_=false;
public:
  void clear(){epoch_={};generated_=false;}
  void sync(const Epoch& epoch){if(!(epoch==epoch_)){epoch_=epoch;generated_=false;}}
  bool refreshing() const {return epoch_.valid()&&!generated_;}
  const Epoch& epoch() const {return epoch_;}
  bool observe(const EdvrFgBatch& batch,bool importedAndUsable) {
    if(!refreshing()||!importedAndUsable||!batch.generatedCount||
       !nativeFgFixedMultiplier(batch.multiplier)||
       batch.generatedCount!=batch.multiplier-1||batch.generation!=epoch_.generation||
       batch.configGeneration!=epoch_.config||batch.referenceGeneration!=epoch_.reference||
       batch.historyGeneration!=epoch_.history)return false;
    generated_=true;return true;
  }
};
} // namespace edvr::openxr::fgbootstrap

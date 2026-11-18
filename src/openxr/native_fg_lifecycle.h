#pragma once
#include <stdint.h>

namespace edvr::openxr::fglifecycle {
// OFF/ARMING/READY are the normal coordinator cycle, not lifecycle events.
inline uint32_t stateClass(uint32_t state) {return state<=2u?0u:state;}
// Observation only. Sequence/counter changes are deliberately not transition
// keys. Flags contain persistent admission/refusal conditions only.
struct Key {
  uint64_t flags=0,config=0,history=0;
  uint32_t state=0,failure=0,multiplier=0,disableReason=0;
  int32_t status=0;
  bool operator==(const Key& b) const {
    return flags==b.flags&&config==b.config&&history==b.history&&state==b.state&&
      failure==b.failure&&multiplier==b.multiplier&&disableReason==b.disableReason&&status==b.status;
  }
};
struct Decision {bool emit=false,changed=false;uint64_t transitions=0,suppressed=0;};
class Reporter {
  bool seen_=false,active_=false,pendingOff_=false,emitted_=false;
  uint64_t window_=0,lastLine_=0,transitions_=0,suppressed_=0;
  unsigned extra_=0;
  Key previous_{};
public:
  bool observing() const {return active_||pendingOff_;}
  Decision observe(uint64_t now,bool armed,const Key& key) {
    // Cold Off has no reporting work or eventual heartbeat.
    if(!armed&&!active_&&!pendingOff_)return {};
    if(!emitted_||now<window_||now-window_>=1000){window_=now;extra_=0;}
    const bool changed=!seen_||armed!=active_||!(key==previous_);
    if(changed)++transitions_;
    seen_=true;active_=armed;previous_=key;pendingOff_=!armed;
    const bool heartbeat=!emitted_||now<lastLine_||now-lastLine_>=1000;
    const bool emit=heartbeat||(changed&&extra_<4);
    if(emit){if(!heartbeat)++extra_;lastLine_=now;emitted_=true;if(!armed)pendingOff_=false;}
    else if(changed)++suppressed_;
    return {emit,changed,transitions_,suppressed_};
  }
};
} // namespace edvr::openxr::fglifecycle

#pragma once
#include <cstdint>
#include <limits>
#include <optional>

namespace edvr::openxr::fgbootstrap {

// OpenXR's reported period forecasts the next app frame and may cover
// several panel intervals. The runtime can adjust its predictions dynamically.
// https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#frame-loop
// Require exact owner wait timestamps within one epoch and panel rate.
// The aggregate interval check catches drift that would add an opportunity.
class Timeline {
  // The longest horizon is 4x plus three intermediate phases, and one entry
  // is needed for the original source pose.
  static constexpr unsigned capacity=8;
  struct Slot {uint64_t tick=0;int64_t time=0,nextPeriod=0;};
  Slot slots_[capacity]{};
  unsigned latest_=0,size_=0;
  int64_t panelPeriod_=0;

  // Match to a unique nearest panel interval. A midpoint cannot identify
  // either opportunity. This classifies an observed interval without an
  // absolute nanosecond tolerance or rounding the original pose time.
  int64_t intervals(int64_t duration) const {
    const int64_t remainder=duration%panelPeriod_,other=panelPeriod_-remainder;
    return remainder==other ? -1 : duration/panelPeriod_+(remainder>other);
  }
public:
  void syncPeriod(int64_t panelPeriod) {
    if(panelPeriod!=panelPeriod_){size_=latest_=0;panelPeriod_=panelPeriod;}
  }
  void note(uint64_t tick,int64_t time,int64_t nextPeriod,int64_t panelPeriod) {
    syncPeriod(panelPeriod);
    if(!tick||time<=0||nextPeriod<=0||panelPeriod_<=0) {size_=latest_=0;return;}
    if(size_) {
      const auto& prior=slots_[latest_];
      const auto expected=intervals(prior.nextPeriod);
      if(tick<=prior.tick||tick-prior.tick!=1||time<=prior.time||expected<=0||
         expected>=capacity||intervals(time-prior.time)!=expected)size_=latest_=0;
    }
    if(size_)latest_=(latest_+1)%capacity;
    if(size_<capacity)++size_;
    slots_[latest_]={tick,time,nextPeriod};
  }

  // Require the original pose and current time to name exact ledger entries.
  // The whole span must still project to the same number of opportunities as
  // its individual steps, so forecast drift cannot accumulate extra time.
  std::optional<unsigned> opportunities(int64_t source,int64_t display,int64_t panelPeriod,int64_t nextPeriod) const {
    if(!size_||panelPeriod<=0||panelPeriod!=panelPeriod_||source<=0||source>display||nextPeriod<0||
       slots_[latest_].time!=display)return {};
    unsigned count=0;
    int64_t elapsed=display-source;
    if(nextPeriod) {
      if(nextPeriod!=slots_[latest_].nextPeriod||elapsed>(std::numeric_limits<int64_t>::max)()-nextPeriod)return {};
      const auto steps=intervals(nextPeriod);
      if(steps<=0||steps>=capacity)return {};
      count=unsigned(steps);
      elapsed+=nextPeriod;
    }
    for(unsigned back=0;back<size_;++back) {
      const auto& slot=slots_[(latest_+capacity-back)%capacity];
      if(slot.time==source) {
        if(intervals(elapsed)==count)return count;
        return {};
      }
      if(slot.time<source||back+1==size_)break;
      const auto& prior=slots_[(latest_+capacity-back-1)%capacity];
      count+=unsigned(intervals(prior.nextPeriod));
    }
    return {};
  }
};
} // namespace edvr::openxr::fgbootstrap

#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace edvr::openxr {
// Imported wrappers are keyed by allocation incarnation and image role. Their
// independent GPU-read owners remain NativeFgImportPins below.
template<class Pin,class Descriptor,size_t Capacity=3> class NativeFgImportCache {
 public:
  static_assert(Capacity>0,"Import cache requires bounded storage");
  using Signature=std::array<uint32_t,5>; // dimensions, format, adapter LUID
  enum class Result { Opened, Reused, Invalid, OpenFailed };
  // The XR owner serializes calls. Describe validates every use, including a
  // hit, and returns the complete immutable texture descriptor fingerprint.
  // Failed opens never evict a healthy allocation. Pin copies held by active
  // consumer fences are independent of cache eviction or clear().
  template<class Open,class Describe>
  Result resolve(uint64_t allocation,const Signature& signature,size_t role,uintptr_t handle,
                 Open open,Describe describe,Pin& output) {
    output=Pin{};
    if(!allocation||!handle||role>=kResources)return Result::Invalid;
    Entry* entry=nullptr;
    for(auto& candidate:entries_)if(candidate.allocation==allocation){entry=&candidate;break;}
    if(entry&&entry->signature!=signature)return Result::Invalid;
    if(entry)for(size_t i=0;i<kResources;++i)
      if(i!=role&&entry->resources[i].pin&&entry->resources[i].handle==handle)return Result::Invalid;
    if(entry&&entry->resources[role].pin) {
      const auto& resource=entry->resources[role];
      if(resource.handle!=handle)return Result::Invalid;
      Descriptor actual{};
      if(!describe(resource.pin,actual)||actual!=resource.description)return Result::Invalid;
      output=resource.pin;return Result::Reused;
    }
    Pin opened=open();if(!opened)return Result::OpenFailed;
    Descriptor actual{};if(!describe(opened,actual))return Result::Invalid;
    if(!entry) {
      size_t selected=next_;
      for(size_t i=0;i<Capacity;++i)if(!entries_[i].allocation){selected=i;break;}
      entry=&entries_[selected];*entry=Entry{};
      entry->allocation=allocation;entry->signature=signature;
      next_=(selected+1)%Capacity;
    }
    auto& resource=entry->resources[role];
    resource.handle=handle;resource.description=actual;resource.pin=std::move(opened);
    output=resource.pin;return Result::Opened;
  }
  void clear(){for(auto& entry:entries_)entry=Entry{};next_=0;}
  size_t size()const{size_t count=0;for(const auto& entry:entries_)if(entry.allocation)++count;return count;}
 private:
  static constexpr size_t kResources=12; // four stereo phases, UI and native world
  struct Resource {uintptr_t handle=0;Pin pin{};Descriptor description{};};
  struct Entry {uint64_t allocation=0;Signature signature{};std::array<Resource,kResources> resources{};};
  std::array<Entry,Capacity> entries_{};size_t next_=0;
};
// One active consumer-copy batch. Pin is ComPtr<IUnknown> in production and
// a counted shared owner in CPU fixtures. No driver calls or waits here.
template<class Pin> class NativeFgImportPins {
 public:
  enum class Phase { Empty, Opened, Unfenced, Fenced, Quarantined };
  bool begin(const Pin& ready) {
    if(pending())return false;
    ready_=ready;phase_=Phase::Opened;return true;
  }
  bool add(const Pin& pin) {
    if(phase_!=Phase::Opened||count_==pins_.size())return false;
    pins_[count_++]=pin;return true;
  }
  void readsMayStart(){phase_=Phase::Unfenced;}
  bool signal(uint64_t value) {
    if(phase_!=Phase::Unfenced||!value||value==max()){quarantine();return false;}
    value_=value;phase_=Phase::Fenced;return true;
  }
  void quarantine(){phase_=Phase::Quarantined;}
  void invalidate(){} // scheduling invalidation is never GPU completion proof
  bool cancelBeforeReads() {
    if(phase_!=Phase::Opened)return !pending();
    clear();return true;
  }
  bool collect(uint64_t completed) {
    if(!pending())return true;
    if(phase_!=Phase::Fenced)return false;
    if(completed==max()){quarantine();return false;}
    if(completed<value_)return false;
    clear();return true;
  }
  bool close(uint64_t completed){return collect(completed);}
  bool pending()const{return phase_!=Phase::Empty;}
  bool quarantined()const{return phase_==Phase::Quarantined;}
  size_t count()const{return count_;}
  uint64_t value()const{return value_;}
 private:
  static constexpr uint64_t max(){return (std::numeric_limits<uint64_t>::max)();}
  void clear(){for(auto& pin:pins_)pin=Pin{};ready_=Pin{};count_=0;value_=0;phase_=Phase::Empty;}
  std::array<Pin,12> pins_{};Pin ready_{};size_t count_=0;uint64_t value_=0;Phase phase_=Phase::Empty;
};
} // namespace edvr::openxr

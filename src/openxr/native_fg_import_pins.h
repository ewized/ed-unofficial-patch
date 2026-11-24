#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace edvr::openxr {
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

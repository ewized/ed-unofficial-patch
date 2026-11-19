#pragma once
#include "../../src/d3d11/native_vrs_policy.h"

inline void witnessRateCases(void (*check)(bool,const char*)){
    using namespace edvr::native_vrs;
    VendorStateCache cache;
    unsigned rates=0,resources=0;
    auto setRates=[&](bool){++rates;return 0;};
    auto setResource=[&](uintptr_t){++resources;return 0;};
    check(!pauseForWitness(cache,7,setRates)&&rates==0,"unknown vendor state cannot be retained");
    check(applyCoarse(cache,7,setRates,setResource)&&!pauseForWitness(cache,8,setRates)&&rates==1,
          "another eye cannot borrow the originally bound view");
    check(pauseForWitness(cache,7,setRates)&&cache.retainedForWitness(7)&&!cache.fullRateKnown()&&!cache.active(7),
          "the witness disables rates without publishing a strong reset");
    check(applyCoarse(cache,7,setRates,setResource)&&cache.active(7)&&rates==3&&resources==1,
          "same-eye fine and coarse issues require two rate changes and zero resource changes");
    check(pauseForWitness(cache,7,setRates)&&applyFullRate(cache,setRates,setResource)&&cache.fullRateKnown()&&
          !cache.retainedForWitness(7)&&rates==4&&resources==2,"external full-rate boundary unbinds a retained view");
    applyCoarse(cache,7,setRates,setResource);pauseForWitness(cache,7,setRates);cache.invalidate();
    check(!cache.retainedForWitness(7)&&!pauseForWitness(cache,7,setRates)&&!cache.fullRateKnown(),
          "command-list invalidation discards any retained-view proof");
    applyCoarse(cache,7,setRates,setResource);
    check(!pauseForWitness(cache,7,[](bool){return -1;})&&!cache.bindingKnown&&!cache.ratesKnown&&
          applyFullRate(cache,setRates,setResource)&&cache.fullRateKnown(),"failed disable loses both acknowledgements and permits strong recovery");
}

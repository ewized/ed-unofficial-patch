#pragma once
#include "../../src/d3d11/native_vrs_policy.h"

inline void witnessRateCases(void (*check)(bool,const char*)){
    using namespace edvr::native_vrs;
    VendorStateCache cache;
    unsigned rates=0,resources=0;
    ViewportRates lastMode=ViewportRates::Disabled;
    auto setRates=[&](ViewportRates mode){lastMode=mode;++rates;return 0;};
    auto setResource=[&](uintptr_t){++resources;return 0;};
    bool uniform=true;
    for(auto entry:palette(ViewportRates::Uniform1x1))uniform=uniform&&entry==kFullRate;
    check(uniform,"every retained map index resolves to per-pixel shading during the witness");
    check(!pauseForWitness(cache,7,setRates)&&rates==0,"unknown vendor state cannot be retained");
    check(applyCoarse(cache,7,setRates,setResource)&&!pauseForWitness(cache,8,setRates)&&rates==1,
          "another eye cannot borrow the originally bound view");
    check(pauseForWitness(cache,7,setRates)&&lastMode==ViewportRates::Uniform1x1&&
          cache.retainedForWitness(7)&&!cache.fullRateKnown()&&!cache.active(7),
          "the private uniform palette publishes neither coarse nor public full rate");
    check(applyCoarse(cache,7,setRates,setResource)&&lastMode==ViewportRates::Foveated&&cache.active(7)&&rates==3&&resources==1,
          "same-eye fine and coarse issues require two rate changes and zero resource changes");
    check(pauseForWitness(cache,7,setRates)&&applyFullRate(cache,setRates,setResource)&&cache.fullRateKnown()&&
          lastMode==ViewportRates::Disabled&&!cache.retainedForWitness(7)&&rates==5&&resources==2,
          "external full-rate boundary disables the uniform viewport and unbinds its retained view");
    applyCoarse(cache,7,setRates,setResource);pauseForWitness(cache,7,setRates);cache.invalidate();
    check(!cache.retainedForWitness(7)&&!pauseForWitness(cache,7,setRates)&&!cache.fullRateKnown(),
          "command-list invalidation discards any retained-view proof");
    applyCoarse(cache,7,setRates,setResource);
    check(!pauseForWitness(cache,7,[](ViewportRates){return -1;})&&!cache.bindingKnown&&!cache.ratesKnown&&
          applyFullRate(cache,setRates,setResource)&&cache.fullRateKnown(),"failed uniform palette loses both acknowledgements and permits strong recovery");
}

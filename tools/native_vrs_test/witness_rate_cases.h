#pragma once
#include "../../src/d3d11/native_vrs_policy.h"
#include "../../src/d3d11/native_vrs.h"

inline void witnessRateCases(void (*check)(bool,const char*)){
    using namespace edvr::native_vrs;
    using edvr::NativeVrsWitnessStamp;
    const NativeVrsWitnessStamp stamp{0,4,23,7,8,9};
    edvr::NativeVrsWitnessReceipt receipt;
    check(!receipt.take().matches(stamp),"empty receipt cannot authorize material");
    receipt.stamp=stamp;
    check(receipt.take().matches(stamp)&&!receipt.take().matches(stamp),"one prepared receipt authorizes one material only");
    for(unsigned field=0;field<6;++field){
        auto changed=stamp;
        if(field==0)++changed.eye;
        if(field==1)++changed.map;
        if(field==2)++changed.frame;
        if(field==3)++changed.config;
        if(field==4)++changed.gazeEpoch;
        if(field==5)++changed.invalidation;
        receipt.stamp=stamp;
        check(!receipt.take().matches(changed)&&!receipt.take().matches(stamp),"changed eye, view, frame or generation consumes and refuses the receipt");
    }
    for(bool expires:{false,true}){
        bool ready=true,activated=false,reset=false;
        const bool accepted=activateWhileCurrent([&]{return ready;},[&]{
            activated=true;ready=!expires;return true;
        },[&]{reset=true;});
        check(activated&&accepted==!expires&&reset==expires,"activation rechecks admission after the driver and resets on expiry");
    }
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

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
    check(palette(ViewportRates::Foveated)[0]==kFullRate,"zero tile index selects one shade per raster pixel");
    check(!bindWitnessMap(cache,7,9,setResource)&&resources==0,"unknown vendor state cannot start a private map");
    check(applyCoarse(cache,7,setRates,setResource)&&!bindWitnessMap(cache,8,9,setResource)&&
          !bindWitnessMap(cache,7,7,setResource),"private map requires the matching active eye and a distinct zero map");
    check(bindWitnessMap(cache,7,9,setResource)&&cache.uniformWitnessMap()&&!cache.fullRateKnown()&&!cache.active(9)&&
          lastMode==ViewportRates::Foveated&&rates==1&&resources==2,
          "the zero map keeps the foveated table and publishes neither coarse nor public full rate");
    check(applyCoarse(cache,7,setRates,setResource)&&cache.active(7)&&rates==1&&resources==3,
          "same-eye fine and material draws require two resource switches and no viewport changes");
    check(bindWitnessMap(cache,7,9,setResource)&&applyFullRate(cache,setRates,setResource)&&cache.fullRateKnown()&&
          lastMode==ViewportRates::Disabled&&!cache.uniformWitnessMap()&&rates==2&&resources==5,
          "external boundary disables viewport shading and removes the zero map");
    applyCoarse(cache,7,setRates,setResource);bindWitnessMap(cache,7,9,setResource);cache.invalidate();
    check(!cache.uniformWitnessMap()&&!bindWitnessMap(cache,7,9,setResource)&&!cache.fullRateKnown(),
          "command-list invalidation discards the private map acknowledgement");
    applyCoarse(cache,7,setRates,setResource);
    check(!bindWitnessMap(cache,7,9,[](uintptr_t){return -1;})&&!cache.bindingKnown&&!cache.ratesKnown&&
          applyFullRate(cache,setRates,setResource)&&cache.fullRateKnown(),"failed zero-map switch permits a strong recovery");
}

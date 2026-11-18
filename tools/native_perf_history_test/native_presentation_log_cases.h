#pragma once
#include "../../src/common/native_presentation_log.h"
#include "../../src/common/native_fg_readout.h"

namespace edvr::test {
template<class Check> void nativePresentationLogCases(Check check) {
    NativePresentationLogCadence cadence;
    NativePresentationSnapshot sample{};
    check(!cadence.due(false,sample,10),"presentation log: disabled clock writes nothing");
    check(cadence.due(true,sample,10),"presentation log: missing old-module callback is reported immediately");
    check(!cadence.due(true,sample,1009)&&cadence.due(true,sample,1010),
        "presentation log: unavailable heartbeat runs at one second, not each render tick");
    sample.active=sample.available=true;sample.generation=1;sample.context=7;
    sample.sequence=sample.nativeFrames=1;sample.capturedAtMs=1050;
    check(cadence.due(true,sample,1050),"presentation log: first actual packet bypasses heartbeat deadline");
    sample.sequence=sample.nativeFrames=2;sample.capturedAtMs=1051;
    check(!cadence.due(true,sample,1051)&&!cadence.due(true,sample,2049)&&cadence.due(true,sample,2050),
        "presentation log: advancing per-frame packets do not exceed heartbeat cadence");
    check(nativePresentationPacketFresh(sample,3051)&&!nativePresentationPacketFresh(sample,3052),
        "presentation log: exact two-second freshness boundary");
    check(cadence.due(true,sample,4051)&&!nativePresentationPacketFresh(sample,4051)&&sample.nativeFrames==2,
        "presentation log: stale packet heartbeat retains original totals and timestamp");
    auto missing=sample;missing.available=false;
    check(cadence.due(true,missing,4052)&&!nativePresentationPacketFresh(missing,4052),
        "presentation log: lost capability is explicit immediately");
    sample.capturedAtMs=4053;
    check(cadence.due(true,sample,4053),"presentation log: recovered capability is immediate");
    ++sample.generation;
    check(cadence.due(true,sample,4054),"presentation log: new generation is immediate");
    ++sample.context;
    check(cadence.due(true,sample,4055),"presentation log: replacement context is immediate");
    check(cadence.due(true,sample,4000),"presentation log: reversed observer clock does not underflow cadence");
    check(!cadence.due(false,sample,4056)&&cadence.due(true,sample,4057),
        "presentation log: re-enabled application clock starts a new observation baseline");
    for(unsigned kind=0;kind<7;++kind){
        auto bad=sample;
        if(kind==0)bad.active=false;
        if(kind==1)bad.available=false;
        if(kind==2)bad.generation=0;
        if(kind==3)bad.context=0;
        if(kind==4)bad.sequence=0;
        if(kind==5)bad.capturedAtMs=0;
        if(kind==6)bad.capturedAtMs=5001;
        check(!nativePresentationPacketFresh(bad,5000),"presentation log: invalid or future packet cannot qualify rates");
    }
    // A benchmark can span native -> active 2x -> native without multiplying a
    // source rate. Only successful XR categories contribute to submitted FPS.
    NativePresentationHistory history;
    sample={};sample.active=sample.available=true;sample.generation=1;sample.context=7;
    sample.sequence=sample.nativeFrames=10;sample.capturedAtMs=1000;
    history.observe(sample,1000);
    sample.sequence=sample.nativeFrames=20;sample.capturedAtMs=2000;
    history.observe(sample,2000);
    auto rates=history.read(2000);
    check(rates.nativeFps==10&&rates.generatedFps==0&&rates.submittedFps==10,
        "presentation log: native mode has observed zero generation");
    sample.sequence=47;sample.nativeFrames=30;sample.generatedFrames=10;sample.repeatedFrames=3;
    sample.emptyFrames=2;sample.failedFrames=1;sample.backgroundFrames=1;
    sample.fgEnabled=true;sample.activeMultiplier=2;sample.capturedAtMs=3000;
    history.observe(sample,3000);rates=history.read(3000);
    check(rates.nativeFps==10&&rates.generatedFps==10&&rates.repeatedFps==3&&rates.submittedFps==23,
        "presentation log: active 2x counts actual native/generated/repeated, excluding empty/failure/background");
    sample.sequence+=10;sample.nativeFrames+=10;sample.fgEnabled=false;sample.activeMultiplier=0;sample.capturedAtMs=4000;
    history.observe(sample,4000);rates=history.read(4000);
    check(rates.nativeFps==10&&rates.generatedFps==0&&rates.repeatedFps==0&&rates.submittedFps==10&&sample.generatedFrames==10,
        "presentation log: returning native keeps cumulative generated total with zero current generated rate");
}
}

#pragma once
#include "../../src/common/perf_presented.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace edvr::test {
// Measured counter streams deliberately differ from multiplier predictions;
// these cases must not pass with generated FPS inferred from configured mode.
template<class Check> void nativeFgReadoutCases(Check check) {
    const auto nearValue=[](double a,double b){return std::fabs(a-b)<.00001;};
    const auto sample=[](uint64_t at,uint64_t n,uint64_t g,uint64_t r=0){
        NativePresentationSnapshot s;s.active=s.available=true;s.generation=7;s.context=17;s.capturedAtMs=at;
        s.nativeFrames=n;s.generatedFrames=g;s.repeatedFrames=r;s.sequence=n+g+r;
        s.fgEnabled=true;s.activeMultiplier=2;return s;
    };
    NativePresentationHistory h;
    auto s=sample(1000,100,100);h.observe(s,1000);
    check(h.read(1000).measurement==NativeFgMeasureState::Measuring,"native FG: first observation is a baseline, not invented FPS");
    for(unsigned i=1;i<=10;++i){s=sample(1000+i*100,100+45*i/10,100+45*i/10);h.observe(s,s.capturedAtMs);}
    auto r=h.read(2000);r.automatic=true;r.selectedMultiplier=2;
    check(r.measurement==NativeFgMeasureState::Counting&&nearValue(r.nativeFps,45)&&nearValue(r.generatedFps,45)&&nearValue(r.submittedFps,90),"native FG: actual 45 native plus 45 generated submissions produce 90 submitted FPS");
    check(r.totals.generatedFrames==145,"native FG: cumulative total counts frames, including the baseline's prior frames");
    for(unsigned i=0;i<100;++i)h.observe(s,2000);
    check(nearValue(h.read(2000).generatedFps,45),"native FG: repeated reads of the same sample do not reweight rates");
    char text[160];formatNativeFgCompact(r,text,sizeof(text));
    check(std::string(text)=="90 FPS | Auto: 2x","native FG: compact overlay shows measured FPS and actual Auto mode");
    formatNativeFgTotals(r,text,sizeof(text));check(std::string(text)=="Auto: 2x | gen total 145 | repeat total 0","native FG: detailed totals have the same Auto mode and counters");
    PerfReadout card;card.haveRendered=true;card.renderedFps=999;card.nativeFg=r;
    formatDetailRates(card,text,sizeof(text));check(std::string(text)=="native 45.0 + gen 45.0 + repeat 0.0 fps","native FG: detailed FPS card uses measured XR counters rather than the unrelated Present rate");
    card.renderedMs=17.1f;formatDetailTimes(card,text,sizeof(text));
    check(std::string(text).find("render 17.1 ms")==0,"native FG: detailed timing labels the rendered clock separately from XR submissions");
    formatBridgeNote(card,text,sizeof(text));check(std::string(text)=="Auto: 2x | gen total 145 | repeat total 0","native FG: detailed card includes actual mode and lifetime totals");
    check(formatCompactLine(card,text,sizeof(text))&&std::string(text)=="90 FPS | Auto: 2x","native FG: compact FPS card uses the total from native presentation metrics");
    auto stale=h.read(4001);formatNativeFgRates(stale,text,sizeof(text));
    check(stale.measurement==NativeFgMeasureState::Stale&&std::string(text)=="FG counts stale"&&stale.totals.generatedFrames==145,"native FG: stale counts keep the last total but never claim a current FPS");
    h.clear();s=sample(1000,100,100);s.activeMultiplier=4;h.observe(s,1000);
    for(unsigned i=1;i<=10;++i){s=sample(1000+i*100,100+30*i/10,100+90*i/10);s.activeMultiplier=4;h.observe(s,s.capturedAtMs);}
    r=h.read(2000);check(nearValue(r.nativeFps,30)&&nearValue(r.generatedFps,90)&&nearValue(r.submittedFps,120),"native FG: 4x counts three generated frames per actual native frame, not one completed batch");
    r.totals.activeMultiplier=3;
    formatNativeFgMode(r,text,sizeof(text));check(std::string(text)=="3x","native FG: monitor shows the active 3x mode");
    formatNativeFgTileValue(r,text,sizeof(text));check(std::string(text)=="3x","native FG: compact tile keeps the 3x mode");
    h.clear();s=sample(1000,100,0);h.observe(s,1000);
    for(unsigned i=1;i<=10;++i){s=sample(1000+i*100,100+45*i/10,0,45*i/10);h.observe(s,s.capturedAtMs);}
    r=h.read(2000);check(nearValue(r.generatedFps,0)&&nearValue(r.repeatedFps,45)&&nearValue(r.submittedFps,90),"native FG: repeated native frames never become generated frames, even with a selected 2x pump");
    formatNativeFgCompact(r,text,sizeof(text));check(std::string(text)=="90 FPS | 2x","native FG: repeated submissions still count toward compact FPS");
    formatNativeFgRates(r,text,sizeof(text));check(std::string(text)=="native 45.0 + gen 0.0 + repeat 45.0 fps","native FG: detailed rates still distinguish repeats from generation");
    h.clear();s=sample(1000,100,0);h.observe(s,1000);
    for(unsigned i=1;i<=10;++i){s=sample(1000+i*100,100,0);s.emptyFrames=i;s.failedFrames=i;s.sequence+=2*i;h.observe(s,s.capturedAtMs);}
    r=h.read(2000);check(r.measurement==NativeFgMeasureState::Counting&&nearValue(r.generatedFps,0)&&nearValue(r.submittedFps,0),"native FG: measured empty/failed ticks produce genuine zero rates, not fake frames");
    formatNativeFgCompact(r,text,sizeof(text));check(std::string(text)=="0 FPS | 2x","native FG: a measured zero is shown without falling back");
    s=sample(2100,1000,1000);s.context=18;h.observe(s,2100);
    check(h.read(2100).measurement==NativeFgMeasureState::Measuring,"native FG: a new context with the same runtime generation starts a fresh rate baseline");
    s=sample(2400,1009,1027);s.context=18;h.observe(s,2400);
    check(nearValue(h.read(2400).generatedFps,90),"native FG: fresh context rates use its own deltas");
    s=sample(2500,1010,999);s.context=18;h.observe(s,2500);
    check(h.read(2500).measurement==NativeFgMeasureState::Measuring,"native FG: any counter regression rebases rather than underflowing into huge rates");
    s=sample(6000,1100,1100);s.context=18;h.observe(s,6000);
    check(h.read(6000).measurement==NativeFgMeasureState::Measuring,"native FG: a loading gap cannot dilute the next live rate");
    check(h.read(5999).measurement==NativeFgMeasureState::Stale,"native FG: backwards observation time cannot claim freshness");
    s.available=false;h.observe(s,6000);r=h.read(6000);formatNativeFgCompact(r,text,sizeof(text));
    check(r.measurement==NativeFgMeasureState::Unavailable&&std::string(text)=="FPS unavailable | Off","native FG: an older runtime without the optional publication reports unavailable, never zero");
    card.nativeFg=r;card.haveRendered=true;card.renderedFps=58.4f;
    formatDetailRates(card,text,sizeof(text));check(std::string(text)=="render 58.4 fps | gen unavailable","native FG: mixed older runtime preserves rendered FPS while generation counts stay unavailable");
    check(formatCompactLine(card,text,sizeof(text))&&std::string(text)=="58 FPS | Off","native FG: compact mixed-version readout preserves the known rendered rate");
    r={};r.visible=true;r.automatic=true;r.selectedMultiplier=2;formatNativeFgMode(r,text,sizeof(text));
    check(std::string(text)=="Auto: 2x waiting","native FG: selecting Auto 2x does not claim generated output before the XR pump is active");
    formatNativeFgTileValue(r,text,sizeof(text));check(std::string(text)=="--","native FG: big tile value is short while full waiting mode remains in the note");
    r.selectedMultiplier=0;formatNativeFgMode(r,text,sizeof(text));check(std::string(text)=="Auto: native","native FG: Auto native mode remains visible rather than looking manually Off");
    formatNativeFgTileValue(r,text,sizeof(text));check(std::string(text)=="1x","native FG: Auto native mode fits the large tile face without ellipsis");
    r.failed=true;formatNativeFgMode(r,text,sizeof(text));check(std::string(text)=="Auto: failed","native FG: Auto failure is explicit");
    r.failed=false;r.backendUnavailable=true;formatNativeFgMode(r,text,sizeof(text));check(std::string(text)=="Auto: unavailable","native FG: unavailable backend is explicit");
    r.backendUnavailable=false;r.totals.available=true;r.totals.generatedFrames=r.totals.repeatedFrames=std::numeric_limits<uint64_t>::max();
    char bounded[128];formatNativeFgTotals(r,bounded,sizeof(bounded));check(std::string(bounded).find("18446744073709551615")!=std::string::npos&&std::strlen(bounded)<sizeof(bounded)-1,"native FG: maximum lifetime totals fit without truncation");
    r.measurement=NativeFgMeasureState::Counting;r.nativeFps=45;r.generatedFps=45;r.submittedFps=90;
    formatNativeFgOverlay(r,45,"   gpu 999.9 ms   cpu (pre-submit) 999.9 ms",text,sizeof(text));
    check(std::string(text)=="90 FPS | Auto: native   gpu 999.9 ms   cpu (pre-submit) 999.9 ms","native FG: compact panel ignores lifetime counts and keeps timings within its line");
    const std::string longTimings(kNativeFgCompactMaxChars,' ');
    formatNativeFgOverlay(r,45,longTimings.c_str(),text,sizeof(text));
    check(std::string(text)=="90 FPS | Auto: native","native FG: compact panel omits timings when they would exceed its line");
    r.automatic=false;r.selectedMultiplier=0;r.totals.generatedFrames=0;r.totals.repeatedFrames=0;
    formatNativeFgOverlay(r,45,"   gpu 4.0 ms   cpu 3.0 ms",text,sizeof(text));
    check(std::string(text).find("gpu 4.0 ms")!=std::string::npos,"native FG: compact timings are retained whenever all metrics fit");
    char tiny[1]={'x'};formatNativeFgCompact(r,tiny,sizeof(tiny));check(tiny[0]==0,"native FG: even a one-byte compact destination is terminated");
}
}

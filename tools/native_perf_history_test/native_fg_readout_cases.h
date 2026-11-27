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
    check(std::string(text)=="90 fps | 45 native + 45 gen | x2","native FG: active Auto shows the measured split and applied multiplier");
    formatNativeFgTotals(r,text,sizeof(text));check(std::string(text)=="Auto: 2x | gen total 145 | repeat total 0","native FG: detailed totals have the same Auto mode and counters");
    PerfReadout card;card.haveRendered=true;card.renderedFps=999;card.nativeFg=r;
    formatDetailRates(card,text,sizeof(text));check(std::string(text)=="native 45.0 + gen 45.0 + repeat 0.0 fps","native FG: detailed FPS card uses measured XR counters rather than the unrelated Present rate");
    card.renderedMs=17.1f;formatDetailTimes(card,text,sizeof(text));
    check(std::string(text).find("render 17.1 ms")==0,"native FG: detailed timing labels the rendered clock separately from XR submissions");
    formatBridgeNote(card,text,sizeof(text));check(std::string(text)=="Auto: 2x | gen total 145 | repeat total 0","native FG: detailed card includes actual mode and lifetime totals");
    check(formatCompactLine(card,text,sizeof(text))&&std::string(text)=="90 fps | 45 native + 45 gen | x2",
          "native FG: compact FPS card uses native presentation metrics");
    r.automatic=false;
    formatNativeFgOverlay(r,999,"   gpu 13.3 ms   cpu 1.4 ms",text,sizeof(text));
    check(std::string(text)=="90 fps | 45 native + 45 gen | x2",
          "native FG: active fixed mode shows exactly the split without the render rate or timings");
    auto stale=h.read(4001);formatNativeFgRates(stale,text,sizeof(text));
    check(stale.measurement==NativeFgMeasureState::Stale&&std::string(text)=="FG counts stale"&&stale.totals.generatedFrames==145,"native FG: stale counts keep the last total but never claim a current FPS");
    stale.automatic=true;stale.selectedMultiplier=2;
    formatNativeFgCompact(stale,text,sizeof(text));
    check(std::string(text)=="fps stale | auto: x2 waiting","native FG: stale counts cannot claim a split or an active multiplier");
    formatNativeFgCompact(stale,text,sizeof(text),58);
    check(std::string(text)=="58 fps | auto: x2 waiting","native FG: stale counts can fall back to rendered FPS");

    h.clear();s=sample(1000,100,100);s.activeMultiplier=4;h.observe(s,1000);
    for(unsigned i=1;i<=10;++i){s=sample(1000+i*100,100+30*i/10,100+90*i/10);s.activeMultiplier=4;h.observe(s,s.capturedAtMs);}
    r=h.read(2000);check(nearValue(r.nativeFps,30)&&nearValue(r.generatedFps,90)&&nearValue(r.submittedFps,120),"native FG: 4x counts three generated frames per actual native frame, not one completed batch");
    formatNativeFgCompact(r,text,sizeof(text));
    check(std::string(text)=="120 fps | 30 native + 90 gen | x4","native FG: measured 4x split");
    r.totals.activeMultiplier=3;
    formatNativeFgCompact(r,text,sizeof(text));
    check(std::string(text)=="120 fps | 30 native + 90 gen | x3","native FG: 3x mode does not infer generated FPS");
    formatNativeFgMode(r,text,sizeof(text));check(std::string(text)=="3x","native FG: monitor shows the active 3x mode");
    formatNativeFgTileValue(r,text,sizeof(text));check(std::string(text)=="3x","native FG: compact tile keeps the 3x mode");
    h.clear();s=sample(1000,100,0);h.observe(s,1000);
    for(unsigned i=1;i<=10;++i){s=sample(1000+i*100,100+30*i/10,5*i/10,55*i/10);h.observe(s,s.capturedAtMs);}
    r=h.read(2000);check(nearValue(r.generatedFps,5)&&nearValue(r.repeatedFps,55)&&nearValue(r.submittedFps,90),
        "native FG: repeated frames remain separate from generated frames and still count in submission totals");
    formatNativeFgOverlay(r,90,"   gpu 4.0 ms",text,sizeof(text));
    check(std::string(text)=="35 fps | 30 native + 5 gen | x2","native FG: repeated submissions are excluded from the compact headline");
    formatNativeFgRates(r,text,sizeof(text));
    check(std::string(text)=="native 30.0 + gen 5.0 + repeat 55.0 fps","native FG: detailed rates retain repeats");
    h.clear();s=sample(1000,100,0);h.observe(s,1000);
    for(unsigned i=1;i<=10;++i){s=sample(1000+i*100,100,0);s.emptyFrames=i;s.failedFrames=i;s.sequence+=2*i;h.observe(s,s.capturedAtMs);}
    r=h.read(2000);check(r.measurement==NativeFgMeasureState::Counting&&nearValue(r.generatedFps,0)&&nearValue(r.submittedFps,0),"native FG: measured empty/failed ticks produce genuine zero rates, not fake frames");
    formatNativeFgCompact(r,text,sizeof(text));check(std::string(text)=="0 fps | 0 native + 0 gen | x2","native FG: a measured zero is shown without falling back");
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
    check(r.measurement==NativeFgMeasureState::Unavailable&&std::string(text)=="fps unavailable | off","native FG: an older runtime without the optional publication reports unavailable, never zero");
    card.nativeFg=r;card.haveRendered=true;card.renderedFps=58.4f;
    formatDetailRates(card,text,sizeof(text));check(std::string(text)=="render 58.4 fps | gen unavailable","native FG: mixed older runtime preserves rendered FPS while generation counts stay unavailable");
    check(formatCompactLine(card,text,sizeof(text))&&std::string(text)=="58 fps | off","native FG: compact mixed-version readout preserves the known rendered rate");
    r={};r.visible=true;r.automatic=true;r.selectedMultiplier=2;formatNativeFgMode(r,text,sizeof(text));
    check(std::string(text)=="Auto: 2x waiting","native FG: selecting Auto 2x does not claim generated output before the XR pump is active");
    formatNativeFgCompact(r,text,sizeof(text),45);
    check(std::string(text)=="45 fps | auto: x2 waiting","native FG: compact Auto waiting state is explicit and lowercase");
    formatNativeFgTileValue(r,text,sizeof(text));check(std::string(text)=="--","native FG: big tile value is short while full waiting mode remains in the note");
    r.selectedMultiplier=0;formatNativeFgMode(r,text,sizeof(text));check(std::string(text)=="Auto: native","native FG: Auto native mode remains visible rather than looking manually Off");
    formatNativeFgTileValue(r,text,sizeof(text));check(std::string(text)=="1x","native FG: Auto native mode fits the large tile face without ellipsis");
    r.failed=true;formatNativeFgMode(r,text,sizeof(text));check(std::string(text)=="Auto: failed","native FG: Auto failure is explicit");
    formatNativeFgCompact(r,text,sizeof(text),45);
    check(std::string(text)=="45 fps | auto: failed","native FG: compact failure warning remains visible");
    r.failed=false;r.backendUnavailable=true;formatNativeFgMode(r,text,sizeof(text));check(std::string(text)=="Auto: unavailable","native FG: unavailable backend is explicit");
    formatNativeFgCompact(r,text,sizeof(text));
    check(std::string(text)=="fps unavailable | auto: unavailable","native FG: unavailable compact backend remains explicit");
    r.backendUnavailable=false;r.totals.available=true;r.totals.generatedFrames=r.totals.repeatedFrames=std::numeric_limits<uint64_t>::max();
    char bounded[128];formatNativeFgTotals(r,bounded,sizeof(bounded));check(std::string(bounded).find("18446744073709551615")!=std::string::npos&&std::strlen(bounded)<sizeof(bounded)-1,"native FG: maximum lifetime totals fit without truncation");
    r.measurement=NativeFgMeasureState::Counting;r.nativeFps=45;r.generatedFps=45;r.submittedFps=90;
    formatNativeFgOverlay(r,45,"   gpu 999.9 ms   cpu (pre-submit) 999.9 ms",text,sizeof(text));
    check(std::string(text)=="90 fps | auto: native   gpu 999.9 ms   cpu (pre-submit) 999.9 ms","native FG: compact panel ignores lifetime counts and keeps timings within its line");
    const std::string longTimings(kNativeFgCompactMaxChars,' ');
    formatNativeFgOverlay(r,45,longTimings.c_str(),text,sizeof(text));
    check(std::string(text)=="90 fps | auto: native","native FG: compact panel omits timings when they would exceed its line");
    r.automatic=false;r.selectedMultiplier=0;r.totals.generatedFrames=0;r.totals.repeatedFrames=0;
    formatNativeFgOverlay(r,45,"   gpu 4.0 ms   cpu 3.0 ms",text,sizeof(text));
    check(std::string(text).find("gpu 4.0 ms")!=std::string::npos,"native FG: compact timings are retained whenever all metrics fit");
    char tiny[1]={'x'};formatNativeFgCompact(r,tiny,sizeof(tiny));check(tiny[0]==0,"native FG: even a one-byte compact destination is terminated");
}
}

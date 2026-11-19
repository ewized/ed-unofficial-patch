#pragma once
#include "../../src/openxr/native_fg_bootstrap.h"

namespace edvr::openxr::test {
template<class Check> void runFgCadenceCases(Check check) {
  using namespace fgbootstrap;
  constexpr int64_t period=11111111;
  const Epoch epoch{1,2,3,4};
  struct Batch {
    uint64_t generation=1,configGeneration=2,referenceGeneration=3,historyGeneration=4;
    unsigned multiplier=2,generatedCount=1;
    uint64_t previousSequence=1,currentSequence=2;
  } proof;
  const auto defer=[&](Bootstrap& policy,uint64_t displayed,uint64_t queued,uint64_t exported) {
    return policy.deferNative(displayed,queued,exported,10*period,12*period,period,period,2);
  };

  // Cold or unavailable generation always leaves the game on native cadence.
  Bootstrap cold;cold.sync(epoch);
  uint64_t nextCredit=0;
  for(uint64_t tick=1;tick<=12;++tick) {
    cold.completeNative(tick);
    check(!defer(cold,tick-1,tick,tick-1)&&creditDue(tick,nextCredit,false),
      "FG cold path presents and credits natively");
    cold.displayNative(tick);
    nextCredit=tick+2;
  }

  // Captured source 32851. Normalized pose times preserve the original
  // deadline and the difference between the runtime and admission periods.
  constexpr uint64_t source=32851;
  constexpr int64_t sourcePose=1000000000;
  constexpr int64_t runtimePeriod=11111100;
  constexpr int64_t firstPoll=sourcePose+22221700; // slot 2974, no batch
  constexpr int64_t importTime=sourcePose+33333000; // slot 2975, imports B
  Bootstrap observed;observed.sync(epoch);
  observed.displayNative(source-2);
  observed.displayNative(source-1);
  proof.previousSequence=source-2;proof.currentSequence=source-1;
  // Slot 2972 imported the timely exact previous pair after displaying A.
  observed.observeBehind(proof,interpolationFits(sourcePose,sourcePose-22222600,period,2,1));
  observed.completeNative(source); // completed after 2973 closed
  uint64_t observedWatermark=source-1;
  if(!observed.deferNative(observedWatermark,source,source-1,sourcePose,firstPoll,runtimePeriod,period,2))
    observedWatermark=source;
  check(interpolationFits(importTime,sourcePose,period,2,1)&&
    firstPhase(source-1,source,1,observedWatermark)==1,
    "observed source 32851 stays eligible when its first poll has no batch and the next imports inside its age limit");
  check(creditDue(2974,2975,false),"FG hold leaves the unready slot's pose credit available");

  // The slot that imports B can already have C staged and submitted. A
  // generated B still commits B's endpoint before the newer native C.
  observed.completeNative(source+1);
  proof.previousSequence=source-1;proof.currentSequence=source;
  observed.displayGenerated(proof);
  check(observed.needsEndpoint(source)&&!observed.needsEndpoint(source+1),
    "FG retains the committed native endpoint behind its generated phase");
  observed.displayNative(source);
  check(!observed.needsEndpoint(source)&&observed.completedSequence()==source+1,
    "FG committed B releases its endpoint and preserves newer staged C");

  Bootstrap stalled;stalled.sync(epoch);stalled.displayNative(2);
  proof.previousSequence=1;proof.currentSequence=2;stalled.displayGenerated(proof);
  stalled.completeNative(3);
  check(!defer(stalled,2,2,2)&&!defer(stalled,2,3,3)&&defer(stalled,2,3,2),
    "FG hold requires exact queued work, without an already exported endpoint");
  check(!defer(stalled,2,3,2),"FG stalled backend gets one further poll for the same pair");
  stalled.completeNative(4);
  check(!defer(stalled,2,4,2)&&stalled.completedSequence()==4,
    "FG rapid newer C cannot slide the pending B deadline");
  stalled.abandonNativeAttempt();stalled.displayNative(4);
  for(uint64_t tick=5;tick<=8;++tick) {
    proof.previousSequence=tick-2;proof.currentSequence=tick-1;
    stalled.observeBehind(proof,true);
    stalled.completeNative(tick);
    check(!defer(stalled,tick-1,tick,tick-1)&&creditDue(tick,tick+1,false),
      "FG failed handoff restores native cadence despite repeated timely behind imports");
    stalled.displayNative(tick);
  }
  proof.previousSequence=8;proof.currentSequence=9;
  stalled.displayGenerated(proof);stalled.displayNative(9);stalled.completeNative(10);
  check(defer(stalled,9,10,9),"FG an actually submitted phase re-arms a later handoff");
  const Epoch cuts[]{{1,2,3,5},{1,5,3,5}};
  for(const Epoch& cut:cuts) {
    stalled.sync(cut);stalled.completeNative(10);
    check(!stalled.needsEndpoint(9)&&!defer(stalled,9,10,9),
      "FG epoch or configuration cut retires proof and returns to native immediacy");
  }

  // A late, mismatched or native-only result cannot create a cold trial.
  Bootstrap rejected;rejected.sync(epoch);rejected.displayNative(1);rejected.displayNative(2);
  rejected.completeNative(3);
  proof.previousSequence=1;proof.currentSequence=2;rejected.observeBehind(proof,false);
  proof.previousSequence=0;rejected.observeBehind(proof,true);
  proof.previousSequence=1;proof.generatedCount=0;rejected.observeBehind(proof,true);
  check(!defer(rejected,2,3,2),"FG rejects late, mismatched and native-only cold proof");
  proof.generatedCount=1;rejected.observeBehind(proof,true);
  check(!rejected.deferNative(2,3,2,sourcePose,firstPoll+1000,runtimePeriod,period,2)&&defer(rejected,2,3,2),
    "FG does not spend a hold beyond the existing first-phase deadline");
  rejected.abandonNativeAttempt();rejected.displayNative(3);
  proof.previousSequence=2;proof.currentSequence=3;rejected.observeBehind(proof,true);
  rejected.completeNative(4);
  check(!defer(rejected,3,4,3),"FG failed cold trial cannot rearm from another timely behind batch");
  check(creditDue(20,21,false),"no-render tick cannot spend a spacing slot");
  check(!interpolationFits(17*period,13*period,period,2,1),"late 2x phase cannot restart a handoff");
  check(interpolationFits(24*period,20*period,period,3,1)&&
        interpolationFits(25*period,20*period,period,3,2)&&
        !interpolationFits(26*period,20*period,period,3,2),
    "higher phases get successive real display periods, without a new deadline at import");
  check(!interpolationFits(10,11,period,2,1)&&!interpolationFits(11,10,0,2,1),
    "future time or invalid runtime period cannot prove a generated slot");

  // Drive repeated A/B handoffs. Source renders at 45 FPS, with generation
  // ready 3 ms after native completion and the owner displaying at 90 Hz.
  Bootstrap paced;paced.sync(epoch);
  uint64_t watermark=2,pendingEndpoint=0,next=0;
  paced.displayNative(1);paced.displayNative(watermark);
  proof.previousSequence=1;proof.currentSequence=2;
  paced.observeBehind(proof,true);
  unsigned generated=0,native=0,credits=0,spaced=0;
  for(uint64_t tick=4;tick<=103;++tick) {
    const bool pairCompleted=tick%2==0;
    if(pairCompleted)paced.completeNative(tick/2+1);
    const uint64_t current=(tick-1)/2+1;
    const bool outputReady=!pairCompleted&&((tick-1)*period+3000000)<tick*period;
    const bool gen=outputReady&&firstPhase(current-1,current,1,watermark)&&
        interpolationFits(tick*period,(tick-3)*period,period,2,1);
    if(creditDue(tick,next,gen)) {
      if(pairCompleted){++credits;next=tick+2;}
    } else ++spaced;
    if(gen) {
      proof.previousSequence=current-1;proof.currentSequence=current;
      paced.displayGenerated(proof);pendingEndpoint=current;++generated;
    } else if(paced.needsEndpoint(pendingEndpoint)) {
      watermark=pendingEndpoint;paced.displayNative(watermark);pendingEndpoint=0;++native;
    } else if(paced.completedSequence()&&
        !paced.deferNative(watermark,tick/2+1,watermark,(tick-2)*period,tick*period,period,period,2)) {
      paced.abandonNativeAttempt();
      watermark=paced.completedSequence();paced.displayNative(watermark);++native;
    }
  }
  check(generated==50&&native==49&&credits==50&&spaced==50&&watermark==51&&pendingEndpoint==52,
    "45 FPS source sustains ordered generated/native XR presentation at 90 Hz without catch-up starvation");
}
} // namespace edvr::openxr::test

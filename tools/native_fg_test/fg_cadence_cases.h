#pragma once
#include "../../src/openxr/native_fg_bootstrap.h"

namespace edvr::openxr::test {
template<class Check> void runFgRuntimeOpportunityCases(Check check) {
  using namespace fgbootstrap;
  constexpr int64_t poseTime=1000000000,period=11111111,runtimePeriod=11111100;
  constexpr int64_t before=poseTime+22221200,due=poseTime+33333400;
  constexpr Epoch epoch{1,2,3,4};
  // Exact prediction sequence from fa343 slots 858–861. The final wait
  // moves the predicted timestamp by 1.1 us from the preceding forecast.
  const auto capture=[&](Bootstrap& timeline) {
    timeline.clear();timeline.sync(epoch);
    timeline.noteSlot(858,poseTime,runtimePeriod,period);
    timeline.noteSlot(859,poseTime+11110400,runtimePeriod,period);
    timeline.noteSlot(860,before,runtimePeriod,period);
  };
  Bootstrap timeline;
  capture(timeline);
  check(timeline.phaseFits(before,poseTime,period,2,1,runtimePeriod),"FG forecasts at most the original opportunity horizon");
  timeline.noteSlot(861,due,runtimePeriod,period);
  check(!interpolationFits(due,poseTime,period,2,1)&&timeline.acceptPhase(due,poseTime,period,2,1),
    "FG exact third opportunity survives dynamic runtime re-prediction");
  check(!timeline.phaseFits(due,poseTime-1,period,2,1),"FG unknown source time keeps strict wall age");
  check(!timeline.phaseFits(due,poseTime,period,2,1,runtimePeriod),"FG forecasts no phase after the original final opportunity");

  // fa343 slot 1081: two owner waits cross four predicted display
  // opportunities because each earlier wait projected a double interval.
  Bootstrap skipped;skipped.sync(epoch);
  skipped.noteSlot(1079,poseTime,2*runtimePeriod,period);
  skipped.noteSlot(1080,poseTime+2*runtimePeriod,2*runtimePeriod,period);
  check(!skipped.phaseFits(poseTime+2*runtimePeriod,poseTime,period,2,1,2*runtimePeriod),
    "FG double-period forecast cannot create a hold for a skipped opportunity");
  skipped.noteSlot(1081,poseTime+4*runtimePeriod,2*runtimePeriod,period);
  check(!skipped.acceptPhase(poseTime+4*runtimePeriod,poseTime,period,2,1),
    "FG rejects four opportunities during two double-period waits");

  constexpr int64_t testPeriod=1000;
  Bootstrap drift;drift.sync(epoch);
  for(uint64_t i=1;i<=4;++i)drift.noteSlot(i,poseTime+int64_t(i-1)*1400,testPeriod,testPeriod);
  check(!drift.phaseFits(poseTime+4200,poseTime,testPeriod,2,1),"FG a chain cannot accumulate timing drift into an extra opportunity");
  drift.noteSlot(5,poseTime+5700,testPeriod,testPeriod);
  check(!drift.phaseFits(poseTime+5700,poseTime+1400,testPeriod,2,1),"FG exact half-interval ambiguity retains strict wall age");
  Bootstrap later;later.sync(epoch);
  for(uint64_t i=1;i<=8;++i)later.noteSlot(i,poseTime+int64_t(i-1)*1001,testPeriod,testPeriod);
  check(later.phaseFits(poseTime+7007,poseTime,testPeriod,4,3)&&!later.phaseFits(poseTime+7007,poseTime,testPeriod,4,2),
    "FG bounded history retains the last legal 4x phase and enforces each phase's horizon");

  const Epoch cuts[]{{2,2,3,4},{1,5,3,4},{1,2,6,4},{1,2,3,7}};
  for(const auto& cut:cuts) {
    capture(timeline);timeline.sync(cut);timeline.noteSlot(861,due,runtimePeriod,period);
    check(!timeline.phaseFits(due,poseTime,period,2,1),"FG generation/config/reference/history change retires the old timeline");
  }
  capture(timeline);
  timeline.syncDisplayPeriod(period+1);
  timeline.noteSlot(861,due,runtimePeriod,period+1);
  check(!timeline.phaseFits(due,poseTime,period+1,2,1),"FG refresh rate change retires the old timeline");
  const int64_t unknownPeriods[]{0,-period,(std::numeric_limits<int64_t>::min)()};
  for(const int64_t unknownPeriod:unknownPeriods) {
    capture(timeline);timeline.noteSlot(861,due,runtimePeriod,unknownPeriod);
    check(!timeline.phaseFits(due,poseTime,period,2,1),"FG invalid or unknown refresh period cannot prove opportunity age");
  }
  const uint64_t invalidTicks[]{859,860,862};
  for(const uint64_t tick:invalidTicks) {
    capture(timeline);timeline.noteSlot(tick,due,runtimePeriod,period);
    check(!timeline.phaseFits(due,poseTime,period,2,1),"FG reordered, repeated or missing runtime slot retains strict wall age");
  }
  capture(timeline);timeline.noteSlot(861,before-1,runtimePeriod,period);
  timeline.noteSlot(862,due,runtimePeriod,period);
  check(!timeline.phaseFits(due,poseTime,period,2,1),"FG nonmonotonic prediction breaks the chain");
  capture(timeline);timeline.noteSlot(861,0,runtimePeriod,period);
  timeline.noteSlot(862,due,runtimePeriod,period);
  check(!timeline.phaseFits(due,poseTime,period,2,1),"FG missing successful runtime prediction breaks the chain");
  constexpr int64_t max=(std::numeric_limits<int64_t>::max)(),min=(std::numeric_limits<int64_t>::min)();
  check(!timeline.phaseFits(max-1,max-2,period,2,1,runtimePeriod)&&
        !timeline.phaseFits(due,min,period,2,1)&&!timeline.phaseFits(due,poseTime,period,2,1,min)&&
        !timeline.phaseFits(due,poseTime,period,2,1,max),"FG checked age arithmetic rejects extreme timestamps and forecasts");
}

template<class Check> void runFgRecoveryCases(Check check) {
  using namespace fgbootstrap;
  using Handoff=fghandoff::Handoff;
  constexpr int64_t period=11111111;
  constexpr Epoch epoch{1,2,3,4};
  struct Batch {
    uint64_t generation=1,configGeneration=2,referenceGeneration=3,historyGeneration=4;
    uint64_t previousSequence=3,currentSequence=4;
    unsigned multiplier=2,generatedCount=1,phaseMask=3;
    int64_t previousPoseDisplayTime=13*period,currentPoseDisplayTime=15*period;
  } fresh;
  const auto observe=[](Bootstrap& policy,const Batch& batch,uint64_t tick) {
    const int64_t display=int64_t(tick)*period;
    policy.observeBehind(batch,policy.phaseFits(display,batch.currentPoseDisplayTime,period,2,1),tick,display,period);
  };
  Handoff decision=Handoff::None;
  const auto defer=[&](Bootstrap& policy,uint64_t displayed,uint64_t tick,int64_t sourcePose) {
    return policy.deferNative(displayed,displayed+1,displayed,sourcePose,int64_t(tick)*period,period,period,2,&decision);
  };
  const auto afterMiss=[&](uint64_t nativeTick) {
    Bootstrap policy;policy.sync(epoch);
    auto generated=fresh;generated.previousSequence=1;generated.currentSequence=2;
    policy.displayNative(1,10);policy.displayGenerated(generated);policy.displayNative(2,11);
    for(uint64_t tick=11;tick<=14;++tick)policy.noteSlot(tick,int64_t(tick)*period,period,period);
    policy.completeNative(3);
    check(defer(policy,2,14,13*period)&&decision==Handoff::Deferred,"FG warm cadence charges one slot");
    policy.noteSlot(15,15*period,period,period);
    check(!defer(policy,2,15,13*period)&&decision==Handoff::AttemptAlreadySpent,
      "FG empty next poll always releases immediate native fallback");
    policy.abandonNativeAttempt();policy.displayNative(3,nativeTick);
    policy.noteSlot(16,16*period,period,period);policy.completeNative(4);
    return policy;
  };
  Bootstrap policy=afterMiss(15);
  auto attempted=fresh;attempted.previousSequence=2;attempted.currentSequence=3;
  attempted.previousPoseDisplayTime=12*period;attempted.currentPoseDisplayTime=13*period;
  observe(policy,attempted,16);
  check(!defer(policy,3,16,15*period)&&decision==Handoff::ColdTrialSpent,
    "FG later arrival of the charged source cannot replenish its trial");
  policy.displayNative(4,16);policy.noteSlot(17,17*period,period,period);policy.completeNative(5);

  Bootstrap recovery=policy;
  observe(recovery,fresh,17);
  check(defer(recovery,4,17,16*period)&&decision==Handoff::RecoveryDeferred,
    "FG newer exact full batch on the immediately following owner tick recovers one trial");
  recovery.noteSlot(18,18*period,period,period);
  check(!defer(recovery,4,18,16*period)&&decision==Handoff::AttemptAlreadySpent&&creditDue(18,19,false),
    "FG a recovery trial can miss once without extending the hold or credit spacing");
  recovery.abandonNativeAttempt();recovery.displayNative(5,18);
  recovery.noteSlot(19,19*period,period,period);recovery.completeNative(6);
  observe(recovery,fresh,19);
  check(!defer(recovery,5,19,18*period)&&decision==Handoff::ColdTrialSpent,
    "FG duplicate proof cannot recover a second time after progress consumes it");

  Batch invalid[]{fresh,fresh,fresh,fresh,fresh,fresh,fresh,fresh,fresh,fresh};
  invalid[0].previousSequence=2;invalid[1].phaseMask=1;invalid[2].generatedCount=0;
  invalid[3].currentPoseDisplayTime=18*period;invalid[4].currentPoseDisplayTime=12*period;
  invalid[4].previousPoseDisplayTime=11*period;invalid[5].currentPoseDisplayTime=15*period+1;
  invalid[6].generation=9;invalid[7].configGeneration=9;invalid[8].referenceGeneration=9;invalid[9].historyGeneration=9;
  for(const auto& batch:invalid) {
    Bootstrap refused=policy;observe(refused,batch,17);
    check(!defer(refused,4,17,16*period)&&decision==Handoff::ColdTrialSpent,
      "FG recovery rejects unproven endpoints, phase metadata, pose time or epoch");
  }
  Bootstrap skipped=policy;
  skipped.displayNative(4,17);skipped.noteSlot(18,18*period,period,period);
  observe(skipped,fresh,18);
  check(!defer(skipped,4,18,17*period),"FG a repeated native endpoint cannot restart the next-tick evidence window");
  Bootstrap unknown=afterMiss(0);
  unknown.displayNative(4,16);unknown.noteSlot(17,17*period,period,period);unknown.completeNative(5);
  observe(unknown,fresh,17);
  check(!defer(unknown,4,17,16*period),"FG both endpoints must have real successful native submission ticks");
  Bootstrap changed=policy;
  changed.syncDisplayPeriod(period+1);
  changed.noteSlot(17,17*period,period+1,period+1);
  observe(changed,fresh,17);
  check(!defer(changed,4,17,16*period),"FG strict wall age alone after a rate change cannot recover a trial");
  Bootstrap broken=policy;observe(broken,fresh,17);
  broken.noteSlot(18,0,period,period);broken.noteSlot(19,19*period,period,period);
  check(!defer(broken,4,19,18*period),"FG an intervening runtime timeline break invalidates acquired recovery proof");
  policy.sync({1,2,3,5});policy.completeNative(5);observe(policy,fresh,17);
  check(!defer(policy,4,17,16*period)&&decision==Handoff::NoSynthesisProof,
    "FG a history reset discards the previous pair and charged source");
}

template<class Check> void runFgCadenceCases(Check check) {
  using namespace fgbootstrap;
  runFgRuntimeOpportunityCases(check);
  runFgRecoveryCases(check);
  constexpr int64_t period=11111111;
  const Epoch epoch{1,2,3,4};
  struct Batch {
    uint64_t generation=1,configGeneration=2,referenceGeneration=3,historyGeneration=4;
    unsigned multiplier=2,generatedCount=1;
    uint64_t previousSequence=1,currentSequence=2;
    unsigned phaseMask=3;
    int64_t previousPoseDisplayTime=0,currentPoseDisplayTime=0;
  } proof;
  const auto defer=[&](Bootstrap& policy,uint64_t displayed,uint64_t queued,uint64_t exported) {
    return policy.deferNative(displayed,queued,exported,10*period,12*period,period,period,2);
  };

  // The native decision is identical with and without a trace sink. A
  // consumed trial and a missing synthesis proof need distinct diagnostics.
  using Handoff=fghandoff::Handoff;
  Handoff observedDecision=Handoff::None;
  Bootstrap diagnostic;diagnostic.sync(epoch);diagnostic.displayNative(1);diagnostic.displayNative(2);
  diagnostic.completeNative(3);
  const auto observeDecision=[&](uint64_t queued,int64_t displayTime) {
    return diagnostic.deferNative(2,queued,2,10*period,displayTime,period,period,2,&observedDecision);
  };
  check(!observeDecision(3,12*period)&&observedDecision==Handoff::NoSynthesisProof,
    "FG diagnostic distinguishes missing synthesis proof");
  diagnostic.observeBehind(proof,true);
  check(!observeDecision(2,12*period)&&observedDecision==Handoff::SourceIdentity,
    "FG diagnostic identifies a mismatched queued source");
  check(!observeDecision(3,12*period+1)&&observedDecision==Handoff::Time,
    "FG diagnostic identifies a source past its forecast horizon");
  check(observeDecision(3,12*period)&&observedDecision==Handoff::Deferred,
    "FG diagnostic records the actual charged trial");
  check(!observeDecision(3,12*period)&&observedDecision==Handoff::AttemptAlreadySpent,
    "FG diagnostic identifies the same source's already charged slot");
  diagnostic.abandonNativeAttempt();diagnostic.displayNative(3);diagnostic.completeNative(4);
  check(!diagnostic.deferNative(3,4,3,10*period,12*period,period,period,2,&observedDecision)&&
        observedDecision==Handoff::ColdTrialSpent,"FG diagnostic distinguishes exhausted cold recovery");

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

  // Capture 62c1b3aa, slots 2099–2105. The owner presented G(51810,51811)
  // and 51811. G(51811,51812) was then 67 ns late with no trial in progress,
  // so the owner presented the newer complete 51813 immediately.
  constexpr int64_t origin=24697100000000LL;
  constexpr int64_t pose51812=24697117797500LL-origin,slot2101=24697151130900LL-origin;
  constexpr int64_t pose51814=24697162248700LL-origin,slot2104=24697184463900LL-origin;
  constexpr int64_t slot2105=24697195575200LL-origin;
  Bootstrap recovered;recovered.sync(epoch);recovered.displayNative(51810);
  proof.previousSequence=51810;proof.currentSequence=51811;
  recovered.displayGenerated(proof);recovered.displayNative(51811);recovered.completeNative(51813);
  check(!recovered.acceptPhase(slot2101,pose51812,period,2,1),"FG slot 2101 still rejects the strictly late phase");
  recovered.consumeNative(51813);recovered.displayNative(51813);
  proof.previousSequence=51812;proof.currentSequence=51813;
  recovered.observeBehind(proof,true);
  recovered.completeNative(51814);
  const bool held51814=recovered.deferNative(51813,51814,51813,pose51814,slot2104,runtimePeriod,period,2);
  check(interpolationFits(slot2105,pose51814,period,2,1)&&
    firstPhase(51813,51814,1,held51814?51813:51814)==1,
    "FG slot 2105 presents source 51814 after an isolated unheld late import");

  // A real phase replenishes the cold trial. Suppression clears warm proof,
  // so recovery requires a timely batch for the exact displayed endpoints.
  Bootstrap retry;retry.sync(epoch);retry.displayNative(1);
  proof.previousSequence=1;proof.currentSequence=2;
  retry.displayGenerated(proof);retry.displayNative(2);retry.miss();retry.completeNative(3);
  check(!defer(retry,2,3,2),"FG recovery needs real phase delivery or exact cold proof");
  retry.observeBehind(proof,true);
  check(defer(retry,2,3,2),"FG a presented phase replenishes one cold recovery trial");
  check(!retry.acceptPhase(slot2101,pose51812,period,2,1),"FG a late phase still fails the spent recovery trial");
  retry.displayNative(3);retry.completeNative(4);
  proof.previousSequence=2;proof.currentSequence=3;retry.observeBehind(proof,true);
  check(!defer(retry,3,4,3)&&creditDue(4,5,false),"FG a failed recovery trial cannot loop on behind evidence");

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

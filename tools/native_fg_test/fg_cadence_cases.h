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
  } proof;
  Bootstrap policy;policy.sync(epoch);

  // At startup the game can run natively even if the worker is still warming.
  uint64_t nextCredit=0;
  for(uint64_t tick=1;tick<=12;++tick) {
    policy.completeNative(tick,tick);
    check(policy.refreshDue(tick)&&creditDue(tick,nextCredit,false),"FG cold path presents and credits natively");
    policy.displayNative(tick);
    nextCredit=tick+2;
  }

  // 45 FPS source on a 90 Hz runtime. B receives a credit at tick 13,
  // finishes in slot 15, and its NGX result finishes 3 ms later. Import
  // before slot 16's presentation, then present B in slot 17.
  policy.observe(proof,interpolationFits(13*period,11*period,period,2,1));
  policy.completeNative(13,15);
  check(!policy.refreshDue(15),"FG proof preserves displayed A for one handoff slot");
  check(creditDue(15,17,false),"B completion without a ready phase cannot hold next native credit");
  constexpr int64_t ngxReady=15*period+3000000;
  const bool generatedSlot=ngxReady<16*period&&interpolationFits(16*period,13*period,period,2,1);
  check(generatedSlot&&!creditDue(16,17,generatedSlot),"timely A->B phase can spend one spacing slot");
  policy.displayNative(13);
  check(creditDue(17,17,false)&&!policy.refreshDue(17),"B appears after its intermediate and retires handoff");

  // Lost/suppressed synthesis after warmup: promote complete native B by
  // the next real XR slot, and never push the deadline by staging a newer C.
  policy.completeNative(14,18);
  policy.completeNative(15,19);
  check(policy.completedSequence()==15&&policy.refreshDue(19)&&creditDue(19,20,false),
    "missing generated slot promotes newest complete native and cancels credit hold");
  policy.miss();
  policy.displayNative(15);
  policy.completeNative(16,20);
  check(policy.refreshDue(20),"after a miss native presents immediately until new synthesis proof");
  policy.displayNative(16);
  check(creditDue(20,21,false),"no-render tick cannot spend a spacing slot");
  check(!interpolationFits(17*period,13*period,period,2,1),"late 2x phase cannot restart a handoff");
  check(interpolationFits(24*period,20*period,period,3,1)&&
        interpolationFits(25*period,20*period,period,3,2)&&
        !interpolationFits(26*period,20*period,period,3,2),
    "higher phases get successive real display periods, not a new deadline from import");
  check(!interpolationFits(10,11,period,2,1)&&!interpolationFits(11,10,0,2,1),
    "future time or invalid runtime period cannot prove a generated slot");

  // Drive repeated A/B handoffs. The source is naturally limited to one
  // render per two runtime slots. Each ready time follows native completion
  // by 3 ms, so synthesis cannot accidentally pass before the current pair.
  Bootstrap paced;paced.sync(epoch);paced.observe(proof,true);
  uint64_t watermark=2,pendingEndpoint=0,next=0;
  unsigned generated=0,native=0,credits=0,spaced=0;
  for(uint64_t tick=4;tick<=103;++tick) {
    const bool pairCompleted=tick%2==0;
    if(pairCompleted)paced.completeNative(tick/2+1,tick);
    const uint64_t current=(tick-1)/2+1;
    const bool outputReady=!pairCompleted&&((tick-1)*period+3000000)<tick*period;
    const bool gen=outputReady&&firstPhase(current-1,current,1,watermark)&&
        interpolationFits(tick*period,(tick-3)*period,period,2,1);
    if(creditDue(tick,next,gen)) {
      if(pairCompleted){++credits;next=tick+2;}
    } else ++spaced;
    if(gen){pendingEndpoint=current;++generated;}
    else if(pendingEndpoint) {
      watermark=pendingEndpoint;paced.displayNative(watermark);pendingEndpoint=0;++native;
    } else if(paced.refreshDue(tick)) {
      watermark=paced.completedSequence();paced.displayNative(watermark);++native;
    }
  }
  check(generated==50&&native==49&&credits==50&&spaced==50&&watermark==51&&pendingEndpoint==52,
    "45 FPS source sustains ordered generated/native XR presentation at 90 Hz without catch-up starvation");

  policy.sync({1,2,3,5});
  policy.completeNative(16,21);
  check(policy.refreshDue(21),"history cut clears warm proof and restores native immediacy");
}
} // namespace edvr::openxr::test

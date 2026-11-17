#pragma once
#include "../../src/d3d11/vrs_work_reference_policy.h"
#include <stdexcept>
namespace edvr {namespace vrs_reference_fixture {inline unsigned begins=0,ends=0;}
inline VrsWorkReferenceToken vrsWorkReferenceBeforeNative(ID3D11DeviceContext*,const VrsCoverageSignature&) {++vrs_reference_fixture::begins;return {1,0,true};}
inline void vrsWorkReferenceAfterNative(ID3D11DeviceContext*,const VrsWorkReferenceToken&) {++vrs_reference_fixture::ends;}
}
template<class Check> void workReferenceTests(Check check) {
    using namespace edvr;using namespace edvr::vrs_work_reference;
    bool marker=true;const bool own=consumeInternal(marker);const bool recursive=consumeInternal(marker);
    check(own&&!recursive&&!marker,"own-invocation marker is consumed before driver reentry, which remains external");
    check(!consumeInternal(marker),"ordinary external setter never inherits an internal invocation marker");
    VrsCoverageSignature s{vrs_coverage::kWorldVs,vrs_coverage::kWorldPsE46,288,4,14674580,42,2491586,'X'};
    check(selected(s),"explicit useful draw selector accepted");s.startInstance++;check(selected(s),"batch offset is recorded and paired, not silently baked into selector");
    s.count++;check(!selected(s),"adjacent draw cannot substitute for predeclared work");s.count--;s.ps++;check(!selected(s),"VS alone never selects a reference");
    Schedule schedule;auto p=schedule.peek(0,42,1000);check(p.ready&&p.arm==Arm::Natural&&p.cycle==1,"cycle begins with natural VRS");schedule.issued(0,42,1000);
    check(!schedule.peek(0,43,1000).ready,"exact matching startInstance enforced within burst");
    p=schedule.peek(0,42,1001);check(p.ready&&p.arm==Arm::Full&&p.cycle==1,"same-offset full-rate reference follows natural immediately");schedule.issued(0,42,1001);
    p=schedule.peek(0,42,1002);check(p.ready&&p.arm==Arm::Coarse&&p.cycle==1,"same-offset 2x2 calibration shares cycle");schedule.issued(0,42,1002);
    check(!schedule.peek(0,42,4001).ready&&schedule.peek(0,99,4002).arm==Arm::Natural&&schedule.peek(1,99,1000).cycle==1,"three-query burst has at least3s cooldown and eyes remain separate");
    schedule.issued(0,99,4002);p=schedule.peek(0,99,5003);check(p.expired&&!p.ready,"missing matched work expires without fallback");
    Rate original;original.parsed=true;original.count=1;original.enabled=true;original.palette.fill(8);
    Override clamp;clamp.start(Arm::Full,original);auto actual=clamp.request(original,true,true);check(actual.uniform(5)&&clamp.lastOriginal.same(original),"own full-rate apply preserves original request");clamp.result(actual,0);
    Rate nested=original;nested.palette.fill(11);actual=clamp.request(nested,false,true);clamp.result(actual,0,&nested);
    check(actual.uniform(5)&&clamp.lastOriginal.same(nested)&&clamp.nestedRates==1,"nested Magic re-enable remains full-rate and records latest original palette");
    clamp.restoring=true;actual=clamp.request(clamp.lastOriginal,true,true);check(actual.same(nested),"restore forwards latest original descriptor, not the modified palette");clamp.result(actual,0);
    clamp.start(Arm::Coarse,original);actual=clamp.request(nested,false,true);check(actual.uniform(8),"2x2 calibration reuses scoped palette override");
    Rate unsupported=original;unsupported.count=2;actual=clamp.request(unsupported,false,true);check(actual.count==2&&clamp.unexpected==1,"unsupported viewport descriptor forwards unchanged and cannot qualify");
    clamp.result(actual,0,&unsupported);
    check(clamp.surrendered&&!clamp.forced&&!clamp.restoreRequired,"accepted unsupported descriptor supersedes override and forbids stale restoration");
    actual=clamp.request(nested,false,true);check(actual.same(nested),"surrendered scope never overrides a later supported request");
    clamp.start(Arm::Full,original);actual=clamp.request(nested,false,false);check(actual.same(nested)&&clamp.unexpected==1,"concurrent foreign-thread setter is never rewritten or qualified");
    check(clamp.surrendered&&!clamp.restoreRequired&&!clamp.forced,"in-flight foreign setter forbids stale cleanup before its driver result returns");
    check(!clamp.internalWriteAllowed(0),"surrendered scope cannot send an obsolete internal apply or restore");
    clamp.result(actual,0,&nested,false);check(clamp.surrendered&&!clamp.restoreRequired&&clamp.lastOriginal.same(original),"accepted foreign intent is not replaced by a stale owner-thread restore");
    clamp.start(Arm::Full,original);actual=clamp.request(nested,false,true);clamp.result(actual,-1,&nested);
    check(clamp.lastOriginal.same(original)&&clamp.restoreRequired&&!clamp.surrendered,"failed setter cannot replace last successful original restoration target");
    check(clamp.internalWriteAllowed(0)&&!clamp.internalWriteAllowed(1),"internal driver write refuses any already in-flight external writer without waiting");
    clamp.result(actual,-1);check(clamp.unexpected>=2,"driver failure poisons reference evidence");
    Rate zero;zero.parsed=true;clamp.start(Arm::Full,original);actual=clamp.request(zero,false,true);clamp.result(actual,0,&zero);check(actual.same(zero)&&clamp.lastOriginal.same(zero),"zero-viewport disable retains its exact descriptor and restoration intent");
    clamp.start(Arm::Coarse,original);actual=clamp.request(zero,false,true);clamp.result(actual,0,&zero);check(clamp.unexpected>0,"disabled viewport is never a successful 2x2 calibration");
    original.palette[0]=0;clamp.start(Arm::Full,original);actual=clamp.request(original,true,true);check(actual.palette[0]==0&&actual.palette[1]==5,"uniform reference preserves every original cull entry");
    check(workMatches(1152,384,1)&&!workMatches(1152,384,0)&&!workMatches(2304,768,9)&&!workMatches(0,0,0),"zero and excess native IA work cannot qualify");
    unsigned draws=0;bool armed=true;vrs_reference_fixture::begins=vrs_reference_fixture::ends=0;
    {VrsWorkReferenceScope scope(nullptr,{});++draws;armed=false;}
    check(!armed&&vrs_reference_fixture::begins==1&&draws==1&&vrs_reference_fixture::ends==1,"production native scope cleans up after mid-issue OFF and exactly one forwarding call");
    try{VrsWorkReferenceScope scope(nullptr,{});throw std::runtime_error("fixture");}catch(const std::runtime_error&){}
    check(vrs_reference_fixture::ends==2,"production native scope cleanup runs during exception unwind");
    EdvrPvrGazeSnapshotV1 snapshot{};snapshot.size=sizeof(snapshot);snapshot.version=1;snapshot.sequence=1;snapshot.mode=1;snapshot.qpcFrequency=1000000;snapshot.callbackQpc=999000;snapshot.trackerPresent=snapshot.providerQueried=snapshot.providerFiniteValid=1;
    check(modeOne(snapshot,1000000,1000000)&&snapshot.returnedValid==0,"mode proof uses recent published mode, without inventing sensor validity");snapshot.mode=0;
    check(!modeOne(snapshot,1000000,1000000),"mode zero cannot arm interventions");snapshot.mode=1;
    check(!modeOne(snapshot,1100000,1000000)&&!modeOne(snapshot,1000000,1000001),"stale or mismatched publication clock refuses reference");
}

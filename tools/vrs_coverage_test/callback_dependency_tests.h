#pragma once
#include "../../src/d3d11/vrs_callback_dependency.h"
#include "../../src/d3d11/vrs_coverage_policy.h"
#include <limits>
// Caller supplies its existing check(bool,const char*) function; no runtime calls.
template<class Check> void callbackDependencyTests(Check check) {
    using namespace edvr::vrs_callback_dependency;
    Sample a;a.worldVerified=true;a.worldIdentity=1;a.mapIdentity=2;a.eye=0;
    a.rtW=a.rtH=2133;a.mapW=a.mapH=a.tilesW=a.tilesH=134;
    a.qpcFrequency=1000000;a.nowQpc=1100000;
    a.gaze.size=sizeof(a.gaze);a.gaze.version=1;a.gaze.sequence=1;
    a.gaze.qpcFrequency=a.qpcFrequency;a.gaze.callbackQpc=1099000;
    a.gaze.trackerPresent=a.gaze.providerQueried=a.gaze.providerFiniteValid=1;a.gaze.mode=1;
    check(eligible(a)&&a.gaze.returnedValid==0,"Time0-style callback geometry qualifies separately without asserting returned validity");
    Sample b=a;b.gaze.sequence++;b.gaze.callbackQpc+=1000000;b.nowQpc+=1000000;
    check(pairEligible(a,b),"same verified world/map and monotonic callbacks can form a geometry pair");
    for(unsigned flag=0;flag<5;++flag){Sample bad=a;
        if(flag==0)bad.gaze.cachedFallback=1;if(flag==1)bad.gaze.ignored=1;
        if(flag==2)bad.gaze.providerQueried=0;if(flag==3)bad.gaze.providerFiniteValid=0;
        if(flag==4)bad.gaze.trackerPresent=0;
        check(!eligible(bad),"cached/ignored/unqueried/invalid/absent provider geometry refused");}
    Sample bad=a;bad.gaze.callbackQpc=a.nowQpc-50001;check(!eligible(bad),"old callback refused");
    bad=a;bad.gaze.callbackQpc=a.nowQpc+1;check(!eligible(bad),"future callback refused");
    bad=a;bad.gaze.qpcFrequency++;check(!eligible(bad),"clock frequency mismatch refused");
    bad=a;bad.gaze.size--;check(!eligible(bad),"snapshot ABI mismatch refused");
    bad=a;bad.gaze.returnedGazeTan[1][1]=std::numeric_limits<float>::infinity();check(!eligible(bad),"nonfinite returned geometry refused in either eye");
    bad=b;bad.gaze.sequence=a.gaze.sequence;check(!pairEligible(a,bad),"repeated callback sequence refused");
    bad=b;bad.gaze.callbackQpc=a.gaze.callbackQpc;check(!pairEligible(a,bad),"nonmonotonic callback clock refused");
    bad=b;bad.gaze.mode=2;check(!pairEligible(a,bad),"mode change cannot form a pair");
    bad=b;bad.gaze.invertY=1;check(!pairEligible(a,bad),"inversion change cannot form a pair");
    bad=b;bad.worldIdentity++;check(!pairEligible(a,bad),"different verified world identity refused");
    bad=b;bad.mapIdentity++;check(!pairEligible(a,bad),"different accepted map identity refused");
    bad=b;bad.palette[1]++;check(!pairEligible(a,bad),"changed palette refused");
    bad=b;bad.slice++;check(!pairEligible(a,bad),"changed map subresource refused");
    bad=b;bad.rtW++;check(!pairEligible(a,bad),"changed world dimensions refused");
    bad=b;bad.gaze.callbackQpc+=3000000;bad.nowQpc+=3000000;check(!pairEligible(a,bad),"long observation gap refused");
    edvr::vrs_coverage::DirectionCounts direction;
    direction.add(0,0,0,0,134,134);
    check(direction.pairs==0&&direction.xPairs==0&&direction.yPairs==0&&direction.noise==1,"flat mannequin signal remains insufficient excitation");
    direction.add(.1,.1,1,1,134,134);
    check(direction.pairs==1&&direction.xAgree==1&&direction.ySameAgree==1,"separate eligible geometry can use existing centroid direction policy");
    check(a.gaze.returnedValid==0&&b.gaze.returnedValid==0,"helper leaves Time0 returned validity unchanged");
}

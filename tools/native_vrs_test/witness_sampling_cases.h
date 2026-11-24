#pragma once
#include "../../src/d3d11/native_vrs_witness_sampling.h"
#include "../../src/d3d11/native_vrs_depth_copy_census.h"

inline void witnessSamplingCases(void (*check)(bool,const char*)){
    using namespace edvr::native_vrs;
    WitnessSamplePlan plan;
    for(unsigned k=0;k<plan.strata;++k)for(unsigned n=0;n<32;++n){
        const auto p=plan.event(k);
        if(p.weight || p.outside){check(false,"pilot observes without sampling");return;}
    }
    unsigned visits[WitnessSamplePlan::strata]{};
    bool weights=true,cap=true,overflow=true;
    for(unsigned frame=0;frame<256;++frame){
        plan.advance();
        unsigned selected=0;
        for(unsigned k=0;k<plan.strata;++k){
            const auto width=plan.rows[k].extent;
            // Visit the entire selection window, then one uncovered ordinal.
            for(unsigned n=1;n<=width+1;++n){
                const auto p=plan.event(k);
                if(p.weight){
                    ++selected;++visits[k];
                    weights&=p.weight==uint64_t(width)*plan.strata/plan.perFrame && n<=width;
                }
                overflow&=p.outside==(n>width);
            }
            plan.rows[k].seen=32; // stable pilot population for the next trial
        }
        cap&=selected==plan.perFrame;
    }
    bool covered=true;for(auto v:visits)covered&=v>0;
    check(weights,"sample weight includes stratum and rank selection");
    check(cap,"at most three distinct intervals can be chosen each frame");
    check(overflow,"events outside the pilot envelope remain visible");
    check(covered,"both synchronous and all cohort size/copy strata get sampled");
    check(witnessDepthOnlyReads(false,true,false,true,false,true,false,true),"zero stencil mask and depth disabled read only");
    check(!witnessDepthOnlyReads(false,true,false,true,true,true,false,true),"either stencil face can write even with depth disabled");
    check(witnessDepthOnlyReads(true,false,false,true,true,true,false,false),"depth-only formats have no stencil plane");
    check(!witnessDepthOnlyReads(true,true,false,false,false,false,false,false),"writable depth counts even with no stencil");
    check(witnessDepthOnlyReads(true,true,true,true,true,true,true,true),"read-only depth and stencil view flags independently prevent writes");

    const WitnessDepthStencilTuple base{1,1,7,1,0xff,0x3, {8,1,2,3},{7,1,3,2},1};
    WitnessDepthCopyCensus census;
    census.record(&base,false);
    check(!census.captureDescriptor() && !census.sampled,"depth copy census starts off");
    census.request(true);
    census.request(false);
    census.record(&base,false);
    check(!census.reportReady() && !census.sampled,"disarming before any copy leaves no sample");
    census.request(true);
    census.record(&base,false);
    census.record(&base,false);
    const std::array<uint32_t WitnessDepthStencilTuple::*,7> scalars={
        &WitnessDepthStencilTuple::depthEnable,&WitnessDepthStencilTuple::depthWriteMask,
        &WitnessDepthStencilTuple::depthFunc,&WitnessDepthStencilTuple::stencilEnable,
        &WitnessDepthStencilTuple::stencilReadMask,&WitnessDepthStencilTuple::stencilWriteMask,
        &WitnessDepthStencilTuple::stencilRef};
    for(auto field:scalars){
        auto k=base;
        k.*field ^= (field==&WitnessDepthStencilTuple::stencilRef ? 256u : 1u);
        census.record(&k,false);
    }
    for(unsigned op=0;op<4;++op){
        auto k=base;k.front[op]^=1;census.record(&k,false);
        k=base;k.back[op]^=1;census.record(&k,false);
    }
    check(census.rowCount==16 && census.rows[0].copies==2,"every depth/stencil field, including high reference bits, distinguishes a tuple");
    census.record(nullptr,true);
    census.record(nullptr,false);
    for(unsigned i=0;i<20;++i){auto k=base;k.stencilRef+=512+i;census.record(&k,false);}
    census.record(&base,false); // A known tuple remains countable at table capacity.
    for(unsigned n=census.sampled;n<census.copyLimit;++n)census.record(&base,false);
    census.record(nullptr,false);
    unsigned listed=0;
    for(unsigned i=0;i<census.rowCount;++i)listed+=census.rows[i].copies;
    check(census.sampled==8192 && census.rowCount==32 && census.defaultNull==1 && census.unknown==1 && census.overflow==4 &&
          listed+census.defaultNull+census.unknown+census.overflow==census.sampled,"copy cap and table overflow reconcile without dropping a counted copy");
    check(census.reportReady() && census.complete() && !census.captureDescriptor(),"full census is ready once and metadata collection stops");
    census.markReported();
    census.request(false);
    census.request(true);
    check(!census.reportReady() && !census.captureDescriptor(),"a reported process cannot reopen its census");

    WitnessDepthCopyCensus partial;
    partial.request(true);
    partial.record(&base,false);
    partial.request(false);
    partial.request(true); // The owner has yet to see the Off request.
    partial.record(&base,false);
    check(partial.sampled==1 && partial.reportReady() && !partial.complete(),"Off seals a partial window even if rearmed before its report");
}

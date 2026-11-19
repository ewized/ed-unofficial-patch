#pragma once
#include "../../src/d3d11/native_vrs_witness_sampling.h"

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
}

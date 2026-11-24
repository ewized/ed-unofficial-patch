#pragma once
#include "native_vrs_material.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdio>

namespace edvr::native_vrs::ps_measure {
using Sink=void(*)(const char*);
struct Key {
    uint64_t vs=0,ps=0,state=0;
    uint32_t count=0,instances=0,start=0,startInstance=0,width=0,height=0;
    int32_t base=0;char kind='?';
    bool operator==(const Key& b)const noexcept {
        return vs==b.vs&&ps==b.ps&&state==b.state&&count==b.count&&instances==b.instances&&
            start==b.start&&startInstance==b.startInstance&&width==b.width&&height==b.height&&base==b.base&&kind==b.kind;
    }
};
inline constexpr uint64_t kUsefulFullPs=4096;
inline constexpr uint64_t kMatchFrames=12;
// Rotate discovery beyond the first draw prefix. Counting is CPU-only, even
// after this eye has issued its single query; metadata reads remain capped.
class Discovery {
    struct Eye {uint64_t frame=0;unsigned calls=0,base=0,next=0,attempts=0;};
    std::array<Eye,2> eyes_{};
public:
    bool admit(unsigned eye,uint64_t frame,bool priority,bool canSample)noexcept {
        if(eye>=2||!frame)return false;auto& e=eyes_[eye];
        if(e.frame!=frame){e.base=e.calls?e.next%e.calls:0;e.frame=frame;e.calls=e.attempts=0;}
        const unsigned ordinal=e.calls++;
        if(!canSample||e.attempts>=16||(!priority&&(ordinal<e.base||ordinal-e.base>=16)))return false;
        // Continue after the last metadata read, not a fixed frame stride:
        // an issued query stops this frame after its first successful read.
        // Frames blocked on pending queries leave the cursor untouched.
        ++e.attempts;e.next=ordinal+1;return true;
    }
};
struct Receipt {
    uint64_t frame=0,sequence=0,config=0,gazeEpoch=0,invalidation=0,map=0;
    unsigned eye=2;bool qualified=false;
};
inline bool sameEpoch(const Receipt& a,const Receipt& b)noexcept {
    return a.qualified&&b.qualified&&a.eye==b.eye&&a.config==b.config&&a.gazeEpoch==b.gazeEpoch&&
        a.invalidation==b.invalidation&&a.map==b.map;
}
inline bool sameSeries(const Receipt& a,const Receipt& b)noexcept {
    // Each query separately proves no mutation within its own raw draw. The
    // normal context mutation serial can advance between revalidated frames;
    // config/provider/map identity must still match across the three samples.
    return a.qualified&&b.qualified&&a.eye==b.eye&&a.config==b.config&&a.gazeEpoch==b.gazeEpoch&&a.map==b.map;
}
inline bool sameWork(const D3D11_QUERY_DATA_PIPELINE_STATISTICS& a,const D3D11_QUERY_DATA_PIPELINE_STATISTICS& b)noexcept {
    return a.IAVertices==b.IAVertices&&a.IAPrimitives==b.IAPrimitives&&a.VSInvocations==b.VSInvocations&&
        a.GSInvocations==b.GSInvocations&&a.GSPrimitives==b.GSPrimitives&&a.CInvocations==b.CInvocations&&a.CPrimitives==b.CPrimitives;
}
inline bool stableFull(uint64_t a,uint64_t b)noexcept {
    return a&&b&&(a>b?a-b:b-a)<=((a<b?a:b)/100u); // full/coarse/full, at most 1% full-rate drift
}
struct Plan {unsigned candidate=16,slot=6,phase=0;uint64_t id=0;bool full=true,active=false;};
class Recorder {
    struct Sample {Receipt receipt{};D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};bool valid=false;};
    struct Candidate {Key key{};uint64_t identity=0,lastSeen=0;unsigned phase=0;bool used=false,pending=false,rejected=false,useful=false;std::array<Sample,3> samples{};};
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Query> query;
        Plan plan{};Receipt before{},after{};uint64_t startedMs=0;
        bool active=false,ended=false,valid=false,timedOut=false;
    };
    std::array<Candidate,16> candidates_{};
    std::array<Slot,6> slots_{};
    std::array<uint64_t,2> issuedFrame_{};
    std::array<unsigned,2> triples_{};
    uint64_t firstFrame_=0,pollFrame_=UINT64_MAX,nextId_=0,nextCandidate_=0;
    Discovery discovery_;
    bool requested_=false,finished_=false;
    Sink sink_=nullptr;
    void line(const char* text)const noexcept {try {if(sink_)sink_(text);}catch(...) {}}
    void finish(Slot& s,HRESULT result,const D3D11_QUERY_DATA_PIPELINE_STATISTICS& stats) {
        auto& c=candidates_[s.plan.candidate];c.pending=false;
        auto& sample=c.samples[s.plan.phase];sample={s.before,stats,result==S_OK&&requested_&&s.valid&&sameEpoch(s.before,s.after)};
        const uint64_t expected=uint64_t(c.key.count)*c.key.instances;
        sample.valid=sample.valid&&stats.IAVertices==expected&&stats.IAPrimitives==expected/3;
        char row[2048]{};
        std::snprintf(row,sizeof(row),"native vrs ps sample: schema=1 id=%llu candidate=%u candidate_instance=%llu phase=%u control=%s eye=%u frame=%llu sequence=%llu config=%llu gaze_epoch=%llu invalidation=%llu map=%llx valid=%u query_hr=0x%08x vs=%016llx ps=%016llx state=%016llx kind=%c count=%u instances=%u start=%u base=%d start_instance=%u size=%ux%u useful_full_floor=%llu PSInvocations=%llu IAVertices=%llu IAPrimitives=%llu VSInvocations=%llu GSInvocations=%llu GSPrimitives=%llu CInvocations=%llu CPrimitives=%llu scope=forwarded_scene_material hud_excluded=1 diagnostic_only=1 outside_old_shader_list=%u",
            (unsigned long long)s.plan.id,s.plan.candidate,(unsigned long long)c.identity,s.plan.phase,s.plan.full?"full":"native_map",s.before.eye,
            (unsigned long long)s.before.frame,(unsigned long long)s.before.sequence,(unsigned long long)s.before.config,
            (unsigned long long)s.before.gazeEpoch,(unsigned long long)s.before.invalidation,(unsigned long long)s.before.map,
            unsigned(sample.valid),unsigned(result),(unsigned long long)c.key.vs,(unsigned long long)c.key.ps,(unsigned long long)c.key.state,c.key.kind,c.key.count,c.key.instances,c.key.start,c.key.base,c.key.startInstance,c.key.width,c.key.height,(unsigned long long)kUsefulFullPs,
            (unsigned long long)stats.PSInvocations,(unsigned long long)stats.IAVertices,(unsigned long long)stats.IAPrimitives,
            (unsigned long long)stats.VSInvocations,(unsigned long long)stats.GSInvocations,(unsigned long long)stats.GSPrimitives,(unsigned long long)stats.CInvocations,(unsigned long long)stats.CPrimitives,
            unsigned(!knownWorldFamily(c.key.vs,c.key.ps)));
        line(row);
        if(!sample.valid||!stats.PSInvocations||(s.plan.phase==0&&stats.PSInvocations<kUsefulFullPs)){c.rejected=true;}
        else if(s.plan.phase==0){c.useful=true;++c.phase;}
        else if(s.plan.phase<2){++c.phase;}
        else {
            const auto& a=c.samples[0];const auto& b=c.samples[1];const auto& z=c.samples[2];
            const bool matched=a.valid&&b.valid&&z.valid&&sameSeries(a.receipt,b.receipt)&&sameSeries(a.receipt,z.receipt)&&
                sameWork(a.stats,b.stats)&&sameWork(a.stats,z.stats)&&stableFull(a.stats.PSInvocations,z.stats.PSInvocations)&&
                a.receipt.frame<b.receipt.frame&&b.receipt.frame<z.receipt.frame&&z.receipt.frame-a.receipt.frame<=kMatchFrames&&
                a.receipt.sequence<b.receipt.sequence&&b.receipt.sequence<z.receipt.sequence;
            if(matched)++triples_[s.before.eye];
            std::snprintf(row,sizeof(row),"native vrs ps triple: schema=1 candidate=%u candidate_instance=%llu eye=%u valid=%u full_before=%llu native_map=%llu full_after=%llu full_frame=%llu coarse_frame=%llu after_frame=%llu matched_triples=%u scope=sampled_world_only performance_claim=0",
                s.plan.candidate,(unsigned long long)c.identity,s.before.eye,unsigned(matched),(unsigned long long)a.stats.PSInvocations,(unsigned long long)b.stats.PSInvocations,(unsigned long long)z.stats.PSInvocations,
                (unsigned long long)a.receipt.frame,(unsigned long long)b.receipt.frame,(unsigned long long)z.receipt.frame,triples_[s.before.eye]);
            line(row);c.phase=0;
        }
        s.active=s.ended=false;
    }
public:
    void configure(bool requested,Sink sink)noexcept {requested_=requested;sink_=sink;}
    bool pending()const noexcept {for(const auto& s:slots_)if(s.active)return true;return false;}
    bool finished()const noexcept {return finished_;}
    bool requested()const noexcept {return requested_;}
    void collect(ID3D11DeviceContext* context,uint64_t frame,uint64_t now) {
        if(frame==pollFrame_)return;pollFrame_=frame;
        for(auto& s:slots_)if(s.active&&s.ended){
            D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
            const HRESULT hr=context->GetData(s.query.Get(),&stats,sizeof(stats),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(hr==S_FALSE){
                if(now-s.startedMs>=2000&&!s.timedOut){s.timedOut=true;s.valid=false;line("native vrs ps pending: timeout_ms=2000 pending_retained=1 evidence_valid=0");}
                continue; // Never reuse an unresolved GPU query or force completion.
            }
            if(FAILED(hr))finished_=true;
            finish(s,hr,stats);
        }
        if(!finished_&&firstFrame_&&((triples_[0]>=5&&triples_[1]>=5)||frame-firstFrame_>=600)) {
            finished_=true;char row[256]{};
            std::snprintf(row,sizeof(row),"native vrs ps finished: schema=1 matched_left=%u matched_right=%u frame_budget=600 pending=%u measurement_not_performance=1",triples_[0],triples_[1],unsigned(pending()));line(row);
        }
    }
    bool metadataWanted(const Key& cheap,const Receipt& receipt)noexcept {
        bool priority=false,tracking=false;
        if(receipt.eye<2)for(unsigned i=receipt.eye*8;i<(receipt.eye+1)*8;++i){
            const auto& c=candidates_[i];const auto& k=c.key;
            const bool live=c.used&&c.useful&&!c.rejected&&receipt.frame>=c.lastSeen&&receipt.frame-c.lastSeen<=kMatchFrames;
            tracking=tracking||live;
            priority=priority||(live&&!c.pending&&
                k.vs==cheap.vs&&k.ps==cheap.ps&&k.kind==cheap.kind&&k.count==cheap.count&&k.instances==cheap.instances&&
                k.start==cheap.start&&k.base==cheap.base&&k.startInstance==cheap.startInstance);
        }
        bool canSample=requested_&&!finished_&&receipt.qualified&&receipt.eye<2&&
            issuedFrame_[receipt.eye]!=receipt.frame&&(priority||!tracking);
        if(canSample){
            // Do not rotate through draw identities while no query can own
            // their result; completion cadence must not alias discovery.
            canSample=false;
            for(const auto& slot:slots_)if(!slot.active){canSample=true;break;}
        }
        return discovery_.admit(receipt.eye,receipt.frame,priority,canSample);
    }
    Plan plan(const Key& key,const Receipt& receipt) {
        Plan p{};
        if(!requested_||finished_||!receipt.qualified||receipt.eye>=2||!receipt.frame||!receipt.sequence||!receipt.config||!receipt.gazeEpoch||!receipt.map||
            !key.vs||!key.ps||!key.count||!key.instances||key.count%3||issuedFrame_[receipt.eye]==receipt.frame)return p;
        if(!firstFrame_)firstFrame_=receipt.frame;
        unsigned found=16,free=16;
        for(unsigned i=receipt.eye*8;i<(receipt.eye+1)*8;++i){
            auto& c=candidates_[i];
            const bool expired=c.used&&!c.pending&&receipt.frame>c.lastSeen&&receipt.frame-c.lastSeen>kMatchFrames;
            if(!c.used||expired){if(free==16)free=i;continue;}
            if(c.key==key){found=i;break;}
            if(c.rejected&&!c.pending&&free==16)free=i;
        }
        if(found==16){if(free==16)return p;found=free;auto& c=candidates_[found];
            const char* reason=!c.used?"unused":c.rejected?"screen_or_invalid":"expired";
            c=Candidate{};c.used=true;c.key=key;c.identity=++nextCandidate_;
            char row[256]{};std::snprintf(row,sizeof(row),"native vrs ps candidate: schema=1 candidate=%u candidate_instance=%llu eye=%u frame=%llu reason=%s useful_full_floor=%llu",found,(unsigned long long)c.identity,receipt.eye,(unsigned long long)receipt.frame,reason,(unsigned long long)kUsefulFullPs);line(row);}
        auto& c=candidates_[found];if(c.pending||c.rejected)return p;
        if(c.phase&&(!sameSeries(c.samples[0].receipt,receipt)||receipt.frame-c.samples[0].receipt.frame>kMatchFrames)){
            c.phase=0;c.useful=false;c.samples={};c.identity=++nextCandidate_;
        }
        c.lastSeen=receipt.frame;
        for(unsigned i=0;i<slots_.size();++i)if(!slots_[i].active){p={found,i,c.phase,++nextId_,c.phase!=1,true};return p;}
        return p;
    }
    bool begin(ID3D11Device* device,ID3D11DeviceContext* context,const Plan& p,const Receipt& receipt,uint64_t now) {
        if(!requested_||finished_||!receipt.qualified||receipt.eye>=2||!device||!context||
            !p.active||p.slot>=slots_.size()||p.candidate>=candidates_.size()||p.candidate/8!=receipt.eye)return false;
        auto& s=slots_[p.slot];if(s.active)return false;
        if(!s.query){D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS,0};if(FAILED(device->CreateQuery(&desc,&s.query)))return false;}
        s.plan=p;s.before=receipt;s.after={};s.startedMs=now;s.valid=false;s.active=true;s.ended=false;s.timedOut=false;
        candidates_[p.candidate].pending=true;issuedFrame_[receipt.eye]=receipt.frame;context->Begin(s.query.Get());return true;
    }
    void end(ID3D11DeviceContext* context,const Plan& p,const Receipt& after,bool restored) {
        if(!p.active||p.slot>=slots_.size())return;auto& s=slots_[p.slot];
        if(!s.active||s.ended||s.plan.id!=p.id)return;
        context->End(s.query.Get());s.after=after;s.valid=restored&&requested_;s.ended=true;
    }
    void invalidate(const Plan& p)noexcept {if(p.slot<slots_.size()&&slots_[p.slot].plan.id==p.id)slots_[p.slot].valid=false;}
};
} // namespace edvr::native_vrs::ps_measure

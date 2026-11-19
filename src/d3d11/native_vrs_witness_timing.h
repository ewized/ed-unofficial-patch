#pragma once
#include <iterator>
#include "engine_velocity.h"
#include "gpu_timing.h"
#include "native_vrs_witness.h"
#include "native_vrs_witness_sampling.h"
#include "../common/log.h"
#include "../common/timing.h"

// Private diagnostic. Entry, poll and teardown are serialized by the engine-motion
// lock. Only the configured immediate-context owner issues query commands.
namespace edvr::native_vrs {
struct WitnessQueryScope {
    bool previous=engineWitnessTimingQueryInternal;
    WitnessQueryScope(){engineWitnessTimingQueryInternal=true;}
    ~WitnessQueryScope(){engineWitnessTimingQueryInternal=previous;}
};

struct WitnessTiming {
    static constexpr unsigned phases=unsigned(WitnessPhase::Count);
    static constexpr unsigned lag=4, slots=(lag+2)*WitnessSamplePlan::perFrame;
    static constexpr uint64_t durationMs=30000,drainMs=2000;
    struct Totals {
        uint64_t events=0,draws=0,validated=0,paired=0,beyond=0,chosen=0,noSlot=0;
        uint64_t readOnly=0,writable=0,readOnlyWeight=0,writableWeight=0,readOnlyDrawWeight=0;
        struct Time {
            uint64_t cpuSamples=0,gpuSamples=0,missed=0,gpuWeight=0;
            double weightedCpuMs=0,weightedGpuMs=0,weightedReadOnlyGpuMs=0;
        } time[phases];
    };
    struct Sample {
        GpuTimer gpu[phases];
        bool pending[phases]{},busy=false,readOnly=false;
        unsigned row=0;
        uint64_t weight=0,frame=0;
    };
    struct Depth {
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
        unsigned width=0,height=0;
        uint64_t copies=0,bytes=0;
    };
    enum class Mode { Idle, Pilot, Collect, Drain, Done };
    Mode mode=Mode::Idle;
    std::atomic<bool> requested{false};
    ID3D11DeviceContext* owner=nullptr;
    DWORD thread=0;
    WitnessSamplePlan plan{};
    Totals rows[WitnessSamplePlan::strata]{};
    Sample samples[slots];
    Depth depths[8]{};
    uint64_t frame=0,boundary=0,frames=0,first=0,last=0,started=0,draining=0,unlistedCopies=0,frequency=0;
    uint64_t captures=0,captureSamples=0,prearms=0,prearmSamples=0,captureTicks=0,prearmTicks=0;
    uint32_t cpuRandom=321891u;
    unsigned cpuSamplesThisFrame=0;
    uint64_t cappedAdmissions=0;
    const char* reason="";
    bool active(ID3D11DeviceContext* ctx) const noexcept {
        return (mode==Mode::Pilot || mode==Mode::Collect) && owner==ctx && thread==GetCurrentThreadId();
    }
    static uint64_t ticks() noexcept {
        LARGE_INTEGER n{};QueryPerformanceCounter(&n);return uint64_t(n.QuadPart);
    }
    bool admissionSample() noexcept {
        if(mode!=Mode::Collect)return false;
        ++captures;
        // Independent of cohort rank selection; periodic draw order must not
        // select the same material on every frame.
        cpuRandom ^= cpuRandom<<13;cpuRandom ^= cpuRandom>>17;cpuRandom ^= cpuRandom<<5;
        if(cpuRandom&63)return false;
        if(cpuSamplesThisFrame==32){++cappedAdmissions;return false;}
        ++cpuSamplesThisFrame;return true;
    }
    void recordCopy(const WitnessMirror& m) noexcept {
        for(auto& d:depths)if(!d.copies || (d.format==m.format && d.width==m.width && d.height==m.height)){
            d.format=m.format;d.width=m.width;d.height=m.height;++d.copies;
            unsigned bpp=0;
            switch(m.format){
            case DXGI_FORMAT_D16_UNORM:bpp=2;break;
            case DXGI_FORMAT_D24_UNORM_S8_UINT:case DXGI_FORMAT_D32_FLOAT:bpp=4;break;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:bpp=8;break;
            default:break;
            }
            d.bytes+=uint64_t(bpp)*d.width*d.height;
            return;
        }
        ++unlistedCopies;
    }
    void clearTimers(ID3D11DeviceContext* ctx) noexcept {
        WitnessQueryScope guard;
        for(auto& s:samples){
            for(unsigned j=0;j<phases;++j){
                if(s.pending[j])++rows[s.row].time[j].missed;
                s.gpu[j].reset(ctx);s.pending[j]=false;
            }
            s.busy=false;
        }
    }
    bool poll(ID3D11DeviceContext* ctx) noexcept {
        WitnessQueryScope guard;
        bool outstanding=false;
        for(auto& s:samples)if(s.busy){
            if(boundary-s.frame<lag){outstanding=true;continue;}
            bool pending=false;
            for(unsigned j=0;j<phases;++j)if(s.pending[j]){
                double ms=0;
                const auto result=s.gpu[j].poll(ctx,ms);
                if(result==GpuTimerPoll::Pending){pending=true;continue;}
                s.pending[j]=false;
                auto& t=rows[s.row].time[j];
                if(result==GpuTimerPoll::Ready){
                    ++t.gpuSamples;t.gpuWeight+=s.weight;t.weightedGpuMs+=ms*s.weight;
                    if(s.readOnly)t.weightedReadOnlyGpuMs+=ms*s.weight;
                }else ++t.missed;
            }
            s.busy=pending;outstanding|=pending;
        }
        return outstanding;
    }
    void report() const noexcept {
        auto& log=Log::get();
        // GPU timestamps span replay, including material shading. First-admission
        // prearm has a separate CPU sample and falls outside these GPU spans.
        log.noteReserved("VRS witness timing: gpu_scope=replay_only admission_prearm=cpu_only reason=%s frames=%llu first=%llu last=%llu cap_per_frame=%u "
            "capture_calls=%llu capture_samples=%llu capture_sample_ms=%.6f prearm_calls=%llu prearm_samples=%llu prearm_sample_ms=%.6f admission_cpu_capped=%llu unlisted_copies=%llu",
            reason,frames,first,last,WitnessSamplePlan::perFrame,captures,captureSamples,frequency?1000.*captureTicks/frequency:0.,
            prearms,prearmSamples,frequency?1000.*prearmTicks/frequency:0.,cappedAdmissions,unlistedCopies);
        constexpr const char* names[]={"sync_guard","sync_other","cohort_1","cohort_2_8","cohort_9_32","cohort_33_64"};
        constexpr const char* stages[]={"prepare","copy","fine","coarse_bind","material_restore"};
        static_assert(std::size(names)==unsigned(WitnessRoute::Count) && std::size(stages)==phases);
        for(unsigned i=0;i<WitnessSamplePlan::strata;++i){
            const auto& r=rows[i];if(!r.events)continue;
            log.noteReserved("VRS witness stratum: route=%s copy_expected=%u events=%llu draws=%llu validated=%llu paired=%llu "
                "rank_overflow=%llu selected=%llu ring_busy=%llu sampled_read_only=%llu sampled_writable=%llu read_only_weight=%llu writable_weight=%llu read_only_draw_weight=%llu",
                names[i/2],i%2,r.events,r.draws,r.validated,r.paired,r.beyond,r.chosen,r.noSlot,r.readOnly,r.writable,
                r.readOnlyWeight,r.writableWeight,r.readOnlyDrawWeight);
            for(unsigned j=0;j<phases;++j){
                const auto& t=r.time[j];if(!t.cpuSamples && !t.missed)continue;
                log.noteReserved("VRS witness interval: route=%s copy_expected=%u phase=%s cpu_samples=%llu gpu_valid=%llu gpu_missing=%llu gpu_weight=%llu "
                    "weighted_cpu_ms=%.6f weighted_gpu_ms=%.6f weighted_read_only_gpu_ms=%.6f",
                    names[i/2],i%2,stages[j],t.cpuSamples,t.gpuSamples,t.missed,t.gpuWeight,t.weightedCpuMs,t.weightedGpuMs,t.weightedReadOnlyGpuMs);
            }
        }
        for(const auto& d:depths)if(d.copies)
            log.noteReserved("VRS witness depth copy: width=%u height=%u dsv_format=%u copies=%llu logical_bytes=%llu",
                d.width,d.height,unsigned(d.format),d.copies,d.bytes);
    }
    void tick(ID3D11DeviceContext* ctx,uint64_t sequence,bool vrs) noexcept {
        if(!ctx || !sequence)return;
        const bool on=requested.load(std::memory_order_relaxed);
        if(mode==Mode::Done){if(!on)mode=Mode::Idle;return;}
        if(mode==Mode::Idle){
            if(!on || !vrs)return;
            if(!gpuTimingAccepts(ctx))return;
            WitnessPtr<ID3D11Device> dev;ctx->GetDevice(&dev);
            LARGE_INTEGER hz{};QueryPerformanceFrequency(&hz);frequency=uint64_t(hz.QuadPart);
            plan={};for(auto& r:rows)r={};for(auto& d:depths)d={};
            first=last=frames=unlistedCopies=captures=captureSamples=prearms=prearmSamples=captureTicks=prearmTicks=cappedAdmissions=0;
            started=stampMs();owner=ctx;thread=GetCurrentThreadId();frame=sequence;boundary=0;
            WitnessQueryScope guard;
            for(auto& s:samples)for(auto& t:s.gpu)if(!t.prepareBorrowedFrame(dev.Get(),ctx)){
                reason="preallocation_failed";clearTimers(ctx);report();mode=Mode::Done;owner=nullptr;return;
            }
            mode=Mode::Pilot;
            Log::get().noteReserved("VRS witness timing armed: pilot then 30 s, %u preallocated timestamp pairs, %u spans/frame, application-frame disjoint only",slots*phases,WitnessSamplePlan::perFrame);
            return;
        }
        if(owner!=ctx || thread!=GetCurrentThreadId() || frame==sequence)return;
        if(mode==Mode::Collect){++frames;last=frame;}
        // Monotonic local frame age handles host frame sequence changes.
        frame=sequence;++boundary;cpuSamplesThisFrame=0;
        if(mode==Mode::Pilot || mode==Mode::Collect){
            if(!on || !vrs || elapsedMs(started,durationMs)){
                reason=!on?"config_off":!vrs?"vrs_off":mode==Mode::Pilot?"no_replays":"complete";
                mode=Mode::Drain;draining=stampMs();
            }else if(mode==Mode::Collect){plan.advance();}
            else{
                bool seen=false;for(auto& r:plan.rows)seen|=r.seen!=0;
                if(seen){plan.advance();mode=Mode::Collect;started=stampMs();first=sequence;}
            }
        }
        const bool outstanding=poll(ctx);
        if(mode==Mode::Drain && (!outstanding || elapsedMs(draining,drainMs))){
            clearTimers(ctx);report();mode=on?Mode::Done:Mode::Idle;owner=nullptr;
        }
    }
};

struct WitnessObservation final : WitnessObserver {
    WitnessTiming& t;
    const unsigned length,row;
    WitnessTiming::Sample* sample=nullptr;
    ID3D11DepthStencilState* captured;
    bool cohort;
    uint64_t startedCpu=0;
    unsigned phase=WitnessTiming::phases;
    WitnessObservation(WitnessTiming& timing,WitnessRoute route,unsigned count,bool needsCopy,ID3D11DepthStencilState* ds=nullptr)
        :t(timing),length(count),row(2*unsigned(route)+unsigned(needsCopy)),captured(ds),
         cohort(route!=WitnessRoute::SyncGuard && route!=WitnessRoute::SyncOther){
        const auto pick=t.plan.event(row);
        if(t.mode!=WitnessTiming::Mode::Collect)return;
        auto& r=t.rows[row];++r.events;r.draws+=length;r.beyond+=pick.outside;
        if(!pick.weight)return;
        ++r.chosen;
        for(auto& s:t.samples)if(!s.busy){sample=&s;break;}
        if(!sample){++r.noSlot;return;}
        sample->busy=true;sample->readOnly=false;sample->frame=t.boundary;sample->row=row;sample->weight=pick.weight;
        stage(WitnessPhase::Prepare);
    }
    void endStage() noexcept {
        if(!sample || phase==WitnessTiming::phases)return;
        auto& a=t.rows[row].time[phase];
        ++a.cpuSamples;a.weightedCpuMs+=1000.*double(WitnessTiming::ticks()-startedCpu)/double(t.frequency)*sample->weight;
        if(sample->pending[phase]){
            WitnessQueryScope guard;sample->gpu[phase].end(t.owner);
        }
        phase=WitnessTiming::phases;
    }
    void stage(WitnessPhase p) noexcept override {
        if(!sample)return;
        endStage();phase=unsigned(p);
        {
            WitnessQueryScope guard;
            sample->pending[phase]=sample->gpu[phase].beginPreparedBorrowedFrame(t.owner);
        }
        if(!sample->pending[phase])++t.rows[row].time[phase].missed;
        startedCpu=WitnessTiming::ticks();
    }
    void validated(const WitnessMirror& m,bool copied) noexcept override {
        if(t.mode!=WitnessTiming::Mode::Collect)return;
        auto& r=t.rows[row];++r.validated;
        if(copied)t.recordCopy(m);
        if(!sample)return;
        WitnessPtr<ID3D11DepthStencilState> current;
        if(!cohort)t.owner->OMGetDepthStencilState(&current,nullptr);
        auto* state=cohort?captured:current.Get();
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable=TRUE;d.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        if(state)state->GetDesc(&d);
        auto writes=[](const D3D11_DEPTH_STENCILOP_DESC& op){
            return op.StencilFailOp!=D3D11_STENCIL_OP_KEEP || op.StencilDepthFailOp!=D3D11_STENCIL_OP_KEEP || op.StencilPassOp!=D3D11_STENCIL_OP_KEEP;
        };
        // The production mirror accepts only single-sample DSVs with flags==0.
        const bool stencil=m.format==DXGI_FORMAT_D24_UNORM_S8_UINT || m.format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        sample->readOnly=witnessDepthOnlyReads(d.DepthEnable!=FALSE,d.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO,false,
            d.StencilEnable!=FALSE,d.StencilWriteMask!=0,writes(d.FrontFace)||writes(d.BackFace),false,stencil);
        if(sample->readOnly){++r.readOnly;r.readOnlyWeight+=sample->weight;r.readOnlyDrawWeight+=length*sample->weight;}
        else{++r.writable;r.writableWeight+=sample->weight;}
    }
    void finish(uint64_t paired) noexcept {
        endStage();
        if(t.mode==WitnessTiming::Mode::Collect)t.rows[row].paired+=paired;
    }
};
} // namespace edvr::native_vrs

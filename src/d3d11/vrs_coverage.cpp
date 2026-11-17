#include "vrs_coverage.h"
#include "binding_shadow.h"
#include "vrs_callback_dependency.h"
#include "vrs_work_reference_policy.h"
#include "../common/code_hook.h"
#include "../common/guard.h"
#include "../common/log.h"
#include "../common/pvr_gaze_snapshot.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <utility>

namespace edvr {
std::atomic<bool> g_vrsCoverageEnabled{false};
std::atomic<bool> g_vrsWorkReferenceRequested{false};
namespace {
using Microsoft::WRL::ComPtr;
using vrs_coverage::AcceptedState;
// Minimal public NVIDIA NVAPI V1 declarations. Source: NVIDIA/nvapi,
// nvapi.h15735..15890 and nvapi_interface.h350..352 (MIT).
struct ViewportRate { uint8_t enabled,padding[3];uint32_t palette[16]; };
struct RateDesc {uint32_t version,count;ViewportRate* rates;};
struct SrrvDesc {uint32_t version;DXGI_FORMAT format;uint32_t dimension;uint32_t mip,slice,count;};
struct __declspec(uuid("E14BE7F6-8FF5-4F5E-B63A-AD016EB8FBE5")) NvRateView:ID3D11View {
    virtual HRESULT STDMETHODCALLTYPE GetDesc(SrrvDesc*)=0;
};
static_assert(sizeof(RateDesc)==16&&sizeof(ViewportRate)==68&&sizeof(SrrvDesc)==24,"NVAPI V1 Win64 ABI");
using Rates=int(__cdecl*)(IUnknown*,RateDesc*);
using Resource=int(__cdecl*)(IUnknown*,IUnknown*);
using Query=void*(__cdecl*)(uint32_t);
struct Counts {
    uint64_t issued=0,substituted=0,enabled=0,disabled=0,unknown=0,ambiguous=0;
    uint64_t copies=0,ready=0,invalid=0,nonuniform=0,coarse=0,ringBusy=0;
    uint64_t gazeAvailable=0,gazeFresh=0,gazeStale=0,gazeAmbiguous=0,pairedChange=0;
    vrs_coverage::DirectionCounts direction;
    uint64_t cbEligible=0,cbIneligible=0,cbPairs=0,cbPairRefused=0,cbGazeChanges=0,cbMapChanges=0,cbPairedChanges=0,cbInsufficientExcitation=0;
    vrs_coverage::DirectionCounts callbackDirection;
};
struct Sample {
    uint64_t copyId=0,drawId=0,qpcUs=0,beforeEpoch=0,afterEpoch=0,rateEpoch=0,bindingEpoch=0;
    unsigned eye=2,rtW=0,rtH=0,mapW=0,mapH=0,tilesW=0,tilesH=0,mip=0,slice=0;
    bool issued=false,ambiguous=false,gazeAvailable=false,gazeFresh=false,gazeChanged=false,mapChanged=false,pairedChange=false,bindingsVerified=false,valid=false;
    EdvrPvrGazeSnapshotV1 gaze{};
    uint64_t gazeAgeUs=0;
    std::array<uint32_t,16> palette{};
    vrs_coverage::Histogram histogram{};
    uint64_t contentHash=0;
    vrs_callback_dependency::Sample callback{};
    ComPtr<IUnknown> callbackWorld,callbackMap; // Bound lifetime prevents identity reuse in paired observations.
};
struct Slot {ComPtr<ID3D11Texture2D> staging;Sample sample{};bool pending=false,drawFinished=false;};
struct PipelineSample {
    uint64_t drawId=0,beforeEpoch=0,afterEpoch=0,rateEpoch=0,bindingEpoch=0,generation=0;
    VrsCoverageSignature signature{};
    unsigned eye=2,rtW=0,rtH=0,samples=0,quality=0,forcedSamples=0,conservative=0,topology=0;
    uint32_t sampleMask=0,blendEnabledMask=0,writeMasks=0;
    unsigned fillMode=0,cullMode=0,scissorCount=0;
    bool frontCounterClockwise=false,depthClip=false,scissorEnabled=false;
    D3D11_RECT scissor0{};
    uint64_t scissorHash=0;
    std::array<float,4> blendFactor{};
    bool enabled=false,stateKnown=false,issued=false,ambiguous=false,substituted=false,bindingsVerified=false;
    bool rasterKnown=false,forcedKnown=false,conservativeKnown=false,blendKnown=false,alphaToCoverage=false,actualVsOriginal=false,actualPsOriginal=false,queryReady=false,timedOut=false,cancelled=false;
    HRESULT status=S_FALSE;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
    bool nativePrimary=false;
    struct Work {
        bool present=false,modeOne=false,stateProven=false,restoreVerified=false;
        uint64_t cycle=0,modeAgeUs=0,expectedEpoch=0,initialBindingEpoch=0;
        DWORD thread=0;
        vrs_work_reference::Override override;
        vrs_work_reference::Rate executed{},restored{};
        int applyStatus=0,restoreStatus=0;
        unsigned bindingCalls=0,unexpected=0;
    } work;
};
struct PipelineSlot {
    ComPtr<ID3D11Query> query;
    PipelineSample sample{};
    vrs_coverage::PipelineQueryProgress progress{};
    bool active=false;
    uint64_t startedMs=0;
};
struct Diagnostic {
    // Deliberately process-lifetime: no hot unhook or trampoline destruction.
    std::mutex mutex;
    CodeHook ratesHook,resourceHook;
    AcceptedState state;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IUnknown> identity,boundView;
    HMODULE nvapi=nullptr,nvapiOwner=nullptr,shim=nullptr;
    EdvrPvrSetGazeSnapshotEnabled setGaze=nullptr;
    EdvrPvrGetGazeSnapshot getGaze=nullptr;
    bool attempted=false,deviceChanged=false,gazeEnabled=false,needDisable=false,offNoted=false,lastOffAck=false;
    uint64_t nextDiscovery=0,nextLog=0,nextCopy[2]{},nextDraw=0,nextCopyId=0;
    uint64_t ratesOk=0,ratesError=0,bindingOk=0,bindingError=0,invalidations=0,metadataError=0;
    Counts counts[2];std::array<Slot,3> ring;unsigned drawDepth=0;uint64_t nestedDraws=0;DWORD observerThread=0;
    vrs_coverage::GateCounts gates;
    std::array<Sample,32> completed{};unsigned completedCount=0;uint64_t outputOverflow=0;
    Sample previous[2]{};bool havePrevious[2]{};
    std::array<PipelineSlot,3> pipelineRing;
    uint64_t nextPipeline[2]{},pipelineStarted[2]{},pipelineReady[2]{},pipelineErrors[2]{},pipelineRejected[2]{},pipelineBusy[2]{};
    std::array<PipelineSample,32> pipelineCompleted{};unsigned pipelineCompletedCount=0;
    uint64_t pipelineOverflow=0;
    uint64_t pipelineGeneration=1;
    VrsCoverageDraw referenceWorld{};
    vrs_work_reference::Schedule referenceSchedule;
    PipelineSlot* activeReference=nullptr;
    bool referenceFaulted=false,referenceOffNoted=true,referenceLastAck=true,referenceRestoreInFlight=false;
    struct ReferenceCounts {uint64_t hits=0,sampled=0,ring=0,cadence=0,unknown=0,provider=0,restoreFailure=0,expired=0;} referenceCounts[2];
    uint64_t referenceAttemptNext[2]{},referenceUnknownEye=0;
};
thread_local bool referenceInternal=false;
Diagnostic& diag(){static Diagnostic* const value=new Diagnostic;return *value;}
std::atomic<Rates> originalRates{nullptr};
std::atomic<Resource> originalResource{nullptr};
std::atomic<bool> hooksReady{false};
std::atomic<bool> referenceScopeActive{false};
uint64_t qpcUs(){LARGE_INTEGER c{},f{};return QueryPerformanceCounter(&c)&&QueryPerformanceFrequency(&f)&&f.QuadPart>0?uint64_t(double(c.QuadPart)*1000000.0/double(f.QuadPart)):0;}
bool canonical(IUnknown* ctx) {
    auto& d=diag();if(ctx==d.context.Get()||ctx==d.identity.Get())return true;
    ComPtr<IUnknown> identity;bool same=false;
    guarded("vrsCoverage/contextIdentity",[&]{same=ctx&&SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&identity)))&&identity.Get()==d.identity.Get();});
    return same;
}
bool track(IUnknown* ctx){return (vrsCoverageEnabled()||referenceScopeActive.load(std::memory_order_acquire))&&hooksReady.load(std::memory_order_acquire)&&canonical(ctx);}
bool prepareRates(void* f,void*) noexcept {originalRates.store(reinterpret_cast<Rates>(f),std::memory_order_release);return true;}
bool prepareResource(void* f,void*) noexcept {originalResource.store(reinterpret_cast<Resource>(f),std::memory_order_release);return true;}
int __cdecl setRates(IUnknown* ctx,RateDesc* desc) {
    // Consume only our immediate call. A driver callback reentering this hook
    // is an original caller, even while this invocation has not returned.
    const bool ownRequest=vrs_work_reference::consumeInternal(referenceInternal);
    const auto forward=originalRates.load(std::memory_order_acquire);
    if(!track(ctx))return forward(ctx,desc);
    std::array<uint32_t,16> palette{};bool parsed=false,on=false;unsigned requestedCount=0;
    const bool read=guarded("vrsCoverage/rateDescriptor",[&]{
        if(!desc||desc->version!=0x10010)return;
        requestedCount=desc->count;
        if(!desc->count){parsed=true;return;}
        if(desc->count!=1||!desc->rates||desc->rates[0].enabled>1)return;
        on=desc->rates[0].enabled!=0;
        for(unsigned i=0;i<16;++i){palette[i]=desc->rates[0].palette[i];if(palette[i]>=12)return;}
        parsed=true;
    });
    auto& d=diag();uint64_t token=0;ViewportRate modifiedViewport{};RateDesc modifiedDesc{};RateDesc* forwarded=desc;
    vrs_work_reference::Rate requested{read&&parsed,on,requestedCount,palette},actual=requested;
    {std::lock_guard<std::mutex> lock(d.mutex);
        if(d.activeReference){auto& w=d.activeReference->sample.work;
            // This rejection applies only to our own apply/restore writes.
            // Original caller requests are always forwarded exactly once.
            if(ownRequest&&!w.override.internalWriteAllowed(d.state.writers)){++w.unexpected;return -1;}
            actual=w.override.request(requested,ownRequest,GetCurrentThreadId()==w.thread);
            if(actual.supported()&&actual.count==1&&actual.palette!=requested.palette){modifiedViewport.enabled=actual.enabled?1u:0u;
                for(unsigned i=0;i<16;++i)modifiedViewport.palette[i]=actual.palette[i];modifiedDesc={0x10010,1,&modifiedViewport};forwarded=&modifiedDesc;}}
        token=d.state.enter();}
    const int result=forward(ctx,forwarded); // EXACTLY ONCE; only an explicitly armed reference substitutes a local descriptor.
    {std::lock_guard<std::mutex> lock(d.mutex);if(result==0)++d.ratesOk;else ++d.ratesError;
     d.state.rates(token,result,actual.parsed,actual.enabled,actual.palette);
     if(d.activeReference){auto& w=d.activeReference->sample.work;w.override.result(actual,result,ownRequest?nullptr:&requested,GetCurrentThreadId()==w.thread);w.expectedEpoch=d.state.epoch;}}
    return result;
}
int __cdecl setResource(IUnknown* ctx,IUnknown* view) {
    const auto forward=originalResource.load(std::memory_order_acquire);
    if(!track(ctx))return forward(ctx,view);
    auto& d=diag();uint64_t token=0;{std::lock_guard<std::mutex> lock(d.mutex);token=d.state.enter();}
    const int result=forward(ctx,view); // EXACTLY ONCE; arguments/result unchanged.
    ComPtr<IUnknown> held;
    const bool retained=result!=0||!view||guarded("vrsCoverage/retainView",[&]{held=view;});
    ComPtr<IUnknown> old;
    {std::lock_guard<std::mutex> lock(d.mutex);if(result==0)++d.bindingOk;else ++d.bindingError;
     if(d.activeReference){++d.activeReference->sample.work.bindingCalls;++d.activeReference->sample.work.unexpected;}
     d.state.resource(token,result,reinterpret_cast<uintptr_t>(view));
     if(!retained){++d.metadataError;d.state.invalidate();}
     if(result==0&&retained){old=std::move(d.boundView);d.boundView=std::move(held);}}
    // Driver Release is outside our mutex; it may call unrelated driver code.
    return result;
}
bool moduleOwns(HMODULE module,void* address) {
    HMODULE owner=nullptr;return address&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&owner)&&owner==module;
}
bool modulePath(HMODULE module,wchar_t (&path)[MAX_PATH]) {
    const DWORD length=module?GetModuleFileNameW(module,path,MAX_PATH):0;
    return length&&length<MAX_PATH;
}
HMODULE addressOwner(void* address) {
    HMODULE owner=nullptr;
    return address&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&owner)?owner:nullptr;
}
void discover(Diagnostic& d,uint64_t now) {
    if(now<d.nextDiscovery)return;d.nextDiscovery=now+1000;
    if(!d.attempted){
        HMODULE module=GetModuleHandleW(L"nvapi64.dll");
        if(module){
            d.attempted=true;const auto query=reinterpret_cast<Query>(GetProcAddress(module,"nvapi_QueryInterface"));
            void* rates=query?query(0x34f7938f):nullptr;void* resource=query?query(0x1b0c2f83):nullptr;
            wchar_t systemPath[MAX_PATH]{},stubPath[MAX_PATH]{},ratesPath[MAX_PATH]{},resourcePath[MAX_PATH]{};
            const UINT systemLength=GetSystemDirectoryW(systemPath,MAX_PATH);
            const bool systemOk=systemLength&&systemLength<MAX_PATH;
            const bool stubRead=modulePath(module,stubPath);
            const HMODULE ratesOwner=addressOwner(rates),resourceOwner=addressOwner(resource);
            const bool ownerSame=ratesOwner&&ratesOwner==resourceOwner;
            const bool ownerRead=modulePath(ratesOwner,ratesPath)&&modulePath(resourceOwner,resourcePath);
            const bool stubOk=systemOk&&stubRead&&vrs_coverage::nvapiSystemStubPath(systemPath,stubPath);
            const unsigned ownerKind=stubOk&&ownerRead&&ownerSame?vrs_coverage::nvapiOwnerPathKind(systemPath,stubPath,ratesPath,resourcePath):0;
            HMODULE retained=nullptr,retainedOwner=nullptr,own=nullptr;
            const bool pinNvapi=ownerKind&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(module),&retained)&&retained==module;
            const bool pinOwner=pinNvapi&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(rates),&retainedOwner)&&retainedOwner==ratesOwner;
            const bool pinOwn=pinOwner&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(&vrsCoverageTick),&own);
            const bool pinned=pinNvapi&&pinOwner&&pinOwn;
            void* unused=nullptr;
            const bool a=pinned&&d.ratesHook.install(rates,reinterpret_cast<void*>(&setRates),&unused,"vrsCoverage/setRates",prepareRates,nullptr,true);
            const bool b=a&&d.resourceHook.install(resource,reinterpret_cast<void*>(&setResource),&unused,"vrsCoverage/setResource",prepareResource,nullptr,true);
            // Stages distinguish resolution/path/pinning from CodeHook's own
            // unchanged prologue/alignment/range refusal. No retry is attempted.
            const unsigned refusal=!query?1u:!rates?2u:!resource?3u:!systemOk||!stubRead?4u:!stubOk?5u:
                !ratesOwner||!resourceOwner||!ownerRead?6u:!ownerSame?7u:!ownerKind?8u:!pinNvapi?9u:!pinOwner?10u:!pinOwn?11u:!a?12u:!b?13u:0u;
            Log::get().note("vrs coverage discovery: schema=2 query_present=%u rates_present=%u resource_present=%u system_path_read=%u stub_path_read=%u stub_path_valid=%u owners_read=%u owners_same=%u owner_kind=%u pin_nvapi=%u pin_owner=%u pin_own=%u rates_attempted=%u rates_installed=%u resource_attempted=%u resource_installed=%u refusal_stage=%u",
                query?1u:0u,rates?1u:0u,resource?1u:0u,systemOk?1u:0u,stubRead?1u:0u,stubOk?1u:0u,ownerRead?1u:0u,ownerSame?1u:0u,ownerKind,
                pinNvapi?1u:0u,pinOwner?1u:0u,pinOwn?1u:0u,pinned?1u:0u,a?1u:0u,a?1u:0u,b?1u:0u,refusal);
            if(pinned&&!a)Log::get().note("vrs coverage refusal: api=rates reason=%s",d.ratesHook.refusal());
            if(a&&!b)Log::get().note("vrs coverage refusal: api=resource reason=%s",d.resourceHook.refusal());
            d.nvapi=retained;d.nvapiOwner=retainedOwner;d.state.invalidate();hooksReady.store(a&&b,std::memory_order_release);
            Log::get().note("vrs coverage control: schema=1 active=1 hooks_ready=%u hook_failure_unqualified=%u no_hot_unhook=1",a&&b?1u:0u,a&&b?0u:1u);
        }
    }
    if(!d.shim){
        HMODULE shim=GetModuleHandleW(L"libpvrclient64.dll");
        if(shim){
            wchar_t shimPath[MAX_PATH]{},ownPath[MAX_PATH]{};HMODULE own=nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&vrsCoverageTick),&own);
            GetModuleFileNameW(shim,shimPath,MAX_PATH);GetModuleFileNameW(own,ownPath,MAX_PATH);
            wchar_t* end=wcsrchr(ownPath,L'\\');if(end)wcscpy_s(end+1,MAX_PATH-size_t(end+1-ownPath),L"libpvrclient64.dll");
            const auto set=reinterpret_cast<EdvrPvrSetGazeSnapshotEnabled>(GetProcAddress(shim,"edvrPvrSetGazeSnapshotEnabled"));
            const auto get=reinterpret_cast<EdvrPvrGetGazeSnapshot>(GetProcAddress(shim,"edvrPvrGetGazeSnapshot"));
            HMODULE retained=nullptr;
            if(end&&_wcsicmp(shimPath,ownPath)==0&&moduleOwns(shim,reinterpret_cast<void*>(set))&&moduleOwns(shim,reinterpret_cast<void*>(get))&&
               GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(shim),&retained)){
                d.shim=retained;d.setGaze=set;d.getGaze=get;
            }
        }
    }
    if(hooksReady.load(std::memory_order_acquire)&&d.setGaze&&!d.gazeEnabled)d.gazeEnabled=d.setGaze(EDVR_PVR_GAZE_SNAPSHOT_VERSION_1,TRUE)!=FALSE;
}
void enqueueMap(Diagnostic&,Slot&,IUnknown*,ID3D11RenderTargetView*,ID3D11DepthStencilView*);
void pollMaps(Diagnostic&);
void pollPipeline(Diagnostic&);
void preparePipeline(Diagnostic&,PipelineSlot&,ID3D11RenderTargetView*,ID3D11DepthStencilView*,bool metadataOnly=false);
bool ensurePipelineQuery(Diagnostic&,PipelineSlot&);
bool beginPipelineQuery(Diagnostic&,PipelineSlot&);
void finishPipeline(Diagnostic&,PipelineSlot&);
void finishReferenceNative(ID3D11DeviceContext*,const VrsWorkReferenceToken&,bool);
void referenceControl(Diagnostic&);
int applyReferenceRate(ID3D11DeviceContext*,const vrs_work_reference::Rate&);
void logEvidence(Diagnostic&);
}

void vrsCoverageConfigure(bool enabled) {
    const bool previous=g_vrsCoverageEnabled.exchange(enabled,std::memory_order_acq_rel);
    if(previous==enabled){
        if(!enabled){auto& d=diag();std::lock_guard<std::mutex> lock(d.mutex);d.offNoted=false;}
        return; // A repeated OFF reload requests a fresh guarded Tick acknowledgement.
    }
    auto& d=diag();std::lock_guard<std::mutex> lock(d.mutex);d.state.invalidate();
    d.havePrevious[0]=d.havePrevious[1]=false;d.completedCount=0;
    for(auto& slot:d.ring){slot.pending=false;slot.drawFinished=false;}
    ++d.pipelineGeneration; // Pending query objects remain owned by the render thread.
    if(!enabled){d.needDisable=d.setGaze!=nullptr;d.offNoted=false;}
    else {d.offNoted=false;d.nextLog=GetTickCount64()+5000;}
}
void vrsWorkReferenceConfigure(bool requested) {
    const bool previous=g_vrsWorkReferenceRequested.exchange(requested,std::memory_order_acq_rel);
    auto& d=diag();std::lock_guard<std::mutex> lock(d.mutex);d.referenceOffNoted=false;d.offNoted=false;
    if(previous!=requested){d.referenceSchedule={};d.referenceAttemptNext[0]=d.referenceAttemptNext[1]=0;}
}
void vrsCoverageRegister(ID3D11Device* device,ID3D11DeviceContext* context) {
    if(!vrsCoverageEnabled()||!device||!context)return;
    auto& d=diag();
    if(d.context){if(d.context.Get()!=context){d.deviceChanged=true;vrsCoverageInvalidate(d.context.Get());}return;}
    if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return;
    d.device=device;d.context=context;context->QueryInterface(IID_PPV_ARGS(&d.identity));
}
void vrsCoverageInvalidate(ID3D11DeviceContext* context) {
    if(!vrsCoverageEnabled())return;auto& d=diag();if(context!=d.context.Get())return;
    std::lock_guard<std::mutex> lock(d.mutex);++d.invalidations;d.state.invalidate();
    if(d.activeReference)++d.activeReference->sample.work.unexpected;
}
VrsCoverageDraw vrsCoverageBeforeDraw(ID3D11DeviceContext* context,unsigned eye,ID3D11RenderTargetView* rtv,ID3D11DepthStencilView* dsv,bool substituted,const VrsCoverageSignature& signature) {
    VrsCoverageDraw token{};
    if(!vrsCoverageEnabled()||!hooksReady.load(std::memory_order_acquire))return token;
    auto& d=diag();if(context!=d.context.Get()||d.deviceChanged||!d.observerThread){
        std::lock_guard<std::mutex> lock(d.mutex);
        if(context!=d.context.Get())++d.gates.contextRefused;else if(d.deviceChanged)++d.gates.deviceRefused;else ++d.gates.observerRefused;
        return token;
    }
    if(GetCurrentThreadId()!=d.observerThread){
        {std::lock_guard<std::mutex> lock(d.mutex);++d.gates.threadRefused;}
        vrsCoverageInvalidate(context);return token;
    }
    ComPtr<IUnknown> view;const auto now=GetTickCount64();
    {std::lock_guard<std::mutex> lock(d.mutex);++d.gates.beforeAccepted;token.nested=d.drawDepth!=0;if(d.drawDepth++){++d.nestedDraws;d.state.invalidate();}
     token.id=++d.nextDraw;token.epoch=d.state.epoch;token.eye=eye;token.active=true;token.substituted=substituted;
     token.known=d.state.ratesKnown&&!d.state.writers&&(!d.state.enabled||d.state.bindingKnown);token.enabled=d.state.active();
     if(eye<2&&token.enabled&&now>=d.nextCopy[eye]){d.nextCopy[eye]=now+1000;
        for(unsigned i=0;i<d.ring.size();++i)if(!d.ring[i].pending){token.ring=i;auto& slot=d.ring[i];slot.sample={};slot.sample.copyId=++d.nextCopyId;slot.sample.drawId=token.id;
            slot.sample.eye=eye;slot.sample.qpcUs=qpcUs();slot.sample.beforeEpoch=token.epoch;slot.sample.rateEpoch=d.state.rateEpoch;slot.sample.bindingEpoch=d.state.bindingEpoch;
            slot.sample.palette=d.state.palette;slot.drawFinished=false;slot.pending=true;view=d.boundView;break;}
        if(token.ring==3)++d.counts[eye].ringBusy;}
     if(!vrsWorkReferenceEnabled()&&vrs_coverage::pipelineSampleDue(eye,token.nested,now,d.nextPipeline[eye<2?eye:0])){d.nextPipeline[eye]=now+1000;
        for(unsigned i=0;i<d.pipelineRing.size();++i)if(!d.pipelineRing[i].active){token.pipelineRing=i;auto& slot=d.pipelineRing[i];slot.active=true;slot.progress={};slot.sample={};
            auto& s=slot.sample;s.drawId=token.id;s.eye=eye;s.beforeEpoch=token.epoch;s.rateEpoch=d.state.rateEpoch;s.bindingEpoch=d.state.bindingEpoch;s.generation=d.pipelineGeneration;s.signature=signature;s.enabled=token.enabled;s.stateKnown=token.known;s.substituted=substituted;break;}
        if(token.pipelineRing==3)++d.pipelineBusy[eye];}
     if(vrsWorkReferenceEnabled()&&eye<2&&!token.nested)d.referenceWorld=token;}
    if(view&&token.ring<3)enqueueMap(d,d.ring[token.ring],view.Get(),rtv,dsv);
    if(token.pipelineRing<3)preparePipeline(d,d.pipelineRing[token.pipelineRing],rtv,dsv);
    return token;
}
void vrsCoverageAfterDraw(ID3D11DeviceContext* context,const VrsCoverageDraw& token,bool issued) {
    // An OFF reload may race an issue already begun: always balance its query.
    if(!token.active)return;auto& d=diag();if(context!=d.context.Get())return;
    if(token.pipelineRing<3){auto& slot=d.pipelineRing[token.pipelineRing];
        if(slot.active&&slot.sample.drawId==token.id&&slot.progress.end())context->End(slot.query.Get());}
    std::lock_guard<std::mutex> lock(d.mutex);if(d.drawDepth)--d.drawDepth;
    if(d.referenceWorld.id==token.id)d.referenceWorld={};
    const bool ambiguous=token.nested||token.epoch!=d.state.epoch||d.state.writers!=0;
    if(issued&&token.eye<2){auto& c=d.counts[token.eye];++c.issued;if(token.substituted)++c.substituted;if(ambiguous)++c.ambiguous;else if(!token.known)++c.unknown;else if(token.enabled)++c.enabled;else ++c.disabled;}
    if(token.ring<3){auto& slot=d.ring[token.ring];if(slot.sample.drawId==token.id){slot.sample.issued=issued;slot.sample.afterEpoch=d.state.epoch;slot.sample.ambiguous=ambiguous;slot.drawFinished=true;}}
    if(token.pipelineRing<3){auto& slot=d.pipelineRing[token.pipelineRing];if(slot.active&&slot.sample.drawId==token.id){slot.sample.issued=issued;slot.sample.afterEpoch=d.state.epoch;slot.sample.ambiguous=ambiguous;if(slot.sample.generation!=d.pipelineGeneration)slot.progress.cancel();}}
}
void vrsCoverageNoteGate(uint64_t vs,uint64_t ps,bool owner,bool rtvEye,bool notUi,
                        bool allowedClass,int depthEye,bool beforeAccepted,bool issued) {
    if(!vrsCoverageEnabled()||vs!=vrs_coverage::kWorldVs)return;
    auto& d=diag();std::lock_guard<std::mutex> lock(d.mutex);
    d.gates.note(vs,ps,owner,rtvEye,notUi,allowedClass,depthEye,beforeAccepted,issued);
}
VrsWorkReferenceToken vrsWorkReferenceBeforeNative(ID3D11DeviceContext* context,const VrsCoverageSignature& signature) {
    VrsWorkReferenceToken token{};
    if(!vrsWorkReferenceEnabled()||!vrsCoverageEnabled()||!vrs_work_reference::selected(signature))return token;
    auto& d=diag();if(context!=d.context.Get()||GetCurrentThreadId()!=d.observerThread)return token;
    const auto now=GetTickCount64();unsigned eye=2;vrs_work_reference::Plan plan;
    {std::lock_guard<std::mutex> lock(d.mutex);eye=d.referenceWorld.eye;
        if(eye>=2){++d.referenceUnknownEye;return token;}auto& counts=d.referenceCounts[eye];++counts.hits;
        if(d.activeReference){++d.activeReference->sample.work.unexpected;++counts.unknown;return token;}
        if(d.referenceFaulted||d.deviceChanged||d.drawDepth!=1||!d.referenceWorld.active||!d.state.active()){++counts.unknown;return token;}
        if(now<d.referenceAttemptNext[eye]){++counts.cadence;return token;}
        plan=d.referenceSchedule.peek(eye,signature.startInstance,now);
        if(plan.expired){++counts.expired;return token;}if(!plan.ready){++counts.cadence;return token;}
        d.referenceAttemptNext[eye]=now+3000;}
    EdvrPvrGazeSnapshotV1 mode{};mode.size=sizeof(mode);mode.version=1;LARGE_INTEGER qpc{},frequency{};
    const bool publication=d.gazeEnabled&&d.getGaze&&d.getGaze(1,sizeof(mode),&mode)!=FALSE&&QueryPerformanceCounter(&qpc)&&QueryPerformanceFrequency(&frequency)&&
        qpc.QuadPart>0&&frequency.QuadPart>0&&vrs_work_reference::modeOne(mode,uint64_t(qpc.QuadPart),uint64_t(frequency.QuadPart));
    if(!publication){std::lock_guard<std::mutex> lock(d.mutex);++d.referenceCounts[eye].provider;d.referenceAttemptNext[eye]=now+3000;return token;}
    {std::lock_guard<std::mutex> lock(d.mutex);
        if(!vrsWorkReferenceEnabled()||!vrsCoverageEnabled()||d.activeReference||!d.state.active()||d.referenceWorld.eye!=eye){++d.referenceCounts[eye].unknown;return token;}
        for(unsigned i=0;i<d.pipelineRing.size();++i)if(!d.pipelineRing[i].active){token={d.referenceWorld.id,i,true};auto& slot=d.pipelineRing[i];slot.active=true;slot.progress={};slot.sample={};
            auto& s=slot.sample;s.nativePrimary=true;s.drawId=token.id;s.eye=eye;s.generation=d.pipelineGeneration;s.beforeEpoch=d.state.epoch;s.rateEpoch=d.state.rateEpoch;s.bindingEpoch=d.state.bindingEpoch;s.signature=signature;s.stateKnown=true;s.enabled=true;s.substituted=d.referenceWorld.substituted;
            auto& w=s.work;w.present=true;w.modeOne=true;w.cycle=plan.cycle;w.modeAgeUs=uint64_t(double(uint64_t(qpc.QuadPart)-mode.callbackQpc)*1000000.0/double(frequency.QuadPart));w.thread=GetCurrentThreadId();w.initialBindingEpoch=d.state.bindingEpoch;
            w.override.start(plan.arm,{true,true,1,d.state.palette});w.override.applied=w.override.original;w.executed=w.override.original;break;}
        if(!token.active){++d.referenceCounts[eye].ring;return token;}}
    auto& slot=d.pipelineRing[token.ring];auto& s=slot.sample;auto& w=s.work;
    preparePipeline(d,slot,static_cast<ID3D11RenderTargetView*>(bindingGet(BindSlot::Rtv0)),static_cast<ID3D11DepthStencilView*>(bindingGet(BindSlot::Dsv0)),true);
    if(!slot.active){++d.referenceCounts[eye].unknown;return {};}
    if(!ensurePipelineQuery(d,slot)){finishPipeline(d,slot);return {};}
    {std::lock_guard<std::mutex> lock(d.mutex);
        if(!vrsWorkReferenceEnabled()||!vrsCoverageEnabled()||d.activeReference||s.beforeEpoch!=d.state.epoch||!d.state.active()){++d.referenceCounts[eye].unknown;slot.progress.cancel();slot.active=false;return {};}
        d.activeReference=&slot;referenceScopeActive.store(true,std::memory_order_release);w.expectedEpoch=d.state.epoch;d.referenceOffNoted=false;}
    if(plan.arm!=vrs_work_reference::Arm::Natural)w.applyStatus=applyReferenceRate(context,w.override.original);
    bool applyOk=false;
    {std::lock_guard<std::mutex> lock(d.mutex);s.beforeEpoch=d.state.epoch;s.rateEpoch=d.state.rateEpoch;s.bindingEpoch=d.state.bindingEpoch;
        s.stateKnown=d.state.ratesKnown&&!d.state.writers&&(!d.state.enabled||d.state.bindingKnown);s.enabled=d.state.active();w.expectedEpoch=d.state.epoch;
        w.executed={d.state.ratesKnown,d.state.enabled,d.state.ratesKnown?1u:0u,d.state.palette};applyOk=w.applyStatus==0&&w.override.unexpected==0&&!w.override.surrendered;}
    if(!applyOk||!beginPipelineQuery(d,slot)){finishReferenceNative(context,token,false);return {};}
    {std::lock_guard<std::mutex> lock(d.mutex);d.referenceSchedule.issued(eye,signature.startInstance,now);d.referenceAttemptNext[eye]=0;++d.referenceCounts[eye].sampled;}
    return token;
}
void vrsWorkReferenceAfterNative(ID3D11DeviceContext* context,const VrsWorkReferenceToken& token) {finishReferenceNative(context,token,true);}
void vrsCoverageTick(ID3D11DeviceContext* context) {
    auto& d=diag();
    referenceControl(d);
    if(!vrsCoverageEnabled()){
        if(d.offNoted&&d.lastOffAck)return; // Stable OFF performs no new poll/query/copy work.
        if(d.needDisable&&d.setGaze){d.needDisable=d.setGaze(EDVR_PVR_GAZE_SNAPSHOT_VERSION_1,FALSE)==FALSE;if(!d.needDisable)d.gazeEnabled=false;}
        // OFF creates no query/copy. Only already-ended queries are drained,
        // on their owner thread, before the final performance-safe ack.
        if(context==d.context.Get()&&GetCurrentThreadId()==d.observerThread)pollPipeline(d);
        unsigned pending=0;for(const auto& slot:d.pipelineRing)pending+=slot.active?1u:0u;
        bool ack=false;{std::lock_guard<std::mutex> lock(d.mutex);ack=vrs_coverage::pipelineOffQuiescent(!d.needDisable&&!d.state.writers&&!d.drawDepth,pending)&&!d.activeReference&&!d.referenceRestoreInFlight&&!d.referenceFaulted;}
        if(!d.offNoted||d.lastOffAck!=ack){d.offNoted=true;d.lastOffAck=ack;Log::get().note("vrs coverage control: schema=1 active=0 gaze_publish_disabled_ack=%u forward_only=1 copies=0 queries=0 pending_pipeline_queries=%u",ack?1u:0u,pending);}
        return;
    }
    if(!d.context&&context){ComPtr<ID3D11Device> device;context->GetDevice(&device);vrsCoverageRegister(device.Get(),context);}
    if(context!=d.context.Get()||d.deviceChanged)return;
    if(!d.observerThread)d.observerThread=GetCurrentThreadId();
    if(GetCurrentThreadId()!=d.observerThread){vrsCoverageInvalidate(context);return;}
    const auto now=GetTickCount64();discover(d,now);pollMaps(d);pollPipeline(d);
    if(now>=d.nextLog){d.nextLog=now+5000;logEvidence(d);}
}
namespace {
int applyReferenceRate(ID3D11DeviceContext* context,const vrs_work_reference::Rate& rate) {
    ViewportRate viewport{};viewport.enabled=rate.enabled?1u:0u;for(unsigned i=0;i<16;++i)viewport.palette[i]=rate.palette[i];
    RateDesc desc{0x10010,rate.count,rate.count?&viewport:nullptr};
    struct Internal {bool old;Internal():old(referenceInternal){referenceInternal=true;}~Internal(){referenceInternal=old;}} internal;
    return setRates(context,&desc);
}
void finishReferenceNative(ID3D11DeviceContext* context,const VrsWorkReferenceToken& token,bool issued) {
    if(!token.active||token.ring>=3)return;auto& d=diag();auto& slot=d.pipelineRing[token.ring];auto& s=slot.sample;auto& w=s.work;
    if(context!=d.context.Get()||!slot.active||s.drawId!=token.id||!w.present)return;
    if(slot.progress.end())context->End(slot.query.Get());
    vrs_work_reference::Rate restore;bool restoreNeeded=false;
    {std::lock_guard<std::mutex> lock(d.mutex);s.issued=issued;s.afterEpoch=d.state.epoch;
        w.executed={d.state.ratesKnown,d.state.enabled,d.state.ratesKnown?1u:0u,d.state.palette};
        const bool ordered=d.activeReference==&slot&&!d.state.writers&&d.state.epoch==w.expectedEpoch&&d.state.bindingEpoch==w.initialBindingEpoch;
        const bool rateProof=d.state.active()&&(w.override.forced?w.executed.uniform(vrs_work_reference::uniformRate(w.override.arm)):w.override.nestedRates==0);
        w.stateProven=issued&&ordered&&rateProof&&w.modeOne&&w.override.unexpected==0&&w.unexpected==0&&w.bindingCalls==0;
        s.ambiguous=!ordered||w.override.unexpected!=0||w.unexpected!=0||w.bindingCalls!=0;
        restore=w.override.lastOriginal;restoreNeeded=w.override.restoreRequired;w.override.restoring=true;d.referenceRestoreInFlight=restoreNeeded;}
    if(restoreNeeded)w.restoreStatus=applyReferenceRate(context,restore);
    {std::lock_guard<std::mutex> lock(d.mutex);
        w.restored={d.state.ratesKnown,d.state.enabled,restore.count,d.state.palette};
        w.restoreVerified=!w.override.surrendered&&(!w.override.restoreRequired||(w.restoreStatus==0&&w.restored.same(restore)&&!d.state.writers));
        if(!w.restoreVerified){d.referenceFaulted=true;++d.referenceCounts[s.eye].restoreFailure;}
        if(w.applyStatus!=0)d.referenceFaulted=true;
        if(s.generation!=d.pipelineGeneration||!vrsCoverageEnabled()||!vrsWorkReferenceEnabled())slot.progress.cancel();
        d.activeReference=nullptr;referenceScopeActive.store(false,std::memory_order_release);d.referenceRestoreInFlight=false;d.referenceOffNoted=false;}
    if(!slot.progress.pending())finishPipeline(d,slot);
}
void referenceControl(Diagnostic& d) {
    if(d.observerThread&&GetCurrentThreadId()!=d.observerThread)return;
    const bool requested=vrsWorkReferenceEnabled();unsigned pending=0;for(const auto& slot:d.pipelineRing)pending+=slot.active&&slot.sample.work.present?1u:0u;
    std::lock_guard<std::mutex> lock(d.mutex);
    const bool ack=!requested&&!pending&&!d.activeReference&&!d.referenceRestoreInFlight&&!d.referenceFaulted;
    if(!d.referenceOffNoted||d.referenceLastAck!=ack){d.referenceOffNoted=true;d.referenceLastAck=ack;
        Log::get().note("vrs work reference control: schema=1 requested=%u active=%u selector=useful_288x4 pending_queries=%u scope_active=%u restore_inflight=%u faulted=%u quiescent_ack=%u burst_limit=3 cooldown_ms=3000 cycle_expiry_ms=1000",requested?1u:0u,requested&&vrsCoverageEnabled()&&!d.referenceFaulted?1u:0u,pending,d.activeReference?1u:0u,d.referenceRestoreInFlight?1u:0u,d.referenceFaulted?1u:0u,ack?1u:0u);}
}
bool sameIdentity(IUnknown* a,IUnknown* b){ComPtr<IUnknown>x,y;return a&&b&&SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x)))&&SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y)))&&x.Get()==y.Get();}
bool pipelineValid(const PipelineSample& s) {
    const bool base=s.queryReady&&s.issued&&!s.ambiguous&&!s.cancelled&&!s.timedOut&&s.bindingsVerified;
    if(s.work.present)return base&&s.work.stateProven&&s.work.restoreVerified&&vrs_work_reference::workMatches(s.stats.IAVertices,s.stats.IAPrimitives,s.stats.PSInvocations);
    return base&&s.beforeEpoch==s.afterEpoch;
}
// Shared original-world target validation for map and pipeline samples. No
// size-based eye inference: the caller already resolved the exact scene DSV.
bool captureBindings(Diagnostic& d,ID3D11RenderTargetView* expectedRtv,ID3D11DepthStencilView* expectedDsv,D3D11_TEXTURE2D_DESC& rtDesc) {
    ID3D11RenderTargetView* raw[4]{};ComPtr<ID3D11DepthStencilView> depth;
    d.context->OMGetRenderTargets(4,raw,&depth);
    std::array<ComPtr<ID3D11RenderTargetView>,4> targets;for(unsigned i=0;i<4;++i)targets[i].Attach(raw[i]);
    if(!sameIdentity(targets[0].Get(),expectedRtv)||!sameIdentity(depth.Get(),expectedDsv))return false;
    for(unsigned i=0;i<4;++i){if(!targets[i])return false;ComPtr<ID3D11Resource> resource;targets[i]->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
        if(FAILED(resource.As(&texture)))return false;D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        if(i==0)rtDesc=desc;else if(desc.Width!=rtDesc.Width||desc.Height!=rtDesc.Height)return false;}
    if(rtDesc.SampleDesc.Count!=1)return false;
    ComPtr<ID3D11Resource> depthResource;depth->GetResource(&depthResource);ComPtr<ID3D11Texture2D> depthTexture;
    if(FAILED(depthResource.As(&depthTexture)))return false;D3D11_TEXTURE2D_DESC depthDesc{};depthTexture->GetDesc(&depthDesc);
    if(depthDesc.Width!=rtDesc.Width||depthDesc.Height!=rtDesc.Height||depthDesc.SampleDesc.Count!=1)return false;
    UINT vpCount=16;D3D11_VIEWPORT viewports[16]{};d.context->RSGetViewports(&vpCount,viewports);
    return vpCount==1&&viewports[0].TopLeftX==0&&viewports[0].TopLeftY==0&&viewports[0].Width==float(rtDesc.Width)&&viewports[0].Height==float(rtDesc.Height);
}
void finishPipeline(Diagnostic& d,PipelineSlot& slot) {
    {std::lock_guard<std::mutex> lock(d.mutex);if(slot.sample.generation!=d.pipelineGeneration||!vrsCoverageEnabled())slot.progress.cancel();}
    auto& s=slot.sample;s.cancelled=slot.progress.cancelled;
    const bool valid=pipelineValid(s);
    if(valid&&s.stateKnown)++d.pipelineReady[s.eye];else ++d.pipelineRejected[s.eye];
    if(d.pipelineCompletedCount<d.pipelineCompleted.size())d.pipelineCompleted[d.pipelineCompletedCount++]=s;else ++d.pipelineOverflow;
    slot.active=false;
}
void preparePipeline(Diagnostic& d,PipelineSlot& slot,ID3D11RenderTargetView* rtv,ID3D11DepthStencilView* dsv,bool metadataOnly) {
    auto& s=slot.sample;
    const bool safe=guarded("vrsCoverage/pipelineMetadata",[&]{
        D3D11_TEXTURE2D_DESC rt{};if(!captureBindings(d,rtv,dsv,rt))return;
        s.rtW=rt.Width;s.rtH=rt.Height;s.samples=rt.SampleDesc.Count;s.quality=rt.SampleDesc.Quality;s.bindingsVerified=true;
        D3D11_PRIMITIVE_TOPOLOGY topology{};d.context->IAGetPrimitiveTopology(&topology);s.topology=unsigned(topology);
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;d.context->VSGetShader(&vs,nullptr,nullptr);d.context->PSGetShader(&ps,nullptr,nullptr);
        s.actualVsOriginal=vs.Get()==bindingGet(BindSlot::Vs);s.actualPsOriginal=ps.Get()==bindingGet(BindSlot::Ps);
        ComPtr<ID3D11RasterizerState> raster;d.context->RSGetState(&raster);s.rasterKnown=true;
        D3D11_RASTERIZER_DESC base{};base.FillMode=D3D11_FILL_SOLID;base.CullMode=D3D11_CULL_BACK;base.DepthClipEnable=TRUE;
        if(raster)raster->GetDesc(&base);
        s.fillMode=unsigned(base.FillMode);s.cullMode=unsigned(base.CullMode);s.frontCounterClockwise=base.FrontCounterClockwise!=FALSE;s.depthClip=base.DepthClipEnable!=FALSE;s.scissorEnabled=base.ScissorEnable!=FALSE;
        D3D11_RECT rectangles[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};UINT rectangleCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        d.context->RSGetScissorRects(&rectangleCount,rectangles);s.scissorCount=rectangleCount;
        if(rectangleCount){s.scissor0=rectangles[0];s.scissorHash=14695981039346656037ull;
            const unsigned bounded=(std::min)(rectangleCount,unsigned(D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE));
            for(unsigned i=0;i<bounded;++i){const uint32_t values[]={uint32_t(rectangles[i].left),uint32_t(rectangles[i].top),uint32_t(rectangles[i].right),uint32_t(rectangles[i].bottom)};
                for(auto value:values)s.scissorHash=(s.scissorHash^value)*1099511628211ull;}}
        if(!raster){s.forcedKnown=s.conservativeKnown=true;}
        else {ComPtr<ID3D11RasterizerState2> r2;ComPtr<ID3D11RasterizerState1> r1;
            if(SUCCEEDED(raster.As(&r2))){D3D11_RASTERIZER_DESC2 desc{};r2->GetDesc2(&desc);s.forcedKnown=s.conservativeKnown=true;s.forcedSamples=desc.ForcedSampleCount;s.conservative=unsigned(desc.ConservativeRaster);}
            else if(SUCCEEDED(raster.As(&r1))){D3D11_RASTERIZER_DESC1 desc{};r1->GetDesc1(&desc);s.forcedKnown=true;s.forcedSamples=desc.ForcedSampleCount;}}
        ComPtr<ID3D11BlendState> blend;FLOAT factor[4]{};d.context->OMGetBlendState(&blend,factor,&s.sampleMask);s.blendKnown=true;
        for(unsigned i=0;i<4;++i)s.blendFactor[i]=factor[i];
        if(blend){D3D11_BLEND_DESC desc{};blend->GetDesc(&desc);s.alphaToCoverage=desc.AlphaToCoverageEnable!=FALSE;
            for(unsigned i=0;i<8;++i){const auto& rtBlend=desc.RenderTarget[desc.IndependentBlendEnable?i:0];if(rtBlend.BlendEnable)s.blendEnabledMask|=1u<<i;s.writeMasks|=uint32_t(rtBlend.RenderTargetWriteMask)<<(4*i);}}
        else s.writeMasks=0xffffffffu;
    });
    if(!safe||!s.bindingsVerified){s.status=E_FAIL;finishPipeline(d,slot);return;}
    if(metadataOnly)return;
    if(!ensurePipelineQuery(d,slot)||!beginPipelineQuery(d,slot)){slot.progress.cancel();finishPipeline(d,slot);}
}
bool ensurePipelineQuery(Diagnostic& d,PipelineSlot& slot) {
    if(slot.query)return true;D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS,0};slot.sample.status=d.device->CreateQuery(&desc,&slot.query);
    if(FAILED(slot.sample.status)){++d.pipelineErrors[slot.sample.eye];return false;}return true;
}
bool beginPipelineQuery(Diagnostic& d,PipelineSlot& slot) {
    auto& s=slot.sample;
    {std::lock_guard<std::mutex> lock(d.mutex);
        if(!vrsCoverageEnabled()||s.generation!=d.pipelineGeneration||s.beforeEpoch!=d.state.epoch||d.state.writers)return false;
        if(!slot.progress.begin())return false;}
    slot.startedMs=GetTickCount64();d.context->Begin(slot.query.Get());++d.pipelineStarted[s.eye];s.status=S_FALSE;return true;
}
void pollPipeline(Diagnostic& d) {
    uint64_t generation=0;{std::lock_guard<std::mutex> lock(d.mutex);generation=d.pipelineGeneration;}
    for(auto& slot:d.pipelineRing){if(!slot.active||!slot.progress.pollable())continue;
        if(slot.sample.generation!=generation)slot.progress.cancel();
        auto& s=slot.sample;s.status=d.context->GetData(slot.query.Get(),&s.stats,sizeof(s.stats),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(s.status==S_FALSE){
            if(!s.timedOut&&GetTickCount64()-slot.startedMs>=5000){s.timedOut=true;slot.progress.cancel();
                Log::get().note("vrs pipeline stalled: schema=1 eye=%u draw_id=%llu elapsed_ms=%llu pending_retained=1 evidence_valid=0 off_ack_blocked=1",s.eye,s.drawId,GetTickCount64()-slot.startedMs);}
            continue; // At most three retained objects; never Flush, wait, or fabricate an OFF acknowledgement.
        }
        if(FAILED(s.status)){++d.pipelineErrors[s.eye];
            Log::get().note("vrs pipeline error: schema=1 eye=%u draw_id=%llu get_data_hr=0x%08x device_removed_hr=0x%08x evidence_valid=0",s.eye,s.drawId,unsigned(s.status),unsigned(d.device->GetDeviceRemovedReason()));
            slot.query.Reset();}
        else s.queryReady=s.status==S_OK;
        slot.progress.complete(s.status==S_OK);finishPipeline(d,slot);
    }
}
void enqueueMap(Diagnostic& d,Slot& slot,IUnknown* view,ID3D11RenderTargetView* expectedRtv,ID3D11DepthStencilView* expectedDsv) {
    auto& s=slot.sample;bool copied=false;
    const bool safe=guarded("vrsCoverage/copyRateMap",[&]{
        D3D11_TEXTURE2D_DESC rtDesc{};
        if(!captureBindings(d,expectedRtv,expectedDsv,rtDesc))return;
        ComPtr<NvRateView> nvView;if(FAILED(view->QueryInterface(IID_PPV_ARGS(&nvView))))return;
        SrrvDesc selected{};selected.version=0x10018;
        if(FAILED(nvView->GetDesc(&selected))||(selected.format!=DXGI_FORMAT_R8_UINT&&selected.format!=DXGI_FORMAT_R8_TYPELESS)||
            (selected.dimension!=4&&selected.dimension!=5)||(selected.dimension==5&&selected.count!=1))return;
        ComPtr<ID3D11Resource> mapResource;nvView->GetResource(&mapResource);ComPtr<ID3D11Texture2D> map;
        if(FAILED(mapResource.As(&map)))return;D3D11_TEXTURE2D_DESC desc{};map->GetDesc(&desc);
        ComPtr<ID3D11Device> mapDevice;map->GetDevice(&mapDevice);
        if((desc.Format!=DXGI_FORMAT_R8_UINT&&desc.Format!=DXGI_FORMAT_R8_TYPELESS)||!sameIdentity(mapDevice.Get(),d.device.Get()))return;
        const unsigned slice=selected.dimension==5?selected.slice:0;
        if(desc.SampleDesc.Count!=1||selected.mip>=desc.MipLevels||slice>=desc.ArraySize||selected.mip>=32)return;
        const unsigned width=(std::max)(1u,desc.Width>>selected.mip),height=(std::max)(1u,desc.Height>>selected.mip);
        const unsigned tilesW=(rtDesc.Width+15)/16,tilesH=(rtDesc.Height+15)/16;
        if(!width||!height||width>512||height>512||width<tilesW||height<tilesH)return;
        // The selected subresource is copied as-is, then decoded with THIS
        // immutable palette and binding epoch, never the later live state.
        D3D11_TEXTURE2D_DESC stagingDesc{};stagingDesc.Width=width;stagingDesc.Height=height;stagingDesc.MipLevels=stagingDesc.ArraySize=1;
        stagingDesc.Format=desc.Format;stagingDesc.SampleDesc.Count=1;stagingDesc.Usage=D3D11_USAGE_STAGING;stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(slot.staging){D3D11_TEXTURE2D_DESC old{};slot.staging->GetDesc(&old);if(old.Width!=width||old.Height!=height||old.Format!=desc.Format)slot.staging.Reset();}
        if(!slot.staging&&FAILED(d.device->CreateTexture2D(&stagingDesc,nullptr,&slot.staging)))return;
        s.rtW=rtDesc.Width;s.rtH=rtDesc.Height;s.mapW=width;s.mapH=height;s.tilesW=tilesW;s.tilesH=tilesH;s.mip=selected.mip;s.slice=slice;s.bindingsVerified=true;
        auto& callback=s.callback;callback.eye=s.eye;callback.rtW=s.rtW;callback.rtH=s.rtH;callback.mapW=width;callback.mapH=height;callback.tilesW=tilesW;callback.tilesH=tilesH;callback.mip=s.mip;callback.slice=s.slice;callback.palette=s.palette;
        callback.worldVerified=SUCCEEDED(expectedDsv->QueryInterface(IID_PPV_ARGS(&s.callbackWorld)))&&SUCCEEDED(map.As(&s.callbackMap));
        callback.worldIdentity=reinterpret_cast<uintptr_t>(s.callbackWorld.Get());callback.mapIdentity=reinterpret_cast<uintptr_t>(s.callbackMap.Get());
        if(d.getGaze&&d.gazeEnabled){s.gaze.size=sizeof(s.gaze);s.gaze.version=EDVR_PVR_GAZE_SNAPSHOT_VERSION_1;
            s.gazeAvailable=d.getGaze(s.gaze.version,sizeof(s.gaze),&s.gaze)!=FALSE;
            if(s.gazeAvailable){LARGE_INTEGER now{},frequency{};QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
                callback.gaze=s.gaze;callback.nowQpc=now.QuadPart>0?uint64_t(now.QuadPart):0;callback.qpcFrequency=frequency.QuadPart>0?uint64_t(frequency.QuadPart):0;
                const bool clock=s.gaze.qpcFrequency==uint64_t(frequency.QuadPart)&&s.gaze.callbackQpc&&s.gaze.callbackQpc<=uint64_t(now.QuadPart);
                s.gazeAgeUs=clock?uint64_t(double(uint64_t(now.QuadPart)-s.gaze.callbackQpc)*1000000.0/double(s.gaze.qpcFrequency)):UINT64_MAX;
                s.gazeFresh=s.gaze.size==sizeof(s.gaze)&&s.gaze.version==1&&s.gaze.sequence&&clock&&s.gazeAgeUs<=50000&&
                    s.gaze.providerQueried==1&&s.gaze.providerFiniteValid==1&&s.gaze.cachedFallback==0&&s.gaze.ignored==0&&s.gaze.trackerPresent==1&&s.gaze.returnedValid==1&&s.gaze.mode>=1&&s.gaze.mode<=4&&s.gaze.invertY<=1&&
                    std::isfinite(s.gaze.returnedGazeTan[s.eye][0])&&std::isfinite(s.gaze.returnedGazeTan[s.eye][1]);}}
        s.qpcUs=qpcUs();d.context->CopySubresourceRegion(slot.staging.Get(),0,0,0,0,map.Get(),D3D11CalcSubresource(selected.mip,slice,desc.MipLevels),nullptr);
        copied=true;++d.counts[s.eye].copies;
    });
    if(!safe||!copied){++d.counts[s.eye].invalid;slot.pending=false;}
}
void correlate(Diagnostic& d,Sample& s) {
    auto& c=d.counts[s.eye];if(s.gazeAvailable)++c.gazeAvailable;if(s.gazeFresh)++c.gazeFresh;else ++c.gazeStale;
    if(vrs_callback_dependency::eligible(s.callback))++c.cbEligible;else ++c.cbIneligible;
    auto& previous=d.previous[s.eye];
    if(d.havePrevious[s.eye]){
        if(vrs_callback_dependency::pairEligible(previous.callback,s.callback)&&s.histogram.hq&&previous.histogram.hq){
            ++c.cbPairs;
            const double gx=double(s.gaze.returnedGazeTan[s.eye][0])-previous.gaze.returnedGazeTan[s.eye][0];
            const double gy=double(s.gaze.returnedGazeTan[s.eye][1])-previous.gaze.returnedGazeTan[s.eye][1];
            const double mx=s.histogram.hqX-previous.histogram.hqX,my=s.histogram.hqY-previous.histogram.hqY;
            const bool gazeChanged=std::hypot(gx,gy)>=1e-4,mapChanged=s.contentHash!=previous.contentHash;
            if(gazeChanged)++c.cbGazeChanges;if(mapChanged)++c.cbMapChanges;if(gazeChanged&&mapChanged)++c.cbPairedChanges;
            if(!gazeChanged||!mapChanged||std::hypot(mx,my)<.25)++c.cbInsufficientExcitation;
            c.callbackDirection.add(gx,gy,mx,my,s.tilesW,s.tilesH);
        }else ++c.cbPairRefused;
        const bool same=s.rtW==previous.rtW&&s.rtH==previous.rtH&&s.tilesW==previous.tilesW&&s.tilesH==previous.tilesH&&s.palette==previous.palette&&
            s.gazeFresh&&previous.gazeFresh&&s.gaze.mode==previous.gaze.mode&&s.gaze.invertY==previous.gaze.invertY&&s.gaze.sequence>previous.gaze.sequence&&
            s.gaze.callbackQpc>previous.gaze.callbackQpc&&s.qpcUs>previous.qpcUs&&s.qpcUs-previous.qpcUs<=2500000;
        if(same&&s.histogram.hq&&previous.histogram.hq){
            const double gx=double(s.gaze.returnedGazeTan[s.eye][0])-previous.gaze.returnedGazeTan[s.eye][0];
            const double gy=double(s.gaze.returnedGazeTan[s.eye][1])-previous.gaze.returnedGazeTan[s.eye][1];
            const double mx=s.histogram.hqX-previous.histogram.hqX,my=s.histogram.hqY-previous.histogram.hqY;
            s.gazeChanged=std::hypot(gx,gy)>=1e-4;s.mapChanged=s.contentHash!=previous.contentHash;s.pairedChange=s.gazeChanged&&s.mapChanged;
            if(s.pairedChange)++c.pairedChange;c.direction.add(gx,gy,mx,my,s.tilesW,s.tilesH);
        }else ++c.gazeAmbiguous;
    }
    previous=s;d.havePrevious[s.eye]=true;
}
void pollMaps(Diagnostic& d) {
    for(auto& slot:d.ring){if(!slot.pending||!slot.drawFinished)continue;
        auto& s=slot.sample;auto& c=d.counts[s.eye];
        if(!s.issued||s.ambiguous||s.beforeEpoch!=s.afterEpoch||!s.bindingsVerified){++c.invalid;slot.pending=false;continue;}
        D3D11_MAPPED_SUBRESOURCE mapped{};const auto result=d.context->Map(slot.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if(result==DXGI_ERROR_WAS_STILL_DRAWING)continue;
        if(FAILED(result)){++c.invalid;slot.pending=false;continue;}
        uint64_t hash=14695981039346656037ull;s.histogram={};
        const bool decoded=guarded("vrsCoverage/decodeRateMap",[&]{
            if(!mapped.pData||mapped.RowPitch<s.tilesW){s.histogram.invalid=true;return;}
            const auto* bytes=static_cast<const uint8_t*>(mapped.pData);
            for(unsigned y=0;y<s.tilesH;++y)for(unsigned x=0;x<s.tilesW;++x){const auto index=bytes[size_t(y)*mapped.RowPitch+x];s.histogram.add(index,s.palette,x,y);hash=(hash^index)*1099511628211ull;}
        });
        d.context->Unmap(slot.staging.Get(),0);s.histogram.finish();s.contentHash=hash;
        s.valid=decoded&&!s.histogram.invalid&&s.histogram.tiles==uint64_t(s.tilesW)*s.tilesH;
        if(s.valid){++c.ready;if(s.histogram.nonuniform())++c.nonuniform;if(s.histogram.coarse)++c.coarse;correlate(d,s);}else ++c.invalid;
        if(d.completedCount<d.completed.size())d.completed[d.completedCount++]=s;else ++d.outputOverflow;
        slot.pending=false;
    }
}
template<class T,size_t N>std::string list(const std::array<T,N>& values){std::string text;for(auto value:values){if(!text.empty())text+=',';text+=std::to_string(value);}return text;}
void logEvidence(Diagnostic& d) {
    uint64_t ratesOk=0,ratesError=0,bindingOk=0,bindingError=0,nestedDraws=0,invalidations=0;
    vrs_coverage::GateCounts gates;
    {std::lock_guard<std::mutex> lock(d.mutex);ratesOk=d.ratesOk;ratesError=d.ratesError;bindingOk=d.bindingOk;bindingError=d.bindingError;nestedDraws=d.nestedDraws;invalidations=d.invalidations;gates=d.gates;}
    Log::get().note("vrs coverage gates: schema=1 cumulative known_vs=%llu ps_60d=%llu ps_e46e=%llu other_ps=%llu owner=%llu rtv_eye=%llu not_ui=%llu allowed_class=%llu exact_family=%llu depth_left=%llu depth_right=%llu depth_unresolved=%llu candidate_before_accepted=%llu candidate_issued=%llu family_depth_before_accepted=%llu family_depth_issued=%llu before_accepted=%llu context_refused=%llu device_refused=%llu observer_refused=%llu thread_refused=%llu",
        gates.knownVs,gates.ps60,gates.psE46,gates.otherPs,gates.owner,gates.rtvEye,gates.notUi,gates.allowedClass,gates.exactFamily,gates.depthLeft,gates.depthRight,gates.depthUnresolved,gates.candidateBeforeAccepted,gates.candidateIssued,gates.familyDepthBeforeAccepted,gates.familyDepthIssued,gates.beforeAccepted,gates.contextRefused,gates.deviceRefused,gates.observerRefused,gates.threadRefused);
    for(unsigned eye=0;eye<2;++eye){const auto& c=d.counts[eye];const auto& q=c.direction;
        // One coherent counter row, including direction partitions. The
        // existing 2800-byte reserved logger avoids normal1200 truncation.
        Log::get().noteReserved("vrs coverage: schema=1 cumulative eye=%u active=1 hooks_ready=%u producer_verified=1 world_issued=%llu world_motion_substituted=%llu enabled=%llu disabled=%llu unknown=%llu ambiguous=%llu rates_ok=%llu rates_error=%llu binding_ok=%llu binding_error=%llu map_copies=%llu map_ready=%llu map_invalid=%llu nested_draws=%llu invalidations=%llu output_overflow=%llu directional_pairs=%llu direction_same_y_agree=%llu direction_same_y_mismatch=%llu direction_flipped_y_agree=%llu direction_flipped_y_mismatch=%llu direction_x_pairs=%llu direction_x_agree=%llu direction_x_mismatch=%llu direction_y_pairs=%llu direction_y_same_agree=%llu direction_y_flipped_agree=%llu direction_noise_skipped=%llu stale_or_unavailable=%llu timestamp_or_topology_ambiguous=%llu paired_changes=%llu mapping_units_verified=0 physical_calibration_verified=0 coordinates_logged=0",
            eye,hooksReady.load()?1u:0u,c.issued,c.substituted,c.enabled,c.disabled,c.unknown,c.ambiguous,ratesOk,ratesError,bindingOk,bindingError,c.copies,c.ready,c.invalid,nestedDraws,invalidations,d.outputOverflow,q.pairs,q.sameYAgree,q.sameYMismatch,q.flippedYAgree,q.flippedYMismatch,q.xPairs,q.xAgree,q.xMismatch,q.yPairs,q.ySameAgree,q.yFlippedAgree,q.noise,c.gazeStale,c.gazeAmbiguous,c.pairedChange);
        const auto& cb=c.callbackDirection;
        Log::get().noteReserved("vrs callback geometry: schema=1 cumulative eye=%u cb_eligible=%llu cb_ineligible=%llu cb_pairs=%llu cb_pair_refused=%llu cb_gaze_changes=%llu cb_map_changes=%llu cb_paired_changes=%llu cb_insufficient_excitation=%llu cb_directional_pairs=%llu cb_same_y_agree=%llu cb_same_y_mismatch=%llu cb_flipped_y_agree=%llu cb_flipped_y_mismatch=%llu cb_x_pairs=%llu cb_x_agree=%llu cb_x_mismatch=%llu cb_y_pairs=%llu cb_y_same_agree=%llu cb_y_flipped_agree=%llu cb_noise_skipped=%llu callback_publication_age_limit_us=50000 returned_valid_requirement=0 legacy_gaze_freshness_unchanged=1 sensor_freshness_verified=0 physical_calibration_verified=0 mapping_units_verified=0 coordinates_logged=0 resource_identities_logged=0",
            eye,c.cbEligible,c.cbIneligible,c.cbPairs,c.cbPairRefused,c.cbGazeChanges,c.cbMapChanges,c.cbPairedChanges,c.cbInsufficientExcitation,cb.pairs,cb.sameYAgree,cb.sameYMismatch,cb.flippedYAgree,cb.flippedYMismatch,cb.xPairs,cb.xAgree,cb.xMismatch,cb.yPairs,cb.ySameAgree,cb.yFlippedAgree,cb.noise);
    }
    for(unsigned i=0;i<d.completedCount;++i){const auto& s=d.completed[i];const auto palette=list(s.palette),histogram=list(s.histogram.rates);
        Log::get().noteReserved("vrs coverage sample: schema=1 copy_id=%llu eye=%u draw_id=%llu qpc_us=%llu rt=%ux%u viewport=%ux%u before_epoch=%llu after_epoch=%llu rate_epoch=%llu binding_epoch=%llu issued=%u ambiguous=%u context_verified=%u valid=%u srrv_mip=%u srrv_slice=%u map=%ux%u active_tiles=%ux%u palette=%s rate_histogram=%s coarse_tiles=%llu hq_tiles=%llu nonuniform=%u gaze_snapshot_available=%u gaze_sequence=%llu gaze_returned_valid=%u gaze_provider_valid=%u gaze_cached=%u gaze_ignored=%u gaze_age_us=%llu gaze_changed=%u map_changed=%u paired_change=%u mapping_units_verified=0 coordinates_logged=0",
            s.copyId,s.eye,s.drawId,s.qpcUs,s.rtW,s.rtH,s.rtW,s.rtH,s.beforeEpoch,s.afterEpoch,s.rateEpoch,s.bindingEpoch,s.issued?1u:0u,s.ambiguous?1u:0u,s.bindingsVerified?1u:0u,s.valid?1u:0u,s.mip,s.slice,s.mapW,s.mapH,s.tilesW,s.tilesH,palette.c_str(),histogram.c_str(),s.histogram.coarse,s.histogram.hq,s.histogram.nonuniform()?1u:0u,s.gazeAvailable?1u:0u,s.gaze.sequence,s.gaze.returnedValid,s.gazeFresh?1u:0u,s.gaze.cachedFallback,s.gaze.ignored,s.gazeAgeUs,s.gazeChanged?1u:0u,s.mapChanged?1u:0u,s.pairedChange?1u:0u);
    }
    d.completedCount=0;
    for(unsigned eye=0;eye<2;++eye)Log::get().note("vrs pipeline: schema=1 cumulative eye=%u started=%llu qualified_ready=%llu errors=%llu rejected=%llu ring_busy=%llu output_overflow=%llu max_per_second_per_eye=%u average_queries_per_second_per_eye=1 burst_limit=%u cooldown_ms=%u ring_capacity=3 scope=observed_draw_wrapper shader_eligibility=unknown",eye,d.pipelineStarted[eye],d.pipelineReady[eye],d.pipelineErrors[eye],d.pipelineRejected[eye],d.pipelineBusy[eye],d.pipelineOverflow,vrsWorkReferenceEnabled()?0u:1u,vrsWorkReferenceEnabled()?3u:1u,vrsWorkReferenceEnabled()?3000u:1000u);
    for(unsigned i=0;i<d.pipelineCompletedCount;++i){const auto& s=d.pipelineCompleted[i];const auto& a=s.signature;
        const bool valid=pipelineValid(s);
        Log::get().noteReserved("vrs pipeline sample: schema=1 eye=%u draw_id=%llu generation=%llu before_epoch=%llu after_epoch=%llu rate_epoch=%llu binding_epoch=%llu enabled=%u state_known=%u control=%s control_qualified=%u valid=%u query_ready=%u get_data_hr=0x%08x cancelled=%u timed_out=%u issued_wrapper=%u ambiguous=%u substituted=%u scope=%s one_hardware_draw_verified=0 draw_identity_complete=0 original_vs=%016llX original_ps=%016llX actual_vs_original=%u actual_ps_original=%u shader_state_sample=%s kind=%c count=%u instances=%u start=%u base=%d start_instance=%u topology=%u rt=%ux%u sample_count=%u sample_quality=%u raster_state_sample=%s raster_known=%u fill=%u cull=%u front_ccw=%u depth_clip=%u scissor_enabled=%u scissor_count=%u scissor_rect0=%d,%d,%d,%d scissor_hash=%016llX forced_samples_known=%u forced_samples=%u conservative_known=%u conservative=%u blend_known=%u blend_enabled_mask=%u write_masks=0x%08x alpha_to_coverage=%u sample_mask=0x%08x blend_factor=%.9g,%.9g,%.9g,%.9g shader_reflection_available=0 shader_depth_output=unknown shader_sample_frequency=unknown shader_eligibility=unknown PSInvocations=%llu IAVertices=%llu IAPrimitives=%llu VSInvocations=%llu GSInvocations=%llu GSPrimitives=%llu CInvocations=%llu CPrimitives=%llu",
            s.eye,s.drawId,s.generation,s.beforeEpoch,s.afterEpoch,s.rateEpoch,s.bindingEpoch,s.enabled?1u:0u,s.stateKnown?1u:0u,s.stateKnown?"known":"unqualified_control",valid&&s.stateKnown?1u:0u,valid?1u:0u,s.queryReady?1u:0u,unsigned(s.status),s.cancelled?1u:0u,s.timedOut?1u:0u,s.issued?1u:0u,s.ambiguous?1u:0u,s.substituted?1u:0u,s.nativePrimary?"primary_real_draw_call":"observed_draw_wrapper",a.vs,a.ps,s.actualVsOriginal?1u:0u,s.actualPsOriginal?1u:0u,s.nativePrimary?"before_native_primary":"before_draw_wrapper",a.kind,a.count,a.instances,a.start,a.base,a.startInstance,s.topology,s.rtW,s.rtH,s.samples,s.quality,s.nativePrimary?"before_native_primary":"before_draw_wrapper",s.rasterKnown?1u:0u,s.fillMode,s.cullMode,s.frontCounterClockwise?1u:0u,s.depthClip?1u:0u,s.scissorEnabled?1u:0u,s.scissorCount,int(s.scissor0.left),int(s.scissor0.top),int(s.scissor0.right),int(s.scissor0.bottom),s.scissorHash,s.forcedKnown?1u:0u,s.forcedSamples,s.conservativeKnown?1u:0u,s.conservative,s.blendKnown?1u:0u,s.blendEnabledMask,s.writeMasks,s.alphaToCoverage?1u:0u,s.sampleMask,double(s.blendFactor[0]),double(s.blendFactor[1]),double(s.blendFactor[2]),double(s.blendFactor[3]),s.stats.PSInvocations,s.stats.IAVertices,s.stats.IAPrimitives,s.stats.VSInvocations,s.stats.GSInvocations,s.stats.GSPrimitives,s.stats.CInvocations,s.stats.CPrimitives);
        if(s.work.present){const auto& w=s.work;const auto original=list(w.override.original.palette),requested=list(w.override.lastOriginal.palette),applied=list(w.executed.palette),restored=list(w.restored.palette);
            const bool matched=vrs_work_reference::workMatches(s.stats.IAVertices,s.stats.IAPrimitives,s.stats.PSInvocations);
            Log::get().noteReserved("vrs work reference sample: schema=1 eye=%u draw_id=%llu generation=%llu cycle_id=%llu selector=useful_288x4 arm=%s native_primary_call=1 mode1_proof=%u mode1_age_us=%llu natural_state_known=1 original_enabled=%u original_palette=%s requested_palette=%s applied_palette=%s restored_enabled=%u restored_palette=%s apply_status=%d restore_status=%d restore_required=%u restore_verified=%u nested_rate_calls=%u nested_binding_calls=%u unexpected_events=%u state_proven=%u matched_work=%u valid=%u expected_ia_vertices=1152 expected_ia_primitives=384 PSInvocations=%llu IAVertices=%llu IAPrimitives=%llu VSInvocations=%llu GSInvocations=%llu GSPrimitives=%llu CInvocations=%llu CPrimitives=%llu query_ready=%u cancelled=%u timed_out=%u get_data_hr=0x%08x diagnostic_only=1 performance_claim=0",
                s.eye,s.drawId,s.generation,w.cycle,vrs_work_reference::armName(w.override.arm),w.modeOne?1u:0u,w.modeAgeUs,w.override.original.enabled?1u:0u,original.c_str(),requested.c_str(),applied.c_str(),w.restored.enabled?1u:0u,restored.c_str(),w.applyStatus,w.restoreStatus,w.override.restoreRequired?1u:0u,w.restoreVerified?1u:0u,w.override.nestedRates,w.bindingCalls,w.unexpected+w.override.unexpected,w.stateProven?1u:0u,matched?1u:0u,valid?1u:0u,s.stats.PSInvocations,s.stats.IAVertices,s.stats.IAPrimitives,s.stats.VSInvocations,s.stats.GSInvocations,s.stats.GSPrimitives,s.stats.CInvocations,s.stats.CPrimitives,s.queryReady?1u:0u,s.cancelled?1u:0u,s.timedOut?1u:0u,unsigned(s.status));
        }
    }
    d.pipelineCompletedCount=0;
    for(unsigned eye=0;eye<2;++eye){const auto& c=d.referenceCounts[eye];
        Log::get().note("vrs work reference funnel: schema=1 cumulative eye=%u selector=useful_288x4 selector_hits=%llu sampled=%llu skipped_ring=%llu skipped_cadence_or_match=%llu skipped_unknown_state=%llu skipped_provider=%llu restore_failures=%llu expired_cycles=%llu unknown_eye=%llu burst_limit=3 cooldown_ms=3000 cycle_expiry_ms=1000",eye,c.hits,c.sampled,c.ring,c.cadence,c.unknown,c.provider,c.restoreFailure,c.expired,d.referenceUnknownEye);}
}
} // namespace
} // namespace edvr

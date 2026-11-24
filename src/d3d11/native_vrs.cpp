#include "native_vrs.h"
#include "native_vrs_policy.h"
#include "native_vrs_ps_measure.h"
#include "native_gaze.h"
#include "native_gaze_readiness.h"
#include "native_render_labels.h"
#include "nvapi_module_path.h"
#include "../common/guard.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <climits>
#include <cwchar>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>
#ifdef EDVR_NATIVE_VRS_TEST
#include <thread>
#endif

namespace edvr {
std::atomic<bool> g_nativeVrsLive{false};
std::atomic<bool> g_nativeVrsPsMeasureLive{false};
namespace {
using Microsoft::WRL::ComPtr;
using native_vrs::ViewportRates;
// Public NVAPI V1 ABI, NVIDIA/nvapi 51e5aae4, nvapi.h lines 17169..17338
// (MIT). IDs are the public nvapi_interface.h values. No detours/code hooks.
struct ViewportRate {uint8_t enabled,padding[3];uint32_t palette[16];};
struct RateDesc {uint32_t version,count;ViewportRate* rates;};
struct SrrvDesc {uint32_t version;DXGI_FORMAT format;uint32_t dimension,mip,slice,count;};
struct __declspec(uuid("E14BE7F6-8FF5-4F5E-B63A-AD016EB8FBE5")) NvRateView:ID3D11View {
    virtual HRESULT STDMETHODCALLTYPE GetDesc(SrrvDesc*)=0;
};
static_assert(sizeof(RateDesc)==16&&sizeof(ViewportRate)==68&&sizeof(SrrvDesc)==24,"NVAPI V1 x64 ABI");
static_assert(offsetof(RateDesc,rates)==8&&offsetof(ViewportRate,palette)==4&&offsetof(SrrvDesc,mip)==12,"NVAPI V1 field offsets");
using Query=void*(__cdecl*)(uint32_t);
using Initialize=int(__cdecl*)();
using Rates=int(__cdecl*)(IUnknown*,RateDesc*);
using Resource=int(__cdecl*)(IUnknown*,NvRateView*);
using Create=int(__cdecl*)(ID3D11Device*,ID3D11Resource*,SrrvDesc*,NvRateView**);
constexpr int kCallFault=INT_MIN;

struct Shared {
    std::mutex configMutex; // configuration only; never acquired by draw calls
    std::atomic<const std::atomic<bool>*> cohortPending{nullptr};
    std::atomic<NativeVrsCohortBarrier> cohortBarrier{nullptr};
    std::atomic<uint64_t> configVersion{0},invalidateVersion{0};
    std::atomic<bool> enabled{false},highlight{false};
    std::atomic<float> hq{.15f},mid{.45f};
    std::atomic<ID3D11DeviceContext*> owner{nullptr};
    std::atomic<DWORD> thread{0};
    std::atomic<bool> driverReady{false},refused{false},magicAbsent{false},gazeProviderQueried{false};
    std::atomic<bool> vendorKnown{false},fullKnown{false},coarseKnown{false};
    std::atomic<NativeVrsFailure> failure{NativeVrsFailure::Disabled};
    std::atomic<int32_t> lastDriver{0};
    std::atomic<uint64_t> ticks{0},polls{0},valid{0},invalid{0},stale{0};
    std::atomic<uint64_t> world{0},accepted{0},full{0},invalidations{0};
    std::atomic<uint64_t> expandedAccepted[2]{};
    std::atomic<uint64_t> retainedWitnessReactivations{0};
    std::atomic<uint64_t> updates[2]{},bytes{0},rates{0},resources{0},errors{0};
    std::atomic<uint64_t> contextRefusals{0},frameRefusals{0},targetRefusals{0};
};
Shared& shared(){static Shared* const value=new Shared;return *value;}
void drainBeforeRateChange(ID3D11DeviceContext* ctx){
    auto& a=shared();const auto* pending=a.cohortPending.load(std::memory_order_acquire);
    if(!pending || !pending->load(std::memory_order_acquire) || !ctx || ctx!=a.owner.load(std::memory_order_acquire) ||
       GetCurrentThreadId()!=a.thread.load(std::memory_order_acquire))return;
    if(const auto barrier=a.cohortBarrier.load(std::memory_order_acquire))barrier(ctx);
}
struct EyeMap {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<NvRateView> view;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    unsigned worldW=0,worldH=0,mapW=0,mapH=0;
    uint64_t frame=0,highlightFrame=0;
    NativeVrsProjection projection{};
    std::vector<uint8_t> cpu;
};
struct State {
    // Process lifetime, with no COM destruction from DllMain. All fields below
    // belong to the one claimed rendering thread. Other callers touch atomics.
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device> device;
    HMODULE stub=nullptr,implementation=nullptr;
    Rates rates=nullptr;Resource resource=nullptr;Create create=nullptr;
    bool attempted=false,failed=false,sampleUsable=false;
    uint64_t frame=0,config=0,invalidation=0,gazeEpoch=0,lastRecovery=UINT64_MAX;
    float hq=.15f,mid=.45f;
    NativeGazeSample gaze{};
    native_vrs::VendorStateCache cache;
    uintptr_t pendingWitness=0; // synchronous stack receipt identity, never dereferenced
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11RasterizerState> rasterState;
    ComPtr<ID3D11BlendState> materialBlend;
    bool materialBlendCompatible=true;
    bool depthTestsScene=true,rasterCompatible=true;
    EyeMap eyes[2];
};
State& state(){static State* const value=new State;return *value;}
struct MeasureRequest {
    std::atomic<bool> enabled{false};std::atomic<uint64_t> generation{0};
    std::atomic<NativeVrsPsSink> sink{nullptr};
};
MeasureRequest& measureRequest(){static MeasureRequest* const p=new MeasureRequest;return *p;}
struct MeasureState {
    native_vrs::ps_measure::Recorder recorder;
    uint64_t generation=0;
};
MeasureState& measureState(){static MeasureState* const p=new MeasureState;return *p;}

#ifdef EDVR_NATIVE_VRS_TEST
thread_local native_vrs::testing::DriverResult* publicationCapture=nullptr;
#endif
void publish(const State& s) noexcept {
    auto& a=shared();
    a.vendorKnown.store(s.cache.ratesKnown&&s.cache.bindingKnown,std::memory_order_relaxed);
    a.fullKnown.store(s.cache.fullRateKnown(),std::memory_order_relaxed);
    a.coarseKnown.store(s.cache.active(s.cache.binding),std::memory_order_relaxed);
#ifdef EDVR_NATIVE_VRS_TEST
    if(publicationCapture){
        auto& result=*publicationCapture;const size_t index=result.publicationCount++;
        if(index<64)result.publications[index]={a.vendorKnown.load(),a.fullKnown.load(),a.coarseKnown.load()};
    }
#endif
}
bool owner(ID3D11DeviceContext* context) noexcept {
    auto& a=shared();
    const auto decision=native_vrs::ownershipDecision(reinterpret_cast<uintptr_t>(a.owner.load(std::memory_order_acquire)),
        reinterpret_cast<uintptr_t>(context),a.thread.load(std::memory_order_acquire),GetCurrentThreadId());
    if(decision!=native_vrs::FrameDecision::New){
        ++a.contextRefusals;++a.invalidateVersion;return false;
    }
    return true;
}
void refuse(State& s,NativeVrsFailure failure,int status=0) noexcept {
    const bool first=!s.failed;s.failed=true;s.sampleUsable=false;shared().refused.store(true,std::memory_order_release);
    if(first)shared().failure.store(failure,std::memory_order_relaxed);
    if(status)shared().lastDriver.store(status,std::memory_order_relaxed);
}
void callResult(int result) noexcept {
    if(result){++shared().errors;shared().lastDriver.store(result,std::memory_order_relaxed);}
}
int setRates(State& s,ViewportRates mode) {
    const bool enabled=mode!=ViewportRates::Disabled;
    ViewportRate viewport{};viewport.enabled=enabled?1:0;
    const auto table=native_vrs::palette(mode);
    for(unsigned i=0;i<table.size();++i)viewport.palette[i]=table[i];
    RateDesc desc{0x10010,enabled?1u:0u,enabled?&viewport:nullptr};
    int result=kCallFault;++shared().rates;
    guarded("nativeVrs/setRates",[&]{result=s.rates(s.context.Get(),&desc);});
    callResult(result);return result;
}
int setResource(State& s,uintptr_t view) {
    int result=kCallFault;++shared().resources;
    guarded("nativeVrs/setResource",[&]{result=s.resource(s.context.Get(),reinterpret_cast<NvRateView*>(view));});
    callResult(result);return result;
}
bool fullRate(State& s) {
    s.pendingWitness=0;
    if(!s.rates||!s.resource)return false;
    // Cache changes publish at the acknowledged transition or invalidation;
    // this owner-only no-op cannot change the three vendor-state snapshots.
    if(s.cache.fullRateKnown())return true;
    // A failing driver is not hammered once per draw. Refusal never reports
    // full rate unless both reset calls were actually acknowledged.
    if(s.failed&&s.lastRecovery==s.frame){publish(s);return false;}
    const bool ok=native_vrs::applyFullRate(s.cache,
        [&](ViewportRates mode){return setRates(s,mode);},[&](uintptr_t view){return setResource(s,view);});
    if(!ok){s.lastRecovery=s.frame;refuse(s,NativeVrsFailure::DriverCall,shared().lastDriver.load(std::memory_order_relaxed));}
    publish(s);return ok;
}
void syncInvalidation(State& s) {
    const auto version=shared().invalidateVersion.load(std::memory_order_acquire);
    if(version!=s.invalidation){s.invalidation=version;s.cache.invalidate();fullRate(s);}
}
bool readPath(HMODULE module,wchar_t (&path)[MAX_PATH]) noexcept {
    const DWORD length=module?GetModuleFileNameW(module,path,MAX_PATH):0;return length&&length<MAX_PATH;
}
HMODULE addressOwner(void* address) noexcept {
    HMODULE module=nullptr;
    return address&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(address),&module)?module:nullptr;
}
bool pin(void* address,HMODULE expected) noexcept {
    HMODULE module=nullptr;
    return address&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(address),&module)&&module==expected;
}
bool discover(State& s) {
#ifdef EDVR_NATIVE_VRS_TEST
    // Test binaries cannot discover/load/call a real NVAPI driver. The CPU
    // adapter fixture below injects the only permitted driver callbacks.
    refuse(s,NativeVrsFailure::NvapiModule);return false;
#else
    if(s.attempted)return !s.failed&&s.rates&&s.resource&&s.create;
    s.attempted=true;
    wchar_t system[MAX_PATH]{},stubPath[MAX_PATH]{},ownerPath[MAX_PATH]{},initPath[MAX_PATH]{};
    const UINT n=GetSystemDirectoryW(system,MAX_PATH);
    if(!n||n>=MAX_PATH){refuse(s,NativeVrsFailure::NvapiPath);return false;}
    HMODULE stub=GetModuleHandleW(L"nvapi64.dll");
    if(!stub){
        wchar_t path[MAX_PATH]{};
        if(swprintf_s(path,L"%s\\nvapi64.dll",system)<0){refuse(s,NativeVrsFailure::NvapiPath);return false;}
        stub=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if(!stub){refuse(s,NativeVrsFailure::NvapiModule);return false;}
    // Validate the stub and QueryInterface's owner BEFORE calling it. An
    // app-local replacement or a forwarded export in another DLL is refused.
    const auto query=reinterpret_cast<Query>(GetProcAddress(stub,"nvapi_QueryInterface"));
    if(!readPath(stub,stubPath)||!nvapi::nvapiSystemStubPath(system,stubPath)){
        refuse(s,NativeVrsFailure::NvapiPath);return false;
    }
    if(!query||addressOwner(reinterpret_cast<void*>(query))!=stub){refuse(s,NativeVrsFailure::NvapiQuery);return false;}
    if(!pin(reinterpret_cast<void*>(query),stub)){refuse(s,NativeVrsFailure::NvapiPin);return false;}
    void* rates=nullptr;void* resource=nullptr;void* create=nullptr;void* init=nullptr;
    if(!guarded("nativeVrs/query",[&]{
        init=query(0x0150e828);create=query(0x99ca2dff);rates=query(0x34f7938f);resource=query(0x1b0c2f83);
    })){refuse(s,NativeVrsFailure::NvapiQuery);return false;}
    const HMODULE implementation=addressOwner(rates),initOwner=addressOwner(init);
    const bool same=implementation&&implementation==addressOwner(resource)&&implementation==addressOwner(create);
    const bool known=same&&readPath(implementation,ownerPath)&&
        nvapi::nvapiOwnerPathKind(system,stubPath,ownerPath,ownerPath)!=0;
    const bool initKnown=initOwner&&readPath(initOwner,initPath)&&
        nvapi::nvapiOwnerPathKind(system,stubPath,initPath,initPath)!=0&&
        (initOwner==stub||initOwner==implementation);
    if(!known||!initKnown){refuse(s,NativeVrsFailure::NvapiOwner);return false;}
    if(!pin(rates,implementation)||!pin(init,initOwner)){refuse(s,NativeVrsFailure::NvapiPin);return false;}
    int initialized=kCallFault;
    guarded("nativeVrs/initialize",[&]{initialized=reinterpret_cast<Initialize>(init)();});
    callResult(initialized);
    if(initialized){refuse(s,NativeVrsFailure::NvapiInitialize,initialized);return false;}
    s.stub=stub;s.implementation=implementation;
    s.rates=reinterpret_cast<Rates>(rates);s.resource=reinterpret_cast<Resource>(resource);s.create=reinterpret_cast<Create>(create);
    shared().driverReady.store(true,std::memory_order_release);
    g_nativeVrsLive.store(true,std::memory_order_release);
    s.cache.invalidate();return fullRate(s);
#endif
}
bool activateMap(State& s,uintptr_t view) {
    s.pendingWitness=0;
    if(s.failed){fullRate(s);return false;}
    const bool ok=native_vrs::applyCoarse(s.cache,view,[&](ViewportRates mode){return setRates(s,mode);},[&](uintptr_t v){return setResource(s,v);});
    if(!ok){refuse(s,NativeVrsFailure::DriverCall,shared().lastDriver.load(std::memory_order_relaxed));fullRate(s);return false;}
    publish(s);return true;
}
bool prepareWitnessRate(State& s,uintptr_t view) {
    if(!s.cache.active(view))return fullRate(s);
    if(!native_vrs::pauseForWitness(s.cache,view,[&](ViewportRates mode){return setRates(s,mode);})){
        refuse(s,NativeVrsFailure::DriverCall,shared().lastDriver.load(std::memory_order_relaxed));
        fullRate(s);return false;
    }
    publish(s);return true;
}
bool targetDimensions(State& s,EyeMap& map,ID3D11RenderTargetView* rtv,ID3D11DepthStencilView* dsv,
                      unsigned& width,unsigned& height) {
    if(!rtv||!dsv)return false;
    if(map.target.Get()==rtv&&map.depth.Get()==dsv){width=map.worldW;height=map.worldH;return width&&height;}
    D3D11_RENDER_TARGET_VIEW_DESC rd{};D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    rtv->GetDesc(&rd);dsv->GetDesc(&dd);
    if(rd.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2D||dd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D)return false;
    ComPtr<ID3D11Resource> rr,dr;rtv->GetResource(&rr);dsv->GetResource(&dr);
    ComPtr<ID3D11Texture2D> rt,dt;
    if(!rr||!dr||FAILED(rr.As(&rt))||FAILED(dr.As(&dt)))return false;
    D3D11_TEXTURE2D_DESC r{},d{};rt->GetDesc(&r);dt->GetDesc(&d);
    if(r.SampleDesc.Count!=1||d.SampleDesc.Count!=1||rd.Texture2D.MipSlice>=r.MipLevels||dd.Texture2D.MipSlice>=d.MipLevels)return false;
    width=r.Width>>rd.Texture2D.MipSlice;height=r.Height>>rd.Texture2D.MipSlice;
    if(!width||!height||width!=(d.Width>>dd.Texture2D.MipSlice)||height!=(d.Height>>dd.Texture2D.MipSlice))return false;
    ComPtr<ID3D11Device> rdev,ddev;rt->GetDevice(&rdev);dt->GetDevice(&ddev);
    if(rdev.Get()!=s.device.Get()||ddev.Get()!=s.device.Get())return false;
    return native_vrs::mapShape(width,height).bytes!=0;
}
bool materialRasterCompatible(State& s) {
    ComPtr<ID3D11DepthStencilState> depthState;
    s.context->OMGetDepthStencilState(&depthState,nullptr);
    if(depthState.Get()!=s.depthState.Get()){
        s.depthTestsScene=true;
        if(depthState){
            D3D11_DEPTH_STENCIL_DESC depthDesc{};
            depthState->GetDesc(&depthDesc);
            s.depthTestsScene=depthDesc.DepthEnable&&depthDesc.DepthFunc!=D3D11_COMPARISON_ALWAYS&&
                depthDesc.DepthFunc!=D3D11_COMPARISON_NEVER;
        }
        s.depthState=std::move(depthState);
    }
    if(!s.depthTestsScene)return false;
    ComPtr<ID3D11BlendState> blend;
    s.context->OMGetBlendState(&blend,nullptr,nullptr);
    if(blend.Get()!=s.materialBlend.Get()){
        s.materialBlendCompatible=true;
        if(blend){
            D3D11_BLEND_DESC desc{};
            blend->GetDesc(&desc);
            s.materialBlendCompatible=!desc.AlphaToCoverageEnable && (desc.RenderTarget[0].RenderTargetWriteMask & 7)!=0;
        }
        s.materialBlend=std::move(blend);
    }
    if(!s.materialBlendCompatible)return false;
    ComPtr<ID3D11RasterizerState> rasterizer;
    s.context->RSGetState(&rasterizer);
    if(rasterizer.Get()!=s.rasterState.Get()){
        s.rasterCompatible=true;
        if(rasterizer){
            ComPtr<ID3D11RasterizerState2> rasterizer2;
            ComPtr<ID3D11RasterizerState1> rasterizer1;
            if(SUCCEEDED(rasterizer.As(&rasterizer2))){
                D3D11_RASTERIZER_DESC2 rasterizerDesc{};
                rasterizer2->GetDesc2(&rasterizerDesc);
                s.rasterCompatible=!rasterizerDesc.ForcedSampleCount&&
                    rasterizerDesc.ConservativeRaster==D3D11_CONSERVATIVE_RASTERIZATION_MODE_OFF;
            }else if(SUCCEEDED(rasterizer.As(&rasterizer1))){
                D3D11_RASTERIZER_DESC1 rasterizerDesc{};
                rasterizer1->GetDesc1(&rasterizerDesc);
                s.rasterCompatible=!rasterizerDesc.ForcedSampleCount;
            }
        }
        s.rasterState=std::move(rasterizer);
    }
    return s.rasterCompatible;
}
bool sceneTargetsBound(State& s,ID3D11RenderTargetView* rtv,ID3D11DepthStencilView* dsv) {
    ComPtr<ID3D11RenderTargetView> actualRtv;ComPtr<ID3D11DepthStencilView> actualDsv;
    s.context->OMGetRenderTargets(1,&actualRtv,&actualDsv);
    return rtv && dsv && actualRtv.Get()==rtv && actualDsv.Get()==dsv;
}
bool sameProjection(const NativeVrsProjection& a,const NativeVrsProjection& b) {
    if(a.sequence!=b.sequence)return false;
    for(unsigned i=0;i<4;++i)if(a.frustum[i]!=b.frustum[i])return false;
    for(unsigned i=0;i<2;++i)if(a.shift[i]!=b.shift[i])return false;
    return true;
}
NativeVrsWitnessStamp witnessStamp(const State& s,unsigned eye) {
    return {eye,reinterpret_cast<uintptr_t>(s.eyes[eye].view.Get()),s.frame,s.config,s.gazeEpoch,s.invalidation};
}
bool prepareMap(State& s,unsigned eye,ID3D11RenderTargetView* rtv,ID3D11DepthStencilView* dsv,const NativeVrsProjection& projection) {
    auto& map=s.eyes[eye];unsigned width=0,height=0;
    if(!sceneTargetsBound(s,rtv,dsv))return false;
    if(!materialRasterCompatible(s))return false;
    if(!targetDimensions(s,map,rtv,dsv,width,height))return false;
    // A target resize within a frame never causes a second eye upload or use
    // of a map normalized for the earlier dimensions.
    UINT count=1;D3D11_VIEWPORT vp{};s.context->RSGetViewports(&count,&vp);
    if(!native_vrs::fullViewport(count,vp.TopLeftX,vp.TopLeftY,vp.Width,vp.Height,width,height))return false;
    if(map.frame==s.frame){
        if(!sameProjection(map.projection,projection)||map.worldW!=width||map.worldH!=height||map.depth.Get()!=dsv||!map.view)return false;
        map.target=rtv;
        return true;
    }
    const auto shape=native_vrs::mapShape(width,height);
    if(!map.texture||map.mapW!=shape.width||map.mapH!=shape.height){
        // Unbind before replacing a resource whose vendor view may be current.
        if(!fullRate(s))return false;
        ComPtr<ID3D11Texture2D> texture;ComPtr<NvRateView> view;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=shape.width;desc.Height=shape.height;desc.MipLevels=desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8_UINT;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(s.device->CreateTexture2D(&desc,nullptr,&texture))){refuse(s,NativeVrsFailure::MapCreate);return false;}
        SrrvDesc vd{0x10018,DXGI_FORMAT_R8_UINT,4,0,0,0};int result=kCallFault;
        guarded("nativeVrs/createView",[&]{result=s.create(s.device.Get(),texture.Get(),&vd,&view);});
        callResult(result);
        if(result||!view){refuse(s,NativeVrsFailure::MapCreate,result);return false;}
        map.texture=std::move(texture);map.view=std::move(view);map.mapW=shape.width;map.mapH=shape.height;
    }
    map.cpu.resize(shape.bytes);
    const auto& point=s.gaze.eyes[eye];
    float x=0,y=0;
    if(!projection.sequence||!point.projectionValid||!native_vrs::projectGaze(point.xNdc,point.yNdc,
        point.projection,projection.frustum,projection.shift,x,y))return false;
    if(!native_vrs::buildMap(map.cpu.data(),map.cpu.size(),width,height,x,y,
                           s.sampleUsable&&point.valid,s.hq,s.mid))return false;
    // No shader, UAV, CS constant, resource slot or other pipeline state is
    // touched. D3D11 performs the ordered upload to this private default map.
    s.context->UpdateSubresource(map.texture.Get(),0,nullptr,map.cpu.data(),shape.width,0);
    ++shared().updates[eye];shared().bytes.fetch_add(shape.bytes,std::memory_order_relaxed);
    map.worldW=width;map.worldH=height;map.target=rtv;map.depth=dsv;map.frame=s.frame;map.projection=projection;
    return true;
}
bool receiptFresh(const State& s) noexcept {
    LARGE_INTEGER now{};
    return QueryPerformanceCounter(&now)&&now.QuadPart>0&&native_vrs::freshReceipt(uint64_t(now.QuadPart),s.gaze.receiptQpc,s.gaze.receiptQpcFrequency);
}
} // namespace

bool nativeVrsCoarseRequested() noexcept {
    return shared().enabled.load(std::memory_order_acquire);
}
void nativeVrsInstallCohortBarrier(const std::atomic<bool>* pending,NativeVrsCohortBarrier barrier) noexcept {
    shared().cohortBarrier.store(barrier,std::memory_order_release);
    shared().cohortPending.store(pending,std::memory_order_release);
}
void nativeVrsConfigure(bool enabled,float hqRadius,float midRadius) noexcept {
    try {
        auto& a=shared();std::lock_guard<std::mutex> lock(a.configMutex);
        ++a.configVersion; // odd means writer in progress
        const bool valid=native_vrs::validRadii(hqRadius,midRadius);
        a.hq.store(hqRadius,std::memory_order_relaxed);a.mid.store(midRadius,std::memory_order_relaxed);
        a.enabled.store(enabled&&valid,std::memory_order_release);
        g_nativeVrsLive.store((enabled&&valid)||a.driverReady.load(std::memory_order_acquire),std::memory_order_release);
        if(!a.refused.load(std::memory_order_acquire))
            a.failure.store(!valid?NativeVrsFailure::InvalidRadii:enabled?NativeVrsFailure::None:NativeVrsFailure::Disabled,std::memory_order_relaxed);
        ++a.configVersion;
    }catch(...){shared().enabled.store(false,std::memory_order_release);}
}
void nativeVrsConfigureFocus(bool enabled,const char* profile) noexcept {
    const auto radii=native_vrs::focusRadii(profile);
    nativeVrsConfigure(enabled,radii.sharp,radii.transition);
}
void nativeVrsTick(ID3D11DeviceContext* context,uint64_t frameOrdinal,bool allowProducer) noexcept {
    drainBeforeRateChange(context);
    try {
        auto& a=shared();
        if(!allowProducer||!a.enabled.load(std::memory_order_acquire)){
            if(!a.owner.load(std::memory_order_acquire))return;
            if(native_vrs::ownershipDecision(reinterpret_cast<uintptr_t>(a.owner.load(std::memory_order_acquire)),
                reinterpret_cast<uintptr_t>(context),a.thread.load(std::memory_order_acquire),GetCurrentThreadId())!=native_vrs::FrameDecision::New){++a.contextRefusals;return;}
            auto& s=state();
            // Recovery cadence remains the owning Present ordinal after OFF.
            // Duplicate/zero/regressed/foreign ticks neither retry nor poll gaze.
            if(native_vrs::ordinalDecision(s.frame,frameOrdinal)!=native_vrs::FrameDecision::New)return;
            s.frame=frameOrdinal;s.sampleUsable=false;s.gaze={};s.gazeEpoch=0;syncInvalidation(s);fullRate(s);return;
        }
        if(!context||!frameOrdinal){
            ++a.frameRefusals;
            if(context&&a.owner.load(std::memory_order_acquire)&&owner(context)){
                auto& s=state();s.sampleUsable=false;fullRate(s);
            }
            return;
        }
        if(!a.owner.load(std::memory_order_acquire)){
            if(context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE){++a.contextRefusals;return;}
            ID3D11DeviceContext* empty=nullptr;
            if(a.owner.compare_exchange_strong(empty,context,std::memory_order_acq_rel)){
                auto& s=state();s.context=context;context->GetDevice(&s.device);
                a.thread.store(GetCurrentThreadId(),std::memory_order_release);
            }
        }
        if(!owner(context))return;
        auto& s=state();
        const auto ordinal=native_vrs::ordinalDecision(s.frame,frameOrdinal);
        if(ordinal==native_vrs::FrameDecision::Duplicate)return;
        if(ordinal!=native_vrs::FrameDecision::New){++a.frameRefusals;s.sampleUsable=false;fullRate(s);return;}
        const uint64_t config=a.configVersion.load(std::memory_order_acquire);
        if(config&1){s.sampleUsable=false;fullRate(s);return;}
        s.hq=a.hq.load(std::memory_order_relaxed);s.mid=a.mid.load(std::memory_order_relaxed);
        if(config!=a.configVersion.load(std::memory_order_acquire)){s.sampleUsable=false;fullRate(s);return;}
        s.config=config;s.frame=frameOrdinal;s.sampleUsable=false;++a.ticks;
        syncInvalidation(s);
        const auto readiness=nativeTemporalGazeReadiness(s.device.Get(),GetCurrentThreadId());
        NativeRenderLabels labels{};
        // Capture the readiness epoch before the runtime name. The callback
        // rechecks this epoch under the same mutex that temporal.close uses
        // before XR teardown, so a new instance cannot inherit the old label.
        if(!readiness.ready || !nativeRenderLabels(&labels) ||
            std::strcmp(labels.runtimeName,kNativeGazeOpenXrRuntime)!=0){
            s.gaze={};s.gazeEpoch=0;
            if(!a.refused.load(std::memory_order_relaxed))a.failure.store(NativeVrsFailure::GazeNotReady,std::memory_order_relaxed);
            fullRate(s);return;
        }
        const bool magicAbsent=GetModuleHandleW(L"LibMagicD3D1164.dll")==nullptr;
        a.magicAbsent.store(magicAbsent,std::memory_order_relaxed);
        if(!magicAbsent){refuse(s,NativeVrsFailure::MagicPresent);fullRate(s);return;}
        if(s.failed||!discover(s)){fullRate(s);return;}
        if(!fullRate(s))return;
        struct Poll {NativeGazeSample sample{};bool completed=false;} poll;
        const auto callback=[](void* opaque,uint64_t epoch){
            auto& value=*static_cast<Poll*>(opaque);
            // The temporal channel stays leased through the synchronous call;
            // only the sample, never an SDK pointer, escapes the callback.
            value.completed=guarded("nativeVrs/gaze",[&]{value.sample=nativeGazePoll(epoch);});
        };
        const bool admitted=nativeTemporalWithGazeReady(s.device.Get(),GetCurrentThreadId(),readiness.epoch,callback,&poll);
        if(admitted&&!poll.completed)refuse(s,NativeVrsFailure::GazeInvalid,kCallFault);
        if(!admitted||!poll.completed||nativeTemporalGazeEpoch()!=readiness.epoch){
            s.gaze={};s.gazeEpoch=0;
            if(!a.refused.load(std::memory_order_relaxed))a.failure.store(NativeVrsFailure::GazeNotReady,std::memory_order_relaxed);
            fullRate(s);return;
        }
        ++a.polls;s.gaze=poll.sample;s.gazeEpoch=readiness.epoch;
        if(s.gaze.providerQueried)a.gazeProviderQueried.store(true,std::memory_order_relaxed);
        s.sampleUsable=s.gaze.usable&&s.gaze.eyes[0].valid&&s.gaze.eyes[1].valid&&
            native_vrs::validPoint(s.gaze.eyes[0].xNdc,s.gaze.eyes[0].yNdc)&&native_vrs::validPoint(s.gaze.eyes[1].xNdc,s.gaze.eyes[1].yNdc);
        if(s.sampleUsable){++a.valid;a.failure.store(NativeVrsFailure::None,std::memory_order_relaxed);}
        else {++a.invalid;a.failure.store(NativeVrsFailure::GazeInvalid,std::memory_order_relaxed);}
    }catch(...){auto& s=state();refuse(s,NativeVrsFailure::DriverCall,kCallFault);fullRate(s);}
}
bool nativeVrsWitnessReady(ID3D11DeviceContext* context) noexcept {
    auto& a=shared();
    // This read-only probe must not invalidate the owner's state on a foreign thread.
    if(!a.driverReady.load(std::memory_order_acquire)||!a.enabled.load(std::memory_order_acquire)||
       !context||context!=a.owner.load(std::memory_order_acquire)||GetCurrentThreadId()!=a.thread.load(std::memory_order_acquire))return false;
    const auto& s=state();
    return !s.failed && s.sampleUsable && s.config==a.configVersion.load(std::memory_order_acquire) &&
        s.gazeEpoch && s.gazeEpoch==nativeTemporalGazeEpoch() && receiptFresh(s);
}
bool nativeVrsPrepareWitness(ID3D11DeviceContext* context,unsigned eye,ID3D11RenderTargetView* rtv,ID3D11DepthStencilView* dsv,const NativeVrsProjection& projection,NativeVrsWitnessReceipt& prepared) noexcept {
    drainBeforeRateChange(context);
    prepared.stamp={};
    if(eye>=2||!nativeVrsWitnessReady(context))return false;
    auto& s=state();
    try {
        s.pendingWitness=0;
        syncInvalidation(s);
        if(s.failed || !prepareMap(s,eye,rtv,dsv,projection))return false;
        const auto view=reinterpret_cast<uintptr_t>(s.eyes[eye].view.Get());
        if(!prepareWitnessRate(s,view))return false;
        if(nativeVrsWitnessReady(context) && s.invalidation==shared().invalidateVersion.load(std::memory_order_acquire)){
            prepared.stamp=witnessStamp(s,eye);
            s.pendingWitness=reinterpret_cast<uintptr_t>(&prepared);
            return true;
        }
        fullRate(s);return false;
    }catch(...){refuse(s,NativeVrsFailure::MapCreate,kCallFault);fullRate(s);return false;}
}
bool nativeVrsBeforeWorld(ID3D11DeviceContext* context,unsigned eye,ID3D11RenderTargetView* rtv,ID3D11DepthStencilView* dsv,const NativeVrsProjection* projection,bool expandedMaterial,NativeVrsWitnessReceipt* prepared) noexcept {
    drainBeforeRateChange(context);
    try {
        auto& a=shared();
        const auto stamp=prepared?prepared->take():NativeVrsWitnessStamp{};
        if(!a.enabled.load(std::memory_order_acquire)&&!a.driverReady.load(std::memory_order_acquire))return false;
        ++a.world;if(!owner(context))return false;
        auto& s=state();syncInvalidation(s);
        const auto pending=std::exchange(s.pendingWitness,uintptr_t(0));
        if(!a.enabled.load(std::memory_order_acquire)||s.config!=a.configVersion.load(std::memory_order_acquire)||s.failed||!s.sampleUsable||eye>1||
           !s.gazeEpoch||s.gazeEpoch!=nativeTemporalGazeEpoch()){fullRate(s);return false;}
        if(!receiptFresh(s)){++a.stale;a.failure.store(NativeVrsFailure::GazeStale,std::memory_order_relaxed);fullRate(s);return false;}
        auto& map=s.eyes[eye];
        bool targetReady=false;
        if(projection && prepared){
            // The synchronous witness changed only PS and OM targets. The
            // actual D3D attachment setter is void, so verify its result.
            targetReady=pending==reinterpret_cast<uintptr_t>(prepared) && stamp.matches(witnessStamp(s,eye)) &&
                map.frame==s.frame && map.target.Get()==rtv && map.depth.Get()==dsv &&
                sameProjection(map.projection,*projection) && sceneTargetsBound(s,rtv,dsv);
        }else if(projection)targetReady=prepareMap(s,eye,rtv,dsv,*projection);
        if(!targetReady){
            ++a.targetRefusals;if(!s.failed)a.failure.store(NativeVrsFailure::TargetRefused,std::memory_order_relaxed);
            fullRate(s);return false;
        }
        auto current=[&]{
            if(!a.enabled.load(std::memory_order_acquire)||s.config!=a.configVersion.load(std::memory_order_acquire)||
               s.invalidation!=a.invalidateVersion.load(std::memory_order_acquire)||s.gazeEpoch!=nativeTemporalGazeEpoch())return false;
            if(receiptFresh(s))return true;
            ++a.stale;a.failure.store(NativeVrsFailure::GazeStale,std::memory_order_relaxed);return false;
        };
        const auto view=reinterpret_cast<uintptr_t>(map.view.Get());
        const bool switched=prepared && s.cache.retainedForWitness(view);
        if(!native_vrs::activateWhileCurrent(current,[&]{return activateMap(s,view);},[&]{syncInvalidation(s);fullRate(s);}))return false;
        if(switched)++a.retainedWitnessReactivations;
        if(a.highlight.load(std::memory_order_relaxed))s.eyes[eye].highlightFrame=s.frame;
        if(expandedMaterial)++a.expandedAccepted[eye];
        ++a.accepted;return true;
    }catch(...){auto& s=state();refuse(s,NativeVrsFailure::MapCreate,kCallFault);fullRate(s);return false;}
}
void nativeVrsConfigureHighlight(bool enabled) noexcept {
    shared().highlight.store(enabled,std::memory_order_relaxed);
}
bool nativeVrsHighlightRequested() noexcept {
    return shared().highlight.load(std::memory_order_relaxed);
}
bool nativeVrsHighlightMap(ID3D11Device* device,uint64_t sequence,unsigned eye,
    unsigned width,unsigned height,ID3D11DeviceContext*& context,ID3D11Texture2D*& texture) noexcept {
    context=nullptr;texture=nullptr;
    try {
        auto& a=shared();
        if(!nativeVrsHighlightRequested()||!a.enabled.load(std::memory_order_acquire)||
           !device||!sequence||eye>1||a.thread.load(std::memory_order_acquire)!=GetCurrentThreadId())return false;
        auto& s=state();const auto& map=s.eyes[eye];
        if(device!=s.device.Get()||s.failed||s.config!=a.configVersion.load(std::memory_order_acquire)||
           !s.gazeEpoch||s.gazeEpoch!=nativeTemporalGazeEpoch()||map.frame!=s.frame||
           map.highlightFrame!=s.frame||map.projection.sequence!=sequence||
           map.worldW!=width||map.worldH!=height||!map.texture)return false;
        syncInvalidation(s);
        if(s.failed||!fullRate(s))return false;
        context=s.context.Get();texture=map.texture.Get();return true;
    }catch(...){return false;}
}
void nativeVrsFullRate(ID3D11DeviceContext* context) noexcept {
    drainBeforeRateChange(context);
    try {
        if(!shared().driverReady.load(std::memory_order_acquire))return;
        ++shared().full;if(!owner(context))return;
        auto& s=state();syncInvalidation(s);fullRate(s);
    }catch(...){auto& s=state();refuse(s,NativeVrsFailure::DriverCall,kCallFault);s.cache.invalidate();publish(s);}
}
void nativeVrsInvalidate(ID3D11DeviceContext* context) noexcept {
    drainBeforeRateChange(context);
    ++shared().invalidations;++shared().invalidateVersion;
    if(!shared().driverReady.load(std::memory_order_acquire))return;
    nativeVrsFullRate(context);
}
bool nativeVrsSetCohortRate(ID3D11DeviceContext* ctx,unsigned eye,ID3D11RenderTargetView* rtv,ID3D11DepthStencilView* dsv,
    const NativeVrsProjection& projection,bool expanded,NativeVrsCohortReceipt& receipt,NativeVrsCohortStage stage) noexcept {
    if(!ctx || eye>=2)return false;
    auto& s=state();auto& a=shared();
    switch(stage){
    case NativeVrsCohortStage::PrepareFine:
        receipt.active={};
        return nativeVrsPrepareWitness(ctx,eye,rtv,dsv,projection,receipt.first);
    case NativeVrsCohortStage::FirstMaterial:{
        const auto stamp=receipt.first.stamp;
        if(!nativeVrsBeforeWorld(ctx,eye,rtv,dsv,&projection,expanded,&receipt.first))return false;
        receipt.active=stamp;
        return true;
    }
    case NativeVrsCohortStage::NextMaterial:
        ++a.world;
        if(nativeVrsWitnessReady(ctx) && receipt.active.matches(witnessStamp(s,eye)) &&
           s.invalidation==a.invalidateVersion.load(std::memory_order_acquire) &&
           s.eyes[eye].target.Get()==rtv && s.eyes[eye].depth.Get()==dsv &&
           sameProjection(s.eyes[eye].projection,projection) && s.cache.active(receipt.active.map)){
            if(expanded)++a.expandedAccepted[eye];
            ++a.accepted;return true;
        }
        [[fallthrough]];
    case NativeVrsCohortStage::StrongFull:
        receipt.active={};receipt.first.take();
        nativeVrsFullRate(ctx);
        return stage==NativeVrsCohortStage::StrongFull && a.owner.load(std::memory_order_acquire)==ctx &&
            a.thread.load(std::memory_order_acquire)==GetCurrentThreadId() && s.cache.fullRateKnown();
    }
    return false;
}
NativeVrsStatus nativeVrsStatus() noexcept {
    auto& a=shared();NativeVrsStatus out{};
    out.configured=a.enabled.load(std::memory_order_relaxed);out.driverReady=a.driverReady.load(std::memory_order_relaxed);
    out.gazeProviderQueried=a.gazeProviderQueried.load(std::memory_order_relaxed);
    out.refused=a.refused.load(std::memory_order_relaxed);out.magicAbsent=a.magicAbsent.load(std::memory_order_relaxed);
    out.vendorStateKnown=a.vendorKnown.load(std::memory_order_relaxed);out.fullRateAcknowledged=a.fullKnown.load(std::memory_order_relaxed);
    out.coarseAcknowledged=a.coarseKnown.load(std::memory_order_relaxed);out.failure=a.failure.load(std::memory_order_relaxed);
    out.lastDriverStatus=a.lastDriver.load(std::memory_order_relaxed);
    out.sharpRadius=a.hq.load(std::memory_order_relaxed);out.transitionRadius=a.mid.load(std::memory_order_relaxed);
    out.ticks=a.ticks.load();out.providerPolls=a.polls.load();out.validPairs=a.valid.load();out.invalidPairs=a.invalid.load();out.staleRefusals=a.stale.load();
    out.worldCalls=a.world.load();out.worldAccepted=a.accepted.load();out.fullRateCalls=a.full.load();out.invalidations=a.invalidations.load();
    for(unsigned eye=0;eye<2;++eye)out.expandedMaterialAccepted[eye]=a.expandedAccepted[eye].load();
    for(unsigned i=0;i<2;++i)out.mapUpdates[i]=a.updates[i].load();
    out.mapBytes=a.bytes.load();out.rateSets=a.rates.load();out.resourceSets=a.resources.load();out.driverErrors=a.errors.load();
    out.retainedWitnessReactivations=a.retainedWitnessReactivations.load();
    out.contextRefusals=a.contextRefusals.load();out.frameRefusals=a.frameRefusals.load();out.targetRefusals=a.targetRefusals.load();return out;
}
void nativeVrsPsMeasureConfigure(bool enabled,NativeVrsPsSink sink)noexcept {
    auto& r=measureRequest();r.sink.store(sink,std::memory_order_release);
    if(r.enabled.exchange(enabled,std::memory_order_acq_rel)!=enabled)++r.generation;
    // OFF still permits already-ended query objects to drain on their owner.
    if(enabled)g_nativeVrsPsMeasureLive.store(true,std::memory_order_release);
}
namespace {
bool measureOwner(ID3D11DeviceContext* context)noexcept {
    auto& a=shared();return context&&context==a.owner.load(std::memory_order_acquire)&&
        GetCurrentThreadId()==a.thread.load(std::memory_order_acquire);
}
native_vrs::ps_measure::Receipt measureReceipt(State& s,unsigned eye,bool full) {
    native_vrs::ps_measure::Receipt r{};if(eye>=2)return r;
    auto& a=shared();r.frame=s.frame;r.sequence=s.eyes[eye].projection.sequence;r.eye=eye;
    r.config=s.config;r.gazeEpoch=s.gazeEpoch;r.invalidation=s.invalidation;r.map=reinterpret_cast<uintptr_t>(s.eyes[eye].view.Get());
    r.qualified=a.enabled.load(std::memory_order_acquire)&&s.config==a.configVersion.load(std::memory_order_acquire)&&
        s.invalidation==a.invalidateVersion.load(std::memory_order_acquire)&&!s.failed&&s.sampleUsable&&receiptFresh(s)&&
        s.gazeEpoch&&s.gazeEpoch==nativeTemporalGazeEpoch()&&s.eyes[eye].frame==s.frame&&
        (full?s.cache.fullRateKnown():s.cache.active(uintptr_t(r.map)));
    return r;
}
bool measureKey(ID3D11DeviceContext* context,const NativeVrsPsDraw& draw,native_vrs::ps_measure::Key& key) {
    ComPtr<ID3D11RenderTargetView> target;ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(1,&target,&depth);
    if(target.Get()!=draw.target||depth.Get()!=draw.depth||!target||!depth)return false;
    ComPtr<ID3D11Resource> resource;target->GetResource(&resource);ComPtr<ID3D11Texture2D> texture;
    if(FAILED(resource.As(&texture)))return false;D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    if(desc.SampleDesc.Count!=1)return false;
    D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);
    if(topology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)return false;
    UINT views=1;D3D11_VIEWPORT viewport{};context->RSGetViewports(&views,&viewport);
    if(!native_vrs::fullViewport(views,viewport.TopLeftX,viewport.TopLeftY,viewport.Width,viewport.Height,desc.Width,desc.Height))return false;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;context->VSGetShader(&vs,nullptr,nullptr);context->PSGetShader(&ps,nullptr,nullptr);
    ComPtr<ID3D11RasterizerState> raster;context->RSGetState(&raster);
    ComPtr<ID3D11BlendState> blend;FLOAT factors[4]{};UINT mask=0;context->OMGetBlendState(&blend,factors,&mask);
    ComPtr<ID3D11DepthStencilState> ds;UINT stencil=0;context->OMGetDepthStencilState(&ds,&stencil);
    ComPtr<ID3D11Buffer> indices;DXGI_FORMAT format{};UINT offset=0;context->IAGetIndexBuffer(&indices,&format,&offset);
    uint64_t hash=14695981039346656037ull;
    const auto mix=[&](uint64_t value){hash=(hash^value)*1099511628211ull;};
    for(const auto pointer:{static_cast<IUnknown*>(target.Get()),static_cast<IUnknown*>(depth.Get()),static_cast<IUnknown*>(vs.Get()),
        static_cast<IUnknown*>(ps.Get()),static_cast<IUnknown*>(raster.Get()),static_cast<IUnknown*>(blend.Get()),static_cast<IUnknown*>(ds.Get()),static_cast<IUnknown*>(indices.Get())})mix(reinterpret_cast<uintptr_t>(pointer));
    mix(mask);mix(stencil);mix(unsigned(format));mix(offset);
    uint32_t floats[4]{};std::memcpy(floats,factors,sizeof(floats));for(auto value:floats)mix(value);
    UINT rectCount=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetScissorRects(&rectCount,rects);if(rectCount>D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)return false;
    mix(rectCount);for(unsigned i=0;i<rectCount;++i){mix(uint32_t(rects[i].left));mix(uint32_t(rects[i].right));mix(uint32_t(rects[i].top));mix(uint32_t(rects[i].bottom));}
    key={draw.vs,draw.ps,hash,draw.count,draw.instances,draw.start,draw.startInstance,desc.Width,desc.Height,draw.base,draw.kind};return true;
}
}
NativeVrsPsToken nativeVrsPsBefore(ID3D11DeviceContext* context,const NativeVrsPsDraw& draw)noexcept {
    drainBeforeRateChange(context);
    NativeVrsPsToken token{};if(!nativeVrsPsMeasureLive()||!measureOwner(context))return token;
    try {
        auto& s=state();auto& m=measureState();auto& request=measureRequest();
        const auto generation=request.generation.load(std::memory_order_acquire);
        // A request transition invalidates outstanding evidence, even if OFF
        // and ON both happened before this render-thread service opportunity.
        m.recorder.configure(m.generation==generation&&request.enabled.load(std::memory_order_acquire),request.sink.load(std::memory_order_acquire));
        m.recorder.collect(context,s.frame,GetTickCount64());
        if(m.generation!=generation){if(m.recorder.pending())return token;m.recorder=native_vrs::ps_measure::Recorder{};m.generation=generation;
            m.recorder.configure(request.enabled.load(std::memory_order_acquire),request.sink.load(std::memory_order_acquire));
            if(m.recorder.requested())if(auto sink=request.sink.load(std::memory_order_acquire))
                sink("native vrs ps armed: schema=1 default_off=1 frame_budget=600 per_eye_per_frame=1 no_flush=1 legacy_observer=0 performance_window=invalid");}
        if((!m.recorder.requested()||m.recorder.finished())&&!m.recorder.pending()){
            g_nativeVrsPsMeasureLive.store(false,std::memory_order_release);
            if(auto sink=request.sink.load(std::memory_order_acquire))sink("native vrs ps drained: pending=0 new_queries=0 performance_window=eligible_after_settle");return token;}
        if(draw.eye>=2||!draw.target||!draw.depth)return token;
        auto receipt=measureReceipt(s,draw.eye,false);if(!receipt.qualified)return token;
        const native_vrs::ps_measure::Key cheap{draw.vs,draw.ps,0,draw.count,draw.instances,draw.start,draw.startInstance,0,0,draw.base,draw.kind};
        if(!m.recorder.metadataWanted(cheap,receipt))return token;
        native_vrs::ps_measure::Key key{};if(!measureKey(context,draw,key))return token;
        const auto p=m.recorder.plan(key,receipt);if(!p.active)return token;
        if(p.full&&!fullRate(s))return token;
        receipt=measureReceipt(s,draw.eye,p.full);
        if(!receipt.qualified||!m.recorder.begin(s.device.Get(),context,p,receipt,GetTickCount64())){
            // A failed or invalid sample must not re-enable a map on an old
            // receipt. The next normal world call performs fresh admission.
            if(p.full)fullRate(s);return token;
        }
        token={p.candidate,p.slot,p.phase,p.id,receipt.map,p.full,true};return token;
    }catch(...){return token;}
}
void nativeVrsPsAfter(ID3D11DeviceContext* context,const NativeVrsPsToken& token)noexcept {
    drainBeforeRateChange(context);
    if(!token.active||!measureOwner(context))return;
    try {
        auto& s=state();auto& m=measureState();const unsigned eye=token.candidate/8;
        const native_vrs::ps_measure::Plan p{token.candidate,token.slot,token.phase,token.id,token.full,true};
        const auto receipt=measureReceipt(s,eye,token.full);
        // The query ends immediately after the one actual API draw. Driver
        // restoration is outside its scope and cannot add shader work.
        m.recorder.end(context,p,receipt,true);
        bool restored=receipt.qualified;
        if(token.full){restored=restored&&shared().enabled.load(std::memory_order_acquire)&&eye<2&&
            reinterpret_cast<uintptr_t>(s.eyes[eye].view.Get())==token.map&&activateMap(s,uintptr_t(token.map));
            if(!restored)fullRate(s);}
        if(!restored)m.recorder.invalidate(p);
    }catch(...){measureState().recorder.invalidate({token.candidate,token.slot,token.phase,token.id,token.full,true});}
}
#ifdef EDVR_NATIVE_VRS_TEST
namespace native_vrs::testing {
namespace {
struct Capture {DriverResult result;int nextRate=0,nextResource=0;};
thread_local Capture* capture=nullptr;
int __cdecl mockRates(IUnknown* context,RateDesc* desc) {
    auto& c=*capture;auto& out=c.result.calls[c.result.count++];
    out.rates=true;out.context=reinterpret_cast<uintptr_t>(context);out.version=desc->version;out.count=desc->count;
    out.enabled=desc->count&&desc->rates&&desc->rates[0].enabled;
    if(desc->rates)for(unsigned i=0;i<16;++i)out.palette[i]=desc->rates[0].palette[i];
    out.result=std::exchange(c.nextRate,0);return out.result;
}
int __cdecl mockResource(IUnknown* context,NvRateView* view) {
    auto& c=*capture;auto& out=c.result.calls[c.result.count++];
    out.context=reinterpret_cast<uintptr_t>(context);out.resource=reinterpret_cast<uintptr_t>(view);
    out.result=std::exchange(c.nextResource,0);return out.result;
}
}
DriverResult runDriverFixture(const DriverOp* operations,size_t count) {
    Capture c;capture=&c;publicationCapture=&c.result;State& s=state();s=State{};s.rates=&mockRates;s.resource=&mockResource;s.frame=1;
    auto* context=reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(0x101));
    uint64_t emittedOrdinal=1;
    auto& a=shared();a.refused.store(false);a.failure.store(NativeVrsFailure::None);a.lastDriver.store(0);
    a.enabled.store(true);a.driverReady.store(true);a.owner.store(context);a.thread.store(GetCurrentThreadId());
    g_nativeVrsLive.store(true);
    a.invalidateVersion.store(0);a.polls.store(0);
    for(size_t i=0;i<count&&c.result.count<56;++i){
        switch(operations[i]){
            case DriverOp::Full:nativeVrsFullRate(context);break;
            case DriverOp::Left:activateMap(s,0x100);break;
            case DriverOp::Right:activateMap(s,0x200);break;
            case DriverOp::WitnessLeft:prepareWitnessRate(s,0x100);break;
            case DriverOp::Invalidate:nativeVrsInvalidate(context);break;
            case DriverOp::NextFrame:s.frame=++emittedOrdinal;break;
            case DriverOp::FailRate:c.nextRate=-9;break;
            case DriverOp::FailResource:c.nextResource=-10;break;
            case DriverOp::Reconfigure:nativeVrsConfigure(true);break;
            case DriverOp::Off:nativeVrsConfigure(false);break;
            case DriverOp::TickNext:nativeVrsTick(context,++emittedOrdinal);break;
            case DriverOp::FlatTickNext:nativeVrsTick(context,++emittedOrdinal,false);break;
            case DriverOp::TickCurrent:nativeVrsTick(context,emittedOrdinal);break;
            case DriverOp::TickZero:nativeVrsTick(context,0);break;
            case DriverOp::TickRegressed:nativeVrsTick(context,emittedOrdinal-1);break;
            case DriverOp::TickForeignContext:nativeVrsTick(reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(0x202)),emittedOrdinal+1000);break;
            case DriverOp::TickForeignThread:{std::thread foreign([&]{nativeVrsTick(context,emittedOrdinal+1000);});foreign.join();break;}
        }
    }
    c.result.fullKnown=s.cache.fullRateKnown();c.result.coarseKnown=s.cache.active(s.cache.binding);
    c.result.refused=a.refused.load();c.result.failure=a.failure.load();c.result.configured=a.enabled.load();
    c.result.live=nativeVrsLive();c.result.coarseRequested=nativeVrsCoarseRequested();
    c.result.frameOrdinal=s.frame;c.result.providerPolls=a.polls.load();
    a.enabled.store(false);a.driverReady.store(false);a.owner.store(nullptr);a.thread.store(0);s=State{};
    g_nativeVrsLive.store(false);
    publicationCapture=nullptr;capture=nullptr;return c.result;
}
}
#endif
} // namespace edvr

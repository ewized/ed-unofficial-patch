#pragma once
#include "launch_centre_cases.h"
#include "../../src/common/system_d3d11.h"

namespace edvr::openxr::test::fgbootstrap_fixture {
struct ExclusiveExecutor final : ImmediateExecutor {
  bool invoke(std::function<void()> callback) override {callback();return true;}
};
struct Provider {
  bool allowed=false,taken=false;
  unsigned discarded=0,released=0,retired=0,closeCalls=0,pendingCloses=0,invalidations=0;
  bool stayPending=false;
  EdvrFgStatus status{};
  EdvrFgBatch batch{};
} inline provider;
inline HRESULT WINAPI status(void*,EdvrFgStatus* out){*out=provider.status;return S_OK;}
inline HRESULT WINAPI begin(void*,const EdvrFgSource*){return S_OK;}
inline HRESULT WINAPI poll(void*,EdvrFgBatch* out){if(provider.taken)return S_FALSE;provider.taken=true;*out=provider.batch;return S_OK;}
inline HRESULT WINAPI retain(void*){return S_OK;}
inline void WINAPI release(void*){++provider.released;}
inline HRESULT WINAPI retire(void*,HANDLE,uint64_t){++provider.retired;return S_OK;}
inline HRESULT WINAPI discard(void*){++provider.discarded;return S_OK;}
inline HRESULT WINAPI invalidate(void*,uint32_t){++provider.invalidations;return S_OK;}
inline HRESULT WINAPI close(void*){
  ++provider.closeCalls;
  if(provider.stayPending)return E_PENDING;
  if(provider.pendingCloses){--provider.pendingCloses;return E_PENDING;}
  return S_OK;
}
}
#pragma comment(linker,"/export:edvrAcquireNativeFrameGeneration")
extern "C" HRESULT WINAPI edvrAcquireNativeFrameGeneration(const EdvrFgRequest*,EdvrFgTable* table) {
  using namespace edvr::openxr::test::fgbootstrap_fixture;
  if(!provider.allowed)return E_NOINTERFACE;
  *table={sizeof(*table),1,&provider,status,begin,poll,retain,release,retire,discard,invalidate,close};return S_OK;
}

namespace edvr::openxr::test {
template<class Check> void runFgBootstrapCases(Check check) {
  using Microsoft::WRL::ComPtr;
  using namespace fgbootstrap_fixture;
  {
    launch_fixture::Fixture observed;auto& h=observed.host;
    EndFrameInfo info;info.layers=true;info.shouldRender=true;
    h.endFrameReturned(info); // The ordinary native successful-end seam.
    h.frameWithheld=true;h.endFrameReturned(info);
    info.background=true;h.endFrameReturned(info);
    info.background=false;info.result=XR_ERROR_RUNTIME_FAILURE;h.endFrameReturned(info);
    check(h.presentationCounts.sequence==4&&h.presentationCounts.nativeFrames==1&&
      h.presentationCounts.repeatedFrames==1&&h.presentationCounts.backgroundFrames==1&&
      h.presentationCounts.failedFrames==1&&!h.presentationCounts.generatedFrames,
      "presentation: native observer separates native/replay/loading/failed stereo frames");
    h.frameWithheld=false;h.fgEnabled=true;h.fgMultiplier=2;
    observed.fake.shouldRender=true;
    XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space=h.seated.frame().space;layer.viewCount=2;layer.views=views;
    check(h.boundary.waitAndBegin()==XR_SUCCESS&&
      h.fgEndPrepared(&layer,edvr::NativePresentationKind::Generated)==XR_SUCCESS&&
      h.presentationCounts.generatedFrames==1&&observed.fake.endCalls==1,
      "presentation: one actual successful generated xrEndFrame counts once, not once per eye");
    observed.fake.frameTime+=10;observed.fake.shouldRender=false;
    check(h.boundary.waitAndBegin()==XR_SUCCESS&&
      h.fgEndPrepared(&layer,edvr::NativePresentationKind::Generated)==XR_SUCCESS&&
      h.presentationCounts.generatedFrames==1&&h.presentationCounts.emptyFrames==1,
      "presentation: shouldRender false cannot fabricate generated output");
    observed.fake.frameTime+=10;observed.fake.shouldRender=true;observed.fake.endResult=XR_ERROR_RUNTIME_FAILURE;
    check(h.boundary.waitAndBegin()==XR_SUCCESS&&
      h.fgEndPrepared(&layer,edvr::NativePresentationKind::Generated)==XR_ERROR_RUNTIME_FAILURE&&
      h.presentationCounts.generatedFrames==1&&h.presentationCounts.failedFrames==2&&observed.fake.endCalls==3,
      "presentation: refused xrEndFrame increments failure only, not generated count");
    check(h.timing.publishPresentation(h.presentationCounts)==S_FALSE,
      "presentation: missing optional timing capability remains unavailable without disrupting XR");
    h.fgEnabled=false;
  }
  ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
  const auto create=systemD3D11CreateDevice();
  check(create&&SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"FG bootstrap: WARP device");
  if(!device)return;
  ComPtr<ID3D11Device> producer;ComPtr<ID3D11DeviceContext> producerContext;
  check(SUCCEEDED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&producer,nullptr,&producerContext)),"FG bootstrap: separate producer device");
  if(!producer)return;
  launch_fixture::Fixture fixture;auto& h=fixture.host;
  ExclusiveExecutor executor; // This fixture alone owns both WARP context roles.
  check(fixture.initialized&&SUCCEEDED(h.captured.initializeShared(producer.Get(),device.Get(),&executor))&&SUCCEEDED(h.fgFallback.initialize(device.Get()))&&
    SUCCEEDED(h.fgPendingNative.initialize(device.Get()))&&
    SUCCEEDED(h.frameGeneration.images().phases[0].initialize(device.Get())),"FG bootstrap: actual host/local capture owners");
  h.fgConfig=7;h.fgMultiplier=2;h.fgNativePixels=true;h.frameSpace=h.seated.frame().space;
  h.frameViews[0]=h.frameGeometry.views[0];h.frameViews[1]=h.frameGeometry.views[1];
  const fgbootstrap::Epoch epoch{h.runtimeGeneration,7,h.poses.read().originGeneration,3};
  h.fgBootstrap.sync(epoch);
  D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=4;td.ArraySize=td.MipLevels=td.SampleDesc.Count=1;
  td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> sources[2];
  check(SUCCEEDED(producer->CreateTexture2D(&td,nullptr,&sources[0]))&&SUCCEEDED(producer->CreateTexture2D(&td,nullptr,&sources[1])),"FG bootstrap: distinct native eyes");
  if(!sources[0]||!sources[1])return;
  constexpr XrDuration period=11111111;
  const auto nativePair=[&](uint64_t sequence,bool right=true,bool promote=true) {
    fixture.fake.frameTime=1000000000+XrTime(sequence)*22222222;
    check(h.boundary.waitAndBegin()==XR_SUCCESS,"FG bootstrap: actual frame clock");
    h.activeFrameSequence=sequence;h.frameGeometry.displayTime=fixture.fake.frameTime;
    for(unsigned eye=0;eye<(right?2u:1u);++eye) {
      uint32_t pixels[16];std::fill_n(pixels,16,0xff000000u|uint32_t(sequence)|(eye?0x00010000u:0));
      producerContext->UpdateSubresource(sources[eye].Get(),0,nullptr,pixels,16,0);
      const vr::Texture_t texture{sources[eye].Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
      check(h.captured.capture(vr::EVREye(eye),&texture,nullptr,vr::Submit_Default,true,nullptr,true)==vr::VRCompositorError_None,"FG bootstrap: real native eye capture");
    }
    if(h.captured.completePending()==S_OK) {
      h.fgStageNativeFallback(epoch);
      if(promote)h.fgRefreshNativeFallback(fixture.fake.frameTime);
    }
  };
  const auto pixel=[&](unsigned eye) {
    auto desc=td;desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> read;uint32_t result=0;
    if(FAILED(device->CreateTexture2D(&desc,nullptr,&read))||!h.fgFallback.texture(vr::EVREye(eye)))return result;
    context->CopyResource(read.Get(),h.fgFallback.texture(vr::EVREye(eye)));D3D11_MAPPED_SUBRESOURCE mapped{};
    if(SUCCEEDED(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped))){result=*static_cast<const uint32_t*>(mapped.pData);context->Unmap(read.Get(),0);}return result;
  };
  bool noEmpty=true,monotonic=true;XrTime displayedTime=0;
  for(uint64_t sequence=1;sequence<=70;++sequence) {
    nativePair(sequence);
    noEmpty=noEmpty&&h.fgFallbackUsable(fixture.fake.frameTime);
    monotonic=monotonic&&h.fgFallbackTime>=displayedTime;
    displayedTime=h.fgFallbackTime;h.fgDisplayedSequence=h.fgFallbackSequence;
    h.fgBootstrap.displayNative(h.fgFallbackSequence);
  }
  check(noEmpty&&fixture.fake.frameTime-1022222222>1500000000,"FG bootstrap: fresh native fallback prevents empty projections beyond 1.5 seconds");
  check(monotonic&&h.fgFallbackSequence==70&&pixel(0)==0xff000046u&&pixel(1)==0xff010046u,"FG bootstrap: newer stereo bytes/pose publish together without time reversal");
  check(!h.fgFallbackUsable(h.fgFallbackTime+500000001),"FG bootstrap: unchanged 500 ms fallback expiry");
  // Start an independent complete-pair/handoff case from a known endpoint,
  // even when the old-code negative above deliberately retained endpoint 1.
  h.fgFallbackValid=false;h.fgDisplayedSequence=0;nativePair(70);
  h.fgDisplayedSequence=70;h.fgBootstrap.displayNative(70);displayedTime=h.fgFallbackTime;
  h.captured.reset();nativePair(71,false);
  check(h.fgFallbackSequence==70&&pixel(0)==0xff000046u&&pixel(1)==0xff010046u,"FG bootstrap: failed second-eye capture retains complete previous pair");
  h.captured.reset();nativePair(71,true,false);
  const XrTime firstPoll=fixture.fake.frameTime+period;
  EdvrFgStatus report{};report.generation=epoch.generation;report.configGeneration=epoch.config;report.historyGeneration=epoch.history;
  report.latestSourceSequence=71;report.latestReadySequence=70;
  auto batch=[&](uint64_t previous,uint64_t current) {
    EdvrFgBatch b{};b.generation=epoch.generation;b.configGeneration=epoch.config;b.referenceGeneration=epoch.reference;b.historyGeneration=epoch.history;
    b.previousSequence=previous;b.currentSequence=current;b.previousPoseDisplayTime=1000000000+XrTime(previous)*22222222;
    b.currentPoseDisplayTime=1000000000+XrTime(current)*22222222;b.multiplier=2;b.generatedCount=1;b.phaseMask=3;return b;
  };
  auto& images=h.frameGeneration.images();const auto candidate=batch(69,70);
  for(unsigned variant=0;variant<8;++variant) {
    images.metadata=candidate;
    if(variant==0)++images.metadata.generation;if(variant==1)++images.metadata.configGeneration;
    if(variant==2)++images.metadata.referenceGeneration;if(variant==3)++images.metadata.historyGeneration;
    if(variant==4)images.metadata.currentPoseDisplayTime=fixture.fake.frameTime+1;
    if(variant==5){images.metadata.currentPoseDisplayTime=1;images.metadata.previousPoseDisplayTime=1;}
    if(variant==6){images.metadata.generatedCount=0;images.metadata.phaseMask=1;}
    if(variant==7)--images.metadata.previousSequence;
    images.ready=true;h.fgPrepareImages(report,fixture.fake.frameTime,period,true);
    check(!h.fgDeferNativeFallback(report,firstPoll,period,period),
      "FG bootstrap: stale/future/wrong epoch/native-only/mismatched output cannot delay refresh");
  }
  images.ready=true;images.metadata=candidate;
  h.fgPrepareImages(report,fixture.fake.frameTime,period,true);
  check(!images.ready&&h.fgFallbackSequence==70&&h.fgDeferNativeFallback(report,firstPoll,period,period),
    "FG bootstrap: exact timely behind batch proves one trial using real staged A and B");
  check(!h.fgDeferNativeFallback(report,firstPoll+period,period,period),
    "FG bootstrap: a second unavailable poll cannot extend the hold");
  images.metadata=batch(70,71);images.ready=true;h.fgPrepareImages(report,firstPoll+period,period,true);
  check(images.ready&&images.next==1&&fgschedule::phaseTime(images.metadata,images.next)>displayedTime,
    "FG bootstrap: generated phase catches up without backward presentation or native starvation");
  const XrTime poseTime=images.metadata.currentPoseDisplayTime;
  const XrTime finalOpportunity=poseTime+3*period+67;
  h.fgBootstrap.noteSlot(1,poseTime,period,period);
  h.fgBootstrap.noteSlot(2,poseTime+period,period,period);
  h.fgBootstrap.noteSlot(3,poseTime+2*period,period,period);
  h.fgBootstrap.noteSlot(4,finalOpportunity,2*period,period);
  check(h.fgPrepareImages(report,finalOpportunity,period,true)==fghandoff::Images::Generated&&images.next==1,
    "FG bootstrap: actual host admits a phase on the last proven runtime opportunity");
  images.ready=false;++h.fgTick;
  h.activeFrameSequence=73; // Credit 73 already advanced live geometry.
  h.fgBootstrap.abandonNativeAttempt();
  h.fgRefreshNativeFallback(fixture.fake.frameTime+period);
  // If credit closed the frame after promotion, retry before advancing the
  // displayed sequence. The staged textures have already changed owners.
  h.fgRefreshNativeFallback(fixture.fake.frameTime+period*2);
  check(!h.fgBootstrap.completedSequence()&&h.fgFallbackSequence==71&&h.fgFallbackTime==fixture.fake.frameTime&&
    pixel(0)==0xff000047u&&pixel(1)==0xff010047u,
    "FG bootstrap: missed generation slot promotes the complete staged stereo pair, not the outstanding credit");
  h.fgDisplayedSequence=71;h.fgBootstrap.displayNative(71);
  h.fgBootstrap.sync({epoch.generation,epoch.config,epoch.reference,epoch.history+1});
  check(!h.fgDeferNativeFallback(report,firstPoll+period,period,period),"FG bootstrap: new history clears the old pending handoff");
  // Drive the actual import client with a real, unsignalled shared WARP
  // fence. No provider GPU reads/retirement may occur, and no ready proof is
  // manufactured just because the metadata says generatedCount=1.
  provider={};provider.allowed=true;
  provider.status={sizeof(provider.status),1};provider.status.requestedMultiplier=2;
  provider.status.generation=epoch.generation;provider.status.configGeneration=epoch.config;
  provider.status.historyGeneration=epoch.history;provider.status.state=EDVR_FG_ARMING;
  images.ready=false;
  const bool acquired=h.frameGeneration.acquire(GetModuleHandleW(nullptr),device.Get(),epoch.generation)==S_OK;
  check(acquired&&h.frameGeneration.initializeConsumer(device.Get(),context.Get())==S_OK,"FG bootstrap: actual import owner initialized");
  ComPtr<ID3D11Device5> device5;ComPtr<ID3D11Fence> fence;HANDLE fenceHandle=nullptr;
  bool shared=SUCCEEDED(device.As(&device5))&&SUCCEEDED(device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)))&&
    SUCCEEDED(fence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&fenceHandle));
  ComPtr<ID3D11Texture2D> sharedTextures[4];HANDLE textureHandles[4]{};
  auto sharedDesc=td;sharedDesc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  for(unsigned i=0;i<4&&shared;++i) {
    ComPtr<IDXGIResource1> resource;
    shared=SUCCEEDED(device->CreateTexture2D(&sharedDesc,nullptr,&sharedTextures[i]))&&SUCCEEDED(sharedTextures[i].As(&resource))&&
      SUCCEEDED(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&textureHandles[i]));
  }
  check(shared,"FG bootstrap: real pending fence and texture handles");
  if(shared&&acquired) {
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapterDesc{};
    const bool adapterKnown=SUCCEEDED(device.As(&dxgi))&&SUCCEEDED(dxgi->GetAdapter(&adapter))&&SUCCEEDED(adapter->GetDesc(&adapterDesc));
    check(adapterKnown,"FG bootstrap: import adapter identity");
    provider.batch=batch(70,71);auto& pending=provider.batch;
    pending.size=sizeof(pending);pending.version=EDVR_FG_BATCH_VERSION_3;pending.token=&provider;pending.width=pending.height=4;
    pending.format=DXGI_FORMAT_R8G8B8A8_UNORM;pending.readyFence=fenceHandle;pending.readyValue=1;
    pending.adapterLuidLow=adapterDesc.AdapterLuid.LowPart;pending.adapterLuidHigh=adapterDesc.AdapterLuid.HighPart;
    for(unsigned phase=0;phase<2;++phase)for(unsigned eye=0;eye<2;++eye)pending.textures[phase][eye]=textureHandles[phase*2+eye];
    for(auto* views:{&pending.previousViews,&pending.currentViews})for(auto& view:*views) {
      view.orientation[3]=1;view.fov[0]=view.fov[3]=-.7f;view.fov[1]=view.fov[2]=.7f;
    }
    h.fgBootstrap.sync(epoch);
    check(h.frameGeneration.poll(epoch.generation,epoch.reference,epoch.config,2,4,4,fixture.fake.frameTime)==S_FALSE&&
      provider.taken&&!images.ready,"FG bootstrap: pending provider fence cannot publish ready images");
    h.fgPrepareImages(report,fixture.fake.frameTime,period,true);
    check(!h.fgDeferNativeFallback(report,firstPoll+period,period,period)&&provider.retired==0,
      "FG bootstrap: pending import cannot delay native refresh or start provider reads");
  }
  if(acquired) {
    const unsigned invalidations=provider.invalidations;
    provider.pendingCloses=2;
    check(h.frameGeneration.close(5000)==S_OK&&provider.closeCalls==3&&
      provider.invalidations==invalidations+1&&!h.frameGeneration.acquired()&&
      provider.discarded==(provider.taken?1u:0u)&&provider.released==provider.discarded,
      "FG shutdown: one invalidation and token release across temporary worker delays");
    check(h.frameGeneration.acquire(GetModuleHandleW(nullptr),device.Get(),epoch.generation)==S_OK,
      "FG shutdown: reusable client after a confirmed close");
    provider.stayPending=true;
    check(h.frameGeneration.close(2)==E_PENDING&&h.frameGeneration.acquired(),
      "FG shutdown: a permanently pending worker retains its context at the deadline");
    provider.stayPending=false;
    check(h.frameGeneration.close(0)==S_OK&&!h.frameGeneration.acquired()&&provider.invalidations==invalidations+2,
      "FG shutdown: later completion releases the retained context without a second invalidation");
  }
  for(auto handle:textureHandles)if(handle)CloseHandle(handle);
  if(fenceHandle)CloseHandle(fenceHandle);provider.allowed=false;
  h.fgBootstrap.clear();check(!h.fgBootstrap.completedSequence(),"FG bootstrap: Off clears the pending handoff");
  h.boundary.clear();check(h.captured.shutdownShared()==S_OK,"FG bootstrap: shared capture retirement");
  h.captured.shutdown();h.fgPendingNative.shutdown();h.fgFallback.shutdown();images.phases[0].shutdown();
}
} // namespace edvr::openxr::test

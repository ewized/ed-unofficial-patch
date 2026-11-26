#pragma once
#include "launch_centre_cases.h"
#include "../../src/common/system_d3d11.h"

namespace edvr::openxr::test::fgbootstrap_fixture {
struct ExclusiveExecutor final : ImmediateExecutor {
  bool invoke(std::function<void()> callback) override {callback();return true;}
};
struct ResumeSink final : FrameSink {
  XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  unsigned capturedEyes=0,composed=0,finished=0;
  XrResult composedResult=XR_SUCCESS;
  vr::EVRCompositorError capture(vr::EVREye eye,const vr::Texture_t*,const vr::VRTextureBounds_t*,vr::EVRSubmitFlags,bool pixels) override {
    if(!pixels)return vr::VRCompositorError_InvalidTexture;
    capturedEyes|=1u<<unsigned(eye);return vr::VRCompositorError_None;
  }
  XrResult compose(XrCompositionLayerProjection& layer) override {
    ++composed;layer.space=launch_fixture::local();layer.viewCount=2;layer.views=views;return composedResult;
  }
  void sceneFinished(bool pixels,XrResult result) override {if(pixels&&result==XR_SUCCESS)++finished;}
};
struct Provider {
  bool allowed=false,taken=false;
  unsigned announced=0;
  unsigned discarded=0,released=0,retired=0,closeCalls=0,pendingCloses=0,invalidations=0;
  bool stayPending=false;
  EdvrFgStatus status{};
  EdvrFgBatch batch{};
} inline provider;
inline HRESULT WINAPI status(void*,EdvrFgStatus* out){*out=provider.status;return S_OK;}
inline HRESULT WINAPI begin(void*,const EdvrFgSource*){++provider.announced;return S_OK;}
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
template<class Check> void runFgLatePhaseCases(Check check) {
  using Outcome=fghandoff::Images;
  launch_fixture::Fixture fixture;auto& h=fixture.host;
  constexpr XrDuration period=10000000;
  constexpr XrTime sourceTime=5000000000;
  EdvrFgStatus status{};h.fgConfig=status.configGeneration=7;status.historyGeneration=3;
  auto& images=h.frameGeneration.images();
  const auto prepare=[&](unsigned multiplier) {
    h.fgMultiplier=multiplier;h.fgDisplayedSequence=70;
    images.ready=true;images.next=1;auto& b=images.metadata;b={};
    b.generation=h.runtimeGeneration;b.referenceGeneration=h.poses.read().originGeneration;
    b.configGeneration=h.fgConfig;b.historyGeneration=status.historyGeneration;
    b.previousSequence=70;b.currentSequence=71;b.previousPoseDisplayTime=sourceTime-multiplier*period;
    b.currentPoseDisplayTime=sourceTime;b.multiplier=multiplier;b.generatedCount=multiplier-1;b.phaseMask=(1u<<multiplier)-1u;
    h.fgBootstrap.sync({b.generation,b.configGeneration,b.referenceGeneration,b.historyGeneration});
  };
  const auto choose=[&](XrTime age,bool imported) {return h.fgPrepareImages(status,sourceTime+age,period,imported);};
  prepare(4);
  check(choose(5*period,true)==Outcome::Generated&&images.next==1,
    "FG late phases: original first SDK index remains eligible through its exact deadline");
  check(choose(6*period,true)==Outcome::Generated&&images.next==2&&
    fgschedule::phaseTime(images.metadata,images.next)==sourceTime-2*period,
    "FG late phases: skip one expired 4x image and choose original index two and its own pose");
  prepare(4);
  check(choose(7*period,true)==Outcome::Generated&&images.next==3&&
    fgschedule::phaseTime(images.metadata,images.next)==sourceTime-period,
    "FG late phases: skip two expired 4x images and choose original index three and its own pose");
  prepare(4);
  h.fgBootstrap.displayGenerated(images.metadata);images.next=2;
  check(choose(6*period+1,false)==Outcome::Generated&&images.next==3&&
    h.fgBootstrap.needsEndpoint(images.metadata.currentSequence),
    "FG late phases: an already presented series advances to its own timely third phase");
  prepare(4);
  check(choose(7*period+1,true)==Outcome::Late&&images.ready&&!images.next&&
    choose(7*period+2,false)==Outcome::Endpoint,"FG late phases: after the final deadline only the native endpoint remains");
  prepare(3);
  check(choose(5*period,true)==Outcome::Generated&&images.next==2,"FG late phases: original 3x final index can still fit");
  prepare(2);
  check(choose(3*period+1,true)==Outcome::Late&&!images.next,"FG late phases: a missed 2x phase has no successor");
  prepare(4);--h.fgDisplayedSequence;
  check(choose(6*period,true)==Outcome::PreviousMismatch&&!images.next,
    "FG late phases: a new batch cannot skip an unmatched displayed endpoint");
  prepare(4);images.next=2;--h.fgDisplayedSequence;
  check(choose(7*period,false)==Outcome::PreviousMismatch&&!images.next,
    "FG late phases: a retained series cannot skip across a different displayed endpoint");
  prepare(4);++images.metadata.historyGeneration;
  check(choose(7*period,true)==Outcome::WrongEpoch&&!images.ready,
    "FG late phases: obsolete history cannot supply a later phase");
  check(h.fgPhaseSalvaged==4&&h.fgPhasesExhausted==2,
    "FG late phases: count rescued selections and expired series separately from rejected owners and epochs");
}

template<class Check> void runFgBootstrapCases(Check check) {
  runFgLatePhaseCases(check);
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
      h.presentationCounts.failedFrames==1&&!h.presentationCounts.generatedFrames&&!h.fgPresentationGeneration,
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
    h.fgEnabled=false;info.result=XR_SUCCESS;h.endFrameReturned(info);
    check(h.presentationCounts.nativeFrames==2&&h.presentationCounts.generatedFrames==1&&
      !h.presentationCounts.fgEnabled&&h.fgPresentationGeneration==h.runtimeGeneration,
      "presentation: warm Off and generated ends share cumulative owner counters across the mode switch");
  }
  {
    launch_fixture::Fixture resumed;resumed.fake.shouldRender=true;
    ResumeSink sink;FrameBoundary boundary(resumed.host.state,sink);
    check(!boundary.resumeNativeCredit({},1,true),"FG handoff: closed slot refuses a native credit");
    for(unsigned mask:{0u,1u,2u}) {
      ++resumed.fake.frameTime;
      check(boundary.waitAndBegin()==XR_SUCCESS,"FG handoff: real runtime slot");
      const auto slot=boundary.frame();auto stale=slot;--stale.sequence;
      bool foreign=true;std::thread other([&]{foreign=boundary.resumeNativeCredit(slot,mask,true);});other.join();
      check(!foreign&&!boundary.resumeNativeCredit(stale,mask,true)&&!boundary.resumeNativeCredit(slot,3,true),
        "FG handoff: wrong owner, stale slot and already complete stereo pair refused");
      check(boundary.resumeNativeCredit(slot,mask,true)&&!boundary.resumeNativeCredit(slot,mask,true),
        "FG handoff: exactly one adoption of zero, left or right real eye");
      sink.capturedEyes=0;const auto count=sink.finished;const auto ends=resumed.fake.endCalls;
      for(unsigned eye=0;eye<2;++eye)if(!(mask&(1u<<eye)))
        check(boundary.submit(vr::EVREye(eye),nullptr)==vr::VRCompositorError_None,"FG handoff: remaining native eye accepted");
      check(sink.capturedEyes==(3u^mask)&&sink.finished==count+1&&resumed.fake.endCalls==ends+1,
        "FG handoff: ordinary compose presents once without recapturing the admitted eye");
    }
    check(boundary.waitAndBegin()==XR_SUCCESS&&boundary.resumeNativeCredit(boundary.frame(),1,true),
      "FG handoff: prepare a native pair whose final consumer copy fails");
    sink.composedResult=XR_ERROR_RUNTIME_FAILURE;const auto count=sink.finished;
    check(boundary.submit(vr::Eye_Right,nullptr)==vr::VRCompositorError_InvalidTexture&&boundary.failed()&&sink.finished==count,
      "FG handoff: copy failure rejects second eye with no successful stereo receipt");
  }
  {
    launch_fixture::Fixture pose;auto& h=pose.host;
    const XrPosef origin{{0,0,0,1},{3,4,5}};
    check(h.seated.setOrigin(origin)==XR_SUCCESS,"FG plugin: distinct seated origin");
    pose.fake.head={{0,1,0,0},{4,6,8}};
    EdvrFgBatch b{};b.multiplier=3;b.previousPoseDisplayTime=100;b.currentPoseDisplayTime=130;
    for(const XrTime instant:{fgschedule::phaseTime(b,1),fgschedule::phaseTime(b,0),XrTime(95)}) {
      const auto before=pose.fake.locates;
      XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
      const auto* head=h.fgLocatePluginHead(h.seated.frame(),instant,location);
      check(head&&head->position.x==1&&head->position.y==2&&head->position.z==3&&head->orientation.y==1&&
        pose.fake.locates==before+1&&pose.fake.sampleTime==instant&&pose.fake.argumentsValid,
        "FG plugin: one genuine VIEW locate at the phase, endpoint or retained repeat time in the eyes' seated origin");
    }
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    pose.fake.flags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    check(!h.fgLocatePluginHead(h.seated.frame(),110,location),"FG plugin: partial tracking supplies no update pose");
    pose.fake.flags=launch_fixture::tracked;pose.fake.rejectTime=110;
    check(!h.fgLocatePluginHead(h.seated.frame(),110,location),"FG plugin: rejected historical locate supplies no update pose");
  }
  {
    launch_fixture::Fixture transfer;auto& h=transfer.host;
    unsigned receives=0;
    const auto receive=[&]{++receives;return S_OK;};
    h.fgNativePixels=true;h.frameWithheld=true;
    check(h.fgCompleteNativePair(receive)==vr::VRCompositorError_None&&!receives,"FG transfer: withheld pair never attempts the pending copy");
    h.frameWithheld=false;h.fgNativePixels=false;
    check(h.fgCompleteNativePair(receive)==vr::VRCompositorError_None&&!receives,"FG transfer: no-pixel pair never attempts the pending copy");
    h.fgNativePixels=true;
    check(h.fgCompleteNativePair(receive)==vr::VRCompositorError_None&&receives==1&&!h.serviceFailed,
      "FG transfer: successful pixel receive admits the second eye");
  }
  for(HRESULT failure:{HRESULT(WAIT_TIMEOUT),HRESULT(DXGI_ERROR_DEVICE_REMOVED)}) {
    launch_fixture::Fixture transfer;auto& h=transfer.host;
    h.fgNativePixels=true;h.fgEnabled=true;h.timingFrameActive=true;h.timingFrameMask=1;
    const auto before=h.compositorSubmits;
    h.frameGeneration.bindOwner();h.frameGeneration.images().ready=true;
    const auto result=h.fgCompleteNativePair([&]{return failure;});
    check(result==vr::VRCompositorError_InvalidTexture&&h.serviceFailed&&h.lastCompositorResult==XR_ERROR_RUNTIME_FAILURE&&
      !h.fgEnabled&&!h.frameGeneration.images().ready&&!h.timingFrameActive&&!h.timingFrameMask&&h.compositorSubmits==before,
      "FG transfer: timeout or fault rejects the second eye, stops the failed session and retires incomplete timing");
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
  if(acquired) {
    auto& proof=provider.status;
    h.fgAvailable=true;
    Frame source{};source.sequence=90;source.predictedDisplayTime=fixture.fake.frameTime;
    h.fgAnnounceSource(source,h.frameGeometry);
    check(provider.announced==1&&!fgadmission::providerReady(proof,h.runtimeGeneration),
      "FG eligibility: ordinary native pose still announces a source during warmup");
    proof.state=EDVR_FG_READY;proof.activeMultiplier=2;
    check(fgadmission::providerReady(proof,h.runtimeGeneration)&&h.fgCanSubmitBatch(candidate),"FG eligibility: current READY proof admits this exact batch");
    ++proof.generation;
    check(!h.fgCanSubmitBatch(candidate),"FG eligibility: foreign session cannot submit cached images");
    --proof.generation;++proof.configGeneration;
    check(!h.fgCanSubmitBatch(candidate),"FG eligibility: changed configuration invalidates cached images");
    --proof.configGeneration;proof.activeMultiplier=3;
    check(!fgadmission::providerReady(proof,h.runtimeGeneration),"FG eligibility: a different active multiplier cannot arm cadence");
    proof.activeMultiplier=0;proof.state=EDVR_FG_UNAVAILABLE;
    // The left eye of the already-lent credit was accepted while the
    // provider was qualified. The right eye remains a valid native submit.
    auto nativePose=h.poses.read();nativePose.sequence=h.activeFrameSequence;nativePose.posesAvailable=true;
    check(h.poses.publish(nativePose),"FG handoff: authentic live native credit");
    h.fgBootstrap.sync(epoch);h.startupOptions.separateDevice=true;
    h.fgEnabled=true;h.fgNativeOutstanding=true;h.fgAcceptedEyes=1;images.ready=true;
    h.gameFrameCallMs=GetTickCount64();
    auto operation=h.gate.tryEnter(h.runtimeGeneration);
    check(!h.fgCanSubmitBatch(candidate)&&h.fgTickOwner(operation)&&!h.fgEnabled&&!h.fgNativeOutstanding&&
      !h.fgAcceptedEyes&&!images.ready&&h.fgResumedNative&&h.state.frameOpen(),
      "FG eligibility: lost proof drops cached images and hands the real partial credit to native cadence");
    const vr::Texture_t right{sources[1].Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
    const auto ends=fixture.fake.endCalls;
    check(h.boundary.submit(vr::Eye_Left,&right)==vr::VRCompositorError_InvalidTexture&&
      h.boundary.submit(vr::Eye_Right,&right)==vr::VRCompositorError_None&&fixture.fake.endCalls==ends+1&&!h.state.frameOpen(),
      "FG eligibility: real accepted left survives proof loss and the ordinary right submit completes the no-render slot");
    h.fgAnnounceSource(source,h.frameGeometry);
    check(provider.announced==2,"FG eligibility: native sources continue while the provider recovers");
    ++proof.historyGeneration;proof.state=EDVR_FG_READY;proof.activeMultiplier=2;
    auto fresh=candidate;fresh.historyGeneration=proof.historyGeneration;
    check(fgadmission::providerReady(proof,h.runtimeGeneration)&&!h.fgCanSubmitBatch(candidate)&&h.fgCanSubmitBatch(fresh),
      "FG eligibility: recovered proof admits only the new history");
    h.fgConsumerAllocated=true;proof.requestedMultiplier=proof.activeMultiplier=0;proof.state=EDVR_FG_ARMING;
    h.fgReleaseOffImages(proof);
    check(h.fgConsumerAllocated&&h.fgFallback.texture(vr::Eye_Left),"FG storage: Auto probation retains native fallback pixels");
    proof.state=EDVR_FG_OFF;h.fgReleaseOffImages(proof);
    check(!h.fgConsumerAllocated&&!h.fgFallback.texture(vr::Eye_Left)&&h.frameGeneration.acquired(),
      "FG storage: explicit Off drops the native capture storage and retains the control table");
    proof.historyGeneration=epoch.history;h.fgAvailable=false;
  }
  ComPtr<ID3D11Device5> device5;ComPtr<ID3D11Fence> fence;HANDLE fenceHandle=nullptr;
  bool shared=SUCCEEDED(device.As(&device5))&&SUCCEEDED(device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)))&&
    SUCCEEDED(fence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&fenceHandle));
  ComPtr<ID3D11Texture2D> sharedTextures[12];HANDLE textureHandles[12]{};
  auto sharedDesc=td;sharedDesc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  for(unsigned i=0;i<12&&shared;++i) {
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
    h.fgDisplayedSequence=pending.currentSequence;
    const auto now=pending.currentPoseDisplayTime;
    std::optional<EdvrFgBatch> behind;
    const auto pollBatch=[&](unsigned multiplier,uint64_t watermark) {
      return h.frameGeneration.poll(epoch.generation,epoch.reference,epoch.config,multiplier,4,4,now,epoch.history,watermark,behind);
    };
    const auto& counts=h.frameGeneration.pollCounts();
    check(pollBatch(2,h.fgDisplayedSequence)==S_FALSE&&provider.taken&&!images.ready&&!behind&&
      counts.providerFencePending==1&&!counts.textureOpens&&!counts.eyeCopies,"FG bootstrap: pending provider fence cannot publish ready images");
    check(h.frameGeneration.releaseInactiveImages()==S_FALSE&&!provider.discarded,
      "FG storage: unresolved import stays owned across an eviction request");
    h.fgPrepareImages(report,fixture.fake.frameTime,period,true);
    check(!h.fgDeferNativeFallback(report,firstPoll+period,period,period)&&provider.retired==0,
      "FG bootstrap: pending import cannot delay native refresh or start provider reads");
    ComPtr<ID3D11DeviceContext4> signalContext;
    const bool signalled=SUCCEEDED(context.As(&signalContext))&&SUCCEEDED(signalContext->Signal(fence.Get(),1));
    context->Flush();
    const auto until=GetTickCount64()+2000;
    while(signalled&&fence->GetCompletedValue()<1&&GetTickCount64()<until)Sleep(1);
    check(signalled&&fence->GetCompletedValue()==1,"FG import: producer fence completed on WARP");

    // A completed A->B already on screen may prove one cold handoff. The
    // receipt carries no borrowed handles and never makes an image renderable.
    h.fgBootstrap.displayNative(70);h.fgBootstrap.displayNative(71);h.fgBootstrap.completeNative(72);
    check(pollBatch(2,71)==S_OK&&behind&&!behind->token&&!behind->readyFence&&!behind->textures[1][1]&&
      !images.ready&&!images.phases[1].texture(vr::Eye_Left)&&!counts.textureOpens&&!counts.eyeCopies&&
      counts.behindBeforeCopy==1&&provider.discarded==1&&!provider.retired&&provider.released==1,
      "FG import: already displayed batch starts zero private eye copies and discards its unused token");
    const auto observe=[&]{return h.fgPrepareImages(report,now,period,false,&behind);};
    check(observe()==fghandoff::Images::BehindWatermark&&!behind&&observe()==fghandoff::Images::None&&
      h.fgBootstrap.deferNative(71,72,71,now+period,now+period,period,period,2)&&
      !h.fgBootstrap.deferNative(71,72,71,now+period,now+period,period,period,2),
      "FG import: one fenced receipt permits exactly one cold trial and cannot replay");
    check(pollBatch(2,71)==S_FALSE&&!behind&&counts.noExport==1,"FG import: no further export repeats a receipt");

    pending.uiFlags=EDVR_FG_UI_NATIVE_COVERAGE;
    for(unsigned e=0;e<2;++e){pending.uiTextures[e]=textureHandles[8+e];pending.nativeWorldTextures[e]=textureHandles[10+e];}
    for(unsigned m=2;m<=4;++m) {
      pending.multiplier=m;pending.generatedCount=m-1;pending.phaseMask=(1u<<m)-1u;
      for(unsigned phase=0;phase<m;++phase)for(unsigned eye=0;eye<2;++eye)pending.textures[phase][eye]=textureHandles[phase*2+eye];
      const auto before=counts;
      provider.taken=false;
      check(pollBatch(m,71)==S_OK&&behind&&behind->uiFlags&&!behind->uiTextures[1]&&!behind->nativeWorldTextures[0]&&
        !images.ready&&counts.textureOpens==before.textureOpens&&counts.eyeCopies==before.eyeCopies&&
        counts.behindBeforeCopy==before.behindBeforeCopy+1,
        "FG import: 2x/3x/4x already displayed split UI skips every world, UI and phase copy");
      provider.taken=false;
      check(pollBatch(m,70)==S_OK&&images.ready&&!behind&&images.metadata.generatedCount==m-1&&
        counts.textureOpens==before.textureOpens+2*m+4&&counts.eyeCopies==before.eyeCopies+2*m+4&&
        counts.imported==before.imported+1&&images.nativeUi.texture(vr::Eye_Right)&&images.nativeWorld.texture(vr::Eye_Left),
        "FG import: ahead split UI imports all stereo phases and UI/world textures through the real copy path");
      images.ready=false;
      const auto retireDeadline=GetTickCount64()+2000;
      HRESULT collected;
      while((collected=h.frameGeneration.collectImportPins())==S_FALSE&&GetTickCount64()<retireDeadline)Sleep(1);
      check(collected==S_OK,"FG import: private copies retired before next fixture token");
    }
    const auto valid=pending;
    for(unsigned invalid=0;invalid<8;++invalid) {
      pending=valid;provider.taken=false;
      switch(invalid) {
      case 0:++pending.historyGeneration;break;
      case 1:++pending.referenceGeneration;break;
      case 2:++pending.configGeneration;break;
      case 3:pending.phaseMask=1;break;
      case 4:pending.format=DXGI_FORMAT_R32_FLOAT;break;
      case 5:pending.nativeWorldTextures[1]=pending.uiTextures[0];break;
      case 6:pending.textures[3][1]=nullptr;break;
      case 7:pending.currentPoseDisplayTime-=500000001;break;
      }
      const auto before=counts;
      check(pollBatch(4,71)==E_INVALIDARG&&!behind&&!images.ready&&counts.behindBeforeCopy==before.behindBeforeCopy&&
        counts.textureOpens==before.textureOpens&&counts.eyeCopies==before.eyeCopies,
        "FG import: invalid epoch, phase, format, handles or pose cannot yield behind evidence");
    }
    pending=valid;provider.taken=false;++pending.currentPoseDisplayTime;
    check(pollBatch(4,71)==S_FALSE&&!behind&&counts.futurePose==1,
      "FG import: future pose stays pending with no behind evidence");
  }
  if(acquired) {
    const unsigned invalidations=provider.invalidations;
    provider.pendingCloses=2;
    check(h.frameGeneration.close(5000)==S_OK&&provider.closeCalls==3&&
      provider.invalidations==invalidations+1&&!h.frameGeneration.acquired()&&
      provider.released==provider.discarded+provider.retired,
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

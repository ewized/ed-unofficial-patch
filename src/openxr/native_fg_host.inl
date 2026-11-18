// Included inside NativeRuntimeHost: all methods below execute on its existing
// owner except the caller-side wait in fgInvokeWait.
  bool poseSeatReady() {
    if(seatPoll())return false;
    if(introRecentreRequested())switch(seatIntro()) {
      case SeatIntro::Todays:if(applyIntroRecentre())clearIntroRecentreRequest();return false;
      case SeatIntro::Applied:clearIntroRecentreRequest();return false;
      case SeatIntro::Skipped:clearIntroRecentreRequest();break;
      case SeatIntro::Sampling:case SeatIntro::Retry:break;
    }
    return true;
  }
  bool fgInvokeWait(std::function<void()> callback) {
    std::shared_ptr<FgCreditRequest> request;
    const bool invoked=service.invoke([&]{
      if(!fgEnabled.load(std::memory_order_acquire)){callback();return;}
      if(fgCredit)throw std::runtime_error("duplicate native FG pose credit");
      if(fgNativeOutstanding) {
        // A new native Wait abandons an incomplete source just as the normal
        // boundary clears its missing/one-eye frame. It cannot block forever
        // waiting for a second Submit the game has already abandoned.
        fgNativeOutstanding=false;fgAcceptedEyes=0;frameGeneration.invalidate(9);++fgRefused;
      }
      request=std::make_shared<FgCreditRequest>();request->callback=std::move(callback);fgCredit=request;
    },OwnerCallKind::WaitPoses);
    if(!invoked||!request)return invoked;
    // Crucially this is the GAME caller, not the XR owner. stop/failure also
    // completes the request, so shutdown cannot strand a native Wait caller.
    return request->wait();
  }
  void fgCancelCredit() {
    if(fgCredit){auto request=std::move(fgCredit);request->finish(false);}
  }
  void notePresentation(edvr::NativePresentationKind kind) {
    if(GetCurrentThreadId()!=ownerThread)return;
    if(edvr::recordNativePresentation(presentationCounts,runtimeGeneration,kind,
        fgEnabled.load(std::memory_order_acquire),fgMultiplier))
      timing.publishPresentation(presentationCounts); // CPU-only optional capability
  }
  XrResult fgEndPrepared(const XrCompositionLayerProjection* layer,edvr::NativePresentationKind kind) {
    const bool pixels=layer&&boundary.frame().shouldRender&&state.frameShouldRender()&&
        !state.terminal()&&state.lifecycle()!=Lifecycle::Stopping;
    const XrResult result=boundary.prepared(layer);
    notePresentation(result!=XR_SUCCESS?edvr::NativePresentationKind::Failed:
        pixels?kind:edvr::NativePresentationKind::Empty);
    return result;
  }
  void fgRunCredit(const std::shared_ptr<FgCreditRequest>& request,const RuntimeGate::Lease& operation) {
    if(!fgCreditLoan.run(gate,operation,runtimeGeneration,service.isOwner()&&GetCurrentThreadId()==ownerThread,
       [&]{request->run();}))request->finish(false);
  }
  void fgDisable(uint32_t reason,const RuntimeGate::Lease& operation) {
    fgLastDisableReason=reason;
    fgBootstrap.clear();
    fgEnabled.store(false,std::memory_order_release);service.setContinuousIdle(false);
    fgNativeOutstanding=false;fgAcceptedEyes=0;fgFallbackValid=false;fgDisplayedSequence=0;fgHistory=0;
    fgSplitFallbackValid=false;
    frameGeneration.invalidate(reason);
    if(fgCredit){auto request=std::move(fgCredit);fgRunCredit(request,operation);}
  }
  bool fgBeginSource(const Frame& frame,const GeometryInput& located) {
    if(fgMenuVisible||frameWithheld||fovTrim.trimmed()||fovTrim.pending()||zoomAppliedNow||zoomStripNow>0.f||circleNow.on||
       (featureFrameKnown&&featureFrame.version>=EDVR_NATIVE_FRAME_VERSION_10&&eyeshare::fromBits(featureFrame.leftEyeShare)>0.f)||!frameSpace||
       sizes[0].recommendedImageRectWidth!=sizes[1].recommendedImageRectWidth||
       sizes[0].recommendedImageRectHeight!=sizes[1].recommendedImageRectHeight)return false;
    EdvrFgSource source{sizeof(source),EDVR_NATIVE_FRAME_GENERATION_VERSION_1};
    source.generation=runtimeGeneration;source.referenceGeneration=poses.read().originGeneration;
    source.configGeneration=fgConfig;source.sequence=frame.sequence;source.poseDisplayTime=frame.predictedDisplayTime;
    source.width=sizes[0].recommendedImageRectWidth;source.height=sizes[0].recommendedImageRectHeight;
    for(unsigned eye=0;eye<2;++eye) {
      const auto physical=seatxf::toNatural(frameOrigin,located.views[eye].pose);
      auto& v=source.views[eye];v.orientation[0]=physical.orientation.x;v.orientation[1]=physical.orientation.y;
      v.orientation[2]=physical.orientation.z;v.orientation[3]=physical.orientation.w;
      v.position[0]=physical.position.x;v.position[1]=physical.position.y;v.position[2]=physical.position.z;
      const auto& f=located.views[eye].fov;v.fov[0]=f.angleLeft;v.fov[1]=f.angleRight;v.fov[2]=f.angleUp;v.fov[3]=f.angleDown;
    }
    return frameGeneration.begin(source)==S_OK;
  }
  void fgAnnounceSource(const Frame& frame,const GeometryInput& located) {
    if(!fgAvailable.load(std::memory_order_acquire))return;
    EdvrFgStatus status{};
    if(frameGeneration.status(status)!=S_OK||(status.requestedMultiplier!=2&&status.requestedMultiplier!=4)||
       status.state==EDVR_FG_FAILED||status.state==EDVR_FG_CLOSING)return;
    // An unavailable source returns to ordinary native pacing, but keeps
    // authentic metadata flowing. A later supported pair can recover without
    // a settings toggle. No source packets or GPU work are started while Off.
    if(!fgEnabled.load(std::memory_order_acquire))fgConfig=status.configGeneration;
    if(!fgBeginSource(frame,located)) {frameGeneration.invalidate(2);++fgRefused;}
  }
  void fgSeedNativeFallback() {
    fgSplitFallbackValid=false;
    fgDisplayedSequence=lastPublishedFrameSequence;
    if(!activeFrameSequence||frameWithheld||temporalFrameEyes!=3||!frameGeometryAvailable)return;
    const EyeCapture& source=previousPairValid&&previousPairSequence==activeFrameSequence&&previousReference==poses.read().originGeneration?previousPair:captured;
    const XrView* sourceViews=&source==&previousPair?previousViews:frameViews;
    const XrPosef& sourceOrigin=&source==&previousPair?previousOrigin:frameOrigin;
    fgFallbackValid=true;
    for(unsigned eye=0;eye<2;++eye) {
      const vr::Texture_t texture{source.texture(vr::EVREye(eye)),vr::API_DirectX,source.colorSpace(vr::EVREye(eye))};
      const auto region=source.bounds(vr::EVREye(eye));
      if(fgFallback.capture(vr::EVREye(eye),&texture,&region)!=vr::VRCompositorError_None)fgFallbackValid=false;
      fgFallbackViews[eye]=sourceViews[eye];fgFallbackViews[eye].pose=seatxf::toNatural(sourceOrigin,sourceViews[eye].pose);
    }
    fgFallbackSequence=activeFrameSequence;fgFallbackSpace=frameSpace;fgFallbackTime=&source==&previousPair?previousPairTime:frameGeometry.displayTime;
    fgFallbackReference=poses.read().originGeneration;
  }
  fgbootstrap::Epoch fgBootstrapEpoch(const EdvrFgStatus& status) const {
    if(status.generation!=runtimeGeneration||status.configGeneration!=fgConfig)return {};
    return {runtimeGeneration,fgConfig,poses.read().originGeneration,status.historyGeneration};
  }
  void fgRefreshNativeFallback(const fgbootstrap::Epoch& epoch,bool pairedReady) {
    if(!pairedReady||!epoch.valid()||epoch.generation!=runtimeGeneration||epoch.config!=fgConfig||
       epoch.reference!=poses.read().originGeneration)return;
    fgBootstrap.sync(epoch);
    auto& images=frameGeneration.images();
    const XrTime sourceTime=frameGeometry.displayTime;
    if(!fgBootstrap.refreshing()||images.ready||frameWithheld||!fgNativePixels||!frameGeometryAvailable||
       !frameSpace||!activeFrameSequence||activeFrameSequence<=fgDisplayedSequence||sourceTime<=0||
       sourceTime>boundary.frame().predictedDisplayTime||
       (fgFallbackValid&&(activeFrameSequence<=fgFallbackSequence||sourceTime<=fgFallbackTime)))return;
    // The local native-endpoint import buffer is idle during bootstrap. Copy
    // BOTH eyes there before exchanging it with the complete displayed pair;
    // a partial capture must not corrupt the previous valid fallback.
    auto& complete=images.phases[0];
    for(unsigned eye=0;eye<2;++eye) {
      const vr::Texture_t source{captured.texture(vr::EVREye(eye)),vr::API_DirectX,captured.colorSpace(vr::EVREye(eye))};
      const auto region=captured.bounds(vr::EVREye(eye));
      if(complete.capture(vr::EVREye(eye),&source,&region)!=vr::VRCompositorError_None)return;
    }
    if(!complete.exchangeBuffers(fgFallback))return;
    fgSplitFallbackValid=false;
    for(unsigned eye=0;eye<2;++eye) {
      fgFallbackViews[eye]=frameViews[eye];fgFallbackViews[eye].pose=seatxf::toNatural(frameOrigin,frameViews[eye].pose);
    }
    fgFallbackSpace=frameSpace;fgFallbackTime=sourceTime;fgFallbackSequence=activeFrameSequence;
    fgFallbackReference=epoch.reference;fgFallbackValid=true;
  }
  void fgPrepareImages(const EdvrFgStatus& status,XrTime displayTime,bool imported) {
    auto& images=frameGeneration.images();
    if(images.ready&&(images.metadata.generation!=runtimeGeneration||images.metadata.referenceGeneration!=poses.read().originGeneration||
       images.metadata.configGeneration!=fgConfig||images.metadata.historyGeneration!=status.historyGeneration||
       changes.crosses(images.metadata.generatedCount?images.metadata.previousPoseDisplayTime:images.metadata.currentPoseDisplayTime,displayTime)||
       images.metadata.currentPoseDisplayTime>displayTime||displayTime-images.metadata.currentPoseDisplayTime>500000000)) {
      images.ready=false;++fgRefused;
    }
    if(images.ready) {
      // An imported generated batch proves this epoch's producer is warm even
      // when native fallback already displayed its endpoint. Freeze native
      // advancement now so subsequent generated endpoints can catch up.
      fgBootstrap.observe(images.metadata,true);
      if(images.metadata.currentSequence<=fgDisplayedSequence)images.ready=false;
      else if(imported)images.next=fgschedule::firstPhase(images.metadata,fgDisplayedSequence);
    }
  }
  bool fgFallbackUsable(XrTime displayTime) const {
    return fgFallbackValid&&fgFallbackReference==poses.read().originGeneration&&
      !changes.crosses(fgFallbackTime,displayTime)&&fgFallbackSpace==seated.frame().space&&
      displayTime>=fgFallbackTime&&displayTime-fgFallbackTime<=500000000;
  }
  XrResult fgRenderPrepared(unsigned phase,XrCompositionLayerProjection& world,bool& hasUi) {
    auto& images=frameGeneration.images();const auto& batch=images.metadata;
    hasUi=false;XrView views[2]{};
    const float fade=comfortFade();
    const auto selected=fgprojection::select(phase,batch.uiFlags!=0,fade>0.f);
    const float fraction=phase?float(phase)/float(batch.multiplier):1.f;
    for(unsigned eye=0;eye<2;++eye)
      views[eye]=fgschedule::view(phase?batch.previousViews[eye]:batch.currentViews[eye],batch.currentViews[eye],fraction);
    const EyeCapture& pixels=selected.nativeWorld?images.nativeWorld:images.phases[phase];
    StereoNativeUi overlay{};
    if(selected.separateUi) {
      overlay.pixels=&images.nativeUi;
      for(unsigned eye=0;eye<2;++eye)overlay.views[eye]=fgschedule::view(batch.currentViews[eye],batch.currentViews[eye],1.f);
    }
    // Keep full-resolution native UI outside NGX, but publish one XR layer.
    // The renderer rotates B's coverage into this phase's projection before
    // composition. It does not invent a HUD depth or positional correction.
    const XrResult drawn=stereo.renderCaptured(views,seated.frame().space,pixels,world,
      nullptr,nullptr,nullptr,fade,nullptr,0.f,nullptr,0.f,false,nullptr,nullptr,true,
      selected.separateUi?&overlay:nullptr);
    hasUi=drawn==XR_SUCCESS&&selected.separateUi;return drawn;
  }
  void fgReport() {
    const uint64_t now=GetTickCount64();if(now-fgLastLineMs<1000)return;fgLastLineMs=now;
    nativeTracePrintf("native_fg_xr,multiplier=%u,native_credits=%llu,native_presented=%llu,generated_presented=%llu,repeated=%llu,empty=%llu,refused=%llu,display_ticks=%llu,source_sequence=%llu,displayed_endpoint=%llu,last_phase=%u,source_pose_time=%lld,source_age_ms=%.3f,delivery=app_xrEndFrame_not_scanout,private_native_etw_generated=0\n",
      fgMultiplier,(unsigned long long)fgCredits,(unsigned long long)fgNativePresented,(unsigned long long)fgGeneratedPresented,
      (unsigned long long)fgRepeated,(unsigned long long)fgEmpty,(unsigned long long)fgRefused,(unsigned long long)fgTick,
      (unsigned long long)activeFrameSequence,(unsigned long long)fgDisplayedSequence,fgLastPhase,
      (long long)fgLastSourceTime,double(fgLastAge)/1000000.);
    nativeTracePrintf("native_fg_ui,native_pose_composites=%llu,composition=single_projection_after_fg,ui_rotation_reprojected=1,positional_reprojection=0,content_cadence=native,projection_cadence=runtime,protected_coverage_includes_native_background=1\n",
      (unsigned long long)fgUiPresented);
  }
  void fgLifecycleReport(uint64_t now,HRESULT statusResult,const EdvrFgStatus& status,bool requested,bool callerFresh) {
    const bool armed=(statusResult==S_OK&&(status.requestedMultiplier==2||status.requestedMultiplier==4))||
      fgEnabled.load(std::memory_order_acquire);
    if(!armed&&!fgLifecycle.observing())return;
    const bool sceneReady=featureFrameKnown&&featureFrame.sceneReady;
    const bool loadingVisible=loading.visible(),enabled=fgEnabled.load(std::memory_order_acquire);
    const bool separate=separateGraphics(),vrProfile=edvr::vrProfileActive();
    const bool generationValid=status.generation==runtimeGeneration,configValid=status.configGeneration!=0;
    const bool multiplierValid=status.requestedMultiplier==2||status.requestedMultiplier==4;
    const bool stateValid=status.state==EDVR_FG_OFF||status.state==EDVR_FG_ARMING||status.state==EDVR_FG_READY;
    const bool trimmed=fovTrim.trimmed(),trimPending=fovTrim.pending();
    const bool zoomStrip=!(zoomStripNow<=0.f),eyeShare=featureFrameKnown&&featureFrame.version>=EDVR_NATIVE_FRAME_VERSION_10&&
      eyeshare::fromBits(featureFrame.leftEyeShare)>0.f;
    const bool deferred=featureFrameKnown&&featureFrame.deferredPacing;
    const auto& images=frameGeneration.images();
    const uint64_t refusalMask=uint64_t(statusResult!=S_OK)|(uint64_t(!generationValid)<<1)|(uint64_t(!configValid)<<2)|
      (uint64_t(!multiplierValid)<<3)|(uint64_t(!stateValid)<<4)|(uint64_t(!separate)<<5)|(uint64_t(!vrProfile)<<6)|
      (uint64_t(loadingVisible)<<7)|(uint64_t(!featureFrameKnown)<<8)|(uint64_t(!sceneReady)<<9)|
      (uint64_t(fgMenuVisible)<<10)|(uint64_t(frameWithheld)<<11)|(uint64_t(trimmed)<<12)|(uint64_t(trimPending)<<13)|
      (uint64_t(zoomAppliedNow)<<14)|(uint64_t(zoomStrip)<<15)|(uint64_t(circleNow.on)<<16)|(uint64_t(eyeShare)<<17)|
      (uint64_t(!callerFresh)<<18)|(uint64_t(deferred)<<19);
    fglifecycle::Key key;key.flags=refusalMask|(uint64_t(enabled)<<32);key.config=status.configGeneration;key.history=status.historyGeneration;
    key.state=fglifecycle::stateClass(status.state);key.failure=status.lastFailure;key.multiplier=status.requestedMultiplier;
    key.disableReason=fgLastDisableReason;key.status=int32_t(statusResult);
    const auto report=fgLifecycle.observe(now,armed,key);if(!report.emit)return;
    nativeTracePrintf("native_fg_lifecycle,observation=before_tick,changed=%u,transitions=%llu,suppressed_transitions=%llu,heartbeat_ms=1000,extra_transition_limit=4,status_hr=%08lx,state=%u,last_failure=%u,generation=%llu,config=%llu,history=%llu,requested_multiplier=%u,active_multiplier=%u,source_pairs=%llu,generated_pairs=%llu,suppressed_pairs=%llu,refused_pairs=%llu,busy_pairs=%llu,latest_source=%llu,take_latest_ready=%llu,admission=%u,enabled=%u,scene_known=%u,scene_ready=%u,loading=%u,menu=%u,withheld=%u,caller_fresh=%u,native_outstanding=%u,credit_pending=%u,frame_open=%u,pending_end=%u,images_ready=%u,image_sequence=%llu,image_history=%llu,image_next=%u,fallback_valid=%u,fallback_sequence=%llu,active_sequence=%llu,displayed_endpoint=%llu,credits=%llu,native_presented=%llu,generated_presented=%llu,repeated=%llu,empty=%llu,refused=%llu,display_ticks=%llu,disable_reason=%u,refusal_mask=%llx,separate_graphics=%u,vr_profile=%u,generation_valid=%u,config_valid=%u,multiplier_valid=%u,state_valid=%u,trimmed=%u,trim_pending=%u,zoom_applied=%u,zoom_strip=%u,circle=%u,eye_share=%u,deferred_pacing=%u\n",
      unsigned(report.changed),(unsigned long long)report.transitions,(unsigned long long)report.suppressed,
      (unsigned long)statusResult,status.state,status.lastFailure,(unsigned long long)status.generation,
      (unsigned long long)status.configGeneration,(unsigned long long)status.historyGeneration,status.requestedMultiplier,status.activeMultiplier,
      (unsigned long long)status.sourcePairs,(unsigned long long)status.generatedPairs,(unsigned long long)status.suppressedPairs,
      (unsigned long long)status.refusedPairs,(unsigned long long)status.busyPairs,(unsigned long long)status.latestSourceSequence,
      (unsigned long long)status.latestReadySequence,unsigned(requested),unsigned(enabled),unsigned(featureFrameKnown),unsigned(sceneReady),
      unsigned(loadingVisible),unsigned(fgMenuVisible),unsigned(frameWithheld),unsigned(callerFresh),unsigned(fgNativeOutstanding),
      unsigned(bool(fgCredit)),unsigned(state.frameOpen()),unsigned(pendingFrameEndFinish),unsigned(images.ready),
      (unsigned long long)images.metadata.currentSequence,(unsigned long long)images.metadata.historyGeneration,images.next,
      unsigned(fgFallbackValid),(unsigned long long)fgFallbackSequence,(unsigned long long)activeFrameSequence,
      (unsigned long long)fgDisplayedSequence,(unsigned long long)fgCredits,(unsigned long long)fgNativePresented,
      (unsigned long long)fgGeneratedPresented,(unsigned long long)fgRepeated,(unsigned long long)fgEmpty,
      (unsigned long long)fgRefused,(unsigned long long)fgTick,fgLastDisableReason,(unsigned long long)refusalMask,
      unsigned(separate),unsigned(vrProfile),unsigned(generationValid),unsigned(configValid),unsigned(multiplierValid),unsigned(stateValid),
      unsigned(trimmed),unsigned(trimPending),unsigned(zoomAppliedNow),unsigned(zoomStrip),unsigned(circleNow.on),unsigned(eyeShare),unsigned(deferred));
  }
  bool fgTickOwner(const RuntimeGate::Lease& operation) {
    if(!fgAvailable.load(std::memory_order_acquire)||!state.running()||state.terminal()||serviceStopped)return false;
    const uint64_t now=GetTickCount64();
    const bool callerFresh=now-gameFrameCallMs.load(std::memory_order_relaxed)<=500;
    if(!callerFresh&&fgNativeOutstanding) {fgNativeOutstanding=false;fgAcceptedEyes=0;frameGeneration.invalidate(10);}
    EdvrFgStatus status{};
    const HRESULT statusResult=frameGeneration.status(status);
    const bool requested=statusResult==S_OK&&status.generation==runtimeGeneration&&status.configGeneration&&
      (status.requestedMultiplier==2||status.requestedMultiplier==4)&&
      (status.state==EDVR_FG_OFF||status.state==EDVR_FG_ARMING||status.state==EDVR_FG_READY)&&separateGraphics()&&edvr::vrProfileActive()&&
      !loading.visible()&&featureFrameKnown&&featureFrame.sceneReady&&!fgMenuVisible&&!frameWithheld&&
      !fovTrim.trimmed()&&!fovTrim.pending()&&!zoomAppliedNow&&zoomStripNow<=0.f&&!circleNow.on&&
      !(featureFrameKnown&&featureFrame.version>=EDVR_NATIVE_FRAME_VERSION_10&&eyeshare::fromBits(featureFrame.leftEyeShare)>0.f)&&
      callerFresh&&
      !(featureFrameKnown&&featureFrame.deferredPacing);
    fgLifecycleReport(now,statusResult,status,requested,callerFresh);
    if(!requested) {
      if(fgEnabled.load(std::memory_order_acquire)&&!fgNativeOutstanding) {
        if(state.frameOpen())fgEndPrepared(nullptr,edvr::NativePresentationKind::Empty);
        fgDisable(3,operation);
      }
      if(!fgEnabled.load(std::memory_order_acquire))return false;
    }
    if(!fgEnabled.load(std::memory_order_acquire)) {
      if(state.frameOpen()||pendingFrameEndFinish)return false;
      if(frameGeneration.initializeConsumer(graphics.device(),graphics.context())!=S_OK||fgFallback.initialize(graphics.device())!=S_OK||
         fgWorldFallback.initialize(graphics.device())!=S_OK||fgUiFallback.initialize(graphics.device())!=S_OK){++fgRefused;return false;}
      fgMultiplier=status.requestedMultiplier;fgConfig=status.configGeneration;fgNextCredit=fgTick;
      fgSeedNativeFallback();
      fgEnabled.store(true,std::memory_order_release);service.setContinuousIdle(true);
      nativeTracePrintf("native_fg_xr,enabled=1,multiplier=%u,single_owner=1,turbo=0,consumer_copies_all_phases=1\n",fgMultiplier);
    }
    if((status.configGeneration!=fgConfig||status.requestedMultiplier!=fgMultiplier)&&!fgNativeOutstanding) {
      if(state.frameOpen())fgEndPrepared(nullptr,edvr::NativePresentationKind::Empty);
      fgDisable(4,operation);return true;
    }
    fgBootstrap.sync(fgBootstrapEpoch(status));
    if(!state.frameOpen()) {
      if(!poseSeatReady()){fgCancelCredit();return true;}
      const XrResult waited=boundary.waitAndBegin(FramePacing::Runtime);
      if(waited!=XR_SUCCESS&&waited!=XR_FRAME_DISCARDED&&waited!=XR_SESSION_LOSS_PENDING) {
        publishFatalFailure(waited,"native_fg_wait");fgCancelCredit();return true;
      }
      ++fgTick;
      // Return to the same owner's queue with this real runtime frame open.
      // Native submits/Wait registration queued during xrWaitFrame now run
      // before its prepared presentation; no extra XR frame is invented.
      return true;
    }
    const bool creditDue=fgCredit&&!fgNativeOutstanding&&fgTick>=fgNextCredit;
    const Frame tick=boundary.frame();
    if(creditDue) {
      auto request=std::move(fgCredit);const uint64_t before=lastPublishedFrameSequence;
      fgPreparingCredit=true;fgNativePixels=false;
      fgRunCredit(request,operation);fgPreparingCredit=false;
      fgNativeOutstanding=lastPublishedFrameSequence>before&&poses.read().sequence==activeFrameSequence&&frameGeometryAvailable;
      fgAcceptedEyes=0;fgNextCredit=fgschedule::nextCredit(fgTick,fgMultiplier);
      if(fgNativeOutstanding)++fgCredits;
    }
    if(!state.frameOpen())return true; // a refused credit closed its frame
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    bool hasUi=false;
    bool pixels=false,generated=false,newNative=false;
    auto& images=frameGeneration.images();
    bool imported=false;
    if(!images.ready) {
      const HRESULT polled=frameGeneration.poll(runtimeGeneration,poses.read().originGeneration,fgConfig,fgMultiplier,
        sizes[0].recommendedImageRectWidth,sizes[0].recommendedImageRectHeight,tick.predictedDisplayTime);
      if(FAILED(polled))++fgRefused;
      if(!frameGeneration.healthy()) {
        fgEndPrepared(nullptr,edvr::NativePresentationKind::Empty);publishFatalFailure(XR_ERROR_RUNTIME_FAILURE,"native_fg_copy_retirement");return true;
      }
      imported=images.ready;
    }
    fgPrepareImages(status,tick.predictedDisplayTime,imported);
    if(tick.shouldRender&&images.ready) {
      const auto& b=images.metadata;
      // A comfort fade must cover the full image. Use the already-composited
      // native endpoint while fading instead of an opaque auxiliary HUD.
      const unsigned phase=comfortFade()>0.f?0u:images.next;
      fgLastPhase=phase;fgLastSourceTime=fgschedule::phaseTime(b,phase);fgLastAge=tick.predictedDisplayTime-fgLastSourceTime;
      const XrResult drawn=fgRenderPrepared(phase,layer,hasUi);
      pixels=drawn==XR_SUCCESS;generated=phase!=0;newNative=!phase;
      if(pixels) {
        if(phase)images.next=fgschedule::nextPhase(phase,b.generatedCount);
        else {
          fgFallbackValid=images.phases[0].exchangeBuffers(fgFallback);
          fgSplitFallbackValid=false;
          if(b.uiFlags){
            const bool worldKept=images.nativeWorld.exchangeBuffers(fgWorldFallback);
            const bool uiKept=images.nativeUi.exchangeBuffers(fgUiFallback);
            fgSplitFallbackValid=worldKept&&uiKept;
          }
          for(unsigned eye=0;eye<2;++eye)fgFallbackViews[eye]=fgschedule::view(b.currentViews[eye],b.currentViews[eye],1.f);
          fgFallbackSpace=seated.frame().space;
          fgFallbackTime=b.currentPoseDisplayTime;fgFallbackReference=b.referenceGeneration;
          fgFallbackSequence=b.currentSequence;fgDisplayedSequence=b.currentSequence;fgHistory=b.historyGeneration;images.ready=false;
        }
      } else {images.ready=false;++fgRefused;publishFatalFailure(drawn,"native_fg_stereo");return true;}
    }
    if(tick.shouldRender&&!pixels&&fgFallbackUsable(tick.predictedDisplayTime)) {
      const bool split=fgSplitFallbackValid&&comfortFade()<=0.f;
      StereoNativeUi overlay{};
      if(split){overlay.pixels=&fgUiFallback;for(unsigned eye=0;eye<2;++eye)overlay.views[eye]=fgFallbackViews[eye];}
      const XrResult drawn=stereo.renderCaptured(fgFallbackViews,fgFallbackSpace,split?fgWorldFallback:fgFallback,layer,
        nullptr,nullptr,nullptr,comfortFade(),nullptr,0.f,nullptr,0.f,false,nullptr,nullptr,true,split?&overlay:nullptr);
      if(drawn!=XR_SUCCESS){publishFatalFailure(drawn,"native_fg_fallback");return true;}
      hasUi=split;
      pixels=true;
      fgLastPhase=0;fgLastSourceTime=fgFallbackTime;fgLastAge=tick.predictedDisplayTime-fgFallbackTime;
      generated=false;newNative=fgDisplayedSequence!=fgFallbackSequence;
      if(pixels)fgDisplayedSequence=fgFallbackSequence;
    }
    const auto kind=!pixels?edvr::NativePresentationKind::Empty:
      generated?edvr::NativePresentationKind::Generated:
      newNative?edvr::NativePresentationKind::Native:edvr::NativePresentationKind::Repeated;
    const XrResult ended=fgEndPrepared(pixels?&layer:nullptr,kind);
    if(ended!=XR_SUCCESS){publishFatalFailure(ended,"native_fg_end");fgCancelCredit();return true;}
    if(hasUi)++fgUiPresented;
    if(!pixels)++fgEmpty;else if(generated)++fgGeneratedPresented;else if(newNative)++fgNativePresented;else ++fgRepeated;
    fgReport();return true;
  }
  vr::EVRCompositorError fgSubmitNative(vr::EVREye eye,const vr::Texture_t* texture,
      const vr::VRTextureBounds_t* bounds,vr::EVRSubmitFlags flags) {
    LARGE_INTEGER began{},ended{};const bool clock=counterNow(&began);
    const unsigned bit=1u<<unsigned(eye);
    if(!fgNativeOutstanding||(fgAcceptedEyes&bit))return vr::VRCompositorError_InvalidTexture;
    const auto admitted=capture(eye,texture,bounds,flags,fgNativePixels);
    if(admitted==vr::VRCompositorError_None)fgAcceptedEyes|=bit;
    dispatchGpuEyeMarker(eye,texture,admitted==vr::VRCompositorError_None);
    if(clock&&counterNow(&ended))timingFrame.submitMs[unsigned(eye)]=elapsedMs(began,ended);
    if(admitted!=vr::VRCompositorError_None){frameGeneration.invalidate(5);fgNativeOutstanding=false;fgAcceptedEyes=0;++fgRefused;return finishSubmitTail(eye,admitted);}
    if(fgAcceptedEyes==3) {
      // Preserve the shared capture's existing completion/retirement cadence
      // even after bootstrap stops making extra fallback copies.
      const bool pairedReady=!frameWithheld&&fgNativePixels&&captured.completePending(nullptr,&transferWall)==S_OK;
      if(pairedReady&&fgBootstrap.refreshing()) {
        EdvrFgStatus fallbackStatus{};
        if(frameGeneration.status(fallbackStatus)==S_OK)fgRefreshNativeFallback(fgBootstrapEpoch(fallbackStatus),pairedReady);
      }
      fgNativeOutstanding=false;fgAcceptedEyes=0;
    }
    return finishSubmitTail(eye,admitted);
  }

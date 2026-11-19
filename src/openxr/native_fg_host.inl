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
    const uint64_t sequence=boundary.frame().sequence+frameSequenceOffset;
    const auto* fps=pixels?fpsOverlay():nullptr;
    const XrResult result=boundary.prepared(layer,nullptr,fps);
    notePresentation(result!=XR_SUCCESS?edvr::NativePresentationKind::Failed:
        pixels?kind:edvr::NativePresentationKind::Empty);
    if(fps&&result==XR_SUCCESS)reportFpsOverlay(kind,sequence,fgMultiplier);
    return result;
  }
  void fgRunCredit(const std::shared_ptr<FgCreditRequest>& request,const RuntimeGate::Lease& operation) {
    if(!fgCreditLoan.run(gate,operation,runtimeGeneration,service.isOwner()&&GetCurrentThreadId()==ownerThread,
       [&]{request->run();}))request->finish(false);
  }
  void fgDisable(uint32_t reason,const RuntimeGate::Lease& operation) {
    fgLastDisableReason=reason;
    fgBootstrap.clear();
    fgPendingNativePose={};
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
    if(frameGeneration.status(status)!=S_OK||!edvr::nativeFgFixedMultiplier(status.requestedMultiplier)||
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
  void fgStageNativeFallback(const fgbootstrap::Epoch& epoch) {
    if(!epoch.valid()||epoch.generation!=runtimeGeneration||epoch.config!=fgConfig||
       epoch.reference!=poses.read().originGeneration)return;
    fgBootstrap.sync(epoch);
    const XrTime sourceTime=frameGeometry.displayTime;
    if(frameWithheld||!fgNativePixels||!frameGeometryAvailable||!frameSpace||
       activeFrameSequence<=fgDisplayedSequence||activeFrameSequence<=fgBootstrap.completedSequence()||sourceTime<=0||
       sourceTime>boundary.frame().predictedDisplayTime||
       (fgFallbackValid&&(activeFrameSequence<=fgFallbackSequence||sourceTime<=fgFallbackTime)))return;
    // Keep the complete native pair independently of the next credit and of
    // the import buffers. A partial capture cannot replace the shown pair.
    fgPendingNativePose={};
    for(unsigned eye=0;eye<2;++eye) {
      const vr::Texture_t source{captured.texture(vr::EVREye(eye)),vr::API_DirectX,captured.colorSpace(vr::EVREye(eye))};
      const auto region=captured.bounds(vr::EVREye(eye));
      if(fgPendingNative.capture(vr::EVREye(eye),&source,&region)!=vr::VRCompositorError_None)return;
    }
    fgPendingNativePose.epoch=epoch;
    fgPendingNativePose.space=frameSpace;
    fgPendingNativePose.time=sourceTime;
    fgPendingNativePose.sequence=activeFrameSequence;
    for(unsigned eye=0;eye<2;++eye) {
      auto& view=fgPendingNativePose.views[eye];
      view=frameViews[eye];view.pose=seatxf::toNatural(frameOrigin,frameViews[eye].pose);
    }
    fgBootstrap.completeNative(activeFrameSequence,fgTick);
  }
  void fgRefreshNativeFallback(XrTime displayTime) {
    const auto& source=fgPendingNativePose;
    if(!fgBootstrap.refreshDue(fgTick)||!(source.epoch==fgBootstrap.epoch())||
       source.sequence<=fgDisplayedSequence||source.time>displayTime||
       changes.crosses(source.time,displayTime)||source.space!=seated.frame().space||
       !fgPendingNative.exchangeBuffers(fgFallback))return;
    fgBootstrap.displayNative(source.sequence);
    fgSplitFallbackValid=false;
    for(unsigned eye=0;eye<2;++eye)fgFallbackViews[eye]=source.views[eye];
    fgFallbackSpace=source.space;fgFallbackTime=source.time;fgFallbackSequence=source.sequence;
    fgFallbackReference=source.epoch.reference;fgFallbackValid=true;
  }
  fghandoff::Images fgPrepareImages(const EdvrFgStatus& status,XrTime displayTime,XrDuration period,bool imported) {
    using Outcome=fghandoff::Images;
    auto& images=frameGeneration.images();
    if(images.ready&&(images.metadata.generation!=runtimeGeneration||images.metadata.referenceGeneration!=poses.read().originGeneration||
       images.metadata.configGeneration!=fgConfig||images.metadata.historyGeneration!=status.historyGeneration||
       changes.crosses(images.metadata.generatedCount?images.metadata.previousPoseDisplayTime:images.metadata.currentPoseDisplayTime,displayTime))) {
      images.ready=false;++fgRefused;return Outcome::WrongEpoch;
    }
    if(images.ready&&(images.metadata.currentPoseDisplayTime>displayTime||
       displayTime-images.metadata.currentPoseDisplayTime>500000000)) {
      images.ready=false;++fgRefused;return Outcome::OutsideWindow;
    }
    if(images.ready) {
      // A timely imported result proves that synthesis works in this epoch,
      // even if native fallback already presented that particular endpoint.
      const auto& b=images.metadata;
      if(imported) {
        const bool timely=fgbootstrap::interpolationFits(displayTime,b.currentPoseDisplayTime,period,b.multiplier,1);
        if(b.generatedCount&&timely)fgBootstrap.observe(b,true);
        else fgBootstrap.miss();
      }
      if(b.currentSequence<=fgDisplayedSequence){images.ready=false;return Outcome::BehindWatermark;}
      if(imported)images.next=fgschedule::firstPhase(b,fgDisplayedSequence);
      if(images.ready&&images.next&&
         !fgbootstrap::interpolationFits(displayTime,b.currentPoseDisplayTime,period,b.multiplier,images.next)) {
        images.next=0;fgBootstrap.miss();return Outcome::Late;
      }
      if(images.next)return Outcome::Generated;
      if(!b.generatedCount)return Outcome::NativeOnly;
      if(imported)return Outcome::PreviousMismatch;
      return Outcome::Endpoint;
    }
    return Outcome::None;
  }
  void fgFinishHandoffTrace(const char* reason) {
    fgHandoffTraceAttempted=false;
    if(!fgHandoffTrace)return;
    fgHandoffTrace->finish([&](const fghandoff::Sample* rows,std::size_t count,std::size_t lost) {
      // The bounded capture did no IO. Write the whole block once, off the
      // capture interval. Row clocks are QPC microseconds, not log UTC time.
      NativeTraceBatch block;
      nativeTracePrintf("native_fg_handoff_summary,reason=%s,rows=%llu,dropped=%llu,clock=qpc_us,pose_clock=xr_ns,"
        "import=none:0/retire:1/provider:2/future:3/fence:4/imported:5/rejected:6,"
        "images=none:0/generated:1/endpoint:2/wrong_scope:3/behind:4/previous_mismatch:5/late:6/native_only:7/outside_window:8,"
        "slot=empty:0/generated:1/native:2/repeat:3/refused_credit:4\n",
        reason,(unsigned long long)count,(unsigned long long)lost);
      for(std::size_t i=0;i<count;++i) {
        const auto& row=rows[i];
        nativeTracePrintf("native_fg_handoff,kind=%s,time_us=%llu,tick=%llu,generation=%llu,config=%llu,history=%llu,"
          "source=%llu,frame_open=%u,staged=%u,should_render=%u,native_pending=%llu,watermark=%llu,"
          "latest_source=%llu,latest_exported=%llu,poll_begin_us=%llu,poll_end_us=%llu,import=%u,poll_hr=%08lx,poll_source=%llu,poll_pose_time_ns=%lld,"
          "batch_previous=%llu,batch_current=%llu,display_time_ns=%lld,batch_or_source_time_ns=%lld,runtime_period_ns=%lld,admit_period_ns=%lld,"
          "images=%u,fallback_before=%llu,fallback_after=%llu,slot_begin_us=%llu,slot=%u,phase=%u,presented=%llu,multiplier=%u\n",
          row.completion?"completion":"slot",(unsigned long long)row.timeUs,(unsigned long long)row.tick,
          (unsigned long long)row.generation,(unsigned long long)row.config,(unsigned long long)row.history,
          (unsigned long long)row.source,unsigned(row.frameOpen),unsigned(row.staged),unsigned(row.shouldRender),
          (unsigned long long)row.pendingNative,(unsigned long long)row.watermark,
          (unsigned long long)row.latestSource,(unsigned long long)row.latestReady,
          (unsigned long long)row.pollBeginUs,(unsigned long long)row.pollEndUs,unsigned(row.poll.result),
          (unsigned long)(uint32_t)row.pollHr,(unsigned long long)row.poll.sequence,(long long)row.poll.poseTime,
          (unsigned long long)row.previous,(unsigned long long)row.current,(long long)row.displayTime,(long long)row.sourceTime,
          (long long)row.runtimePeriod,(long long)row.admissionPeriod,unsigned(row.images),
          (unsigned long long)row.fallbackBefore,(unsigned long long)row.fallbackAfter,
          (unsigned long long)row.slotBeginUs,unsigned(row.slot),row.phase,(unsigned long long)row.presented,row.multiplier);
      }
    });
    fgHandoffTrace.reset();
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
    const bool armed=(statusResult==S_OK&&edvr::nativeFgFixedMultiplier(status.requestedMultiplier))||
      fgEnabled.load(std::memory_order_acquire);
    if(!armed&&!fgLifecycle.observing())return;
    const bool sceneReady=featureFrameKnown&&featureFrame.sceneReady;
    const bool loadingVisible=loading.visible(),enabled=fgEnabled.load(std::memory_order_acquire);
    const bool separate=separateGraphics(),vrProfile=edvr::vrProfileActive();
    const bool generationValid=status.generation==runtimeGeneration,configValid=status.configGeneration!=0;
    const bool multiplierValid=edvr::nativeFgFixedMultiplier(status.requestedMultiplier);
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
    if(fgEnabled.load(std::memory_order_acquire)&&activeFrameSequence!=vrsRejectedSequence&&
       rejectIncompleteVrsSource())return true;
    const uint64_t now=GetTickCount64();
    const bool callerFresh=now-gameFrameCallMs.load(std::memory_order_relaxed)<=500;
    if(!callerFresh&&fgNativeOutstanding) {fgNativeOutstanding=false;fgAcceptedEyes=0;frameGeneration.invalidate(10);}
    EdvrFgStatus status{};
    const HRESULT statusResult=frameGeneration.status(status);
    const bool requested=statusResult==S_OK&&status.generation==runtimeGeneration&&status.configGeneration&&
      edvr::nativeFgFixedMultiplier(status.requestedMultiplier)&&
      (status.state==EDVR_FG_OFF||status.state==EDVR_FG_ARMING||status.state==EDVR_FG_READY)&&separateGraphics()&&edvr::vrProfileActive()&&
      !loading.visible()&&featureFrameKnown&&featureFrame.sceneReady&&!fgMenuVisible&&!frameWithheld&&
      !fovTrim.trimmed()&&!fovTrim.pending()&&!zoomAppliedNow&&zoomStripNow<=0.f&&!circleNow.on&&
      !(featureFrameKnown&&featureFrame.version>=EDVR_NATIVE_FRAME_VERSION_10&&eyeshare::fromBits(featureFrame.leftEyeShare)>0.f)&&
      callerFresh&&
      !(featureFrameKnown&&featureFrame.deferredPacing);
    if(requested||fgHandoffTraceAttempted) {
      if(temporal.fgHandoffTraceRequested()) {
        if(!fgHandoffTraceAttempted) {
          fgHandoffTraceAttempted=true;
          // Standalone rigs and normal sessions do not reserve the buffer.
          fgHandoffTrace.reset(new(std::nothrow) fghandoff::Recorder<768>);
          if(fgHandoffTrace)fgHandoffTrace->arm();
          else nativeTracePuts("native_fg_handoff_summary,reason=allocation_failed,rows=0,dropped=0");
        }
      } else fgFinishHandoffTrace("disabled");
    }
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
         fgPendingNative.initialize(graphics.device())!=S_OK||
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
    const Frame tick=boundary.frame();
    const bool tracingHandoff=fgHandoffTrace&&fgHandoffTrace->sampleWanted();
    fghandoff::Sample row{};
    if(tracingHandoff) {
      row.tick=fgTick;row.frameOpen=true;row.shouldRender=tick.shouldRender;
      row.generation=runtimeGeneration;row.config=fgConfig;row.history=status.historyGeneration;
      row.source=activeFrameSequence;row.pendingNative=fgBootstrap.completedSequence();
      row.latestSource=status.latestSourceSequence;row.latestReady=status.latestReadySequence;
      row.watermark=fgDisplayedSequence;row.fallbackBefore=fgFallbackValid?fgFallbackSequence:0;
      row.displayTime=tick.predictedDisplayTime;row.runtimePeriod=tick.predictedDisplayPeriod;row.multiplier=fgMultiplier;
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    bool hasUi=false;
    bool pixels=false,generated=false,newNative=false;
    auto& images=frameGeneration.images();
    bool imported=false;
    if(!images.ready) {
      if(tracingHandoff)row.pollBeginUs=edvrNativeTraceNowUs();
      const HRESULT polled=frameGeneration.poll(runtimeGeneration,poses.read().originGeneration,fgConfig,fgMultiplier,
        sizes[0].recommendedImageRectWidth,sizes[0].recommendedImageRectHeight,tick.predictedDisplayTime,
        tracingHandoff?&row.poll:nullptr);
      if(tracingHandoff){row.pollEndUs=edvrNativeTraceNowUs();row.pollHr=polled;}
      if(FAILED(polled))++fgRefused;
      if(!frameGeneration.healthy()) {
        fgEndPrepared(nullptr,edvr::NativePresentationKind::Empty);publishFatalFailure(XR_ERROR_RUNTIME_FAILURE,"native_fg_copy_retirement");return true;
      }
      imported=images.ready;
    }
    // The runtime can report a predicted period at application cadence. Use
    // its separately published display frequency when available.
    const XrDuration period=instrumentDisplayHz>=20.&&instrumentDisplayHz<=250.
      ? XrDuration(1000000000./instrumentDisplayHz+.5) : tick.predictedDisplayPeriod;
    if(tracingHandoff&&images.ready) {
      row.previous=images.metadata.previousSequence;row.current=images.metadata.currentSequence;
      row.sourceTime=images.metadata.currentPoseDisplayTime;
    }
    const auto imageDecision=fgPrepareImages(status,tick.predictedDisplayTime,period,imported);
    const bool generatedSlot=requested&&tick.shouldRender&&images.ready&&images.next&&comfortFade()<=0.f;
    if(images.ready&&!generatedSlot) {
      if(images.next)fgBootstrap.miss();
      images.next=0;
      // Don't let a suppressed, late or native-only batch take precedence
      // over a newer completion already available on the native path.
      if((fgFallbackValid&&fgFallbackSequence>images.metadata.currentSequence)||
         (fgBootstrap.refreshDue(fgTick)&&fgBootstrap.completedSequence()>images.metadata.currentSequence))images.ready=false;
    }
    if(tick.shouldRender&&!images.ready&&fgBootstrap.refreshDue(fgTick)) {
      fgBootstrap.miss();
      fgRefreshNativeFallback(tick.predictedDisplayTime);
    }
    if(tracingHandoff) {
      row.admissionPeriod=period;row.images=imageDecision;
      row.fallbackAfter=fgFallbackValid?fgFallbackSequence:0;
    }
    // Poll and choose this slot before releasing the caller. A missing phase
    // releases the credit immediately; an old nextCredit cannot throttle it.
    if(fgCredit&&!fgNativeOutstanding&&fgbootstrap::creditDue(fgTick,fgNextCredit,generatedSlot)) {
      auto request=std::move(fgCredit);const uint64_t before=lastPublishedFrameSequence;
      fgPreparingCredit=true;fgNativePixels=false;
      fgRunCredit(request,operation);fgPreparingCredit=false;
      fgNativeOutstanding=lastPublishedFrameSequence>before&&poses.read().sequence==activeFrameSequence&&frameGeometryAvailable;
      fgAcceptedEyes=0;
      if(fgNativeOutstanding){fgNextCredit=fgschedule::nextCredit(fgTick,fgMultiplier);++fgCredits;}
    }
    if(!state.frameOpen()) {
      if(tracingHandoff){row.timeUs=edvrNativeTraceNowUs();row.slot=fghandoff::Slot::RefusedCredit;fgHandoffTrace->append(row);}
      return true; // a refused credit closed its frame
    }
    if(tracingHandoff)row.slotBeginUs=edvrNativeTraceNowUs();
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
          fgBootstrap.displayNative(b.currentSequence);
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
      if(pixels){fgDisplayedSequence=fgFallbackSequence;fgBootstrap.displayNative(fgFallbackSequence);}
    }
    const auto kind=!pixels?edvr::NativePresentationKind::Empty:
      generated?edvr::NativePresentationKind::Generated:
      newNative?edvr::NativePresentationKind::Native:edvr::NativePresentationKind::Repeated;
    const XrResult ended=fgEndPrepared(pixels?&layer:nullptr,kind);
    if(ended!=XR_SUCCESS){publishFatalFailure(ended,"native_fg_end");fgCancelCredit();return true;}
    if(tracingHandoff) {
      row.timeUs=edvrNativeTraceNowUs();row.phase=pixels?fgLastPhase:0;
      row.presented=pixels?(generated?row.current:fgDisplayedSequence):0;
      row.slot=!pixels?fghandoff::Slot::Empty:generated?fghandoff::Slot::Generated:
        newNative?fghandoff::Slot::Native:fghandoff::Slot::Repeat;
      fgHandoffTrace->append(row);
    }
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
    if(rejectIncompleteVrsSource())return vr::VRCompositorError_InvalidTexture;
    if(admitted==vr::VRCompositorError_None)fgAcceptedEyes|=bit;
    dispatchGpuEyeMarker(eye,texture,admitted==vr::VRCompositorError_None);
    if(clock&&counterNow(&ended))timingFrame.submitMs[unsigned(eye)]=elapsedMs(began,ended);
    if(admitted!=vr::VRCompositorError_None){frameGeneration.invalidate(5);fgNativeOutstanding=false;fgAcceptedEyes=0;++fgRefused;return finishSubmitTail(eye,admitted);}
    if(fgAcceptedEyes==3) {
      // Preserve the shared capture's existing completion/retirement cadence
      // even after bootstrap stops making extra fallback copies.
      const bool pairedReady=!frameWithheld&&fgNativePixels&&captured.completePending(nullptr,&transferWall)==S_OK;
      if(pairedReady) {
        const bool tracingHandoff=fgHandoffTrace&&fgHandoffTrace->sampleWanted();
        const uint64_t timeUs=tracingHandoff?edvrNativeTraceNowUs():0;
        EdvrFgStatus fallbackStatus{};
        if(frameGeneration.status(fallbackStatus)==S_OK) {
          fgStageNativeFallback(fgBootstrapEpoch(fallbackStatus));
        }
        if(tracingHandoff) {
          const auto slot=boundary.frame();
          fghandoff::Sample row{};row.completion=true;row.timeUs=timeUs;row.tick=fgTick;
          row.frameOpen=state.frameOpen();row.shouldRender=slot.shouldRender;
          row.generation=runtimeGeneration;row.config=fgConfig;row.history=fallbackStatus.historyGeneration;
          row.source=activeFrameSequence;row.pendingNative=fgBootstrap.completedSequence();row.watermark=fgDisplayedSequence;
          row.staged=fgPendingNativePose.sequence==activeFrameSequence;
          row.displayTime=slot.predictedDisplayTime;row.sourceTime=frameGeometry.displayTime;
          row.runtimePeriod=slot.predictedDisplayPeriod;row.multiplier=fgMultiplier;
          fgHandoffTrace->append(row);
        }
      }
      fgNativeOutstanding=false;fgAcceptedEyes=0;
    }
    return finishSubmitTail(eye,admitted);
  }

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
    if(!edvr::recordNativePresentation(presentationCounts,runtimeGeneration,kind,
        fgEnabled.load(std::memory_order_acquire),fgMultiplier))return;
    timing.publishPresentation(presentationCounts); // CPU-only optional capability
    // Sample the same end-frame counters on both routes. Continue after Off
    // for a matched native baseline; a cold Off session needs no heartbeat.
    const auto& p=presentationCounts;
    if(p.fgEnabled)fgPresentationGeneration=runtimeGeneration;
    if(fgPresentationGeneration!=runtimeGeneration)return;
    const auto now=GetTickCount64();if(now-fgPresentationLastMs<1000)return;
    fgPresentationLastMs=now;
    nativeTracePrintf("native_xr_presentation,owner_ms=%llu,generation=%llu,sequence=%llu,fg_enabled=%u,multiplier=%u,native=%llu,generated=%llu,repeated=%llu,empty=%llu,background=%llu,failed=%llu\n",
      (unsigned long long)now,(unsigned long long)p.generation,(unsigned long long)p.sequence,p.fgEnabled,p.activeMultiplier,
      (unsigned long long)p.nativeFrames,(unsigned long long)p.generatedFrames,(unsigned long long)p.repeatedFrames,
      (unsigned long long)p.emptyFrames,(unsigned long long)p.backgroundFrames,(unsigned long long)p.failedFrames);
  }
  XrResult fgEndPrepared(const XrCompositionLayerProjection* layer,edvr::NativePresentationKind kind) {
    const bool pixels=layer&&boundary.frame().shouldRender&&state.frameShouldRender()&&
        !state.terminal()&&state.lifecycle()!=Lifecycle::Stopping;
    const auto* fps=pixels?fpsOverlay():nullptr;
    const XrResult result=boundary.prepared(layer,nullptr,fps);
    notePresentation(result!=XR_SUCCESS?edvr::NativePresentationKind::Failed:
        pixels?kind:edvr::NativePresentationKind::Empty);
    return result;
  }
  void fgRunCredit(const std::shared_ptr<FgCreditRequest>& request,const RuntimeGate::Lease& operation) {
    if(!fgCreditLoan.run(gate,operation,runtimeGeneration,service.isOwner()&&GetCurrentThreadId()==ownerThread,
       [&]{request->run();}))request->finish(false);
  }
  void fgDisable(uint32_t reason,const RuntimeGate::Lease& operation,bool resumeCredit=false,bool discardDrawn=false) {
    fgResumedNative=false;
    const auto source=poses.read();
    const bool retain=resumeCredit&&fgNativeOutstanding&&source.posesAvailable&&source.sequence==activeFrameSequence&&
      frameGeometryAvailable&&frameSpace==seated.frame().space&&fgBootstrap.epoch().reference==source.originGeneration;
    // A prepared draw already ran its plugin pass. Close that slot before
    // acquiring the one in which an outstanding native credit can finish.
    if(state.frameOpen()&&(!retain||discardDrawn)) {
      const auto ended=fgEndPrepared(nullptr,edvr::NativePresentationKind::Empty);
      if(ended!=XR_SUCCESS){publishFatalFailure(ended,"native_fg_disable");return;}
    }
    if(retain) {
      if(!state.frameOpen()) {
        const auto waited=boundary.waitAndBegin(FramePacing::Runtime);
        if(waited!=XR_SUCCESS&&waited!=XR_FRAME_DISCARDED&&waited!=XR_SESSION_LOSS_PENDING) {
          publishFatalFailure(waited,"native_fg_native_handoff");return;
        }
      }
      const auto slot=boundary.frame();
      if(!changes.crosses(frameGeometry.displayTime,slot.predictedDisplayTime))
        fgResumedNative=boundary.resumeNativeCredit(slot,fgAcceptedEyes,fgNativePixels);
      if(!fgResumedNative) {
        const auto ended=fgEndPrepared(nullptr,edvr::NativePresentationKind::Empty);
        if(ended!=XR_SUCCESS){publishFatalFailure(ended,"native_fg_disable");return;}
      }
    }
    if(fgNativeOutstanding&&!fgResumedNative) {
      clearReflexFinalEyes(activeFrameSequence);invalidateEyeTreatments();timingInvalidate();temporalFrameEyes=0;
    }
    fgLastDisableReason=reason;
    fgBootstrap.clear();
    fgPendingNativePose={};
    fgEnabled.store(false,std::memory_order_release);service.setContinuousIdle(false);
    fgNativeOutstanding=false;fgAcceptedEyes=0;fgFallbackValid=false;fgDisplayedSequence=0;fgHistory=0;
    fgSplitFallbackValid=false;
    frameGeneration.invalidate(reason);
    if(fgCredit){auto request=std::move(fgCredit);fgRunCredit(request,operation);}
  }
  HRESULT fgBeginSource(const Frame& frame,const GeometryInput& located) {
    if(fgMenuVisible||frameWithheld||fovTrim.trimmed()||fovTrim.pending()||zoomAppliedNow||zoomStripNow>0.f||circleNow.on||
       (featureFrameKnown&&featureFrame.version>=EDVR_NATIVE_FRAME_VERSION_10&&eyeshare::fromBits(featureFrame.leftEyeShare)>0.f)||!frameSpace||
       sizes[0].recommendedImageRectWidth!=sizes[1].recommendedImageRectWidth||
       sizes[0].recommendedImageRectHeight!=sizes[1].recommendedImageRectHeight)return E_INVALIDARG;
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
    return frameGeneration.begin(source);
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
    if(FAILED(fgBeginSource(frame,located))) {frameGeneration.invalidate(2);++fgRefused;}
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
    fgBootstrap.completeNative(activeFrameSequence);
  }
  bool fgPendingNativeUsable(XrTime displayTime) const {
    const auto& source=fgPendingNativePose;
    return source.sequence==fgBootstrap.completedSequence()&&source.epoch==fgBootstrap.epoch()&&
      source.sequence>fgDisplayedSequence&&source.time>0&&source.time<=displayTime&&
      !changes.crosses(source.time,displayTime)&&source.space==seated.frame().space;
  }
  bool fgDeferNativeFallback(const EdvrFgStatus& status,XrTime displayTime,XrDuration nextPeriod,XrDuration period,
      fghandoff::Handoff* observation=nullptr) {
    using fghandoff::Handoff;
    if(!fgPendingNativeUsable(displayTime)||fgFallbackSequence!=fgDisplayedSequence)
      return fghandoff::recordHandoff(observation,Handoff::SourceIdentity);
    if(!fgFallbackUsable(displayTime))return fghandoff::recordHandoff(observation,Handoff::FallbackCoverage);
    return fgBootstrap.deferNative(fgDisplayedSequence,status.latestSourceSequence,status.latestReadySequence,
      fgPendingNativePose.time,displayTime,nextPeriod,period,fgMultiplier,observation);
  }
  void fgRefreshNativeFallback(XrTime displayTime) {
    const auto& source=fgPendingNativePose;
    if(!fgPendingNativeUsable(displayTime)||!fgPendingNative.exchangeBuffers(fgFallback))return;
    // The credit handler may close this XR frame before presentation. The
    // staged buffers have still been moved and must never be exchanged twice.
    fgBootstrap.consumeNative(source.sequence);
    fgSplitFallbackValid=false;
    for(unsigned eye=0;eye<2;++eye)fgFallbackViews[eye]=source.views[eye];
    fgFallbackSpace=source.space;fgFallbackTime=source.time;fgFallbackSequence=source.sequence;
    fgFallbackReference=source.epoch.reference;fgFallbackValid=true;
  }
  uint64_t fgPhaseSalvaged=0,fgPhasesExhausted=0;
  fghandoff::Images fgPrepareImages(const EdvrFgStatus& status,XrTime displayTime,XrDuration period,bool imported,
      std::optional<EdvrFgBatch>* behindReceipt=nullptr) {
    const auto behind=behindReceipt?std::exchange(*behindReceipt,std::nullopt):std::nullopt;
    using Outcome=fghandoff::Images;
    auto& images=frameGeneration.images();
    if(!images.ready&&!behind)return Outcome::None;
    const auto& b=behind?*behind:images.metadata;
    if(b.generation!=runtimeGeneration||b.referenceGeneration!=poses.read().originGeneration||
       b.configGeneration!=fgConfig||b.historyGeneration!=status.historyGeneration||
       changes.crosses(b.generatedCount?b.previousPoseDisplayTime:b.currentPoseDisplayTime,displayTime)) {
      images.ready=false;++fgRefused;return Outcome::WrongEpoch;
    }
    if(b.currentPoseDisplayTime>displayTime||displayTime-b.currentPoseDisplayTime>500000000) {
      images.ready=false;++fgRefused;return Outcome::OutsideWindow;
    }
    if(b.currentSequence<=fgDisplayedSequence) {
      if(imported||behind)fgBootstrap.observeBehind(b,
        fgBootstrap.phaseFits(displayTime,b.currentPoseDisplayTime,period,b.multiplier,1),fgTick,displayTime,period);
      images.ready=false;
      return Outcome::BehindWatermark;
    }
    if(images.ready) {
      if(imported)images.next=fgschedule::firstPhase(b,fgDisplayedSequence);
      if(images.next&&b.previousSequence!=fgDisplayedSequence) {
        images.next=0;return Outcome::PreviousMismatch;
      }
      if(images.next) {
        // A later SDK index has its own pose and deadline. Keep its original
        // place in the pair even when the runtime missed an earlier opportunity.
        const unsigned first=images.next;
        while(!fgBootstrap.phaseFits(displayTime,b.currentPoseDisplayTime,period,b.multiplier,images.next)) {
          images.next=fgschedule::nextPhase(images.next,b.generatedCount);
          if(!images.next) {
            fgBootstrap.abandonNativeAttempt();++fgPhasesExhausted;return Outcome::Late;
          }
        }
        if(images.next!=first)++fgPhaseSalvaged;
        return Outcome::Generated;
      }
      if(!b.generatedCount)return Outcome::NativeOnly;
      if(imported)return Outcome::PreviousMismatch;
      return Outcome::Endpoint;
    }
    return Outcome::None;
  }
  bool fgFallbackUsable(XrTime displayTime) const {
    return fgFallbackValid&&fgFallbackReference==poses.read().originGeneration&&
      !changes.crosses(fgFallbackTime,displayTime)&&fgFallbackSpace==seated.frame().space&&
      displayTime>=fgFallbackTime&&displayTime-fgFallbackTime<=500000000;
  }
  // The batch stores physical poses. Use the same seated basis for the
  // plugin's head and eyes and return physical poses to the XR layer.
  const XrPosef* fgLocatePluginHead(const SeatedFrame& seat,XrTime sourceTime,XrSpaceLocation& head) const {
    constexpr auto valid=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if(HeadLocator{}.locateAt({nullptr,api.locateSpace},view,seat,sourceTime,head)!=XR_SUCCESS||
       (head.locationFlags&valid)!=valid)return nullptr;
    return &head.pose;
  }
  XrResult fgRenderCaptured(const XrView (&physicalViews)[2],XrTime sourceTime,const EyeCapture& pixels,
      XrCompositionLayerProjection& layer,float fade,StereoNativeUi* overlay) {
    const auto seat=seated.frame();
    XrView views[2]={physicalViews[0],physicalViews[1]};
    for(unsigned eye=0;eye<2;++eye) {
      views[eye].pose=seatxf::toSeated(seat.origin,views[eye].pose);
      if(overlay)overlay->views[eye].pose=seatxf::toSeated(seat.origin,overlay->views[eye].pose);
    }
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    return stereo.renderCaptured(views,seat.space,pixels,layer,nullptr,nullptr,nullptr,fade,
      fgLocatePluginHead(seat,sourceTime,head),0.f,nullptr,0.f,false,&seat.origin,nullptr,true,overlay);
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
    const XrResult drawn=fgRenderCaptured(views,fgschedule::phaseTime(batch,phase),pixels,world,fade,selected.separateUi?&overlay:nullptr);
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
    using I=fghandoff::Images;using H=fghandoff::Handoff;
    const auto image=[&](I i){return (unsigned long long)fgImages[unsigned(i)];};
    const auto handoff=[&](H h){return (unsigned long long)fgHandoffs[unsigned(h)];};
    const auto& p=frameGeneration.pollCounts();
    nativeTracePrintf("native_fg_delivery,images_none=%llu,images_generated=%llu,images_endpoint=%llu,images_wrong_epoch=%llu,images_behind=%llu,images_previous_mismatch=%llu,images_late=%llu,images_native_only=%llu,images_outside_window=%llu,phase_salvaged=%llu,phases_exhausted=%llu,handoff_skipped=%llu,handoff_source=%llu,handoff_coverage=%llu,handoff_no_proof=%llu,handoff_cold_spent=%llu,handoff_attempt_spent=%llu,handoff_time=%llu,handoff_deferred=%llu,handoff_recovery=%llu\n",
      image(I::None),image(I::Generated),image(I::Endpoint),image(I::WrongEpoch),image(I::BehindWatermark),
      image(I::PreviousMismatch),image(I::Late),image(I::NativeOnly),image(I::OutsideWindow),
      (unsigned long long)fgPhaseSalvaged,(unsigned long long)fgPhasesExhausted,
      handoff(H::None),handoff(H::SourceIdentity),handoff(H::FallbackCoverage),handoff(H::NoSynthesisProof),
      handoff(H::ColdTrialSpent),handoff(H::AttemptAlreadySpent),handoff(H::Time),handoff(H::Deferred),handoff(H::RecoveryDeferred));
    nativeTracePrintf("native_fg_poll,no_export=%llu,prior_import_pending=%llu,future_pose=%llu,provider_fence_pending=%llu,imported=%llu,behind_before_copy=%llu,open_texture_calls=%llu,eye_copies=%llu,poll_failed=%llu,credit_no_request=%llu,no_request_outstanding=%llu,credit_outstanding=%llu,credit_spaced=%llu,credit_on_miss=%llu,credit_on_phase=%llu,credit_refused=%llu\n",
      (unsigned long long)p.noExport,(unsigned long long)p.priorImportPending,(unsigned long long)p.futurePose,
      (unsigned long long)p.providerFencePending,(unsigned long long)p.imported,(unsigned long long)p.behindBeforeCopy,
      (unsigned long long)p.textureOpens,(unsigned long long)p.eyeCopies,(unsigned long long)fgPollFailed,
      (unsigned long long)fgNoRequester,(unsigned long long)fgNoRequestOutstanding,(unsigned long long)fgCreditOutstanding,(unsigned long long)fgCreditSpaced,
      (unsigned long long)fgCreditOnMiss,(unsigned long long)fgCreditOnPhase,(unsigned long long)fgCreditRefused);
  }
  bool fgCanSubmitBatch(const EdvrFgBatch& batch) {
    EdvrFgStatus latest{};
    return frameGeneration.status(latest)==S_OK&&fgadmission::providerReady(latest,runtimeGeneration)&&
      latest.generation==batch.generation&&latest.configGeneration==batch.configGeneration&&
      latest.historyGeneration==batch.historyGeneration&&latest.activeMultiplier==batch.multiplier&&
      batch.referenceGeneration==poses.read().originGeneration;
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
    const bool stateValid=fgadmission::providerReady(status,runtimeGeneration);
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
  void fgReleaseOffImages(const EdvrFgStatus& status) {
    // Auto probation has requestedMultiplier zero but reports ARMING; it can
    // reuse the allocations. Retry explicit Off on owner ticks until fences retire.
    if(!fgConsumerAllocated||fgEnabled.load(std::memory_order_acquire)||status.generation!=runtimeGeneration||
       status.state!=EDVR_FG_OFF||status.requestedMultiplier||frameGeneration.releaseInactiveImages()!=S_OK)return;
    fgFallback.reset();fgPendingNative.reset();fgWorldFallback.reset();fgUiFallback.reset();
    fgConsumerAllocated=false;
  }
  bool fgTickOwner(const RuntimeGate::Lease& operation) {
    if(!fgAvailable.load(std::memory_order_acquire)||!state.running()||state.terminal()||serviceStopped)return false;
    const uint64_t now=GetTickCount64();
    const bool callerFresh=now-gameFrameCallMs.load(std::memory_order_relaxed)<=500;
    EdvrFgStatus status{};
    const HRESULT statusResult=frameGeneration.status(status);
    if(statusResult==S_OK)fgReleaseOffImages(status);
    const bool providerReady=statusResult==S_OK&&fgadmission::providerReady(status,runtimeGeneration);
    const bool requested=providerReady&&separateGraphics()&&edvr::vrProfileActive()&&
      !loading.visible()&&featureFrameKnown&&featureFrame.sceneReady&&!fgMenuVisible&&!frameWithheld&&
      !fovTrim.trimmed()&&!fovTrim.pending()&&!zoomAppliedNow&&zoomStripNow<=0.f&&!circleNow.on&&
      !(featureFrameKnown&&featureFrame.version>=EDVR_NATIVE_FRAME_VERSION_10&&eyeshare::fromBits(featureFrame.leftEyeShare)>0.f)&&
      callerFresh&&
      !(featureFrameKnown&&featureFrame.deferredPacing);
    fgLifecycleReport(now,statusResult,status,requested,callerFresh);
    if(!requested) {
      if(!fgEnabled.load(std::memory_order_acquire))return false;
      // A withheld/native-only scene decision can arrive between the eyes.
      // Finish its real credit, unless the provider proof or caller was lost.
      if(!fgNativeOutstanding||!providerReady||!callerFresh) {
        const bool recoverable=callerFresh&&statusResult==S_OK&&status.generation==runtimeGeneration&&
          status.state!=EDVR_FG_CLOSING&&status.state!=EDVR_FG_FAILED;
        fgDisable(callerFresh?3u:10u,operation,recoverable);return true;
      }
    }
    if(!fgEnabled.load(std::memory_order_acquire)) {
      if(state.frameOpen()||pendingFrameEndFinish)return false;
      if(frameGeneration.initializeConsumer(graphics.device(),graphics.context())!=S_OK||fgFallback.initialize(graphics.device())!=S_OK||
         fgPendingNative.initialize(graphics.device())!=S_OK||
         fgWorldFallback.initialize(graphics.device())!=S_OK||fgUiFallback.initialize(graphics.device())!=S_OK){++fgRefused;return false;}
      fgConsumerAllocated=true;fgResumedNative=false;
      fgMultiplier=status.requestedMultiplier;fgConfig=status.configGeneration;fgHistory=status.historyGeneration;fgNextCredit=fgTick;
      fgSeedNativeFallback();
      fgEnabled.store(true,std::memory_order_release);service.setContinuousIdle(true);
      nativeTracePrintf("native_fg_xr,enabled=1,multiplier=%u,single_owner=1,turbo=0,consumed_batches_copy_all_phases=1\n",fgMultiplier);
    }
    if(status.configGeneration!=fgConfig||status.requestedMultiplier!=fgMultiplier||status.historyGeneration!=fgHistory) {
      fgDisable(4,operation,callerFresh);return true;
    }
    fgBootstrap.sync(fgBootstrapEpoch(status));
    // Only the independently queried refresh rate can classify app-frame
    // intervals against panel cadence. Keep wall-age admission otherwise.
    const XrDuration panelPeriod=instrumentDisplayHz>=20.&&instrumentDisplayHz<=250.
      ? XrDuration(1000000000./instrumentDisplayHz+.5) : 0;
    fgBootstrap.syncDisplayPeriod(panelPeriod);
    if(!state.frameOpen()) {
      if(!poseSeatReady()){fgCancelCredit();return true;}
      const XrResult waited=boundary.waitAndBegin(FramePacing::Runtime);
      if(waited!=XR_SUCCESS&&waited!=XR_FRAME_DISCARDED&&waited!=XR_SESSION_LOSS_PENDING) {
        publishFatalFailure(waited,"native_fg_wait");fgCancelCredit();return true;
      }
      ++fgTick;
      const auto waitedFrame=boundary.frame();
      fgBootstrap.noteSlot(fgTick,waited==XR_SUCCESS?waitedFrame.predictedDisplayTime:0,
        waitedFrame.predictedDisplayPeriod,panelPeriod);
      // Return to the same owner's queue with this real runtime frame open.
      // Native submits/Wait registration queued during xrWaitFrame now run
      // before its prepared presentation; no extra XR frame is invented.
      return true;
    }
    const Frame tick=boundary.frame();
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    bool hasUi=false;
    bool pixels=false,generated=false,newNative=false;
    auto& images=frameGeneration.images();
    bool imported=false,pollFailed=false;
    std::optional<EdvrFgBatch> behind;
    if(!images.ready) {
      const HRESULT polled=frameGeneration.poll(runtimeGeneration,poses.read().originGeneration,fgConfig,fgMultiplier,
        sizes[0].recommendedImageRectWidth,sizes[0].recommendedImageRectHeight,tick.predictedDisplayTime,status.historyGeneration,fgDisplayedSequence,behind);
      pollFailed=FAILED(polled);
      if(pollFailed){++fgRefused;++fgPollFailed;}
      if(!frameGeneration.healthy()) {
        fgEndPrepared(nullptr,edvr::NativePresentationKind::Empty);publishFatalFailure(XR_ERROR_RUNTIME_FAILURE,"native_fg_copy_retirement");return true;
      }
      imported=images.ready;
    }
    // The runtime can report a predicted period at application cadence. Use
    // its separately published display frequency when available.
    const XrDuration period=panelPeriod?panelPeriod:tick.predictedDisplayPeriod;
    const auto imageDecision=fgPrepareImages(status,tick.predictedDisplayTime,period,imported,&behind);
    ++fgImages[unsigned(imageDecision)];
    const bool interpolationAllowed=requested&&tick.shouldRender&&comfortFade()<=0.f;
    const bool generatedSlot=interpolationAllowed&&images.ready&&images.next;
    const bool committed=images.ready&&fgBootstrap.needsEndpoint(images.metadata.currentSequence);
    const bool pending=fgPendingNativeUsable(tick.predictedDisplayTime);
    if(images.ready&&!generatedSlot) {
      if(images.next)fgBootstrap.miss();
      images.next=0;
      // Finish the endpoint of an intermediate already displayed. Other
      // endpoints may give way to a newer complete native pair.
      if(!committed&&((fgFallbackValid&&fgFallbackSequence>images.metadata.currentSequence)||
         (pending&&fgBootstrap.completedSequence()>images.metadata.currentSequence)))images.ready=false;
    }
    if(tick.shouldRender&&pending&&!generatedSlot&&!committed) {
      // The poll above owns any ready phases. If exact B is still queued and
      // its first phase has time for one more slot, retain displayed A here.
      // This never holds pose credit or waits on the provider or GPU.
      const bool usablePoll=!pollFailed&&imageDecision!=fghandoff::Images::WrongEpoch&&
        imageDecision!=fghandoff::Images::OutsideWindow;
      auto handoff=fghandoff::Handoff::None;
      const bool deferred=!images.ready&&interpolationAllowed&&usablePoll&&
        fgDeferNativeFallback(status,tick.predictedDisplayTime,tick.predictedDisplayPeriod,period,&handoff);
      ++fgHandoffs[unsigned(handoff)];
      if(!deferred) {
        fgBootstrap.abandonNativeAttempt();
        if(!images.ready)fgRefreshNativeFallback(tick.predictedDisplayTime);
      }
    }
    // Poll and choose this slot before releasing the caller. A missing phase
    // releases the credit immediately; an old nextCredit cannot throttle it.
    if(!fgCredit){++fgNoRequester;if(fgNativeOutstanding)++fgNoRequestOutstanding;}
    else if(fgNativeOutstanding)++fgCreditOutstanding;
    else if(!fgbootstrap::creditDue(fgTick,fgNextCredit,generatedSlot))++fgCreditSpaced;
    else {
      if(generatedSlot)++fgCreditOnPhase;else ++fgCreditOnMiss;
      auto request=std::move(fgCredit);const uint64_t before=lastPublishedFrameSequence;
      fgPreparingCredit=true;fgNativePixels=false;
      fgRunCredit(request,operation);fgPreparingCredit=false;
      fgNativeOutstanding=lastPublishedFrameSequence>before&&poses.read().sequence==activeFrameSequence&&frameGeometryAvailable;
      fgAcceptedEyes=0;
      if(fgNativeOutstanding){fgNextCredit=fgschedule::nextCredit(fgTick,fgMultiplier);++fgCredits;}
      else ++fgCreditRefused;
    }
    if(!state.frameOpen()) {
      return true; // a refused credit closed its frame
    }
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
          fgFallbackSequence=b.currentSequence;fgDisplayedSequence=b.currentSequence;images.ready=false;
        }
      } else {images.ready=false;++fgRefused;publishFatalFailure(drawn,"native_fg_stereo");return true;}
    }
    if(tick.shouldRender&&!pixels&&fgFallbackUsable(tick.predictedDisplayTime)) {
      const bool split=fgSplitFallbackValid&&comfortFade()<=0.f;
      StereoNativeUi overlay{};
      if(split){overlay.pixels=&fgUiFallback;for(unsigned eye=0;eye<2;++eye)overlay.views[eye]=fgFallbackViews[eye];}
      const XrResult drawn=fgRenderCaptured(fgFallbackViews,fgFallbackTime,split?fgWorldFallback:fgFallback,layer,comfortFade(),split?&overlay:nullptr);
      if(drawn!=XR_SUCCESS){publishFatalFailure(drawn,"native_fg_fallback");return true;}
      hasUi=split;
      pixels=true;
      fgLastPhase=0;fgLastSourceTime=fgFallbackTime;fgLastAge=tick.predictedDisplayTime-fgFallbackTime;
      generated=false;newNative=fgDisplayedSequence!=fgFallbackSequence;
      fgDisplayedSequence=fgFallbackSequence;
    }
    const auto kind=!pixels?edvr::NativePresentationKind::Empty:
      generated?edvr::NativePresentationKind::Generated:
      newNative?edvr::NativePresentationKind::Native:edvr::NativePresentationKind::Repeated;
    // Native credit callbacks and the draw can span a producer proof change.
    // Sample the current generation again at the generated xrEndFrame edge.
    if(generated&&!fgCanSubmitBatch(images.metadata)){++fgRefused;fgDisable(3,operation,callerFresh,true);return true;}
    const XrResult ended=fgEndPrepared(pixels?&layer:nullptr,kind);
    if(ended!=XR_SUCCESS){publishFatalFailure(ended,"native_fg_end");fgCancelCredit();return true;}
    if(generated)fgBootstrap.displayGenerated(images.metadata);
    else if(newNative)fgBootstrap.displayNative(fgDisplayedSequence,fgTick);
    if(hasUi)++fgUiPresented;
    if(!pixels)++fgEmpty;else if(generated)++fgGeneratedPresented;else if(newNative)++fgNativePresented;else ++fgRepeated;
    fgReport();return true;
  }
  // Both eyes have been admitted. Only S_OK from the consumer completes a
  // pixel transfer. A timed-out or faulted transfer retains its resources for
  // the normal failed-session shutdown, as it does in ordinary compose().
  template<class Receive>
  vr::EVRCompositorError fgCompleteNativePair(Receive receive) {
    if(frameWithheld||!fgNativePixels)return vr::VRCompositorError_None;
    if(receive()!=S_OK) {
      frameGeneration.invalidate(5);++fgRefused;
      lastCompositorResult=XR_ERROR_RUNTIME_FAILURE;
      publishFatalFailure(lastCompositorResult,"native_fg_eye_transfer");
      return vr::VRCompositorError_InvalidTexture;
    }
    EdvrFgStatus fallbackStatus{};
    if(frameGeneration.status(fallbackStatus)==S_OK)
      fgStageNativeFallback(fgBootstrapEpoch(fallbackStatus));
    return vr::VRCompositorError_None;
  }
  vr::EVRCompositorError fgSubmitNative(vr::EVREye eye,const vr::Texture_t* texture,
      const vr::VRTextureBounds_t* bounds,vr::EVRSubmitFlags flags) {
    LARGE_INTEGER began{},ended{};const bool clock=counterNow(&began);
    const unsigned bit=1u<<unsigned(eye);
    if(!fgNativeOutstanding||(fgAcceptedEyes&bit))return vr::VRCompositorError_InvalidTexture;
    auto result=capture(eye,texture,bounds,flags,fgNativePixels);
    if(result==vr::VRCompositorError_None) {
      fgAcceptedEyes|=bit;
      if(fgAcceptedEyes==3) {
        fgNativeOutstanding=false;fgAcceptedEyes=0;
        result=fgCompleteNativePair([&]{return captured.completePending(nullptr,&transferWall);});
        if(result!=vr::VRCompositorError_None)return result; // the failed session already retired timing
      }
    }
    if(result!=vr::VRCompositorError_None) {
      frameGeneration.invalidate(5);fgNativeOutstanding=false;fgAcceptedEyes=0;++fgRefused;
    }
    dispatchGpuEyeMarker(eye,texture,result==vr::VRCompositorError_None);
    if(clock&&counterNow(&ended))timingFrame.submitMs[unsigned(eye)]=elapsedMs(began,ended);
    return finishSubmitTail(eye,result);
  }

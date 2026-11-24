  // The XR owner withholds an incomplete stereo source exactly once, including a
  // late second eye or deferred finish. Receipts from earlier frames cannot match.
  bool rejectIncompleteVrsSource() {
    const uint64_t sequence=activeFrameSequence;
    if(!sequence)return false;
    if(sequence==vrsRejectedSequence)return true;
    const auto eyes=temporal.vrsFailures(sequence);
    if(!eyes)return false;
    vrsRejectedSequence=sequence;
    nativeTracePrintf("native_vrs_source_withheld,sequence=%llu,incomplete_eye_mask=%u\n",
      (unsigned long long)sequence,eyes);
    timingInvalidate();invalidateEyeTreatments();menu.invalidate();
    temporalFrameEyes=0;previousPairValid=false;
    fgCancelCredit();
    fgNativeOutstanding=false;fgAcceptedEyes=0;
    fgBootstrap.clear();fgPendingNativePose={};fgFallbackValid=false;fgSplitFallbackValid=false;
    frameGeneration.invalidate(12); // Retire imports and discard every batch derived from this history.
    const auto ended=fgEnabled.load(std::memory_order_acquire)&&state.frameOpen()
      ? fgEndPrepared(nullptr,edvr::NativePresentationKind::Empty) : boundary.clear();
    if(ended!=XR_SUCCESS)publishFatalFailure(ended,"native_vrs_withhold_end");
    return true;
  }

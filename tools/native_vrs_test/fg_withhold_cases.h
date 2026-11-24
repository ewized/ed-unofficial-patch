#pragma once
#include "../../src/common/native_vrs_frame_failure.h"
#include "../../src/common/native_presentation.h"
#include "../../src/openxr/native_fg_bootstrap.h"
#include "../../src/openxr/native_fg_credit.h"
#include <memory>

namespace vrs_withhold_test {
// Run the production XR owner's rejection path without a device or a display.
// Stub only the XR/graphics services at the boundary. Use real failure receipts,
// pending pose credits and bootstrap history throughout.
struct Host {
    static constexpr int XR_SUCCESS=0;
    std::uint64_t activeFrameSequence=0,vrsRejectedSequence=0;
    unsigned temporalFrameEyes=0,fgAcceptedEyes=0;
    bool previousPairValid=false,fgNativeOutstanding=false,fgFallbackValid=false,fgSplitFallbackValid=false;
    std::atomic<bool> fgEnabled{false};
    edvr::openxr::fgbootstrap::Bootstrap fgBootstrap;
    struct {std::uint64_t sequence=0;} fgPendingNativePose;
    std::shared_ptr<edvr::openxr::NativeFgCreditRequest> fgCredit;
    struct {
        edvr::NativeVrsFrameFailure receipt;
        unsigned vrsFailures(std::uint64_t sequence){return receipt.failedEyes(sequence);}
    } temporal;
    struct {bool open=true;bool frameOpen()const{return open;}} state;
    struct {unsigned clears=0;int clear(){++clears;return 0;}} boundary;
    struct {void invalidate(){}} menu;
    struct {unsigned invalidations=0;void invalidate(unsigned){++invalidations;}} frameGeneration;
    unsigned endings=0,fatals=0;
    void fgCancelCredit(){edvr::openxr::cancelNativeFgCredit(fgCredit);}
    int fgEndPrepared(std::nullptr_t,edvr::NativePresentationKind kind) {
        if(kind!=edvr::NativePresentationKind::Empty||!state.open)return -1;
        ++endings;state.open=false;return XR_SUCCESS;
    }
    void timingInvalidate(){}
    void invalidateEyeTreatments(){temporal.receipt.end();}
    void nativeTracePrintf(const char*,unsigned long long,unsigned){}
    void publishFatalFailure(int,const char*){++fatals;}
#include "../../src/openxr/native_vrs_source.inl"
};

template<class Check> void run(Check check) {
    Host h;
    h.activeFrameSequence=500;
    h.temporal.receipt.begin(499);h.temporal.receipt.mark(499,1);
    check(!h.rejectIncompleteVrsSource()&&h.state.open,"old receipt leaves the current source eligible");
    h.temporal.receipt.begin(500);
    h.fgEnabled.store(true);h.fgNativeOutstanding=true;
    h.fgAcceptedEyes=1;h.temporalFrameEyes=1;
    h.fgBootstrap.sync({1,2,3,4});h.fgBootstrap.completeNative(500);
    h.fgPendingNativePose.sequence=500;h.previousPairValid=h.fgFallbackValid=h.fgSplitFallbackValid=true;
    auto credit=std::make_shared<edvr::openxr::NativeFgCreditRequest>();h.fgCredit=credit;
    check(!h.rejectIncompleteVrsSource()&&h.fgAcceptedEyes==1,"a clean first eye retains the pending native pair");
    h.temporal.receipt.mark(500,1);
    check(h.rejectIncompleteVrsSource()&&!h.fgNativeOutstanding&&!h.fgAcceptedEyes&&
        !h.temporalFrameEyes&&!h.previousPairValid&&!h.fgFallbackValid&&!h.fgSplitFallbackValid,
        "a failure on the second eye discards the partial pair and both fallbacks");
    check(!h.fgBootstrap.completedSequence()&&!h.fgPendingNativePose.sequence&&
        h.frameGeneration.invalidations==1&&h.endings==1&&!h.state.open&&!h.fatals,
        "pending generated history is invalidated before the XR owner ends one empty frame");
    check(!h.fgCredit&&credit->done&&!credit->wait(),"the pending pose caller receives a failed credit");
    check(h.rejectIncompleteVrsSource()&&h.endings==1&&h.frameGeneration.invalidations==1,
        "duplicate source submission cannot end or invalidate twice");
    h.activeFrameSequence=501;h.temporal.receipt.begin(501);h.state.open=true;
    auto next=std::make_shared<edvr::openxr::NativeFgCreditRequest>();h.fgCredit=next;
    bool ran=false;next->callback=[&]{ran=true;};
    check(!h.rejectIncompleteVrsSource()&&h.state.open&&h.fgCredit==next,
        "the next clean source retains its open frame and fresh credit");
    next->run();check(ran&&next->wait(),"fresh credit can run after withholding");
}
}

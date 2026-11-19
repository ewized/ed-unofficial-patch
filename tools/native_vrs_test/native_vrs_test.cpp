#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../src/d3d11/native_vrs.h"
#include "../../src/d3d11/native_vrs_policy.h"
#include "witness_rate_cases.h"
#include "witness_replay_cases.h"
#include "../../src/d3d11/native_gaze.h"
#include "../../src/d3d11/native_gaze_readiness.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
namespace edvr {
int guardFilter(unsigned long,const char*){return EXCEPTION_EXECUTE_HANDLER;}
NativeGazeSample nativeGazePoll(bool) noexcept {std::abort();}
void nativeGazeReset() noexcept {}
void nativeGazeSetLifetime(uint64_t) noexcept {std::abort();}
NativeGazeReadySnapshot nativeTemporalGazeReadiness(ID3D11Device*,unsigned long){return {};}
uint64_t nativeTemporalGazeEpoch() noexcept{return 0;}
bool nativeTemporalWithGazeReady(ID3D11Device*,unsigned long,uint64_t,NativeGazeReadyCallback,void*){std::abort();}
}
using namespace edvr::native_vrs;
using namespace edvr::native_vrs::testing;
unsigned checks=0,failures=0;
void check(bool pass,const char* name){++checks;if(!pass){++failures;std::printf("FAIL: %s\n",name);}}
DriverResult run(std::initializer_list<DriverOp> ops){return runDriverFixture(ops.begin(),ops.size());}
#include "ps_measure_cases.h"
#include "ps_draw_scope_cases.h"
int main(){
    witnessRateCases(check);
    witnessReplayCases(check);
    // Poison COM pointers would crash if OFF or a foreign owner touched D3D.
    auto* poison=reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(1));
    check(!edvr::nativeVrsPsBefore(poison,{}).active,"default OFF PS path performs no COM call");
    edvr::nativeVrsPsMeasureConfigure(true,nullptr);
    check(!edvr::nativeVrsPsBefore(poison,{}).active,"foreign PS path performs no COM call even when armed");
    edvr::nativeVrsPsMeasureConfigure(false,nullptr);
    edvr::NativeGazeReadiness milestone;
    milestone.reset(1);milestone.begin(10);milestone.succeeded(10,0);
    check(!milestone.pairReady,"one successful eye is not ready");
    milestone.begin(11);milestone.succeeded(11,1);
    check(!milestone.pairReady,"eyes from different frames cannot form a readiness pair");
    milestone.succeeded(10,0);milestone.succeeded(11,2);
    check(!milestone.pairReady,"wrong sequence and unknown eye cannot create readiness");
    milestone.succeeded(11,0);check(milestone.pairReady&&milestone.pairSequence==11,"same-frame successful stereo outputs open the milestone");
    milestone.reset(2);check(!milestone.pairReady&&!milestone.successfulEyes&&milestone.epoch==2,"invalidate/close/new acquisition reset successful-eye evidence");
    check(!edvr::nativeVrsLive(),"default OFF draw gate");
    check(!edvr::nativeVrsCoarseRequested(),"cold OFF does not request coarse preparation");
    edvr::nativeVrsConfigure(true);
    check(edvr::nativeVrsCoarseRequested()&&edvr::nativeVrsLive(),"ON requests coarse preparation before owning-frame admission");
    edvr::nativeVrsConfigure(false);
    check(!edvr::nativeVrsCoarseRequested()&&!edvr::nativeVrsLive(),"cold ON then OFF without an initialized driver closes both gates");
    edvr::nativeVrsConfigure(true,.5f,.1f);
    check(!edvr::nativeVrsCoarseRequested(),"invalid radii do not request coarse preparation");
    edvr::nativeVrsConfigure(false);
    const auto shape=mapShape(2144,2144);check(shape.width==134&&shape.height==134&&shape.bytes==17956,"134x134 world map");
    check(mapShape(17,17).bytes==4,"partial edge tiles use ceil");
    check(!mapShape(0,1).bytes&&!mapShape(UINT_MAX,1).bytes,"invalid and overflow dimensions refuse");
    check(validRadii(.15f,.45f)&&!validRadii(.5f,.1f)&&!validRadii(0,.3f),"radii validation");
    const auto focused=focusRadii("very_narrow"),normal=focusRadii("normal");
    check(validRadii(focused.sharp,focused.transition)&&focused.sharp<normal.sharp&&focused.transition<normal.transition,
          "very narrow shrinks both shading regions");
    check(!validRadii(focusRadii("invalid").sharp,focusRadii("invalid").transition)&&
          !validRadii(focusRadii(nullptr).sharp,focusRadii(nullptr).transition),"invalid focus refuses coarse shading");
    std::vector<uint8_t> focusedMap(mapShape(1856,1856).bytes),normalMap(focusedMap.size());
    check(buildMap(focusedMap.data(),focusedMap.size(),1856,1856,0,0,true,focused.sharp,focused.transition)&&
          buildMap(normalMap.data(),normalMap.size(),1856,1856,0,0,true,normal.sharp,normal.transition),
          "both focus selections build at Balanced input size");
    const auto focusShape=mapShape(1856,1856);
    check(focusedMap[58*focusShape.width+58]==0&&focusedMap[58*focusShape.width+70]==1&&
          focusedMap[58*focusShape.width+80]==2&&normalMap[58*focusShape.width+70]==0,
          "narrow map preserves gaze centre and reduces real off-gaze tile rates");
    edvr::nativeVrsConfigureFocus(true,"very_narrow");
    auto focusedStatus=edvr::nativeVrsStatus();
    check(focusedStatus.configured&&focusedStatus.sharpRadius==focused.sharp&&focusedStatus.transitionRadius==focused.transition,
          "actual configuration owner publishes selected narrow radii");
    edvr::nativeVrsConfigureFocus(true,"invalid");
    check(!edvr::nativeVrsStatus().configured,"actual configuration owner refuses invalid focus");
    edvr::nativeVrsConfigure(false);
    const float source[4]{1,1,0,0},same[4]{-1,1,-1,1},zero[2]{};
    float gx=0,gy=0;
    check(projectGaze(.25f,-.5f,source,same,zero,gx,gy)&&gx==.25f&&gy==-.5f,"equal projection preserves per-eye NDC");
    const float trimmed[4]{-.5f,.5f,-.5f,.5f};
    check(projectGaze(.25f,-.25f,source,trimmed,zero,gx,gy)&&gx==.5f&&gy==-.5f,"native trim or scope remaps rather than copying source NDC");
    const float asymmetricSource[4]{2,4,.5f,-.5f},shift[2]{.125f,-.125f};
    check(projectGaze(0,0,asymmetricSource,same,shift,gx,gy)&&gx==.125f&&gy==0,"asymmetric source matrix inverse and target jitter use correct signs");
    check(!projectGaze(.75f,0,source,trimmed,zero,gx,gy),"gaze outside current scope is refused without clamp");
    const float malformed[4]{1,1,-1,1},badSource[4]{0,1,0,0},nanShift[2]{NAN,0};
    check(!projectGaze(0,0,source,malformed,zero,gx,gy)&&!projectGaze(0,0,badSource,same,zero,gx,gy)&&
          !projectGaze(0,0,source,same,nanShift,gx,gy),"malformed projection or jitter cannot become valid gaze");
    const auto p=palette(ViewportRates::Foveated);check(p[0]==5&&p[1]==8&&p[2]==11,"official shading enum values");
    check(std::all_of(p.begin(),p.end(),[](uint32_t value){return value==5||value==8||value==11;}),"every palette entry avoids CULL");
    std::vector<uint8_t> left(256),right(256);
    check(buildMap(left.data(),left.size(),256,256,-.5f,.5f,true,.15f,.45f),"left eye map builds");
    check(buildMap(right.data(),right.size(),256,256,.5f,-.5f,true,.15f,.45f),"right eye map builds");
    check(left[4*16+4]==0&&right[12*16+12]==0,"NDC x and flipped y map to independent eyes");
    check(left[12*16+12]==2&&right[4*16+4]==2&&left!=right,"eye maps stay isolated");
    check(std::count(left.begin(),left.end(),0)>0&&std::count(left.begin(),left.end(),1)>0&&std::count(left.begin(),left.end(),2)>0,"three ellipse regions exist");
    check(std::all_of(left.begin(),left.end(),[](uint8_t value){return value<=2;}),"map bytes stay in non-CULL palette");
    check(!buildMap(left.data(),left.size(),256,256,0,0,false,.15f,.45f)&&std::count(left.begin(),left.end(),0)==256,"invalid pair builds full-rate-only map");
    check(!buildMap(left.data(),left.size(),256,256,std::numeric_limits<float>::quiet_NaN(),0,true,.15f,.45f)&&std::count(left.begin(),left.end(),0)==256,"NaN does not substitute centre");
    check(!buildMap(left.data(),left.size(),256,256,1.01f,0,true,.15f,.45f),"out-of-bounds gaze is not clamped");
    uint8_t edge[4]{};check(buildMap(edge,4,17,17,1,-1,true,.15f,.45f)&&edge[3]==0,"partial edge centre uses covered pixels");
    check(!buildMap(edge,3,17,17,0,0,true,.15f,.45f),"insufficient capacity refuses");
    check(freshReceipt(2000,1000,20000)&&!freshReceipt(2001,1000,20000),"50ms receipt-age boundary");
    check(!freshReceipt(999,1000,20000)&&!freshReceipt(1000,0,20000)&&!freshReceipt(1000,1000,0),"future missing or unclocked receipt refuses");
    check(frameDecision(1,1,7,7,3,4)==FrameDecision::New,"new owning frame accepted");
    check(frameDecision(1,1,7,7,3,3)==FrameDecision::Duplicate,"duplicate frame does not advance");
    check(frameDecision(1,1,7,7,3,0)==FrameDecision::Zero&&frameDecision(1,1,7,7,3,2)==FrameDecision::Regressed,"zero and regressed frames refuse");
    check(frameDecision(1,2,7,7,3,4)==FrameDecision::ForeignContext&&frameDecision(1,1,7,8,3,4)==FrameDecision::ForeignThread,"foreign ownership refuses");
    check(fullViewport(1,0,0,2144,2144,2144,2144),"full-eye viewport accepts");
    check(!fullViewport(1,1,0,2144,2144,2144,2144)&&!fullViewport(1,0,0,1072,2144,2144,2144)&&!fullViewport(2,0,0,2144,2144,2144,2144),"offset cropped and multiple viewports refuse");
    std::vector<DriverOp> repeatedFull(256,DriverOp::Full);
    auto r=runDriverFixture(repeatedFull.data(),repeatedFull.size());
    check(r.count==2&&r.publicationCount==1&&r.publications[0].vendorKnown&&
          r.publications[0].fullKnown&&!r.publications[0].coarseKnown,
          "256 acknowledged full calls publish once and retain full-rate status");
    r=run({DriverOp::Full,DriverOp::Left,DriverOp::Full,DriverOp::Full});
    check(r.publicationCount==3&&r.publications[0].fullKnown&&
          r.publications[1].vendorKnown&&r.publications[1].coarseKnown&&!r.publications[1].fullKnown&&
          r.publications[2].vendorKnown&&r.publications[2].fullKnown&&!r.publications[2].coarseKnown,
          "real full/coarse/full transitions publish each acknowledgement, repeated full does not");
    r=run({DriverOp::Full,DriverOp::Left,DriverOp::Invalidate,DriverOp::Full});
    check(r.publicationCount==3&&r.count==6&&r.publications[2].fullKnown&&!r.publications[2].coarseKnown,
          "invalidation resets and republishes actual full-rate acknowledgement once");
    r=run({DriverOp::FailRate,DriverOp::Full,DriverOp::Full,DriverOp::NextFrame,DriverOp::Full,DriverOp::Full});
    check(r.publicationCount==3&&!r.publications[0].vendorKnown&&!r.publications[0].fullKnown&&
          !r.publications[1].vendorKnown&&!r.publications[1].fullKnown&&
          r.publications[2].vendorKnown&&r.publications[2].fullKnown&&r.refused,
          "failed reset and bounded retry publish unknown until the next-frame reset succeeds");
    r=run({DriverOp::Full,DriverOp::Full});
    check(r.count==2&&r.fullKnown&&!r.coarseKnown,"repeated full reset skips setters only after acknowledgement");
    check(r.calls[0].rates&&r.calls[0].version==0x10010&&r.calls[0].count==0&&!r.calls[0].enabled&&!r.calls[1].resource,"actual production disabled descriptor and null resource");
    r=run({DriverOp::Full,DriverOp::Left,DriverOp::Off,DriverOp::Full});
    check(!r.coarseRequested&&!r.configured&&r.live&&r.fullKnown&&!r.coarseKnown&&r.count==6,
          "initialized ON then OFF closes preparation gate while full-rate boundary restores driver state");
    r=run({DriverOp::Full,DriverOp::Off,DriverOp::Reconfigure});
    check(r.coarseRequested&&r.configured&&r.live&&r.fullKnown&&!r.coarseKnown&&r.count==2,
          "re-enable requests preparation but does not activate coarse state without fresh admission");
    r=run({DriverOp::Full,DriverOp::Left,DriverOp::Left,DriverOp::Right});
    check(r.count==5&&r.coarseKnown&&!r.refused,"same eye skips setters and eye switch binds only resource");
    check(r.calls[3].rates&&r.calls[3].version==0x10010&&r.calls[3].count==1&&r.calls[3].enabled,"actual production enabled descriptor");
    check(r.calls[3].palette[0]==5&&r.calls[3].palette[1]==8&&r.calls[3].palette[2]==11,"actual adapter passes public palette");
    check(std::all_of(r.calls[3].palette,r.calls[3].palette+16,[](uint32_t value){return value!=0;}),"actual adapter has no CULL entry");
    r=run({DriverOp::Full,DriverOp::Left,DriverOp::WitnessLeft,DriverOp::Left,DriverOp::WitnessLeft,DriverOp::Full});
    const auto& uniform=r.calls[4];
    check(r.count==9&&uniform.rates&&uniform.version==0x10010&&uniform.count==1&&uniform.enabled&&
          std::all_of(uniform.palette,uniform.palette+16,[](uint32_t rate){return rate==kFullRate;})&&
          r.calls[5].rates&&r.calls[5].enabled&&std::equal(p.begin(),p.end(),r.calls[5].palette),
          "private witness and material use enabled viewports with all-1x1 and foveated palettes on the same view");
    check(r.publicationCount==6&&r.publications[2].vendorKnown&&!r.publications[2].fullKnown&&!r.publications[2].coarseKnown&&
          r.calls[7].rates&&r.calls[7].count==0&&!r.calls[8].rates&&!r.calls[8].resource&&r.fullKnown,
          "public boundary globally disables and unbinds after the private witness");
    r=run({DriverOp::Full,DriverOp::Left,DriverOp::FailRate,DriverOp::WitnessLeft});
    check(r.count==7&&r.calls[4].result==-9&&r.calls[5].rates&&r.calls[5].count==0&&
          !r.calls[6].rates&&!r.calls[6].resource&&r.fullKnown&&!r.coarseKnown&&r.refused,
          "failed private palette change latches refusal and recovers the strong full-rate state");
    r=run({DriverOp::Full,DriverOp::Left,DriverOp::Invalidate});
    check(r.count==6&&r.fullKnown&&!r.coarseKnown,"cache invalidation forces both reset calls");
    r=run({DriverOp::Full,DriverOp::FailRate,DriverOp::Left});
    check(r.count==6&&r.fullKnown&&!r.coarseKnown&&r.refused,"successful reset does not suppress later same-frame driver-error recovery");
    r=run({DriverOp::Full,DriverOp::FailRate,DriverOp::Left,DriverOp::Right});
    check(r.count==6&&r.fullKnown&&r.refused&&!r.coarseKnown,"latched failure never re-enables on a later world call");
    r=run({DriverOp::Full,DriverOp::FailResource,DriverOp::Left});
    check(r.count==5&&r.fullKnown&&r.refused,"resource-bind failure never enables and restores full rate");
    r=run({DriverOp::FailRate,DriverOp::Full,DriverOp::Full,DriverOp::NextFrame,DriverOp::Full});
    check(r.count==3&&r.fullKnown&&r.refused,"failed reset retries next frame and never fakes acknowledgement");
    r=run({DriverOp::FailRate,DriverOp::Left,DriverOp::Reconfigure});
    check(r.refused&&r.failure==edvr::NativeVrsFailure::DriverCall,"reconfigure preserves latched refusal reason");
    r=run({DriverOp::FailRate,DriverOp::Full,DriverOp::Off,DriverOp::TickCurrent,DriverOp::TickZero,
           DriverOp::TickRegressed,DriverOp::TickForeignContext,DriverOp::TickForeignThread});
    check(r.count==2&&!r.fullKnown&&r.frameOrdinal==1&&!r.configured&&r.live&&!r.coarseRequested&&r.providerPolls==0,
        "OFF invalid duplicate and foreign ticks neither advance retry cadence nor poll gaze");
    r=run({DriverOp::FailRate,DriverOp::Full,DriverOp::Off,DriverOp::TickCurrent,DriverOp::TickZero,
           DriverOp::TickRegressed,DriverOp::TickForeignContext,DriverOp::TickForeignThread,DriverOp::TickNext});
    check(r.count==3&&r.fullKnown&&!r.coarseKnown&&r.frameOrdinal==2&&r.refused&&
          r.failure==edvr::NativeVrsFailure::DriverCall&&!r.configured&&r.live&&!r.coarseRequested&&r.providerPolls==0,
        "actual OFF Tick retries failed full-rate reset on the next owning ordinal without gaze polling");
    r=run({DriverOp::FailRate,DriverOp::Full,DriverOp::Off,DriverOp::FailRate,DriverOp::TickNext,
           DriverOp::TickCurrent,DriverOp::TickZero,DriverOp::TickRegressed,DriverOp::TickNext});
    check(r.count==5&&r.fullKnown&&!r.coarseKnown&&r.frameOrdinal==3&&r.refused&&r.providerPolls==0,
        "OFF reset failures remain bounded within an ordinal and retry again on the next one");
    r=run({DriverOp::FailRate,DriverOp::Full,DriverOp::FlatTickNext});
    check(r.count==3&&r.fullKnown&&r.frameOrdinal==2&&r.configured&&r.providerPolls==0,
        "flat Present advances reset recovery even while configured, without provider polling");
    r=run({DriverOp::TickNext});
    check(r.frameOrdinal==2&&r.providerPolls==0&&r.fullKnown&&!r.coarseKnown&&
          r.failure==edvr::NativeVrsFailure::GazeNotReady,"actual enabled Tick before stereo readiness never discovers or polls gaze and stays full rate");
    ps_draw_scope_fixture::run();
    ps_fixture::run();
    std::printf("native VRS fixtures: checks=%u failures=%u PS_query=WARP NVAPI=mock OpenVR=0\n",checks,failures);
    return failures?1:0;
}

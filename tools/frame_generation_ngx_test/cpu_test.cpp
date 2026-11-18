#include "../../src/d3d11/frame_generation_ngx.h"
#include "../../src/d3d11/frame_generation_ngx_policy.h"
#include "../../src/d3d11/frame_generation_interop.h"
#ifdef EDVR_HAVE_NGX
#include "nvsdk_ngx_defs_dlssg.h"
#endif
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <limits>
#include <cstring>

namespace {
unsigned checks=0,failures=0;
void check(bool pass,const char* name){++checks;if(!pass){++failures;std::printf("FAIL %s\n",name);}}
#ifdef EDVR_HAVE_NGX
constexpr auto fail=NVSDK_NGX_Result_FAIL_FeatureNotSupported;
unsigned evaluations=0;NVSDK_NGX_Result evaluationResult=NVSDK_NGX_Result_Success;
bool initFixture=false;NVSDK_NGX_Parameter* fixtureParameters=nullptr;
NVSDK_NGX_Result scratchResult=NVSDK_NGX_Result_Success,createResult=NVSDK_NGX_Result_Success;
size_t scratchSize=0;unsigned scratchCalls=0,createCalls=0;
struct Parameters:NVSDK_NGX_Parameter {
    std::map<std::string,unsigned long long> ui;std::map<std::string,float> floats;std::map<std::string,void*> pointers;
    void Set(const char* n,unsigned long long v)override{ui[n]=v;}
    void Set(const char* n,float v)override{floats[n]=v;}
    void Set(const char* n,double v)override{floats[n]=float(v);}
    void Set(const char* n,unsigned v)override{ui[n]=v;}
    void Set(const char* n,int v)override{ui[n]=static_cast<unsigned>(v);}
    void Set(const char* n,ID3D11Resource* v)override{pointers[n]=v;}
    void Set(const char* n,ID3D12Resource* v)override{pointers[n]=v;}
    void Set(const char* n,void* v)override{pointers[n]=v;}
    NVSDK_NGX_Result Get(const char*,unsigned long long*)const override{return fail;}
    NVSDK_NGX_Result Get(const char*,float*)const override{return fail;}
    NVSDK_NGX_Result Get(const char*,double*)const override{return fail;}
    NVSDK_NGX_Result Get(const char*,unsigned*)const override{return fail;}
    NVSDK_NGX_Result Get(const char*,int*)const override{return fail;}
    NVSDK_NGX_Result Get(const char*,ID3D11Resource**)const override{return fail;}
    NVSDK_NGX_Result Get(const char*,ID3D12Resource**)const override{return fail;}
    NVSDK_NGX_Result Get(const char*,void**)const override{return fail;}
    void Reset()override{ui.clear();floats.clear();pointers.clear();}
};
void coupledInputRing(){
    using namespace edvr;
    // Producer ring ownership, NGX input pins and output's independent original
    // source pins. All three NGX slots can be retained by output tickets even
    // after their GPU work has completed.
    std::shared_ptr<void> producer[3],ngx[3],output[3];
    const uint64_t values[3]={11,12,13};
    for(unsigned i=0;i<3;++i){producer[i]=std::make_shared<unsigned>(i);ngx[i]=producer[i];output[i]=producer[i];}
    const auto reusable=[&](unsigned i,uint64_t outputDone){
        return fg_interop::reusable(unsigned(producer[i].use_count()),1,1,false,true,outputDone,20+i);
    };
    for(unsigned i=0;i<3;++i)check(!reusable(i,23),"three-slot ring blocks while NGX and output both pin original source");
    check(FrameGenerationNgx::testCollectInputs(13,values,0,ngx),"actual completed slot collector runs without another source submit");
    for(unsigned i=0;i<3;++i){
        check(!ngx[i]&&output[i]==producer[i]&&producer[i].use_count()==2,
            "NGX completion releases its pin but output independently retains exact original source");
        check(!reusable(i,19),"unfinished final HUD fence still prevents producer reuse");
    }
    const auto collectOutput=[&](uint64_t done){
        for(unsigned i=0;i<3;++i)if(fg_interop::complete(done,20+i))output[i].reset();
    };
    collectOutput(19);
    for(unsigned i=0;i<3;++i)check(output[i]==producer[i]&&!reusable(i,19),
        "output collector retains original source while final HUD fence is pending");
    collectOutput(23);
    for(unsigned i=0;i<3;++i)check(!output[i]&&reusable(i,23),
        "final HUD completion and independent pin release restore all three producer slots");
    // A partial completion releases only completed inputs. Device removal,
    // expiration and zero/unfenced values must preserve all ownership.
    for(unsigned i=0;i<3;++i)ngx[i]=producer[i];
    check(FrameGenerationNgx::testCollectInputs(11,values,0,ngx)&&!ngx[0]&&ngx[1]&&ngx[2],
        "partial fence completion retains later NGX source inputs");
    for(unsigned i=0;i<3;++i)ngx[i]=producer[i];
    check(!FrameGenerationNgx::testCollectInputs(UINT64_MAX,values,0,ngx)&&ngx[0]&&ngx[1]&&ngx[2],
        "device-removed fence sentinel quarantines every input pin");
    check(!FrameGenerationNgx::testCollectInputs(13,values,2,ngx)&&ngx[0]&&ngx[1]&&ngx[2],
        "expired slot leaves quarantine and every original pin intact");
    const uint64_t unfenced[3]{};
    check(FrameGenerationNgx::testCollectInputs(13,unfenced,0,ngx)&&ngx[0]&&ngx[1]&&ngx[2],
        "unfenced accepted inputs cannot be released by completed-slot collector");
}
struct FlagTrace {unsigned initializations[2]{};uintptr_t pointer[2][3]{};bool allAllowed=true;};
FlagTrace recordFlags(bool shared,bool reused,int leftWrite,int rightWrite,bool laterTrue=false){
    using namespace edvr;using namespace edvr::fg_ngx;FlagTrace trace;
    for(unsigned eye=0;eye<2;++eye){
        uint8_t flags[3][4]{};SuppressionState states[3];
        for(auto& state:states)state=reused?SuppressionState::CopySource:SuppressionState::CopyDestination;
        Parameters params;NVSDK_NGX_DLSSG_Opt_Eval_Params options[3]{};
        for(unsigned i=0;i<3;++i){
            const unsigned owner=suppressionOwner(shared,i);
            prepareSuppressionFlag(shared,i,reused,
                [&](SuppressionState a,SuppressionState b){check(states[owner]==a,"production flag command plan has correct before-state");states[owner]=b;},
                [&]{++trace.initializations[eye];for(auto& byte:flags[owner])byte=255;});
            check(states[owner]==SuppressionState::UnorderedAccess,"flag reaches UAV before evaluation");
            trace.pointer[eye][i]=uintptr_t(0x1000+eye*0x1000+owner*0x100);
            auto* resource=reinterpret_cast<ID3D12Resource*>(trace.pointer[eye][i]);
            check(frameGenerationNgxTestEvaluation(4,i+1,&params,&options[i],resource)&&
                params.pointers[NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation]==resource,
                "actual official evaluation adapter binds planned flag identity");
            const int driverWrite=eye==0?leftWrite:rightWrite;
            if(i==0&&driverWrite>=0){for(auto& byte:flags[owner])byte=0;flags[owner][0]=uint8_t(driverWrite);}
            if(laterTrue&&eye==0&&i==1)flags[owner][0]=1;
            trace.allAllowed=trace.allAllowed&&suppressionAllows(flags[owner]);
            states[owner]=SuppressionState::CopySource; // production fenced readback's final state
        }
    }
    return trace;
}
#endif
}
#ifdef EDVR_HAVE_NGX
// Link-time CPU substitutes. Bootstrap/resource allocation functions abort if
// any fixture accidentally attempts real NGX setup. No NGX library is linked.
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_with_ProjectID(const char*,NVSDK_NGX_EngineType,const char*,const wchar_t*,ID3D12Device*,const NVSDK_NGX_FeatureCommonInfo*,NVSDK_NGX_Version){std::abort();}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetCapabilityParameters(NVSDK_NGX_Parameter**){std::abort();}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_AllocateParameters(NVSDK_NGX_Parameter** out){
    if(!initFixture)std::abort();*out=fixtureParameters;return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_DestroyParameters(NVSDK_NGX_Parameter*){std::abort();}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature feature,const NVSDK_NGX_Parameter* p,size_t* out){
    if(!initFixture||feature!=NVSDK_NGX_Feature_FrameGeneration||p!=fixtureParameters)std::abort();
    ++scratchCalls;*out=scratchSize;return scratchResult;
}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature feature,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** out){
    if(!initFixture||feature!=NVSDK_NGX_Feature_FrameGeneration||p!=fixtureParameters)std::abort();
    ++createCalls;*out=NVSDK_NGX_SUCCEED(createResult)?reinterpret_cast<NVSDK_NGX_Handle*>(1):nullptr;return createResult;
}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle*){std::abort();}
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback_C){++evaluations;return evaluationResult;}
void NVSDK_CONV NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter* p,const char* n,unsigned v){p->Set(n,v);}
void NVSDK_CONV NVSDK_NGX_Parameter_SetF(NVSDK_NGX_Parameter* p,const char* n,float v){p->Set(n,v);}
void NVSDK_CONV NVSDK_NGX_Parameter_SetVoidPointer(NVSDK_NGX_Parameter* p,const char* n,void* v){p->Set(n,v);}
void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d12Resource(NVSDK_NGX_Parameter* p,const char* n,ID3D12Resource* v){p->Set(n,v);}
#endif

int main(){
    using namespace edvr;using namespace edvr::fg_ngx;
    FrameGenerationNgx untouched;check(!untouched.status().initialized,"default backend creates no device");
    check(untouched.collect(),"cold OFF collection creates no backend");
#ifdef EDVR_HAVE_NGX
    coupledInputRing();
#endif
    check(generatedCount(2)==1&&generatedCount(4)==3&&!generatedCount(3)&&!generatedCount(0),"exact 2x/4x only");
    check(supported(2,true,false,1)&&!supported(4,true,false,1)&&supported(4,true,false,3),"capability limits have no silent downgrade");
    check(!supported(2,false,false,3)&&!supported(4,true,true,3),"unavailable and old driver refuse");
    check(!completed(UINT64_MAX,1)&&!completed(0,1)&&completed(7,7),"device-removed sentinel is not completion");
    check(!reusable(6,7,1)&&!reusable(7,7,2)&&reusable(7,7,1),"GPU and consumer leases both gate reuse");
    check(timedOut(2100,100,2000)&&!timedOut(2099,100,2000),"bounded timeout boundary");
    check(resetHistory(0,1,0,1,false)&&resetHistory(1,3,1,1,false)&&resetHistory(1,2,1,2,false),"first gap and generation reset");
    check(!resetHistory(1,2,1,1,false)&&resetHistory(1,2,1,1,true),"continuous history and caller reset");
    check(resetHistoryReasons(0,1,0,1,true)==(kResetFirstSource|kResetGenerationChanged|kResetCaller)&&resetHistoryReasons(1,2,1,1,false)==0,"reset reason metadata distinguishes first source from continuous history");
    const uint8_t sentinelBytes[4]={255,255,255,255},disabled[4]={1,0,0,0},allowed[4]={0,255,255,255};
    check(!suppressionAllows(sentinelBytes)&&!suppressionAllows(disabled)&&suppressionAllows(allowed)&&!suppressionAllows(nullptr),"raw diagnostics never turn sentinel or nonzero suppression into permission");
    float identity[4][4]{};for(unsigned i=0;i<4;++i)identity[i][i]=1;
    check(inversePair(identity,identity),"matrix inverse identity");identity[0][0]=std::numeric_limits<float>::quiet_NaN();
    check(!inversePair(identity,identity),"nonfinite camera refuses");
    FrameGenerationNgxTicket empty;check(empty.poll()==FrameGenerationNgxCompletion::Empty,"empty ticket is not output");
    FrameGenerationNgxSuppressionSnapshot raw;raw.fenced=true;check(!empty.suppressionSnapshot(raw)&&!raw.fenced,"raw snapshot cannot claim an absent completion fence");
#ifdef EDVR_HAVE_NGX
    Parameters p;NVSDK_NGX_DLSSG_Opt_Eval_Params storage{};storage.jitterOffset[0]=.125f;storage.mvecScale[0]=-2.f;
    check(frameGenerationNgxTestEvaluation(2,1,&p,&storage),"2x calls actual official SDK evaluation adapter");
    check(p.ui[NVSDK_NGX_DLSSG_Parameter_MultiFrameCount]==1&&p.ui[NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex]==1,"actual SDK adapter count1 index1");
    check(p.pointers[NVSDK_NGX_DLSSG_Parameter_CameraViewToClip]==storage.cameraViewToClip&&p.pointers[NVSDK_NGX_DLSSG_Parameter_PrevClipToClip]==storage.prevClipToClip,"matrix pointers refer to persistent owner storage");
    check(p.floats[NVSDK_NGX_DLSSG_Parameter_JitterOffsetX]==.125f&&p.floats[NVSDK_NGX_DLSSG_Parameter_MvecScaleX]==-2.f,"jitter and signed motion scale preserved");
    check(p.pointers[NVSDK_NGX_DLSSG_Parameter_Backbuffer]==reinterpret_cast<void*>(1)&&p.pointers[NVSDK_NGX_DLSSG_Parameter_Depth]==reinterpret_cast<void*>(2)&&p.pointers[NVSDK_NGX_DLSSG_Parameter_MVecs]==reinterpret_cast<void*>(3),"paired color depth motion bound");
    check(p.pointers[NVSDK_NGX_DLSSG_Parameter_OutputInterpolated]==reinterpret_cast<void*>(4)&&p.pointers[NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation]==reinterpret_cast<void*>(5),"output and suppression buffer both bound");
    for(unsigned index=1;index<=3;++index){
        check(frameGenerationNgxTestEvaluation(4,index,&p,&storage),"4x indexed evaluation succeeds");
        check(p.ui[NVSDK_NGX_DLSSG_Parameter_MultiFrameCount]==3&&p.ui[NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex]==index,"actual SDK adapter count3 indexed1..3");
    }
    const auto before=evaluations;check(!frameGenerationNgxTestEvaluation(4,0,&p,&storage)&&!frameGenerationNgxTestEvaluation(4,4,&p,&storage)&&!frameGenerationNgxTestEvaluation(3,1,&p,&storage)&&evaluations==before,"malformed modes and indices never evaluate");
    evaluationResult=fail;check(!frameGenerationNgxTestEvaluation(2,1,&p,&storage),"NGX failure never reports evaluation success");
    check(uint32_t(NVSDK_NGX_Result_FAIL_PlatformError)==0xbad00002&&uint32_t(NVSDK_NGX_Result_FAIL_FeatureNotSupported)==0xbad00001,"PlatformError is distinct from unsupported hardware");
    initFixture=true;fixtureParameters=&p;scratchResult=NVSDK_NGX_Result_FAIL_PlatformError;scratchSize=0;scratchCalls=createCalls=0;
    auto trace=FrameGenerationNgx::testFeaturePreparation();
    check(trace.failed&&trace.failedInitStage==FrameGenerationNgxInitStage::ScratchQuery&&trace.failedInitEye==0,"actual preparation latches scratch-query failure stage");
    check(scratchCalls==1&&createCalls==0&&trace.creation[0].scratchCalled&&!trace.creation[0].createCalled&&trace.creation[0].scratchResult==0xbad00002,"failed scratch query is recorded and never silently ignored");
    scratchResult=NVSDK_NGX_Result_Success;createResult=NVSDK_NGX_Result_FAIL_PlatformError;scratchCalls=createCalls=0;
    trace=FrameGenerationNgx::testFeaturePreparation(1);
    check(trace.failed&&trace.failedInitStage==FrameGenerationNgxInitStage::FeatureCreate&&trace.failedInitEye==1,"actual preparation distinguishes feature-create failure");
    check(scratchCalls==1&&createCalls==1&&trace.creation[1].scratchResult==1&&trace.creation[1].scratchBytes==0&&trace.creation[1].createCalled&&trace.creation[1].createResult==0xbad00002&&!trace.creation[1].handleReturned,"successful zero scratch reaches create and preserves its result");
    createResult=NVSDK_NGX_Result_Success;scratchCalls=createCalls=0;trace=FrameGenerationNgx::testFeaturePreparation();
    check(!trace.failed&&trace.creation[0].createCalled&&trace.creation[0].handleReturned&&trace.creation[0].createResult==1,"actual feature helper success records a returned handle");
    scratchSize=64;scratchCalls=createCalls=0;trace=FrameGenerationNgx::testFeaturePreparation();
    check(trace.failed&&trace.failedInitStage==FrameGenerationNgxInitStage::ScratchAllocate&&trace.creation[0].scratchBytes==64&&createCalls==0,"nonzero required scratch cannot be skipped without an allocation");
    scratchResult=NVSDK_NGX_Result_FAIL_PlatformError;createResult=NVSDK_NGX_Result_FAIL_PlatformError;scratchCalls=createCalls=0;
    trace=FrameGenerationNgx::testFeaturePreparation(0,FrameGenerationNgxCreationMode::OfficialFgHelper);
    check(trace.failed&&trace.failedInitStage==FrameGenerationNgxInitStage::FeatureCreate&&scratchCalls==0&&createCalls==1&&!trace.creation[0].scratchCalled&&trace.creation[0].createResult==0xbad00002,"explicit official-helper route never queries scratch and create failure remains fatal");
    createResult=NVSDK_NGX_Result_Success;scratchCalls=createCalls=0;
    trace=FrameGenerationNgx::testFeaturePreparation(0,FrameGenerationNgxCreationMode::OfficialFgHelper);
    check(!trace.failed&&scratchCalls==0&&createCalls==1&&!trace.creation[0].scratchCalled&&trace.creation[0].scratchResult==0&&trace.creation[0].handleReturned,"direct helper success never converts an uncalled scratch query into success");
    createResult=NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall;scratchCalls=createCalls=0;
    trace=FrameGenerationNgx::testFeaturePreparation(0,FrameGenerationNgxCreationMode::OfficialFgHelper);
    check(trace.failed&&trace.failedInitStage==FrameGenerationNgxInitStage::FeatureCreate&&trace.creation[0].createResult==0xbad00006&&scratchCalls==0&&createCalls==1,"direct helper still refuses a real required-scratch creation error");
    initFixture=false;fixtureParameters=nullptr;
    evaluationResult=NVSDK_NGX_Result_Success;
    auto flags=recordFlags(false,false,0,0);
    check(flags.initializations[0]==3&&flags.initializations[1]==3&&!flags.allAllowed,
        "private per-index diagnostic retains untouched later sentinels");
    flags=recordFlags(true,false,0,0);
    check(flags.initializations[0]==1&&flags.initializations[1]==1&&flags.allAllowed,
        "shared native pair initializes FF exactly once per eye");
    check(flags.pointer[0][0]==flags.pointer[0][1]&&flags.pointer[0][1]==flags.pointer[0][2]&&
        flags.pointer[1][0]==flags.pointer[1][2]&&flags.pointer[0][0]!=flags.pointer[1][0],
        "shared identity is stable across phases but isolated between eyes");
    flags=recordFlags(true,true,0,0);
    check(flags.initializations[0]==1&&flags.initializations[1]==1&&flags.allAllowed,
        "reused shared resource resets once then returns COPY_SOURCE to UAV without reinitializing");
    flags=recordFlags(true,false,1,0);check(!flags.allAllowed,"one eye's real suppression blocks entire pair");
    flags=recordFlags(true,false,-1,-1);check(!flags.allAllowed,"an untouched shared sentinel remains forbidden");
    flags=recordFlags(true,false,0,0,true);check(!flags.allAllowed,"later true suppression is retained and blocks the batch");
    // Exercise the production descriptor adapter, including non-default values
    // and persistent SDK-owned array addresses; no SDK struct layout is copied.
    FrameGenerationCamera measured;
    measured.cameraViewToClip[1][2]=3.5f;measured.clipToCameraView[2][1]=4.5f;
    measured.clipToLensClip[3][0]=5.5f;measured.clipToPrevClip[0][3]=6.5f;measured.prevClipToClip[3][2]=7.5f;
    measured.jitterOffset[0]=.25f;measured.mvecScale[1]=-.5f;measured.cameraPinholeOffset[1]=.75f;
    measured.cameraPos[2]=9;measured.cameraUp[1]=10;measured.cameraRight[0]=11;measured.cameraFwd[2]=12;
    measured.cameraNear=.025f;measured.cameraFar=1000;measured.cameraFOV=1.2f;measured.cameraAspectRatio=1.5f;
    measured.motionVectorsInvalidValue=65504;measured.minRelativeLinearDepthObjectSeparation=41;
    measured.depthInverted=measured.cameraMotionIncluded=measured.reset=measured.orthoProjection=true;
    measured.motionVectorsDilated=measured.menuDetectionEnabled=true;
    check(frameGenerationNgxTestEvaluation(2,1,&p,&storage,nullptr,&measured),"measured descriptor reaches actual SDK adapter");
#define CHECK_CAMERA_ARRAY(field) check(std::memcmp(storage.field,measured.field,sizeof(storage.field))==0,"camera adapter preserves " #field)
    CHECK_CAMERA_ARRAY(cameraViewToClip);CHECK_CAMERA_ARRAY(clipToCameraView);CHECK_CAMERA_ARRAY(clipToLensClip);
    CHECK_CAMERA_ARRAY(clipToPrevClip);CHECK_CAMERA_ARRAY(prevClipToClip);CHECK_CAMERA_ARRAY(jitterOffset);
    CHECK_CAMERA_ARRAY(mvecScale);CHECK_CAMERA_ARRAY(cameraPinholeOffset);CHECK_CAMERA_ARRAY(cameraPos);
    CHECK_CAMERA_ARRAY(cameraUp);CHECK_CAMERA_ARRAY(cameraRight);CHECK_CAMERA_ARRAY(cameraFwd);
#undef CHECK_CAMERA_ARRAY
    check(storage.cameraNear==measured.cameraNear&&storage.cameraFar==measured.cameraFar&&
        storage.cameraFOV==measured.cameraFOV&&storage.cameraAspectRatio==measured.cameraAspectRatio&&
        storage.motionVectorsInvalidValue==measured.motionVectorsInvalidValue&&
        storage.minRelativeLinearDepthObjectSeparation==measured.minRelativeLinearDepthObjectSeparation,
        "camera adapter preserves all scalar metadata");
    check(storage.depthInverted&&storage.cameraMotionIncluded&&storage.reset&&storage.orthoProjection&&
        storage.motionVectorsDilated&&storage.menuDetectionEnabled&&!storage.colorBuffersHDR&&
        !storage.automodeOverrideReset&&!storage.notRenderingGameFrames,
        "camera adapter preserves flags and unrelated SDK defaults");
    std::printf("FG NGX CPU checks=%u failures=%u NGX_SDK=enabled NGX=stub GPU=0 driver=0 OpenXR=0\n",checks,failures);
#else
    FrameGenerationNgxInit init;init.device=reinterpret_cast<ID3D12Device*>(1);
    check(!untouched.initialize(init),"SDK-absent backend refuses before touching even a supplied device");
    const auto unavailable=untouched.status();
    check(unavailable.failed&&!unavailable.initialized&&!unavailable.available&&
        unavailable.failure==FrameGenerationNgxFailure::SdkUnavailable&&unavailable.lastHresult==E_NOINTERFACE,
        "SDK absence is an explicit unavailable failure");
    FrameGenerationNgxInput input;input.inputLease=std::make_shared<unsigned>(7);
    input.producerFence=reinterpret_cast<ID3D12Fence*>(1);input.producerValue=1;
    const auto refused=untouched.submit(input);
    check(!refused.lease&&!refused.completionFence&&!refused.evaluationSucceeded&&
        !refused.generatedCount&&refused.poll()==FrameGenerationNgxCompletion::Empty,
        "SDK-absent submit publishes no pixels or GPU ownership");
    check(input.inputLease.use_count()==1,"SDK-absent backend never retains producer input");
    check(untouched.collect()&&untouched.drain(5000),"unavailable backend has no resources to drain");
    std::printf("FG NGX CPU checks=%u failures=%u NGX_SDK=absent adapter_checks=unavailable NGX=0 GPU=0 driver=0 OpenXR=0\n",checks,failures);
#endif
    return failures?1:0;
}

#include "frame_generation_ngx.h"
#include "frame_generation_ngx_policy.h"
#ifdef EDVR_HAVE_NGX
#include "nvsdk_ngx_helpers_dlssg_d3d.h"
#endif
#include <windows.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

namespace edvr {
using Microsoft::WRL::ComPtr;
const char* frameGenerationNgxCreationModeName(FrameGenerationNgxCreationMode mode)noexcept {
    switch(mode){
        case FrameGenerationNgxCreationMode::GenericScratchQuery:return "generic_scratch_query";
        case FrameGenerationNgxCreationMode::OfficialFgHelper:return "official_fg_helper";
    }
    return "unknown";
}
const char* frameGenerationNgxSuppressionLayoutName(FrameGenerationNgxSuppressionLayout layout)noexcept {
    switch(layout){
        case FrameGenerationNgxSuppressionLayout::PrivatePerIndex:return "private_per_index";
        case FrameGenerationNgxSuppressionLayout::SharedNativePair:return "shared_native_pair";
    }
    return "unknown";
}
const char* frameGenerationNgxInitStageName(FrameGenerationNgxInitStage stage)noexcept {
    switch(stage){
        case FrameGenerationNgxInitStage::NotStarted:return "not_started";
        case FrameGenerationNgxInitStage::NgxInitialize:return "ngx_initialize";
        case FrameGenerationNgxInitStage::Capabilities:return "capabilities";
        case FrameGenerationNgxInitStage::GraphicsSetup:return "graphics_setup";
        case FrameGenerationNgxInitStage::AllocateParameters:return "allocate_parameters";
        case FrameGenerationNgxInitStage::ScratchQuery:return "scratch_query";
        case FrameGenerationNgxInitStage::ScratchAllocate:return "scratch_allocate";
        case FrameGenerationNgxInitStage::FeatureCreate:return "feature_create";
        case FrameGenerationNgxInitStage::SubmitCreate:return "submit_create";
        case FrameGenerationNgxInitStage::Ready:return "ready";
    }
    return "unknown";
}
#ifdef EDVR_HAVE_NGX
namespace {
bool ok(NVSDK_NGX_Result value)noexcept{return NVSDK_NGX_SUCCEED(value);}
D3D12_RESOURCE_STATES flagState(fg_ngx::SuppressionState state)noexcept {
    switch(state){
        case fg_ngx::SuppressionState::CopyDestination:return D3D12_RESOURCE_STATE_COPY_DEST;
        case fg_ngx::SuppressionState::UnorderedAccess:return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case fg_ngx::SuppressionState::CopySource:return D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    if(before==after)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);
}
void uavBarrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;b.UAV.pResource=resource;list->ResourceBarrier(1,&b);
}
bool buffer(ID3D12Device* device,uint64_t bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_FLAGS flags,
            D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& out){
    D3D12_HEAP_PROPERTIES h{};h.Type=heap;h.CreationNodeMask=h.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;
    d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
    return SUCCEEDED(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&out)));
}
bool outputTexture(ID3D12Device* device,unsigned w,unsigned h,DXGI_FORMAT format,ComPtr<ID3D12Resource>& out){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;
    d.Format=format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&out)));
}
bool cameraValid(const FrameGenerationCamera& c){
    if(!fg_ngx::inversePair(c.cameraViewToClip,c.clipToCameraView)||!fg_ngx::inversePair(c.clipToPrevClip,c.prevClipToClip)||!fg_ngx::finiteMatrix(c.clipToLensClip))return false;
    const float* groups[]={c.jitterOffset,c.mvecScale,c.cameraPinholeOffset,c.cameraPos,c.cameraUp,c.cameraRight,c.cameraFwd};
    for(unsigned g=0;g<7;++g)for(unsigned i=0;i<(g<3?2u:3u);++i)if(!std::isfinite(groups[g][i]))return false;
    return c.mvecScale[0]!=0&&c.mvecScale[1]!=0&&std::isfinite(c.cameraNear)&&std::isfinite(c.cameraFar)&&
        c.cameraNear>0&&c.cameraFar>c.cameraNear&&std::isfinite(c.cameraFOV)&&c.cameraFOV>0&&c.cameraFOV<3.142f&&
        std::isfinite(c.cameraAspectRatio)&&c.cameraAspectRatio>0&&std::isfinite(c.motionVectorsInvalidValue)&&
        std::isfinite(c.minRelativeLinearDepthObjectSeparation)&&!c.orthoProjection;
}
NVSDK_NGX_Result recordEvaluation(ID3D12GraphicsCommandList* list,NVSDK_NGX_Handle* handle,NVSDK_NGX_Parameter* params,
    NVSDK_NGX_D3D12_DLSSG_Eval_Params& resources,NVSDK_NGX_DLSSG_Opt_Eval_Params& persistent){
    // NGX 3749594, nvsdk_ngx_defs_dlssg.h: BackbufferFrameID counts every fully rendered
    // backbuffer, including frames rendered with FG off. We have an XR source sequence
    // here, so leave the optional render-frame counter unset.
    return NGX_D3D12_EVALUATE_DLSSG(list,handle,params,&resources,&persistent);
}
void sdkRecompositionInputs(const FrameGenerationNgxEye* eye, unsigned width, unsigned height,
    NVSDK_NGX_D3D12_DLSSG_Eval_Params& resources, NVSDK_NGX_DLSSG_Opt_Eval_Params& options) {
    resources.pHudless = eye ? eye->hudless : nullptr;
    resources.pUI = eye ? eye->ui : nullptr;
    const NVSDK_NGX_Dimensions extent{eye ? width : 0u, eye ? height : 0u};
    options.hudLessSubrectSize = extent;
    options.uiSubrectSize = extent;
}
// Preserve SDK defaults for resource rectangles, frame indices and options
// outside ED-UP's measured camera contract. This output lives in the GPU slot.
NVSDK_NGX_DLSSG_Opt_Eval_Params sdkCamera(const FrameGenerationCamera& camera) {
    NVSDK_NGX_DLSSG_Opt_Eval_Params out{};
#define EDVR_COPY_CAMERA_ARRAY(field) std::memcpy(out.field,camera.field,sizeof(out.field))
    EDVR_COPY_CAMERA_ARRAY(cameraViewToClip);EDVR_COPY_CAMERA_ARRAY(clipToCameraView);
    EDVR_COPY_CAMERA_ARRAY(clipToLensClip);EDVR_COPY_CAMERA_ARRAY(clipToPrevClip);EDVR_COPY_CAMERA_ARRAY(prevClipToClip);
    EDVR_COPY_CAMERA_ARRAY(jitterOffset);EDVR_COPY_CAMERA_ARRAY(mvecScale);EDVR_COPY_CAMERA_ARRAY(cameraPinholeOffset);
    EDVR_COPY_CAMERA_ARRAY(cameraPos);EDVR_COPY_CAMERA_ARRAY(cameraUp);EDVR_COPY_CAMERA_ARRAY(cameraRight);EDVR_COPY_CAMERA_ARRAY(cameraFwd);
#undef EDVR_COPY_CAMERA_ARRAY
    out.cameraNear=camera.cameraNear;out.cameraFar=camera.cameraFar;
    out.cameraFOV=camera.cameraFOV;out.cameraAspectRatio=camera.cameraAspectRatio;
    out.motionVectorsInvalidValue=camera.motionVectorsInvalidValue;
    out.minRelativeLinearDepthObjectSeparation=camera.minRelativeLinearDepthObjectSeparation;
    out.depthInverted=camera.depthInverted;out.cameraMotionIncluded=camera.cameraMotionIncluded;
    out.reset=camera.reset;out.orthoProjection=camera.orthoProjection;
    out.motionVectorsDilated=camera.motionVectorsDilated;out.menuDetectionEnabled=camera.menuDetectionEnabled;
    return out;
}
// NGX Init may retain common info/path pointers. With no context-wide Shutdown,
// this small original context descriptor intentionally lives for the process.
struct Paths {
    std::wstring feature,data;
    const wchar_t* paths[1]{};
    NVSDK_NGX_FeatureCommonInfo common{};
    std::unique_ptr<Paths> next;
};
// A successful private initialization can retain its path list for the NGX
// context lifetime. Module pinning keeps this list alive through process exit.
std::mutex contextPathsMutex;
std::unique_ptr<Paths> contextPaths;
NVSDK_NGX_Result initializeDeviceContext(const FrameGenerationNgxInit& in) {
    if(!in.privateNgxDevice)return NVSDK_NGX_Result_Success;
    auto paths=std::make_unique<Paths>();
    paths->feature=in.featureDirectory;paths->data=in.dataDirectory;
    paths->paths[0]=paths->feature.c_str();paths->common.PathListInfo={paths->paths,1};
    const auto result=NVSDK_NGX_D3D12_Init_with_ProjectID("c5f699da-4b32-4dd3-a623-764006836b5b",NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        "EDUP-FG-0.1",paths->data.c_str(),in.device,&paths->common,NVSDK_NGX_Version_API);
    if(ok(result)) {
        std::lock_guard<std::mutex> retain(contextPathsMutex);
        paths->next=std::move(contextPaths);contextPaths=std::move(paths);
    }
    return result;
}

}

struct FrameGenerationNgxLease {
    static constexpr unsigned inputCount = 5;
    ComPtr<ID3D12Fence> fence,producer;
    ComPtr<ID3D12Resource> outputs[2][3],disable[2][3],readback[2][3],inputs[2][inputCount];
    uint8_t* mapped[2][3]{};
    uint32_t evaluationResult[2][3]{};
    std::shared_ptr<void> inputLease;
    uint64_t value=0,submittedMs=0;unsigned count=0;uint32_t timeoutMs=0;
    FrameGenerationNgxSuppressionLayout layout=FrameGenerationNgxSuppressionLayout::PrivatePerIndex;
    mutable std::atomic<bool> expired{false};
    void releaseInputs() noexcept {
        inputLease.reset();
        producer.Reset();
        for (auto& eye : inputs) for (auto& input : eye) input.Reset();
    }
    ~FrameGenerationNgxLease(){
        const D3D12_RANGE noWrites{0,0};
        for(unsigned e=0;e<2;++e)for(unsigned i=0;i<3;++i)if(mapped[e][i])readback[e][i]->Unmap(0,&noWrites);
    }
};
struct FrameGenerationNgxQualification {
    std::weak_ptr<FrameGenerationNgxLease> evaluation;
    ComPtr<ID3D12Fence> evaluationFence, readbackFence;
    uint64_t evaluationValue = 0, readbackValue = 0, sequence = 0, history = 0;
};
FrameGenerationNgxCompletion FrameGenerationNgxTicket::poll()const noexcept {
    if(!lease)return FrameGenerationNgxCompletion::Empty;
    if(!generatedCount||generatedCount>fg_ngx::kMaxGenerated||generatedCount!=lease->count)return FrameGenerationNgxCompletion::FailedEvaluation;
    if(!completionFence||!completionValue)return FrameGenerationNgxCompletion::FailedEvaluation;
    const auto value=completionFence->GetCompletedValue();
    if(value==UINT64_MAX)return FrameGenerationNgxCompletion::DeviceRemoved;
    if(lease->expired.load(std::memory_order_acquire))return FrameGenerationNgxCompletion::Expired;
    const bool aged=fg_ngx::timedOut(GetTickCount64(),lease->submittedMs,lease->timeoutMs);
    if(fg_ngx::completed(value,completionValue)){
        if(aged)return FrameGenerationNgxCompletion::Stale;
        if(!evaluationSucceeded)return FrameGenerationNgxCompletion::FailedEvaluation;
        for(unsigned e=0;e<2;++e)for(unsigned i=0;i<generatedCount;++i)
            if(!fg_ngx::suppressionAllows(lease->mapped[e][i]))return FrameGenerationNgxCompletion::Suppressed;
        return FrameGenerationNgxCompletion::Ready;
    }
    if(aged){
        lease->expired.store(true,std::memory_order_release);return FrameGenerationNgxCompletion::Expired;
    }
    return FrameGenerationNgxCompletion::Pending;
}
bool FrameGenerationNgxTicket::suppressionSnapshot(FrameGenerationNgxSuppressionSnapshot& out)const noexcept {
    out={};
    if(!lease||!generatedCount||generatedCount>fg_ngx::kMaxGenerated||generatedCount!=lease->count||!completionFence||!fg_ngx::completed(completionFence->GetCompletedValue(),completionValue))return false;
    out.fenced=true;out.sequence=sequence;out.generatedCount=generatedCount;out.historyResetReasons=historyResetReasons;
    out.layout=lease->layout;
    for(unsigned e=0;e<2;++e)for(unsigned i=0;i<generatedCount;++i){
        if(!lease->mapped[e][i]){out={};return false;}
        for(unsigned b=0;b<4;++b)out.raw[e][i][b]=lease->mapped[e][i][b];
        out.evaluationResult[e][i]=lease->evaluationResult[e][i];
        out.resourceIdentity[e][i]=reinterpret_cast<uintptr_t>(lease->disable[e][i].Get());
    }
    return true;
}

struct FrameGenerationNgx::Impl {
    std::mutex mutex;
    FrameGenerationNgxInit config{};FrameGenerationNgxStatus status{};
    DWORD owner=0;
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> createAllocator;ComPtr<ID3D12GraphicsCommandList> createList;
    ComPtr<ID3D12Resource> scratch[2],sentinel;
    NVSDK_NGX_Parameter* createParams[2]{};NVSDK_NGX_Handle* features[2]{};
    HANDLE event=nullptr;
    uint64_t nextFence=0,lastFence=0,createSubmittedMs=0,lastSequence=0,lastGeneration=0;
    bool unsafePending=false,cleaned=false;
    struct Slot {
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        std::shared_ptr<FrameGenerationNgxLease> lease;
        NVSDK_NGX_Parameter* params[2][3]{};
        NVSDK_NGX_DLSSG_Opt_Eval_Params options[2][3]{};
        uint64_t value=0;bool counted=false;
    } slots[fg_ngx::kSlots];
    void stage(FrameGenerationNgxInitStage next,unsigned eye=2)noexcept {status.initStage=next;status.initEye=eye;}
    bool fail(FrameGenerationNgxFailure why,NVSDK_NGX_Result ngx=NVSDK_NGX_Result_Success,HRESULT hr=S_OK,bool hard=true){
        if(hard&&!status.failed){status.failedInitStage=status.initStage;status.failedInitEye=status.initEye;}
        status.failure=why;status.lastNgxResult=uint32_t(ngx);status.lastHresult=hr;if(hard)status.failed=true;return false;
    }
    bool prepareFeature(unsigned eye){
        stage(FrameGenerationNgxInitStage::AllocateParameters,eye);
        auto result=NVSDK_NGX_D3D12_AllocateParameters(&createParams[eye]);
        if(!ok(result)||!createParams[eye])return fail(FrameGenerationNgxFailure::NgxParameters,result);
        auto* params=createParams[eye];auto& call=status.creation[eye];
        params->Set(NVSDK_NGX_Parameter_Width,config.width);params->Set(NVSDK_NGX_Parameter_Height,config.height);
        params->Set(NVSDK_NGX_Parameter_CreationNodeMask,1u);params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask,1u);
        params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat,unsigned(config.colorFormat));
        params->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth,config.guideWidth);params->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight,config.guideHeight);
        params->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution,0u);
        params->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,fg_ngx::generatedCount(config.multiplier));params->Set(NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex,1u);
        params->Set(NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled,unsigned(config.uiRecomposition));
        if(config.creationMode==FrameGenerationNgxCreationMode::GenericScratchQuery){
            stage(FrameGenerationNgxInitStage::ScratchQuery,eye);call.scratchCalled=true;
            size_t scratchBytes=0;result=NVSDK_NGX_D3D12_GetScratchBufferSize(NVSDK_NGX_Feature_FrameGeneration,params,&scratchBytes);
            call.scratchResult=uint32_t(result);call.scratchBytes=uint64_t(scratchBytes);
            if(!ok(result))return fail(FrameGenerationNgxFailure::NgxCreate,result);
            if(scratchBytes){
                stage(FrameGenerationNgxInitStage::ScratchAllocate,eye);
                if(!device||!buffer(device.Get(),scratchBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,scratch[eye]))return fail(FrameGenerationNgxFailure::Device);
                params->Set(NVSDK_NGX_Parameter_Scratch,scratch[eye].Get());params->Set(NVSDK_NGX_Parameter_Scratch_SizeInBytes,static_cast<unsigned long long>(scratchBytes));
            }
        }
        stage(FrameGenerationNgxInitStage::FeatureCreate,eye);call.createCalled=true;
        NVSDK_NGX_DLSSG_Create_Params create{config.width,config.height,unsigned(config.colorFormat),config.guideWidth,config.guideHeight,false};
        result=NGX_D3D12_CREATE_DLSSG(createList.Get(),1,1,&features[eye],params,&create);
        call.createResult=uint32_t(result);call.handleReturned=features[eye]!=nullptr;
        return ok(result)&&features[eye]?true:fail(FrameGenerationNgxFailure::NgxCreate,result);
    }
    bool sameThread(){return GetCurrentThreadId()==owner||fail(FrameGenerationNgxFailure::WrongThread);}
    bool execute(ID3D12GraphicsCommandList* list,uint64_t& value,ID3D12Fence* producer=nullptr,uint64_t producerValue=0){
        HRESULT hr=list->Close();if(FAILED(hr))return fail(FrameGenerationNgxFailure::Queue,NVSDK_NGX_Result_Success,hr);
        if(producer){
            hr=queue->Wait(producer,producerValue);
            if(FAILED(hr)){unsafePending=true;status.quarantined=true;return fail(FrameGenerationNgxFailure::Queue,NVSDK_NGX_Result_Success,hr);}
        }
        ID3D12CommandList* lists[]={list};queue->ExecuteCommandLists(1,lists);
        value=++nextFence;hr=queue->Signal(fence.Get(),value);
        if(FAILED(hr)){unsafePending=true;status.quarantined=true;return fail(FrameGenerationNgxFailure::Queue,NVSDK_NGX_Result_Success,hr);}
        lastFence=value;return true;
    }
    bool collectCompleted(uint64_t completed){
        if(completed==UINT64_MAX) {
            const auto reason=device?device->GetDeviceRemovedReason():DXGI_ERROR_DEVICE_REMOVED;
            return fail(FrameGenerationNgxFailure::DeviceRemoved,NVSDK_NGX_Result_Success,
                FAILED(reason)?reason:DXGI_ERROR_DEVICE_REMOVED);
        }
        for(auto& slot:slots)if(slot.lease&&slot.lease->expired.load(std::memory_order_acquire))return fail(FrameGenerationNgxFailure::Timeout);
        if(fg_ngx::completed(completed,1))createSubmittedMs=0;
        if(lastFence&&!fg_ngx::completed(completed,lastFence)){
            const uint64_t now=GetTickCount64();
            if(createSubmittedMs&&fg_ngx::timedOut(now,createSubmittedMs,config.timeoutMs))return fail(FrameGenerationNgxFailure::Timeout);
            for(auto& s:slots)if(s.value&&!fg_ngx::completed(completed,s.value)&&s.lease&&fg_ngx::timedOut(now,s.lease->submittedMs,config.timeoutMs)){
                s.lease->expired.store(true);return fail(FrameGenerationNgxFailure::Timeout);
            }
        }
        for(auto& s:slots)if(s.value&&!s.counted&&fg_ngx::completed(completed,s.value)){
            // The ticket retains generated outputs/readbacks, not completed
            // producer inputs. Output publication owns its own original source
            // lease through the later HUD fence; never borrow these pins there.
            if (s.lease) s.lease->releaseInputs();
            s.counted=true;++status.completed;
        }
        return !status.failed;
    }
    bool collect(){return collectCompleted(fence->GetCompletedValue());}
    bool qualificationHealthy() const noexcept {
        return owner == GetCurrentThreadId() && status.initialized && !status.failed &&
            !status.quarantined && status.failure == FrameGenerationNgxFailure::None &&
            !cleaned && !unsafePending && config.uiRecomposition && config.multiplier == 2 &&
            config.suppressionLayout == FrameGenerationNgxSuppressionLayout::SharedNativePair &&
            fence && lastFence && lastSequence && lastGeneration;
    }
    bool ownsEvaluation(const std::weak_ptr<FrameGenerationNgxLease>& evaluation, uint64_t value) const noexcept {
        if (value != lastFence) return false;
        for (const auto& slot : slots)
            if (slot.lease && slot.value == value && slot.lease->value == value &&
                !evaluation.owner_before(slot.lease) && !slot.lease.owner_before(evaluation)) return true;
        return false;
    }
    bool resourceValid(ID3D12Resource* resource,unsigned w,unsigned h,DXGI_FORMAT format){
        if(!resource)return false;const auto d=resource->GetDesc();ComPtr<ID3D12Device> actual;
        return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D&&d.Width==w&&d.Height==h&&d.DepthOrArraySize==1&&d.MipLevels==1&&d.SampleDesc.Count==1&&d.Format==format&&
            !(d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)&&SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&actual)))&&actual.Get()==device.Get();
    }
    bool guidesValid(const FrameGenerationNgxInput& in){
        if(!in.pairedFullEyeGuides||!in.sequence||!in.historyGeneration||!in.producerFence||!in.producerValue||in.producerValue==UINT64_MAX||!in.inputLease)return false;
        if(in.eyes[0].color==in.eyes[1].color||in.eyes[0].depth==in.eyes[1].depth||in.eyes[0].motion==in.eyes[1].motion)return false;
        ComPtr<ID3D12Device> producerDevice;
        if(FAILED(in.producerFence->GetDevice(IID_PPV_ARGS(&producerDevice)))||producerDevice.Get()!=device.Get()||in.producerFence->GetCompletedValue()==UINT64_MAX)return false;
        for(const auto& e:in.eyes){
            const auto md=e.motion?e.motion->GetDesc():D3D12_RESOURCE_DESC{};
            if(!cameraValid(e.camera)||!resourceValid(e.color,config.width,config.height,config.colorFormat)||
               !resourceValid(e.depth,config.guideWidth,config.guideHeight,DXGI_FORMAT_R32_FLOAT)||
               (md.Format!=DXGI_FORMAT_R16G16_FLOAT&&md.Format!=DXGI_FORMAT_R32G32_FLOAT)||
               !resourceValid(e.motion,config.guideWidth,config.guideHeight,md.Format))return false;
            if (config.uiRecomposition) {
                if (!resourceValid(e.hudless, config.width, config.height, config.colorFormat) ||
                    !resourceValid(e.ui, config.width, config.height, config.colorFormat)) return false;
            } else if (e.hudless || e.ui) return false;
        }
        if (config.uiRecomposition) {
            // A native composite, its HUD-less scene, and UI are independent
            // images, including when one eye has a transparent UI plane.
            const ID3D12Resource* colorPlanes[] = {
                in.eyes[0].color, in.eyes[0].hudless, in.eyes[0].ui,
                in.eyes[1].color, in.eyes[1].hudless, in.eyes[1].ui,
            };
            for (unsigned i = 0; i < 6; ++i)
                for (unsigned j = 0; j < i; ++j)
                    if (colorPlanes[i] == colorPlanes[j]) return false;
        }
        return true;
    }
    bool prepareSlot(Slot& s){
        if(!s.allocator&&FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator))))return fail(FrameGenerationNgxFailure::Device);
        if(!s.list){
            if(FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list)))||FAILED(s.list->Close()))return fail(FrameGenerationNgxFailure::Device);
        }
        if(!s.lease)s.lease=std::make_shared<FrameGenerationNgxLease>();auto& lease=*s.lease;
        lease.count=fg_ngx::generatedCount(config.multiplier);lease.timeoutMs=config.timeoutMs;lease.fence=fence;
        lease.layout=config.suppressionLayout;
        for(unsigned e=0;e<2;++e)for(unsigned i=0;i<lease.count;++i){
            if(lease.outputs[e][i])continue;
            const unsigned flagOwner=fg_ngx::suppressionOwner(lease.layout==FrameGenerationNgxSuppressionLayout::SharedNativePair,i);
            if(!outputTexture(device.Get(),config.width,config.height,config.colorFormat,lease.outputs[e][i])||
               (flagOwner==i&&!buffer(device.Get(),8,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST,lease.disable[e][i]))||
               !buffer(device.Get(),8,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,lease.readback[e][i]))return fail(FrameGenerationNgxFailure::Device);
            if(flagOwner!=i)lease.disable[e][i]=lease.disable[e][flagOwner];
            const D3D12_RANGE read{0,4};void* mapped=nullptr;
            if(FAILED(lease.readback[e][i]->Map(0,&read,&mapped))||!mapped)return fail(FrameGenerationNgxFailure::Device);
            lease.mapped[e][i]=static_cast<uint8_t*>(mapped);
            if(!s.params[e][i]){const auto r=NVSDK_NGX_D3D12_AllocateParameters(&s.params[e][i]);if(!ok(r)||!s.params[e][i])return fail(FrameGenerationNgxFailure::NgxParameters,r);}
        }
        return true;
    }
    void cleanup(){
        if(cleaned)return;
        for(auto& feature:features)if(feature){NVSDK_NGX_D3D12_ReleaseFeature(feature);feature=nullptr;}
        for(auto& p:createParams)if(p){NVSDK_NGX_D3D12_DestroyParameters(p);p=nullptr;}
        for(auto& slot:slots){
            for(auto& eye:slot.params)for(auto& p:eye)if(p){NVSDK_NGX_D3D12_DestroyParameters(p);p=nullptr;}
            slot.lease.reset();slot.list.Reset();slot.allocator.Reset();
        }
        for(auto& s:scratch)s.Reset();sentinel.Reset();createList.Reset();createAllocator.Reset();queue.Reset();fence.Reset();device.Reset();
        if(event){CloseHandle(event);event=nullptr;}cleaned=true;status.initialized=false;
        // Deliberately no NVSDK_NGX_D3D12_Shutdown/Shutdown1: context ownership
        // can overlap game/other callers. Paths remain stable for that context.
    }
};

FrameGenerationNgx::~FrameGenerationNgx(){
    if(!impl_)return;
    if(drain(0)){delete impl_;impl_=nullptr;}
    else {impl_->status.quarantined=true;impl_=nullptr;} // retain every GPU lease until process exit
}
bool FrameGenerationNgx::initialize(const FrameGenerationNgxInit& in){
    if(impl_)return false;impl_=new Impl;auto& s=*impl_;s.owner=GetCurrentThreadId();s.config=in;
    s.status.requestedMultiplier=in.multiplier;s.status.creationMode=in.creationMode;s.status.suppressionLayout=in.suppressionLayout;
    if(!in.device||!in.width||!in.height||!in.guideWidth||!in.guideHeight||in.width>16384||in.height>16384||in.guideWidth>16384||in.guideHeight>16384||
       !fg_ngx::generatedCount(in.multiplier)||!in.timeoutMs||in.timeoutMs>fg_ngx::kMaxTimeoutMs||in.privateNgxDevice==in.ngxAlreadyInitialized||
       (in.creationMode!=FrameGenerationNgxCreationMode::GenericScratchQuery&&in.creationMode!=FrameGenerationNgxCreationMode::OfficialFgHelper)||
       (in.suppressionLayout!=FrameGenerationNgxSuppressionLayout::PrivatePerIndex&&in.suppressionLayout!=FrameGenerationNgxSuppressionLayout::SharedNativePair)||
       (in.colorFormat!=DXGI_FORMAT_R8G8B8A8_UNORM&&in.colorFormat!=DXGI_FORMAT_R16G16B16A16_FLOAT)||
       !in.featureDirectory||!in.dataDirectory)return s.fail(FrameGenerationNgxFailure::InvalidConfiguration);
    s.device=in.device;
    if(in.privateNgxDevice)s.stage(FrameGenerationNgxInitStage::NgxInitialize);
    const auto context=initializeDeviceContext(in);
    if(!ok(context))return s.fail(FrameGenerationNgxFailure::NgxInitialize,context);
    s.stage(FrameGenerationNgxInitStage::Capabilities);
    NVSDK_NGX_Parameter* caps=nullptr;auto r=NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if(!ok(r)||!caps)return s.fail(FrameGenerationNgxFailure::Capability,r);
    int available=0,needsDriver=0;
    const auto queried=caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available,&available);
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver,&needsDriver);
    unsigned queriedMaximum=0;
    const bool maximumKnown=ok(caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,&queriedMaximum));
    const auto limits=fg_ngx::capability(ok(queried),available!=0,needsDriver!=0,maximumKnown,queriedMaximum);
    NVSDK_NGX_D3D12_DestroyParameters(caps);
    s.status.available=ok(queried)&&available!=0;s.status.maximumGenerated=limits.maximumGenerated;
    s.status.knownMaximumMultiplier=limits.maximumMultiplier;
    if(!fg_ngx::supported(in.multiplier,s.status.available,needsDriver!=0,limits.maximumGenerated))return s.fail(FrameGenerationNgxFailure::MultiplierUnsupported,queried);
    s.stage(FrameGenerationNgxInitStage::GraphicsSetup);
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;q.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    HRESULT hr=in.device->CreateCommandQueue(&q,IID_PPV_ARGS(&s.queue));
    if(FAILED(hr)||FAILED(hr=in.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.fence)))||
       FAILED(hr=in.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.createAllocator)))||
       FAILED(hr=in.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.createAllocator.Get(),nullptr,IID_PPV_ARGS(&s.createList))))return s.fail(FrameGenerationNgxFailure::Device,NVSDK_NGX_Result_Success,hr);
    s.event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!s.event)return s.fail(FrameGenerationNgxFailure::Device);
    if(!buffer(in.device,8,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,s.sentinel))return s.fail(FrameGenerationNgxFailure::Device);
    void* memory=nullptr;const D3D12_RANGE noRead{0,0};if(FAILED(s.sentinel->Map(0,&noRead,&memory)))return s.fail(FrameGenerationNgxFailure::Device);
    for(unsigned i=0;i<8;++i)static_cast<uint8_t*>(memory)[i]=0xff;s.sentinel->Unmap(0,nullptr);
    bool featureOk=true;
    for(unsigned e=0;e<2;++e)if(!s.prepareFeature(e)){featureOk=false;break;}
    s.stage(FrameGenerationNgxInitStage::SubmitCreate);
    s.createSubmittedMs=GetTickCount64();if(!s.execute(s.createList.Get(),s.lastFence))return false;
    if(!featureOk)return false;s.stage(FrameGenerationNgxInitStage::Ready);s.status.initialized=true;s.status.failure=FrameGenerationNgxFailure::None;return true;
}
FrameGenerationNgxTicket FrameGenerationNgx::submit(const FrameGenerationNgxInput& in){
    FrameGenerationNgxTicket ticket;if(!impl_)return ticket;auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(!s.sameThread()||!s.status.initialized||s.status.failed||!s.collect())return ticket;
    if(in.sequence<=s.lastSequence){s.fail(FrameGenerationNgxFailure::Sequence,NVSDK_NGX_Result_Success,S_OK,false);return ticket;}
    if(!s.guidesValid(in)){s.fail(FrameGenerationNgxFailure::InvalidGuides,NVSDK_NGX_Result_Success,S_OK,false);return ticket;}
    const auto done=s.fence->GetCompletedValue();Impl::Slot* selected=nullptr;
    for(auto& candidate:s.slots)if(fg_ngx::reusable(done,candidate.value,unsigned(candidate.lease?candidate.lease.use_count():0))){selected=&candidate;break;}
    if(!selected){++s.status.busy;s.fail(FrameGenerationNgxFailure::Busy,NVSDK_NGX_Result_Success,S_OK,false);return ticket;}
    auto& slot=*selected;if(!s.prepareSlot(slot))return ticket;auto& lease=*slot.lease;
    lease.inputLease=in.inputLease;lease.producer=in.producerFence;
    for (unsigned e = 0; e < 2; ++e) {
        const auto& eye = in.eyes[e];
        auto& pins = lease.inputs[e];
        pins[0] = eye.color;
        pins[1] = eye.depth;
        pins[2] = eye.motion;
        pins[3] = eye.hudless;
        pins[4] = eye.ui;
    }
    HRESULT hr=slot.allocator->Reset();if(FAILED(hr)||FAILED(hr=slot.list->Reset(slot.allocator.Get(),nullptr))){s.fail(FrameGenerationNgxFailure::Queue,NVSDK_NGX_Result_Success,hr);return ticket;}
    const uint32_t resetReasons=fg_ngx::resetHistoryReasons(s.lastSequence,in.sequence,s.lastGeneration,in.historyGeneration,in.eyes[0].camera.reset||in.eyes[1].camera.reset);
    const bool reset=resetReasons!=0;
    bool evaluated=true;
    for(unsigned e=0;e<2&&evaluated;++e){
        const auto& eye = in.eyes[e];
        const auto& inputs = lease.inputs[e];
        const D3D12_RESOURCE_STATES states[] = {
            eye.colorState, eye.depthState, eye.motionState, eye.hudlessState, eye.uiState,
        };
        for (unsigned k = 0; k < lease.inputCount; ++k)
            if (inputs[k]) transition(slot.list.Get(), inputs[k].Get(), states[k], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(unsigned i=0;i<lease.count;++i){
            auto& o=slot.options[e][i];o=sdkCamera(eye.camera);o.multiFrameCount=lease.count;o.multiFrameIndex=i+1;o.reset=reset;
            o.backbufferSubrectBase={0,0};o.backbufferSubrectSize={s.config.width,s.config.height};o.outputInterpSubrectBase={0,0};o.outputInterpSubrectSize=o.backbufferSubrectSize;
            o.depthSubrectBase=o.mvecsSubrectBase={0,0};o.depthSubrectSize=o.mvecsSubrectSize={s.config.guideWidth,s.config.guideHeight};
            if(slot.value)transition(slot.list.Get(),lease.outputs[e][i].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            fg_ngx::prepareSuppressionFlag(lease.layout==FrameGenerationNgxSuppressionLayout::SharedNativePair,i,slot.value!=0,
                [&](fg_ngx::SuppressionState a,fg_ngx::SuppressionState b){transition(slot.list.Get(),lease.disable[e][i].Get(),flagState(a),flagState(b));},
                [&]{slot.list->CopyBufferRegion(lease.disable[e][i].Get(),0,s.sentinel.Get(),0,4);});
            if(s.scratch[e]){slot.params[e][i]->Set(NVSDK_NGX_Parameter_Scratch,s.scratch[e].Get());slot.params[e][i]->Set(NVSDK_NGX_Parameter_Scratch_SizeInBytes,static_cast<unsigned long long>(s.scratch[e]->GetDesc().Width));}
            NVSDK_NGX_D3D12_DLSSG_Eval_Params resources{};resources.pBackbuffer=eye.color;resources.pDepth=eye.depth;resources.pMVecs=eye.motion;
            sdkRecompositionInputs(s.config.uiRecomposition ? &eye : nullptr, s.config.width, s.config.height, resources, o);
            resources.pOutputInterpFrame=lease.outputs[e][i].Get();resources.pOutputDisableInterpolation=lease.disable[e][i].Get();
            if(s.scratch[e])uavBarrier(slot.list.Get(),s.scratch[e].Get());
            const auto r=recordEvaluation(slot.list.Get(),s.features[e],slot.params[e][i],resources,o);++s.status.evaluations;
            lease.evaluationResult[e][i]=uint32_t(r);
            if(s.scratch[e])uavBarrier(slot.list.Get(),s.scratch[e].Get());
            uavBarrier(slot.list.Get(),lease.outputs[e][i].Get());uavBarrier(slot.list.Get(),lease.disable[e][i].Get());
            transition(slot.list.Get(),lease.outputs[e][i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(slot.list.Get(),lease.disable[e][i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            slot.list->CopyBufferRegion(lease.readback[e][i].Get(),0,lease.disable[e][i].Get(),0,4);
            if(!ok(r)){s.fail(FrameGenerationNgxFailure::NgxEvaluate,r);evaluated=false;break;}
        }
        for (unsigned k = 0; k < lease.inputCount; ++k)
            if (inputs[k]) transition(slot.list.Get(), inputs[k].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, states[k]);
    }
    lease.submittedMs=GetTickCount64();slot.counted=false;
    ticket.sequence=in.sequence;ticket.generatedCount=lease.count;ticket.historyResetReasons=resetReasons;ticket.lease=slot.lease;
    if(!s.execute(slot.list.Get(),slot.value,in.producerFence,in.producerValue))return ticket;lease.value=slot.value;
    ticket.completionFence=s.fence.Get();ticket.completionValue=slot.value;ticket.evaluationSucceeded=evaluated;
    ticket.uiRecomposed = evaluated && s.config.uiRecomposition;
    for(unsigned e=0;e<2;++e)for(unsigned i=0;i<lease.count;++i)ticket.generated[e][i]=lease.outputs[e][i].Get();
    ++s.status.submitted;if(!evaluated)return ticket;
    s.lastSequence=in.sequence;s.lastGeneration=in.historyGeneration;if(reset)++s.status.resets;s.status.failure=FrameGenerationNgxFailure::None;return ticket;
}
bool FrameGenerationNgx::collect()noexcept {
    if(!impl_)return true;auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(s.cleaned)return true;
    if(!s.sameThread()||!s.status.initialized||s.unsafePending||!s.fence)return false;
    return s.collect();
}
std::shared_ptr<const FrameGenerationNgxQualification> FrameGenerationNgx::recordQualificationReadback(
    const FrameGenerationNgxTicket& ticket, ID3D12Fence* readback, uint64_t readbackValue) noexcept {
    if (!impl_ || !readback || !fg_ngx::completed(readbackValue, readbackValue)) return {};
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.qualificationHealthy() || !s.device || readback == s.fence.Get() ||
        ticket.sequence != s.lastSequence || ticket.completionFence != s.fence.Get() ||
        !ticket.uiRecomposed || !s.ownsEvaluation(ticket.lease, ticket.completionValue) ||
        ticket.poll() != FrameGenerationNgxCompletion::Ready) return {};
    ComPtr<ID3D12Device> actual;
    if (FAILED(readback->GetDevice(IID_PPV_ARGS(&actual))) || actual.Get() != s.device.Get() ||
        readback->GetCompletedValue() == UINT64_MAX) return {};
    try {
        auto receipt = std::make_shared<FrameGenerationNgxQualification>();
        receipt->evaluation = ticket.lease;
        receipt->evaluationFence = s.fence;
        receipt->readbackFence = readback;
        receipt->evaluationValue = ticket.completionValue;
        receipt->readbackValue = readbackValue;
        receipt->sequence = s.lastSequence;
        receipt->history = s.lastGeneration;
        return receipt;
    } catch (...) { return {}; }
}
bool FrameGenerationNgx::resetAfterQualification(
    const std::shared_ptr<const FrameGenerationNgxQualification>& receipt) noexcept {
    if (!impl_ || !receipt) return false;
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.qualificationHealthy() || receipt->evaluationFence.Get() != s.fence.Get() ||
        receipt->sequence != s.lastSequence || receipt->history != s.lastGeneration ||
        !s.ownsEvaluation(receipt->evaluation, receipt->evaluationValue) ||
        !receipt->readbackFence || receipt->readbackFence.Get() == s.fence.Get()) return false;

    const uint64_t completed = s.fence->GetCompletedValue();
    if (!fg_ngx::completed(completed, s.lastFence) ||
        !fg_ngx::completed(receipt->readbackFence->GetCompletedValue(), receipt->readbackValue)) return false;
    for (const auto& slot : s.slots)
        if (slot.lease && (slot.lease.use_count() != 1 || slot.lease->expired.load(std::memory_order_acquire))) return false;

    if (!s.collectCompleted(completed) || s.status.completed != s.status.submitted) return false;
    // Output resource states and every fence value belong to the retained
    // slots. Only software history identity starts over for the live seed.
    s.lastSequence = 0;
    s.lastGeneration = 0;
    return true;
}
void FrameGenerationNgx::quarantineQualification() noexcept {
    if (!impl_) return;
    auto& s = *impl_;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.unsafePending = true;
    s.status.quarantined = true;
}
bool FrameGenerationNgx::drain(uint32_t timeoutMs)noexcept {
    if(!impl_)return true;auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(s.cleaned)return true;if(!s.sameThread())return false;if(s.unsafePending)return false;
    if (s.lastFence && !s.fence) return false;
    if(s.lastFence&&!fg_ngx::completed(s.fence->GetCompletedValue(),s.lastFence)){
        if(!timeoutMs||timeoutMs>fg_ngx::kMaxTimeoutMs)return false;
        if(FAILED(s.fence->SetEventOnCompletion(s.lastFence,s.event))||WaitForSingleObject(s.event,timeoutMs)!=WAIT_OBJECT_0||!fg_ngx::completed(s.fence->GetCompletedValue(),s.lastFence)){
            s.fail(FrameGenerationNgxFailure::Timeout);s.status.quarantined=true;return false;
        }
    }
    // The terminal fence covers every submitted read. Release the input pins
    // even if a failed/expired ticket still owns its generated textures.
    for (auto& slot : s.slots) if (slot.lease) slot.lease->releaseInputs();
    s.cleanup();return true;
}
FrameGenerationNgxStatus FrameGenerationNgx::status()const noexcept {
    if(!impl_)return {};std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->status;
}
#ifdef EDVR_FRAME_GENERATION_NGX_TEST
bool FrameGenerationNgx::testCollectInputs(uint64_t completed,const uint64_t (&values)[3],
    unsigned expiredMask,std::shared_ptr<void> (&inputs)[3]){
    Impl local;
    for(unsigned i=0;i<fg_ngx::kSlots;++i){
        auto& slot=local.slots[i];slot.value=values[i];
        slot.lease=std::make_shared<FrameGenerationNgxLease>();
        slot.lease->inputLease=std::move(inputs[i]);
        slot.lease->expired.store((expiredMask&(1u<<i))!=0);
        slot.lease->submittedMs=GetTickCount64();
        if(values[i]>local.lastFence)local.lastFence=values[i];
    }
    const bool result=local.collectCompleted(completed);
    for(unsigned i=0;i<fg_ngx::kSlots;++i)inputs[i]=std::move(local.slots[i].lease->inputLease);
    return result;
}
bool FrameGenerationNgx::testDrainInput(ID3D12Fence* completion, uint64_t required,
    bool unsafe, bool expired, std::shared_ptr<void>& input) {
    auto local = std::make_unique<Impl>();
    local->owner = GetCurrentThreadId();
    local->fence = completion;
    local->lastFence = required;
    auto retained = std::make_shared<FrameGenerationNgxLease>();
    retained->inputLease = std::move(input);
    retained->expired.store(expired, std::memory_order_release);
    local->slots[0].lease = retained;
    local->slots[0].value = required;

    FrameGenerationNgx backend;
    backend.impl_ = local.get();
    if (unsafe) backend.quarantineQualification();
    const bool drained = backend.drain(0);
    backend.impl_ = nullptr;
    input = std::move(retained->inputLease);
    return drained;
}
FrameGenerationNgxQualificationObservation FrameGenerationNgx::testQualificationReset(
    ID3D12Fence* evaluation, ID3D12Fence* readback, FrameGenerationNgxQualificationFixture fixture) {
    using Fault = FrameGenerationNgxQualificationFixture::Fault;
    Impl local;
    local.owner = fixture.fault == Fault::WrongOwner ? 0 : GetCurrentThreadId();
    local.status.initialized = true;
    local.status.failure = FrameGenerationNgxFailure::None;
    local.status.submitted = 2;
    local.config.uiRecomposition = true;
    local.config.suppressionLayout = FrameGenerationNgxSuppressionLayout::SharedNativePair;
    local.fence = evaluation;
    local.lastFence = local.nextFence = 7;
    local.lastSequence = 12;
    local.lastGeneration = 8;
    local.unsafePending = fixture.fault == Fault::Unsafe;
    for (unsigned i = 0; i < 2; ++i) {
        auto& slot = local.slots[i];
        slot.value = 6u + i;
        slot.lease = std::make_shared<FrameGenerationNgxLease>();
        slot.lease->value = slot.value;
        slot.lease->inputLease = std::make_shared<unsigned>(i);
    }
    auto receipt = std::make_shared<FrameGenerationNgxQualification>();
    receipt->evaluation = local.slots[1].lease;
    receipt->evaluationFence = fixture.fault == Fault::WrongEvaluation ? readback : evaluation;
    receipt->readbackFence = fixture.fault == Fault::MissingReadback ? nullptr : readback;
    receipt->evaluationValue = local.lastFence;
    receipt->readbackValue = 4;
    receipt->sequence = local.lastSequence - unsigned(fixture.fault == Fault::WrongSequence);
    receipt->history = local.lastGeneration - unsigned(fixture.fault == Fault::WrongGeneration);
    if (fixture.fault == Fault::ReusedSlot) {
        auto& slot = local.slots[1];
        slot.lease = std::make_shared<FrameGenerationNgxLease>();
        slot.lease->value = slot.value;
        slot.lease->inputLease = std::make_shared<unsigned>(1);
    }
    if (fixture.fault == Fault::Expired) local.slots[0].lease->expired = true;
    std::shared_ptr<FrameGenerationNgxLease> retainedTicket;
    if (fixture.fault == Fault::PinnedLatest) retainedTicket = local.slots[1].lease;
    if (fixture.fault == Fault::PinnedEarlier) retainedTicket = local.slots[0].lease;

    FrameGenerationNgx backend;
    backend.impl_ = &local;
    FrameGenerationNgxQualificationObservation result;
    result.reset = backend.resetAfterQualification(receipt);
    result.repeatRefused = !backend.resetAfterQualification(receipt);
    backend.impl_ = nullptr;
    result.sequence = local.lastSequence;
    result.history = local.lastGeneration;
    result.completed = local.status.completed;
    result.preservedSlots = local.lastFence == 7 && local.nextFence == 7 && local.status.submitted == 2 &&
        local.slots[0].value == 6 && local.slots[1].value == 7 && local.slots[0].lease->value == 6 &&
        local.slots[1].lease->value == 7;
    result.retiredInputs = !local.slots[0].lease->inputLease && !local.slots[1].lease->inputLease;
    result.firstLiveResetReasons = fg_ngx::resetHistoryReasons(local.lastSequence, 1, local.lastGeneration, 9, true);
    return result;
}
FrameGenerationNgxStatus FrameGenerationNgx::testFeaturePreparation(unsigned eye,FrameGenerationNgxCreationMode creationMode,bool uiRecomposition){
    Impl local;local.config.width=local.config.guideWidth=1280;local.config.height=local.config.guideHeight=720;local.config.multiplier=2;
    local.config.uiRecomposition=uiRecomposition;
    local.config.creationMode=creationMode;local.status.creationMode=creationMode;
    if(eye>1){local.fail(FrameGenerationNgxFailure::InvalidConfiguration);return local.status;}
    local.prepareFeature(eye);return local.status;
}
unsigned frameGenerationNgxTestContextPaths(const FrameGenerationNgxInit& init,NVSDK_NGX_Result& result) {
    result=initializeDeviceContext(init);
    unsigned count=0;
    std::lock_guard<std::mutex> retain(contextPathsMutex);
    for(auto* p=contextPaths.get();p;p=p->next.get())++count;
    return count;
}
FrameGenerationNgxTicket frameGenerationNgxTestTimedTicket(ID3D12Fence* fence) {
    FrameGenerationNgxTicket ticket;
    ticket.generatedCount=1;ticket.completionFence=fence;ticket.completionValue=1;
    ticket.lease=std::make_shared<FrameGenerationNgxLease>();ticket.lease->count=1;
    // Every observation is past the deadline, independent of machine uptime.
    ticket.lease->submittedMs=0;ticket.lease->timeoutMs=0;
    return ticket;
}
bool frameGenerationNgxTestEvaluation(unsigned multiplier,unsigned index,NVSDK_NGX_Parameter* params,NVSDK_NGX_DLSSG_Opt_Eval_Params* persistent,ID3D12Resource* disableOverride,const FrameGenerationCamera* camera,const FrameGenerationNgxEye* source){
    const auto count=fg_ngx::generatedCount(multiplier);if(!count||!index||index>count||!params||!persistent)return false;
    if(camera)*persistent=sdkCamera(*camera);
    persistent->multiFrameCount=count;persistent->multiFrameIndex=index;
    NVSDK_NGX_D3D12_DLSSG_Eval_Params resources{};
    resources.pBackbuffer=reinterpret_cast<ID3D12Resource*>(1);resources.pDepth=reinterpret_cast<ID3D12Resource*>(2);resources.pMVecs=reinterpret_cast<ID3D12Resource*>(3);
    sdkRecompositionInputs(source, 1280, 720, resources, *persistent);
    resources.pOutputInterpFrame=reinterpret_cast<ID3D12Resource*>(4);resources.pOutputDisableInterpolation=disableOverride?disableOverride:reinterpret_cast<ID3D12Resource*>(5);
    return ok(recordEvaluation(nullptr,nullptr,params,resources,*persistent));
}
#endif
#else
// SDK-absent development builds keep the optional interface but cannot create
// NGX resources, accept input ownership, or publish generated pixels.
FrameGenerationNgx::~FrameGenerationNgx()=default;
bool FrameGenerationNgx::initialize(const FrameGenerationNgxInit&){return false;}
FrameGenerationNgxTicket FrameGenerationNgx::submit(const FrameGenerationNgxInput&){return {};}
bool FrameGenerationNgx::collect()noexcept{return true;}
std::shared_ptr<const FrameGenerationNgxQualification> FrameGenerationNgx::recordQualificationReadback(
    const FrameGenerationNgxTicket&, ID3D12Fence*, uint64_t) noexcept { return {}; }
bool FrameGenerationNgx::resetAfterQualification(
    const std::shared_ptr<const FrameGenerationNgxQualification>&) noexcept { return false; }
void FrameGenerationNgx::quarantineQualification() noexcept {}
bool FrameGenerationNgx::drain(uint32_t)noexcept{return true;}
FrameGenerationNgxStatus FrameGenerationNgx::status()const noexcept {
    FrameGenerationNgxStatus out;out.failed=true;out.failure=FrameGenerationNgxFailure::SdkUnavailable;
    out.lastHresult=E_NOINTERFACE;return out;
}
FrameGenerationNgxCompletion FrameGenerationNgxTicket::poll()const noexcept {
    return lease?FrameGenerationNgxCompletion::FailedEvaluation:FrameGenerationNgxCompletion::Empty;
}
bool FrameGenerationNgxTicket::suppressionSnapshot(FrameGenerationNgxSuppressionSnapshot& out)const noexcept {
    out={};return false;
}
#endif
} // namespace edvr

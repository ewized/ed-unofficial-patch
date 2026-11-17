#include "frame_generation_ngx.h"
#include "frame_generation_ngx_policy.h"
#include "../../third_party/ngx/include/nvsdk_ngx_helpers_dlssg_d3d.h"
#include <windows.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
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
bool cameraValid(const NVSDK_NGX_DLSSG_Opt_Eval_Params& c){
    if(!fg_ngx::inversePair(c.cameraViewToClip,c.clipToCameraView)||!fg_ngx::inversePair(c.clipToPrevClip,c.prevClipToClip)||!fg_ngx::finiteMatrix(c.clipToLensClip))return false;
    const float* groups[]={c.jitterOffset,c.mvecScale,c.cameraPinholeOffset,c.cameraPos,c.cameraUp,c.cameraRight,c.cameraFwd};
    for(unsigned g=0;g<7;++g)for(unsigned i=0;i<(g<3?2u:3u);++i)if(!std::isfinite(groups[g][i]))return false;
    return c.mvecScale[0]!=0&&c.mvecScale[1]!=0&&std::isfinite(c.cameraNear)&&std::isfinite(c.cameraFar)&&
        c.cameraNear>0&&c.cameraFar>c.cameraNear&&std::isfinite(c.cameraFOV)&&c.cameraFOV>0&&c.cameraFOV<3.142f&&
        std::isfinite(c.cameraAspectRatio)&&c.cameraAspectRatio>0&&std::isfinite(c.motionVectorsInvalidValue)&&
        std::isfinite(c.minRelativeLinearDepthObjectSeparation)&&!c.orthoProjection;
}
NVSDK_NGX_Result recordEvaluation(ID3D12GraphicsCommandList* list,NVSDK_NGX_Handle* handle,NVSDK_NGX_Parameter* params,
    NVSDK_NGX_D3D12_DLSSG_Eval_Params& resources,NVSDK_NGX_DLSSG_Opt_Eval_Params& persistent,uint64_t sequence){
    params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,static_cast<unsigned long long>(sequence));
    return NGX_D3D12_EVALUATE_DLSSG(list,handle,params,&resources,&persistent);
}
// NGX Init may retain common info/path pointers. With no context-wide Shutdown,
// this small original context descriptor intentionally lives for the process.
struct Paths {std::wstring feature,data;const wchar_t* paths[1]{};NVSDK_NGX_FeatureCommonInfo common{};};
}

struct FrameGenerationNgxLease {
    ComPtr<ID3D12Fence> fence,producer;
    ComPtr<ID3D12Resource> outputs[2][3],disable[2][3],readback[2][3],inputs[2][3];
    uint8_t* mapped[2][3]{};
    uint32_t evaluationResult[2][3]{};
    std::shared_ptr<void> inputLease;
    uint64_t value=0,submittedMs=0;unsigned count=0;uint32_t timeoutMs=0;
    FrameGenerationNgxSuppressionLayout layout=FrameGenerationNgxSuppressionLayout::PrivatePerIndex;
    mutable std::atomic<bool> expired{false};
    ~FrameGenerationNgxLease(){
        const D3D12_RANGE noWrites{0,0};
        for(unsigned e=0;e<2;++e)for(unsigned i=0;i<3;++i)if(mapped[e][i])readback[e][i]->Unmap(0,&noWrites);
    }
};
FrameGenerationNgxCompletion FrameGenerationNgxTicket::poll()const noexcept {
    if(!lease)return FrameGenerationNgxCompletion::Empty;
    if(!generatedCount||generatedCount>fg_ngx::kMaxGenerated||generatedCount!=lease->count)return FrameGenerationNgxCompletion::FailedEvaluation;
    if(!completionFence||!completionValue)return FrameGenerationNgxCompletion::FailedEvaluation;
    if(lease->expired.load(std::memory_order_acquire))return FrameGenerationNgxCompletion::Expired;
    if(fg_ngx::timedOut(GetTickCount64(),lease->submittedMs,lease->timeoutMs)){
        lease->expired.store(true,std::memory_order_release);return FrameGenerationNgxCompletion::Expired;
    }
    const auto value=completionFence->GetCompletedValue();
    if(value==UINT64_MAX)return FrameGenerationNgxCompletion::DeviceRemoved;
    if(fg_ngx::completed(value,completionValue)){
        if(!evaluationSucceeded)return FrameGenerationNgxCompletion::FailedEvaluation;
        for(unsigned e=0;e<2;++e)for(unsigned i=0;i<generatedCount;++i)
            if(!fg_ngx::suppressionAllows(lease->mapped[e][i]))return FrameGenerationNgxCompletion::Suppressed;
        return FrameGenerationNgxCompletion::Ready;
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
    Paths* paths=nullptr;
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
        params->Set(NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled,0u);
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
        if(completed==UINT64_MAX)return fail(FrameGenerationNgxFailure::DeviceRemoved);
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
            if(s.lease){
                s.lease->inputLease.reset();s.lease->producer.Reset();
                for(auto& eye:s.lease->inputs)for(auto& input:eye)input.Reset();
            }
            s.counted=true;++status.completed;
        }
        return !status.failed;
    }
    bool collect(){return collectCompleted(fence->GetCompletedValue());}
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
    s.device=in.device;s.paths=new Paths;s.paths->feature=in.featureDirectory;s.paths->data=in.dataDirectory;
    s.paths->paths[0]=s.paths->feature.c_str();s.paths->common.PathListInfo={s.paths->paths,1};
    if(in.privateNgxDevice){
        s.stage(FrameGenerationNgxInitStage::NgxInitialize);
        const auto r=NVSDK_NGX_D3D12_Init_with_ProjectID("c5f699da-4b32-4dd3-a623-764006836b5b",NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "EDUP-FG-0.1",s.paths->data.c_str(),in.device,&s.paths->common,NVSDK_NGX_Version_API);
        if(!ok(r))return s.fail(FrameGenerationNgxFailure::NgxInitialize,r);
    }
    s.stage(FrameGenerationNgxInitStage::Capabilities);
    NVSDK_NGX_Parameter* caps=nullptr;auto r=NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
    if(!ok(r)||!caps)return s.fail(FrameGenerationNgxFailure::Capability,r);
    int available=0,needsDriver=0;unsigned maximum=1;
    const auto queried=caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available,&available);
    caps->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver,&needsDriver);
    unsigned queriedMaximum=0;if(ok(caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax,&queriedMaximum))&&queriedMaximum>0)maximum=queriedMaximum;
    NVSDK_NGX_D3D12_DestroyParameters(caps);s.status.available=ok(queried)&&available!=0;s.status.maximumGenerated=maximum;
    if(!fg_ngx::supported(in.multiplier,s.status.available,needsDriver!=0,maximum))return s.fail(FrameGenerationNgxFailure::MultiplierUnsupported,queried);
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
    for(unsigned e=0;e<2;++e){lease.inputs[e][0]=in.eyes[e].color;lease.inputs[e][1]=in.eyes[e].depth;lease.inputs[e][2]=in.eyes[e].motion;}
    HRESULT hr=slot.allocator->Reset();if(FAILED(hr)||FAILED(hr=slot.list->Reset(slot.allocator.Get(),nullptr))){s.fail(FrameGenerationNgxFailure::Queue,NVSDK_NGX_Result_Success,hr);return ticket;}
    const uint32_t resetReasons=fg_ngx::resetHistoryReasons(s.lastSequence,in.sequence,s.lastGeneration,in.historyGeneration,in.eyes[0].camera.reset||in.eyes[1].camera.reset);
    const bool reset=resetReasons!=0;
    bool evaluated=true;
    for(unsigned e=0;e<2&&evaluated;++e){
        const auto& eye=in.eyes[e];ID3D12Resource* inputs[]={eye.color,eye.depth,eye.motion};
        D3D12_RESOURCE_STATES states[]={eye.colorState,eye.depthState,eye.motionState};
        for(unsigned k=0;k<3;++k){lease.inputs[e][k]=inputs[k];transition(slot.list.Get(),inputs[k],states[k],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
        for(unsigned i=0;i<lease.count;++i){
            auto& o=slot.options[e][i];o=eye.camera;o.multiFrameCount=lease.count;o.multiFrameIndex=i+1;o.reset=reset;
            o.backbufferSubrectBase={0,0};o.backbufferSubrectSize={s.config.width,s.config.height};o.outputInterpSubrectBase={0,0};o.outputInterpSubrectSize=o.backbufferSubrectSize;
            o.depthSubrectBase=o.mvecsSubrectBase={0,0};o.depthSubrectSize=o.mvecsSubrectSize={s.config.guideWidth,s.config.guideHeight};
            if(slot.value)transition(slot.list.Get(),lease.outputs[e][i].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            fg_ngx::prepareSuppressionFlag(lease.layout==FrameGenerationNgxSuppressionLayout::SharedNativePair,i,slot.value!=0,
                [&](fg_ngx::SuppressionState a,fg_ngx::SuppressionState b){transition(slot.list.Get(),lease.disable[e][i].Get(),flagState(a),flagState(b));},
                [&]{slot.list->CopyBufferRegion(lease.disable[e][i].Get(),0,s.sentinel.Get(),0,4);});
            if(s.scratch[e]){slot.params[e][i]->Set(NVSDK_NGX_Parameter_Scratch,s.scratch[e].Get());slot.params[e][i]->Set(NVSDK_NGX_Parameter_Scratch_SizeInBytes,static_cast<unsigned long long>(s.scratch[e]->GetDesc().Width));}
            NVSDK_NGX_D3D12_DLSSG_Eval_Params resources{};resources.pBackbuffer=eye.color;resources.pDepth=eye.depth;resources.pMVecs=eye.motion;
            resources.pOutputInterpFrame=lease.outputs[e][i].Get();resources.pOutputDisableInterpolation=lease.disable[e][i].Get();
            if(s.scratch[e])uavBarrier(slot.list.Get(),s.scratch[e].Get());
            const auto r=recordEvaluation(slot.list.Get(),s.features[e],slot.params[e][i],resources,o,in.sequence);++s.status.evaluations;
            lease.evaluationResult[e][i]=uint32_t(r);
            if(s.scratch[e])uavBarrier(slot.list.Get(),s.scratch[e].Get());
            uavBarrier(slot.list.Get(),lease.outputs[e][i].Get());uavBarrier(slot.list.Get(),lease.disable[e][i].Get());
            transition(slot.list.Get(),lease.outputs[e][i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            transition(slot.list.Get(),lease.disable[e][i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            slot.list->CopyBufferRegion(lease.readback[e][i].Get(),0,lease.disable[e][i].Get(),0,4);
            if(!ok(r)){s.fail(FrameGenerationNgxFailure::NgxEvaluate,r);evaluated=false;break;}
        }
        for(unsigned k=0;k<3;++k)transition(slot.list.Get(),inputs[k],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,states[k]);
    }
    lease.submittedMs=GetTickCount64();slot.counted=false;
    ticket.sequence=in.sequence;ticket.generatedCount=lease.count;ticket.historyResetReasons=resetReasons;ticket.lease=slot.lease;
    if(!s.execute(slot.list.Get(),slot.value,in.producerFence,in.producerValue))return ticket;lease.value=slot.value;
    ticket.completionFence=s.fence.Get();ticket.completionValue=slot.value;ticket.evaluationSucceeded=evaluated;
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
bool FrameGenerationNgx::drain(uint32_t timeoutMs)noexcept {
    if(!impl_)return true;auto& s=*impl_;std::lock_guard<std::mutex> lock(s.mutex);
    if(s.cleaned)return true;if(!s.sameThread())return false;if(s.unsafePending)return false;
    if(s.lastFence&&s.fence&&!fg_ngx::completed(s.fence->GetCompletedValue(),s.lastFence)){
        if(!timeoutMs||timeoutMs>fg_ngx::kMaxTimeoutMs)return false;
        if(FAILED(s.fence->SetEventOnCompletion(s.lastFence,s.event))||WaitForSingleObject(s.event,timeoutMs)!=WAIT_OBJECT_0||!fg_ngx::completed(s.fence->GetCompletedValue(),s.lastFence)){
            s.fail(FrameGenerationNgxFailure::Timeout);s.status.quarantined=true;return false;
        }
    }
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
FrameGenerationNgxStatus FrameGenerationNgx::testFeaturePreparation(unsigned eye,FrameGenerationNgxCreationMode creationMode){
    Impl local;local.config.width=local.config.guideWidth=1280;local.config.height=local.config.guideHeight=720;local.config.multiplier=2;
    local.config.creationMode=creationMode;local.status.creationMode=creationMode;
    if(eye>1){local.fail(FrameGenerationNgxFailure::InvalidConfiguration);return local.status;}
    local.prepareFeature(eye);return local.status;
}
bool frameGenerationNgxTestEvaluation(unsigned multiplier,unsigned index,NVSDK_NGX_Parameter* params,NVSDK_NGX_DLSSG_Opt_Eval_Params* persistent,ID3D12Resource* disableOverride){
    const auto count=fg_ngx::generatedCount(multiplier);if(!count||!index||index>count||!params||!persistent)return false;
    persistent->multiFrameCount=count;persistent->multiFrameIndex=index;
    NVSDK_NGX_D3D12_DLSSG_Eval_Params resources{};
    resources.pBackbuffer=reinterpret_cast<ID3D12Resource*>(1);resources.pDepth=reinterpret_cast<ID3D12Resource*>(2);resources.pMVecs=reinterpret_cast<ID3D12Resource*>(3);
    resources.pOutputInterpFrame=reinterpret_cast<ID3D12Resource*>(4);resources.pOutputDisableInterpolation=disableOverride?disableOverride:reinterpret_cast<ID3D12Resource*>(5);
    return ok(recordEvaluation(nullptr,nullptr,params,resources,*persistent,7));
}
#endif
} // namespace edvr

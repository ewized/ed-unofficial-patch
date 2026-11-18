#include "../../src/d3d11/frame_generation_ngx.h"
#include "../../src/common/native_fg_multiplier.h"
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

using Microsoft::WRL::ComPtr;
using namespace edvr;
namespace {
bool waitFence(ID3D12Fence* fence,uint64_t value,DWORD limit){
    if(!fence||!value)return true;const auto done=fence->GetCompletedValue();
    if(done==UINT64_MAX)return false;if(done>=value)return true;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return false;
    if(FAILED(fence->SetEventOnCompletion(value,event))){CloseHandle(event);return false;}
    if(WaitForSingleObject(event,limit)!=WAIT_OBJECT_0){
        // The fence still owns a pending event registration. Keep this handle
        // alive with the quarantined GPU owner instead of closing it early.
        return false;
    }
    CloseHandle(event);return fence->GetCompletedValue()!=UINT64_MAX&&fence->GetCompletedValue()>=value;
}
struct ProducerHold {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> rtvs;ComPtr<ID3D12Resource> readback;std::shared_ptr<void> inputs,consumerLease;
};
struct ProducerGuard {
    ProducerHold hold;uint64_t value=0;bool submitted=false,unfenced=false;
    ~ProducerGuard(){
        if(submitted&&(unfenced||!waitFence(hold.fence.Get(),value,5000))){
            auto* retained=new ProducerHold(std::move(hold));(void)retained;
            std::puts("producer quarantine: unfinished/unfenced GPU resources retained until process exit");
        }
    }
};
void printInitTrace(const FrameGenerationNgxStatus& status,bool initialized){
    std::printf("init %s failure=%u ngx=0x%08x hr=0x%08lx available=%u maxGenerated=%u method=%s suppression_layout=%s stage=%s eye=%u failedStage=%s failedEye=%u\n",
        initialized?"submitted":"refused",unsigned(status.failure),status.lastNgxResult,static_cast<unsigned long>(status.lastHresult),status.available,status.maximumGenerated,
        frameGenerationNgxCreationModeName(status.creationMode),frameGenerationNgxSuppressionLayoutName(status.suppressionLayout),frameGenerationNgxInitStageName(status.initStage),status.initEye,frameGenerationNgxInitStageName(status.failedInitStage),status.failedInitEye);
    for(unsigned eye=0;eye<2;++eye){const auto& call=status.creation[eye];
        std::printf("init_call eye=%u scratch_called=%u scratch_result=0x%08x scratch_bytes=%llu create_called=%u create_result=0x%08x handle_returned=%u\n",
            eye,call.scratchCalled,call.scratchResult,static_cast<unsigned long long>(call.scratchBytes),call.createCalled,call.createResult,call.handleReturned);
    }
}
void printSuppression(const FrameGenerationNgxTicket& ticket){
    FrameGenerationNgxSuppressionSnapshot snapshot;
    if(!ticket.suppressionSnapshot(snapshot)){std::puts("suppression_raw fenced=0");return;}
    for(unsigned e=0;e<2;++e)for(unsigned i=0;i<snapshot.generatedCount;++i){
        const auto* b=snapshot.raw[e][i];const bool sentinel=b[0]==0xff&&b[1]==0xff&&b[2]==0xff&&b[3]==0xff;
        std::printf("suppression_raw fenced=1 native=%llu eye=%u index=%u resource=%p bytes=%02x,%02x,%02x,%02x classification=%s reset_reasons=0x%02x eval_result=0x%08x layout=%s\n",
            static_cast<unsigned long long>(snapshot.sequence),e,i+1,reinterpret_cast<void*>(snapshot.resourceIdentity[e][i]),b[0],b[1],b[2],b[3],
            b[0]==0?"zero":sentinel?"sentinel_pattern":"nonzero",snapshot.historyResetReasons,snapshot.evaluationResult[e][i],frameGenerationNgxSuppressionLayoutName(snapshot.layout));
    }
}
void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&v);
}
bool inverse(const float (&m)[4][4],float (&out)[4][4]){
    double a[4][8]{};for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){a[y][x]=m[y][x];a[y][x+4]=x==y?1.:0.;}
    for(unsigned c=0;c<4;++c){unsigned pivot=c;for(unsigned y=c+1;y<4;++y)if(std::abs(a[y][c])>std::abs(a[pivot][c]))pivot=y;
        if(std::abs(a[pivot][c])<1e-12)return false;for(unsigned x=0;x<8;++x)std::swap(a[c][x],a[pivot][x]);
        const double scale=a[c][c];for(auto& value:a[c])value/=scale;
        for(unsigned y=0;y<4;++y)if(y!=c){const double factor=a[y][c];for(unsigned x=0;x<8;++x)a[y][x]-=factor*a[c][x];}}
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)out[y][x]=float(a[y][x+4]);return true;
}
FrameGenerationCamera camera(unsigned w,unsigned h){
    FrameGenerationCamera c{};c.cameraNear=.1f;c.cameraFar=1000.f;c.cameraFOV=1.2f;c.cameraAspectRatio=float(w)/float(h);
    const float sy=1.f/std::tan(c.cameraFOV*.5f),sx=sy/c.cameraAspectRatio;
    c.cameraViewToClip[0][0]=sx;c.cameraViewToClip[1][1]=sy;c.cameraViewToClip[2][2]=c.cameraFar/(c.cameraFar-c.cameraNear);
    c.cameraViewToClip[2][3]=1;c.cameraViewToClip[3][2]=-c.cameraNear*c.cameraFar/(c.cameraFar-c.cameraNear);
    inverse(c.cameraViewToClip,c.clipToCameraView);
    for(unsigned i=0;i<4;++i)c.clipToLensClip[i][i]=c.clipToPrevClip[i][i]=c.prevClipToClip[i][i]=1;
    c.cameraUp[1]=c.cameraRight[0]=c.cameraFwd[2]=1;c.mvecScale[0]=c.mvecScale[1]=1;
    c.cameraMotionIncluded=true;c.motionVectorsDilated=false;c.menuDetectionEnabled=false;return c;
}
bool texture(ID3D12Device* device,unsigned w,unsigned h,DXGI_FORMAT format,ComPtr<ID3D12Resource>& out){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;
    d.Format=format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)));
}
}
int wmain(int argc,wchar_t** argv){
    bool run=false,officialHelper=false,diagnosticSuppressed=false,sharedSuppression=false;
    unsigned multiplier=2,width=1280,height=720,guideWidth=0,guideHeight=0,nativeFrames=4,warmupFrames=1;std::wstring feature,data;
    for(int i=1;i<argc;++i){
        if(wcscmp(argv[i],L"--run")==0)run=true;
        else if(wcscmp(argv[i],L"--official-fg-helper")==0)officialHelper=true;
        else if(wcscmp(argv[i],L"--diagnostic-suppressed-output")==0)diagnosticSuppressed=true;
        else if(wcscmp(argv[i],L"--shared-native-pair-suppression")==0)sharedSuppression=true;
        else if(i+1<argc&&wcscmp(argv[i],L"--multiplier")==0)multiplier=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--feature-dir")==0)feature=argv[++i];
        else if(i+1<argc&&wcscmp(argv[i],L"--data-dir")==0)data=argv[++i];
        else if(i+1<argc&&wcscmp(argv[i],L"--width")==0)width=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--height")==0)height=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--guide-width")==0)guideWidth=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--guide-height")==0)guideHeight=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--native-frames")==0)nativeFrames=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--warmup-frames")==0)warmupFrames=unsigned(_wtoi(argv[++i]));
        else {std::puts("usage: probe [--run --multiplier 2|3|4 --feature-dir ABS --data-dir ABS [--official-fg-helper] [--shared-native-pair-suppression] [--width W --height H --guide-width GW --guide-height GH] [--native-frames N --warmup-frames N] [--diagnostic-suppressed-output]]");return 2;}
    }
    if(!guideWidth)guideWidth=width;if(!guideHeight)guideHeight=height;
    if(!nativeFrames||nativeFrames>64||warmupFrames>=nativeFrames){std::puts("native frames must be1..64 with warmup smaller than frame count");return 2;}
    if(!run){std::printf("DRY: method=%s layout=%s output=%ux%u guides=%ux%u nativeFrames=%u warmupFrames=%u suppressedReadback=%u no DXGI, device, NGX, GPU, game or XR calls. Explicit --run is required.\n",officialHelper?"official_fg_helper":"generic_scratch_query",sharedSuppression?"shared_native_pair":"private_per_index",width,height,guideWidth,guideHeight,nativeFrames,warmupFrames,diagnosticSuppressed);return 0;}
#ifndef EDVR_HAVE_NGX
    std::puts("REFUSED: this build has no NGX/DLSS SDK support; no GPU or driver calls were made.");return 3;
#endif
    if(!nativeFgFixedMultiplier(multiplier)||feature.empty()||data.empty()||!width||!height){std::puts("invalid explicit-run configuration");return 2;}
    // Everything below this line is an explicit GPU probe. No WARP/shader FG
    // fallback, game attachment, XR session, priority or system setting change.
    ComPtr<IDXGIFactory4> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 3;
    ComPtr<ID3D12Device> device;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};adapter->GetDesc1(&d);if(d.VendorId==0x10de&&!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))break;}
    if(!device){std::puts("no NVIDIA D3D12 hardware device");return 3;}
    FrameGenerationNgx backend;FrameGenerationNgxInit init;
    init.device=device.Get();init.width=width;init.height=height;init.guideWidth=guideWidth;init.guideHeight=guideHeight;init.multiplier=multiplier;
    init.creationMode=officialHelper?FrameGenerationNgxCreationMode::OfficialFgHelper:FrameGenerationNgxCreationMode::GenericScratchQuery;
    init.suppressionLayout=sharedSuppression?FrameGenerationNgxSuppressionLayout::SharedNativePair:FrameGenerationNgxSuppressionLayout::PrivatePerIndex;
    init.featureDirectory=feature.c_str();init.dataDirectory=data.c_str();init.privateNgxDevice=true;init.timeoutMs=5000;
    const bool initialized=backend.initialize(init);printInitTrace(backend.status(),initialized);
    if(!initialized){const bool drained=backend.drain(5000);std::printf("init_cleanup drain=%u quarantined=%u\n",drained,backend.status().quarantined);return 4;}
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;q.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    ComPtr<ID3D12Fence> producer;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    if(FAILED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))||FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&producer)))||
       FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))))return 5;
    list->Close();
    ComPtr<ID3D12DescriptorHeap> rtvs;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=6;
    if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs))))return 5;
    const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    struct Inputs {ComPtr<ID3D12Resource> resources[2][3];};auto resources=std::make_shared<Inputs>();
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT};
    for(unsigned e=0;e<2;++e)for(unsigned k=0;k<3;++k){if(!texture(device.Get(),k?guideWidth:width,k?guideHeight:height,formats[k],resources->resources[e][k]))return 5;
        auto h=rtvs->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(e*3+k)*stride;device->CreateRenderTargetView(resources->resources[e][k].Get(),nullptr,h);}
    ComPtr<ID3D12Resource> outputReadback;
    D3D12_HEAP_PROPERTIES readHeap{};readHeap.Type=D3D12_HEAP_TYPE_READBACK;readHeap.CreationNodeMask=readHeap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC readDesc{};readDesc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;readDesc.Width=6*512;readDesc.Height=1;
    readDesc.DepthOrArraySize=readDesc.MipLevels=1;readDesc.SampleDesc.Count=1;readDesc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(device->CreateCommittedResource(&readHeap,D3D12_HEAP_FLAG_NONE,&readDesc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&outputReadback))))return 5;
    ProducerGuard producerGuard;producerGuard.hold={device,queue,producer,allocator,list,rtvs,outputReadback,resources};
    unsigned readyBatches=0,postWarmupReady=0,suppressedReadbacks=0;uint64_t producerValue=0;
    for(uint64_t frame=1;frame<=nativeFrames;++frame){
        if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator.Get(),nullptr)))return 6;
        const float values[3][4]={{.2f,.2f,.2f,1.f},{.5f,0,0,0},{0,0,0,0}};
        for(unsigned e=0;e<2;++e)for(unsigned k=0;k<3;++k){auto* r=resources->resources[e][k].Get();barrier(list.Get(),r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto h=rtvs->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(e*3+k)*stride;list->ClearRenderTargetView(h,values[k],0,nullptr);barrier(list.Get(),r,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);}
        if(FAILED(list->Close()))return 6;ID3D12CommandList* commands[]={list.Get()};
        producerGuard.submitted=producerGuard.unfenced=true;queue->ExecuteCommandLists(1,commands);
        if(FAILED(queue->Signal(producer.Get(),++producerValue)))return 6;
        producerGuard.value=producerValue;producerGuard.unfenced=false;
        FrameGenerationNgxInput input;input.sequence=frame;input.historyGeneration=1;input.pairedFullEyeGuides=true;input.producerFence=producer.Get();input.producerValue=producerValue;input.inputLease=resources;
        for(unsigned e=0;e<2;++e){input.eyes[e].color=resources->resources[e][0].Get();input.eyes[e].depth=resources->resources[e][1].Get();input.eyes[e].motion=resources->resources[e][2].Get();input.eyes[e].camera=camera(width,height);input.eyes[e].camera.reset=frame==1;}
        auto ticket=backend.submit(input);auto completion=ticket.poll();
        while(completion==FrameGenerationNgxCompletion::Pending){Sleep(1);completion=ticket.poll();}
        const auto s=backend.status();std::printf("native=%llu requested=%ux indexedFrames=%u evalAccepted=%u completion=%u evaluations=%llu failure=%u ngx=0x%08x warmup=%u reset_reasons=0x%02x\n",
            static_cast<unsigned long long>(frame),multiplier,ticket.generatedCount,ticket.evaluationSucceeded,unsigned(completion),static_cast<unsigned long long>(s.evaluations),unsigned(s.failure),s.lastNgxResult,frame<=warmupFrames,ticket.historyResetReasons);
        printSuppression(ticket);
        const bool suppressedInspection=diagnosticSuppressed&&completion==FrameGenerationNgxCompletion::Suppressed;
        if(completion==FrameGenerationNgxCompletion::Ready||suppressedInspection){
            producerGuard.hold.consumerLease=ticket.lease;
            if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator.Get(),nullptr)))return 9;
            for(unsigned e=0;e<2;++e)for(unsigned i=0;i<ticket.generatedCount;++i){
                auto* r=ticket.generated[e][i];barrier(list.Get(),r,ticket.outputState,D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=outputReadback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset=(e*3+i)*512;dst.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,1,1,1,256};
                D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=r;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                const D3D12_BOX box{width/2,height/2,0,width/2+1,height/2+1,1};
                list->CopyTextureRegion(&dst,0,0,0,&src,&box);barrier(list.Get(),r,D3D12_RESOURCE_STATE_COPY_SOURCE,ticket.outputState);
            }
            if(FAILED(list->Close()))return 9;ID3D12CommandList* copies[]={list.Get()};producerGuard.unfenced=true;
            queue->ExecuteCommandLists(1,copies);if(FAILED(queue->Signal(producer.Get(),++producerValue)))return 9;
            producerGuard.value=producerValue;producerGuard.unfenced=false;
            if(!waitFence(producer.Get(),producerValue,5000))return 9;
            producerGuard.hold.consumerLease.reset();
            void* mapped=nullptr;const D3D12_RANGE reads{0,6*512};
            if(FAILED(outputReadback->Map(0,&reads,&mapped))||!mapped)return 9;
            bool pixels=true;
            for(unsigned e=0;e<2;++e)for(unsigned i=0;i<ticket.generatedCount;++i){
                const auto* rgba=static_cast<const uint8_t*>(mapped)+(e*3+i)*512;
                std::printf("output eye=%u index=%u centerRGBA=%u,%u,%u,%u suppressed_diagnostic_only=%u\n",e,i+1,rgba[0],rgba[1],rgba[2],rgba[3],suppressedInspection);
                for(unsigned c=0;c<3;++c)pixels=pixels&&rgba[c]>=35&&rgba[c]<=70;
                pixels=pixels&&rgba[3]>=250;
            }
            const D3D12_RANGE noWrites{0,0};outputReadback->Unmap(0,&noWrites);
            if(suppressedInspection){++suppressedReadbacks;std::printf("suppressed_output_diagnostic constantSceneMatch=%u presentable=0\n",pixels);}
            else {
                if(!pixels){std::puts("constant-scene output pixel mismatch");return 10;}
                ++readyBatches;if(frame>warmupFrames)++postWarmupReady;
            }
        }
        if(completion!=FrameGenerationNgxCompletion::Ready&&completion!=FrameGenerationNgxCompletion::Suppressed){backend.drain(5000);return 7;}
        // Backend completion also proves that these producer input resources
        // are restored to COMMON; the next native source may overwrite them.
        ticket={};
    }
    const bool drained=backend.drain(5000);
    std::printf("probe result readyStereoBatches=%u postWarmupReady=%u warmupFrames=%u nativeFrames=%u suppressedDiagnosticReadbacks=%u drain=%u outputPixels=center-per-eye-per-index GPUtiming=not-measured game=0 OpenXR=0 presentation=0; VR qualification remains separate\n",readyBatches,postWarmupReady,warmupFrames,nativeFrames,suppressedReadbacks,drained);
    return postWarmupReady&&drained?0:8;
}

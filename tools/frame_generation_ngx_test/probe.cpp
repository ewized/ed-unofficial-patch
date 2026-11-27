#include "../../src/d3d11/frame_generation_ngx.h"
#include "../../src/d3d11/frame_generation_qualification_fixture.h"
#include "qualification_fixture_cases.h"
#include "../../src/common/native_fg_multiplier.h"
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace edvr;
using namespace edvr::fg_fixture;
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
// Exercise the image check on the default CPU-only build gate. A valid
// halfway image must pass while either endpoint, a swapped eye, a repeated
// intermediate and a blend of the two endpoints must fail.
bool edgeCheckSelfTest(){
    static constexpr int width=256,barWidth=64,previous=64,current=96;
    const auto rowAt=[](int left,unsigned eye){
        std::array<uint8_t,width*4> row{};
        for(int x=0;x<width;++x){
            const bool foreground=x>=left&&x<left+barWidth;
            row[x*4+0]=eye==0?70:10;row[x*4+2]=eye==0?10:70;
            row[x*4+1]=foreground?217:20;row[x*4+3]=255;
        }
        return row;
    };
    const auto old=rowAt(previous,0),next=rowAt(current,0),middle=rowAt((previous+current)/2,0);
    const auto swapped=rowAt((previous+current)/2,1);
    auto blend=old;
    for(size_t i=0;i<blend.size();++i)blend[i]=uint8_t((unsigned(old[i])+next[i])/2);
    const auto native=measureBar(old.data(),width,0);
    const auto match=[&](const auto& row,const EdgeMeasurement& prior){
        return followsMotion(measureBar(row.data(),width,0),prior,previous,current,barWidth,1,2);
    };
    return match(middle,native)&&!match(old,native)&&!match(next,native)&&!match(swapped,native)&&
        !match(blend,native)&&!match(middle,measureBar(middle.data(),width,0));
}

bool hudFixtureSelfTest() {
    for (unsigned width : {256u, 1280u, 3560u}) for (unsigned eye = 0; eye < 2; ++eye) {
        const int worldWidth = int(width / 4), panelWidth = int(width * 2 / 3);
        const auto paint = [&](int world, int panel, unsigned paintedEye) {
            std::vector<uint8_t> row(width * 4);
            for (int x = 0; x < int(width); ++x) {
                auto pixel = x >= world && x < world + worldWidth ? worldBar : worldBackground(paintedEye);
                if (x >= panel && x < panel + panelWidth) pixel = blendHud(pixel, translucentHud(paintedEye));
                for (unsigned c = 0; c < 4; ++c) row[4*x + c] = uint8_t(std::lround(255.f * pixel[c]));
            }
            return row;
        };
        for (unsigned frame = 2; frame <= 13; ++frame) {
            const int worldA = barLeft(width, frame - 1, eye), worldB = barLeft(width, frame, eye);
            const int hudA = hudLeft(width, frame - 1, eye), hudB = hudLeft(width, frame, eye);
            const int middleWorld = int(std::lround((worldA + worldB) * .5));
            const int middleHud = int(std::lround((hudA + hudB) * .5));
            const EdgeMeasurement oldHud{hudA, hudA + panelWidth, 0, true};
            const auto checkHud = [&](const std::vector<uint8_t>& row) {
                auto hud = measureHud(row.data(), int(width), eye);
                return followsMotion(hud, oldHud, hudA, hudB, panelWidth, 1, 2);
            };
            const auto good = paint(middleWorld, middleHud, eye);
            const auto frozenWorld = paint(worldB, middleHud, eye);
            const auto hudAtEndpoint = paint(middleWorld, hudB, eye);
            const auto swappedHud = paint(middleWorld, middleHud, 1 - eye);
            const auto neutral = paint(middleWorld, int(width), eye);
            const auto hud = measureHud(good.data(), int(width), eye);
            if (!checkHud(good) || checkHud(hudAtEndpoint) || checkHud(swappedHud) ||
                measureHud(neutral.data(), int(width), eye).left >= 0 ||
                !measureWorldThroughHud(good.data(), hud, worldA, worldB, worldWidth, 1, 2).matches ||
                measureWorldThroughHud(frozenWorld.data(), hud, worldA, worldB, worldWidth, 1, 2).matches)
                return false;
        }
    }
    return true;
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
    HudFixture hudFixture = HudFixture::None;
    unsigned multiplier=2,width=1280,height=720,guideWidth=0,guideHeight=0,nativeFrames=12,warmupFrames=4;std::wstring feature,data;
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
        else if(i+1<argc && wcscmp(argv[i], L"--ui-recomposition") == 0 && hudFixture == HudFixture::None) {
            const std::wstring mode(argv[++i]);
            if (mode == L"alpha") hudFixture = HudFixture::Alpha;
            else if (mode == L"opaque") hudFixture = HudFixture::Opaque;
            else if (mode == L"neutral") hudFixture = HudFixture::Neutral;
            else if (mode == L"control") hudFixture = HudFixture::Control;
            else { std::puts("expected --ui-recomposition alpha|opaque|neutral|control"); return 2; }
        }
        else {std::puts("usage: probe [--run --multiplier 2|3|4 --feature-dir ABS --data-dir ABS [--official-fg-helper] [--shared-native-pair-suppression] [--width W --height H --guide-width GW --guide-height GH] [--native-frames N --warmup-frames N] [--diagnostic-suppressed-output] [--ui-recomposition alpha|opaque|neutral|control]]");return 2;}
    }
    if(!guideWidth)guideWidth=width;if(!guideHeight)guideHeight=height;
    if(nativeFrames<2||nativeFrames>64||warmupFrames>=nativeFrames||width<256||height<64||
       guideWidth<64||guideHeight<32||width>16384||height>16384||guideWidth>16384||guideHeight>16384){
        std::puts("moving-edge probe needs 2..64 native frames, warmup smaller than frame count, output at least 256x64 and guides at least 64x32 (maximum 16384 per dimension)");
        return 2;
    }
    const bool hudFixtureActive = hudFixture != HudFixture::None;
    const bool uirEnabled = hudFixtureActive && hudFixture != HudFixture::Control;
    if (hudFixtureActive && multiplier != 2) {
        std::puts("UI recomposition probe measures one 2x midpoint per pair");
        return 2;
    }
    if (!edgeCheckSelfTest() || !hudFixtureSelfTest() || !fg_fixture::test::qualificationFixtureSelfTest()) {
        std::puts("moving-edge probe image self-test failed"); return 10;
    }
    if(!run){std::printf("DRY: moving-edge image self-test passed; method=%s layout=%s output=%ux%u guides=%ux%u nativeFrames=%u warmupFrames=%u suppressedReadback=%u ui_fixture=%s uir_enabled=%u no DXGI, device, NGX, GPU, game or XR calls. Explicit --run is required.\n",officialHelper?"official_fg_helper":"generic_scratch_query",sharedSuppression?"shared_native_pair":"private_per_index",width,height,guideWidth,guideHeight,nativeFrames,warmupFrames,diagnosticSuppressed,hudFixtureName(hudFixture),unsigned(uirEnabled));return 0;}
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
    init.uiRecomposition = uirEnabled;
    init.featureDirectory=feature.c_str();init.dataDirectory=data.c_str();init.privateNgxDevice=true;init.timeoutMs=5000;
    const bool initialized=backend.initialize(init);printInitTrace(backend.status(),initialized);
    if(!initialized){const bool drained=backend.drain(5000);std::printf("init_cleanup drain=%u quarantined=%u\n",drained,backend.status().quarantined);return 4;}
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;q.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    ComPtr<ID3D12Fence> producer;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    if(FAILED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))||FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&producer)))||
       FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))))return 5;
    list->Close();
    const unsigned planes = uirEnabled ? 5u : 3u;
    const unsigned scanlines = hudFixtureActive ? 2u : 1u;
    ComPtr<ID3D12DescriptorHeap> rtvs;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2 * planes;
    if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs))))return 5;
    const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    struct Inputs {ComPtr<ID3D12Resource> resources[2][5];};auto resources=std::make_shared<Inputs>();
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM};
    for (unsigned e = 0; e < 2; ++e) for (unsigned k = 0; k < planes; ++k) {
        const bool guide = k == 1 || k == 2;
        if (!texture(device.Get(), guide ? guideWidth : width, guide ? guideHeight : height,
                     formats[k], resources->resources[e][k])) return 5;
        auto handle = rtvs->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(e * planes + k) * stride;
        device->CreateRenderTargetView(resources->resources[e][k].Get(), nullptr, handle);
    }
    ComPtr<ID3D12Resource> outputReadback;
    D3D12_HEAP_PROPERTIES readHeap{};readHeap.Type=D3D12_HEAP_TYPE_READBACK;readHeap.CreationNodeMask=readHeap.VisibleNodeMask=1;
    const UINT rowPitch=(width*4+D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-1u)&~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-1u);
    const UINT64 rowStride=(rowPitch+D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1u)&~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1u);
    const SIZE_T readbackBytes=SIZE_T(6*scanlines*rowStride);
    const auto rowOffset = [&](unsigned eye, unsigned phase, unsigned scanline) {
        return ((eye * 3 + phase) * scanlines + scanline) * rowStride;
    };
    D3D12_RESOURCE_DESC readDesc{};readDesc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;readDesc.Width=readbackBytes;readDesc.Height=1;
    readDesc.DepthOrArraySize=readDesc.MipLevels=1;readDesc.SampleDesc.Count=1;readDesc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(device->CreateCommittedResource(&readHeap,D3D12_HEAP_FLAG_NONE,&readDesc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&outputReadback))))return 5;
    ProducerGuard producerGuard;producerGuard.hold={device,queue,producer,allocator,list,rtvs,outputReadback,resources};
    unsigned readyBatches=0,postWarmupReady=0,suppressedBatches=0,suppressedReadbacks=0,eligiblePhases=0;uint64_t producerValue=0;
    unsigned hudPhases = 0, throughPhases = 0, neutralPhases = 0, translatedPhases = 0;
    for(uint64_t frame=1;frame<=nativeFrames;++frame){
        if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator.Get(),nullptr)))return 6;
        ProbeMotion views[2] = {
            probeMotion(width, height, guideWidth, unsigned(frame), 0, hudFixture),
            probeMotion(width, height, guideWidth, unsigned(frame), 1, hudFixture),
        };
        for (unsigned e = 0; e < 2; ++e) for (unsigned k = 0; k < planes; ++k) {
            auto* resource = resources->resources[e][k].Get();
            barrier(list.Get(), resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto handle = rtvs->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += SIZE_T(e * planes + k) * stride;
            paintInput(k, width, height, guideWidth, guideHeight, unsigned(frame), e, hudFixture, views[e].distantMotion,
                [&](const ProbeColor& color, const Rect* rect) {
                    if (!rect) {
                        list->ClearRenderTargetView(handle, color.data(), 0, nullptr);
                        return;
                    }
                    const D3D12_RECT area{rect->left, rect->top, rect->right, rect->bottom};
                    list->ClearRenderTargetView(handle, color.data(), 1, &area);
                });
            barrier(list.Get(), resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        }
        if(FAILED(list->Close()))return 6;ID3D12CommandList* commands[]={list.Get()};
        producerGuard.submitted=producerGuard.unfenced=true;queue->ExecuteCommandLists(1,commands);
        if(FAILED(queue->Signal(producer.Get(),++producerValue)))return 6;
        producerGuard.value=producerValue;producerGuard.unfenced=false;
        FrameGenerationNgxInput input;input.sequence=frame;input.historyGeneration=1;input.pairedFullEyeGuides=true;input.producerFence=producer.Get();input.producerValue=producerValue;input.inputLease=resources;
        for (unsigned e = 0; e < 2; ++e) {
            auto& eye = input.eyes[e];
            eye.color = resources->resources[e][0].Get();
            eye.depth = resources->resources[e][1].Get();
            eye.motion = resources->resources[e][2].Get();
            eye.hudless = resources->resources[e][3].Get();
            eye.ui = resources->resources[e][4].Get();
            eye.camera = views[e].camera;
        }
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
                D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=r;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                for (unsigned row = 0; row < scanlines; ++row) {
                    const unsigned y = scanlineY(height, hudFixture, row);
                    D3D12_TEXTURE_COPY_LOCATION dst{};
                    dst.pResource = outputReadback.Get();
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint.Offset = rowOffset(e, i, row);
                    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, 1, 1, rowPitch};
                    const D3D12_BOX box{0, y, 0, width, y + 1, 1};
                    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                }
                barrier(list.Get(),r,D3D12_RESOURCE_STATE_COPY_SOURCE,ticket.outputState);
            }
            if(FAILED(list->Close()))return 9;ID3D12CommandList* copies[]={list.Get()};producerGuard.unfenced=true;
            queue->ExecuteCommandLists(1,copies);if(FAILED(queue->Signal(producer.Get(),++producerValue)))return 9;
            producerGuard.value=producerValue;producerGuard.unfenced=false;
            if(!waitFence(producer.Get(),producerValue,5000))return 9;
            producerGuard.hold.consumerLease.reset();
            void* mapped=nullptr;const D3D12_RANGE reads{0,readbackBytes};
            if(FAILED(outputReadback->Map(0,&reads,&mapped))||!mapped)return 9;
            const bool eligible=!suppressedInspection&&frame>1&&frame>warmupFrames;
            bool matches=ticket.generatedCount==multiplier-1;
            for(unsigned e=0;e<2;++e){
                const int previous=barLeft(width,unsigned(frame>1?frame-1:frame),e);
                const int current=barLeft(width,unsigned(frame),e);
                const int barWidth=int(width/4);
                EdgeMeasurement prior{previous,previous+barWidth,0,true};
                const auto* samples = static_cast<const uint8_t*>(mapped);
                for(unsigned i=0;i<ticket.generatedCount;++i){
                    const EyeReadbackRows rows{samples + rowOffset(e, i, 0),
                        hudFixtureActive ? samples + rowOffset(e, i, 1) : nullptr};
                    const auto phase = assessPhase({rows, width, unsigned(frame), e, i+1, multiplier, hudFixture,
                        views[e].xDelta}, &prior);
                    const auto& measured = phase.world;
                    const bool pass = phase.worldMatches;
                    std::printf("edge frame=%llu eye=%u index=%u nativeLeft=%d,%d expectedLeft=%.2f measured=[%d,%d) spread=%d eyeMatch=%u eligible=%u match=%u\n",
                        static_cast<unsigned long long>(frame),e,i+1,previous,current,
                        previous+(current-previous)*double(i+1)/multiplier,measured.left,measured.right,
                        measured.spread,unsigned(measured.eyeMatches),unsigned(eligible),unsigned(pass));
                    if(eligible){matches=matches&&pass;++eligiblePhases;}
                    prior=measured;
                    if (!hudFixtureActive) continue;

                    const auto& hud = phase.hud;
                    const bool translated = phase.translated;
                    if (eligible && translated) ++translatedPhases;
                    if (phase.covered) {
                        const int panelA = phase.panelA, panelB = phase.panelB;
                        const bool hudPass = phase.hudMatches;
                        const auto& through = phase.through;
                        std::printf("hud_edge frame=%llu eye=%u index=%u x_delta_m=%.8g nativeLeft=%d,%d expectedLeft=%.2f measured=[%d,%d) spread=%d eyeMatch=%u throughExpected=%d throughMeasured=%d throughMatch=%u eligible=%u match=%u\n",
                            static_cast<unsigned long long>(frame), e, i + 1, double(views[e].xDelta), panelA, panelB,
                            panelA + (panelB - panelA) * double(i + 1) / multiplier, hud.left, hud.right, hud.spread,
                            unsigned(hud.eyeMatches), through.expected, through.measured, unsigned(through.matches),
                            unsigned(eligible), unsigned(hudPass));
                        if (eligible) {
                            matches = matches && translated && hudPass && through.matches;
                            if (hudPass) ++hudPhases;
                            if (through.matches) ++throughPhases;
                        }
                    } else {
                        const bool neutral = phase.clearMatches;
                        std::printf("hud_neutral frame=%llu eye=%u index=%u x_delta_m=%.8g eligible=%u match=%u\n",
                            static_cast<unsigned long long>(frame), e, i + 1, double(views[e].xDelta),
                            unsigned(eligible), unsigned(neutral));
                        if (eligible) {
                            matches = matches && translated && neutral;
                            if (neutral) ++neutralPhases;
                        }
                    }
                }
            }
            const D3D12_RANGE noWrites{0,0};outputReadback->Unmap(0,&noWrites);
            if(suppressedInspection)++suppressedReadbacks;
            else {
                if(!matches){std::puts("moving-edge phase count, stereo identity, shape or ordered scene/HUD motion mismatch");return 10;}
                ++readyBatches;if(eligible)++postWarmupReady;
            }
        }
        if(completion==FrameGenerationNgxCompletion::Suppressed)++suppressedBatches;
        if(completion!=FrameGenerationNgxCompletion::Ready&&completion!=FrameGenerationNgxCompletion::Suppressed){backend.drain(5000);return 7;}
        // Backend completion also proves that these producer input resources
        // are restored to COMMON; the next native source may overwrite them.
        ticket={};
    }
    const bool drained=backend.drain(5000);
    std::printf("probe result readyStereoBatches=%u postWarmupReady=%u warmupFrames=%u nativeFrames=%u suppressedNativeOnly=%u suppressedDiagnosticReadbacks=%u eligibleEyePhases=%u drain=%u ui_fixture=%s uir_enabled=%u hudEyePhases=%u hudThroughEyePhases=%u neutralEyePhases=%u translatedEyePhases=%u outputPixels=moving-edge-scanline GPUtiming=not-measured game=0 OpenXR=0 presentation=0; VR qualification remains separate\n",
        readyBatches,postWarmupReady,warmupFrames,nativeFrames,suppressedBatches,suppressedReadbacks,eligiblePhases,drained,
        hudFixtureName(hudFixture),unsigned(uirEnabled),hudPhases,throughPhases,neutralPhases,translatedPhases);
    const bool hudProof = !hudFixtureActive || (hudPhases && hudPhases == throughPhases &&
        hudPhases + neutralPhases == eligiblePhases && translatedPhases == eligiblePhases);
    return postWarmupReady && eligiblePhases && drained && hudProof ? 0 : 8;
}

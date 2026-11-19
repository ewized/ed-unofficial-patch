// Opt-in offscreen production-owner proof with a simulated gaze provider.
// Link native_vrs.cpp, not native_gaze.cpp; the fixture supplies the SEH filter.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "../../src/d3d11/native_vrs.h"
#include "../../src/d3d11/native_gaze.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <string>
#include "../../src/d3d11/native_gaze_readiness.h"
#include "witness_gpu_cases.h"
#include "../../third_party/dxbc_hash/DxilHash.cpp"

#ifdef EDVR_NATIVE_VRS_TEST
#error This fixture requires the production native VRS owner path
#endif

namespace edvr {
int guardFilter(unsigned long,const char*) {return EXCEPTION_EXECUTE_HANDLER;}
// This private fixture injects SIMULATED readiness as well as gaze. It does
// not claim a real native channel, runtime startup or physical eye tracking.
NativeGazeReadySnapshot nativeTemporalGazeReadiness(ID3D11Device*,unsigned long){return {1,1,true};}
uint64_t nativeTemporalGazeEpoch() noexcept{return 1;}
bool nativeTemporalWithGazeReady(ID3D11Device*,unsigned long,uint64_t epoch,NativeGazeReadyCallback callback,void* user){if(epoch!=1)return false;callback(user,1);return true;}
void nativeGazeReset() noexcept {}
void nativeGazeSetLifetime(uint64_t) noexcept {}
NativeGazeSample nativeGazePoll(bool enabled) noexcept {
    NativeGazeSample sample{};
    if(!enabled)return sample;
    LARGE_INTEGER now{},frequency{};
    if(!QueryPerformanceCounter(&now)||!QueryPerformanceFrequency(&frequency)||now.QuadPart<=0||frequency.QuadPart<=0) {
        sample.failure=NativeGazeFailure::ReceiptClockUnavailable;return sample;
    }
    static uint64_t sequence=0;++sequence;
    constexpr float patchX=.734375f;
    sample.eyes[0]={sequence==1?0.f:patchX,0.f,true};sample.eyes[1]={patchX,0.f,true};
    for(auto& eye:sample.eyes){eye.projection[0]=eye.projection[1]=1.f;eye.projectionValid=true;}
    sample.providerQueried=sample.providerReturnedValid=sample.usable=true;
    sample.sensorTimestampUnavailable=true;
    sample.receiptQpc=static_cast<uint64_t>(now.QuadPart);
    sample.receiptQpcFrequency=static_cast<uint64_t>(frequency.QuadPart);
    sample.querySequence=sequence;sample.failure=NativeGazeFailure::None;return sample;
}
}

using Microsoft::WRL::ComPtr;
constexpr UINT size=1024,block=16,readX=880,readY=504;
constexpr DWORD waitMs=3000,lifetimeMs=20000;
void require(bool good,const char* why){if(!good)throw std::runtime_error(why);}
void hr(HRESULT value,const char* why){
    if(FAILED(value)){std::printf("{\"error_hr\":\"0x%08lx\",\"stage\":\"%s\"}\n",static_cast<unsigned long>(value),why);throw std::runtime_error(why);}
}
HMODULE systemDll(const wchar_t* leaf){
    wchar_t path[32768]{};const UINT n=GetSystemDirectoryW(path,32768);require(n&&n<32000,"GetSystemDirectory");
    std::wstring full(path);full+=L"\\";full+=leaf;
    HMODULE module=LoadLibraryExW(full.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(module!=nullptr,"System32 DLL load");return module;
}
template<class F> F symbol(HMODULE module,const char* name){
    auto pointer=GetProcAddress(module,name);require(pointer!=nullptr,name);return reinterpret_cast<F>(pointer);
}
template<class T> void waitQuery(ID3D11DeviceContext* context,ID3D11Query* query,T& output){
    const ULONGLONG deadline=GetTickCount64()+waitMs;
    for(;;){const HRESULT h=context->GetData(query,&output,sizeof(output),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(h==S_OK)return;hr(h,"GetData");require(GetTickCount64()<deadline,"query timeout 3000ms");Sleep(1);}
}
DWORD WINAPI watchdog(void* event){
    if(WaitForSingleObject(static_cast<HANDLE>(event),lifetimeMs)==WAIT_TIMEOUT){
        std::fputs("{\"error\":\"whole process deadline 20000ms\"}\n",stderr);std::fflush(nullptr);TerminateProcess(GetCurrentProcess(),124);}
    return 0;
}
struct Deadline {
    HANDLE event=nullptr,thread=nullptr;
    Deadline(){event=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(event!=nullptr,"watchdog event");
        thread=CreateThread(nullptr,0,watchdog,event,0,nullptr);require(thread!=nullptr,"watchdog thread");}
    ~Deadline(){if(event)SetEvent(event);if(thread){WaitForSingleObject(thread,1000);CloseHandle(thread);}if(event)CloseHandle(event);}
};
struct Reset {
    ID3D11DeviceContext* context;
    ~Reset(){edvr::nativeVrsConfigure(false);edvr::nativeVrsTick(context,4);edvr::nativeVrsFullRate(context);context->ClearState();context->Flush();}
};
struct Result {
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> query,done;
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
    std::array<float,block*block> pixels{},centerPixels{},outerPixels{};
    unsigned repeatX=0,repeatY=0,equal2x2=0;
    bool gradient=false,centerFullRate=false,outerFourByFour=false;
};
void measurePixels(Result& result){
    bool finite=true;float low=INFINITY,high=-INFINITY;
    auto equal=[](float a,float b){return std::fabs(a-b)<1e-7f;};
    for(UINT y=0;y<block;++y)for(UINT x=0;x<block;++x){
        const float pixel=result.pixels[y*block+x];finite&=std::isfinite(pixel);
        low=std::min(low,pixel);high=std::max(high,pixel);
        if(x+1<block&&equal(pixel,result.pixels[y*block+x+1]))++result.repeatX;
        if(y+1<block&&equal(pixel,result.pixels[(y+1)*block+x]))++result.repeatY;
        if(!(x%2)&&!(y%2)&&equal(pixel,result.pixels[y*block+x+1])&&equal(pixel,result.pixels[(y+1)*block+x])&&equal(pixel,result.pixels[(y+1)*block+x+1]))++result.equal2x2;
    }
    result.gradient=finite&&high-low>.01f&&low>1.f&&high<2.f;
}
void printStatus(const char* stage){
    const auto status=edvr::nativeVrsStatus();
    std::printf("{\"owner_stage\":\"%s\",\"real_owner_path\":true,\"simulated_provider\":true,\"game_world_claim\":false,\"configured\":%s,\"driver_ready\":%s,\"magic_absent\":%s,\"refused\":%s,\"failure\":%u,\"last_driver_status\":%d,\"full_rate_acknowledged\":%s,\"coarse_acknowledged\":%s,\"mapUpdates\":[%llu,%llu],\"mapBytes\":%llu,\"rateSets\":%llu,\"resourceSets\":%llu,\"driverErrors\":%llu}\n",
        stage,status.configured?"true":"false",status.driverReady?"true":"false",status.magicAbsent?"true":"false",status.refused?"true":"false",static_cast<unsigned>(status.failure),status.lastDriverStatus,
        status.fullRateAcknowledged?"true":"false",status.coarseAcknowledged?"true":"false",
        static_cast<unsigned long long>(status.mapUpdates[0]),static_cast<unsigned long long>(status.mapUpdates[1]),static_cast<unsigned long long>(status.mapBytes),
        static_cast<unsigned long long>(status.rateSets),static_cast<unsigned long long>(status.resourceSets),static_cast<unsigned long long>(status.driverErrors));
}
int run(LUID expected){
    Deadline deadline;
    const auto factoryFn=symbol<HRESULT(WINAPI*)(REFIID,void**)>(systemDll(L"dxgi.dll"),"CreateDXGIFactory1");
    const auto deviceFn=symbol<decltype(&D3D11CreateDevice)>(systemDll(L"d3d11.dll"),"D3D11CreateDevice");
    const auto compileFn=symbol<decltype(&D3DCompile)>(systemDll(L"d3dcompiler_47.dll"),"D3DCompile");
    ComPtr<IDXGIFactory1> factory;hr(factoryFn(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(factory.GetAddressOf())),"CreateDXGIFactory1");
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;const HRESULT h=factory->EnumAdapters1(i,&candidate);
        if(h==DXGI_ERROR_NOT_FOUND)break;hr(h,"EnumAdapters1");DXGI_ADAPTER_DESC1 desc{};hr(candidate->GetDesc1(&desc),"GetDesc1");
        if(desc.AdapterLuid.HighPart==expected.HighPart&&desc.AdapterLuid.LowPart==expected.LowPart){
            require(desc.VendorId==0x10de&&!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE),"requested adapter must be hardware NVIDIA");adapter=candidate;
            std::printf("{\"adapter_luid\":\"%08lx:%08lx\",\"vendor_id\":%u,\"device_id\":%u}\n",static_cast<unsigned long>(expected.HighPart),static_cast<unsigned long>(expected.LowPart),desc.VendorId,desc.DeviceId);break;}}
    require(adapter!=nullptr,"exact requested LUID not found");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL want=D3D_FEATURE_LEVEL_11_0;
    hr(deviceFn(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&want,1,D3D11_SDK_VERSION,&device,&got,&context),"D3D11CreateDevice");
    Reset reset{context.Get()};
    const char shader[]=R"HLSL(
struct Vertex {float4 p:SV_Position;float2 uv:TEXCOORD0;};
Vertex vs(uint id:SV_VertexID){Vertex o;float2 q=float2((id<<1)&2,id&2);o.p=float4(q*float2(2,-2)+float2(-1,1),0,1);o.uv=q;return o;}
float ps(Vertex i):SV_Target0{return i.uv.x+2*i.uv.y;}
)HLSL";
    ComPtr<ID3DBlob> vsCode,psCode,errors;
    auto compile=[&](const char* entry,const char* profile,ComPtr<ID3DBlob>& code){
        const HRESULT h=compileFn(shader,sizeof(shader)-1,"native_owner_fixture",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        if(FAILED(h)&&errors)std::fwrite(errors->GetBufferPointer(),1,errors->GetBufferSize(),stderr);hr(h,"D3DCompile");errors.Reset();};
    compile("vs","vs_5_0",vsCode);compile("ps","ps_5_0",psCode);
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    hr(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs),"CreateVS");
    hr(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps),"CreatePS");
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=size;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,depthTarget;ComPtr<ID3D11RenderTargetView> rtv;ComPtr<ID3D11DepthStencilView> dsv;
    hr(device->CreateTexture2D(&td,nullptr,&target),"CreateTarget");hr(device->CreateRenderTargetView(target.Get(),nullptr,&rtv),"CreateRTV");
    td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    hr(device->CreateTexture2D(&td,nullptr,&depthTarget),"CreateDepthTarget");hr(device->CreateDepthStencilView(depthTarget.Get(),nullptr,&dsv),"CreateDSV");
    // Three disjoint patches preserve the original middle-ring observation
    // and independently check the full-rate centre and 4x4 outer region.
    td.Width=3*block;td.Height=block;td.Format=DXGI_FORMAT_R32_FLOAT;td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    std::array<Result,9> results;
    for(auto& result:results){hr(device->CreateTexture2D(&td,nullptr,&result.staging),"CreateReadback");
        D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS,0};hr(device->CreateQuery(&qd,&result.query),"CreateStatsQuery");
        qd.Query=D3D11_QUERY_EVENT;hr(device->CreateQuery(&qd,&result.done),"CreateEventQuery");}
    D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;hr(device->CreateRasterizerState(&rs,&raster),"CreateRasterState");
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;ds.DepthFunc=D3D11_COMPARISON_LESS;
    ComPtr<ID3D11DepthStencilState> depth;hr(device->CreateDepthStencilState(&ds,&depth),"CreateDepthState");
    D3D11_BLEND_DESC bs{};bs.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;hr(device->CreateBlendState(&bs,&blend),"CreateBlendState");
    ID3D11RenderTargetView* raw=rtv.Get();context->OMSetRenderTargets(1,&raw,dsv.Get());
    context->OMSetDepthStencilState(depth.Get(),0);context->OMSetBlendState(blend.Get(),nullptr,~0u);context->RSSetState(raster.Get());
    D3D11_VIEWPORT viewport{0,0,float(size),float(size),0,1};context->RSSetViewports(1,&viewport);
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
    auto submit=[&](Result& result){
        const float clear[4]{-100.f,0,0,0};context->ClearRenderTargetView(rtv.Get(),clear);
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1.f,0);
        context->Begin(result.query.Get());context->Draw(3,0);context->End(result.query.Get());
        const UINT coordinates[3][2]{{readX,readY},{504,504},{48,880}};
        for(UINT patch=0;patch<3;++patch){
            const UINT x=coordinates[patch][0],y=coordinates[patch][1];
            const D3D11_BOX box{x,y,0,x+block,y+block,1};
            context->CopySubresourceRegion(result.staging.Get(),0,patch*block,0,0,target.Get(),0,&box);
        }
        context->End(result.done.Get());};
    // No waits between Tick and coarse submission: production receipt age applies.
    edvr::nativeVrsConfigure(true);edvr::nativeVrsTick(context.Get(),1);
    edvr::nativeVrsFullRate(context.Get());submit(results[0]);
    edvr::NativeVrsProjection projection{1,{-1.f,1.f,-1.f,1.f},{0.f,0.f}};
    const bool accepted=edvr::nativeVrsBeforeWorld(context.Get(),0,rtv.Get(),dsv.Get(),&projection);
    submit(results[1]);const auto coarseStatus=edvr::nativeVrsStatus();
    // Exercise both palettes on an identical eye map. Keep the original
    // public full-rate, gaze-movement and OFF controls independent.
    const bool uniformReady=edvr::nativeVrsPrepareWitness(context.Get(),0,rtv.Get(),dsv.Get(),projection);
    const auto uniformStatus=edvr::nativeVrsStatus();submit(results[7]);
    const bool patternResumed=edvr::nativeVrsBeforeWorld(context.Get(),0,rtv.Get(),dsv.Get(),&projection);
    const auto resumedStatus=edvr::nativeVrsStatus();submit(results[8]);
    const bool eye1Accepted=edvr::nativeVrsBeforeWorld(context.Get(),1,rtv.Get(),dsv.Get(),&projection);
    submit(results[2]);
    edvr::nativeVrsTick(context.Get(),2);projection.sequence=2;
    const bool movedAccepted=edvr::nativeVrsBeforeWorld(context.Get(),0,rtv.Get(),dsv.Get(),&projection);
    submit(results[3]);const auto movedStatus=edvr::nativeVrsStatus();
    edvr::nativeVrsFullRate(context.Get());submit(results[4]);
    const bool rearmed=edvr::nativeVrsBeforeWorld(context.Get(),0,rtv.Get(),dsv.Get(),&projection);
    submit(results[5]);const auto preOff=edvr::nativeVrsStatus();
    edvr::nativeVrsConfigure(false);edvr::nativeVrsTick(context.Get(),3);submit(results[6]);
    const auto finalStatus=edvr::nativeVrsStatus();context->Flush();
    printStatus("after_off_tick");
    std::printf("{\"coarse_accepted\":%s,\"coarse_acknowledged\":%s,\"coarse_mapUpdates\":[%llu,%llu]}\n",accepted?"true":"false",coarseStatus.coarseAcknowledged?"true":"false",static_cast<unsigned long long>(coarseStatus.mapUpdates[0]),static_cast<unsigned long long>(coarseStatus.mapUpdates[1]));
    const char* names[]={"full_baseline","eye0_center_patch_coarse","eye1_patch_fine","eye0_moved_patch_fine","full_after","coarse_before_off","off_after","private_uniform_1x1","resumed_foveated"};
    for(size_t i=0;i<results.size();++i){auto& result=results[i];waitQuery(context.Get(),result.query.Get(),result.stats);
        BOOL completed=FALSE;waitQuery(context.Get(),result.done.Get(),completed);require(completed!=FALSE,"event completed");
        D3D11_MAPPED_SUBRESOURCE mapped{};const ULONGLONG mapDeadline=GetTickCount64()+waitMs;
        for(;;){const HRESULT h=context->Map(result.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
            if(h==S_OK)break;require(h==DXGI_ERROR_WAS_STILL_DRAWING,"readback Map");require(GetTickCount64()<mapDeadline,"Map timeout 3000ms");Sleep(1);}
        require(mapped.pData&&mapped.RowPitch>=3*block*sizeof(float),"readback pitch");
        Result center{},outer{};
        for(UINT y=0;y<block;++y){
            const auto* row=static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch;
            std::memcpy(result.pixels.data()+y*block,row,block*sizeof(float));
            std::memcpy(center.pixels.data()+y*block,row+block*sizeof(float),block*sizeof(float));
            std::memcpy(outer.pixels.data()+y*block,row+2*block*sizeof(float),block*sizeof(float));
        }
        context->Unmap(result.staging.Get(),0);measurePixels(result);measurePixels(center);measurePixels(outer);
        result.centerPixels=center.pixels;result.outerPixels=outer.pixels;
        result.centerFullRate=center.gradient&&!center.repeatX&&!center.repeatY&&!center.equal2x2;
        result.outerFourByFour=outer.gradient&&outer.repeatX==192&&outer.repeatY==192&&outer.equal2x2==64;
        std::printf("{\"case\":\"%s\",\"center_full_rate\":%s,\"outer_4x4\":%s,\"outer_repeat_x\":%u,\"outer_repeat_y\":%u}\n",names[i],result.centerFullRate?"true":"false",result.outerFourByFour?"true":"false",outer.repeatX,outer.repeatY);
        std::printf("{\"case\":\"%s\",\"PSInvocations\":%llu,\"IAVertices\":%llu,\"IAPrimitives\":%llu,\"VSInvocations\":%llu,\"CInvocations\":%llu,\"CPrimitives\":%llu,\"gradient_valid\":%s,\"repeat_x\":%u,\"repeat_y\":%u,\"equal_2x2_of_64\":%u}\n",names[i],result.stats.PSInvocations,result.stats.IAVertices,result.stats.IAPrimitives,result.stats.VSInvocations,result.stats.CInvocations,result.stats.CPrimitives,result.gradient?"true":"false",result.repeatX,result.repeatY,result.equal2x2);
    }
    const auto& full=results[0];const auto& coarse=results[1];bool topology=true;
    for(const auto& result:results)topology&=result.stats.IAVertices==3&&result.stats.IAPrimitives==1&&result.stats.VSInvocations==full.stats.VSInvocations&&result.stats.CInvocations==full.stats.CInvocations&&result.stats.CPrimitives==full.stats.CPrimitives;
    const bool replication=full.gradient&&!full.repeatX&&!full.repeatY&&!full.equal2x2&&coarse.gradient&&coarse.equal2x2>=60&&coarse.repeatX>=120&&coarse.repeatY>=120;
    const bool ringFootprints=full.centerFullRate&&!full.outerFourByFour&&coarse.centerFullRate&&
        coarse.repeatX==128&&coarse.repeatY==128&&coarse.outerFourByFour;
    auto samePixels=[](const Result& a,const Result& b){
        return a.pixels==b.pixels&&a.centerPixels==b.centerPixels&&a.outerPixels==b.outerPixels;
    };
    const bool uniformWitness=uniformReady&&patternResumed&&uniformStatus.vendorStateKnown&&
        !uniformStatus.fullRateAcknowledged&&!uniformStatus.coarseAcknowledged&&resumedStatus.coarseAcknowledged&&
        uniformStatus.rateSets==coarseStatus.rateSets+1&&resumedStatus.rateSets==uniformStatus.rateSets+1&&
        uniformStatus.resourceSets==coarseStatus.resourceSets&&resumedStatus.resourceSets==coarseStatus.resourceSets&&
        samePixels(results[7],full)&&samePixels(results[8],coarse)&&
        results[7].stats.PSInvocations==full.stats.PSInvocations&&results[8].stats.PSInvocations==coarse.stats.PSInvocations&&
        ringFootprints&&replication;
    std::printf("{\"uniform_witness_fine_then_foveated_pixels_exact\":%s}\n",uniformWitness?"true":"false");
    bool restored=finalStatus.fullRateAcknowledged&&!finalStatus.coarseAcknowledged&&!finalStatus.configured;
    restored&=rearmed&&preOff.coarseAcknowledged&&results[5].outerFourByFour&&results[5].stats.PSInvocations<full.stats.PSInvocations;
    for(unsigned i:{4u,6u})restored&=results[i].stats.PSInvocations==full.stats.PSInvocations&&results[i].gradient&&!results[i].repeatX&&!results[i].repeatY&&!results[i].equal2x2&&!results[i].outerFourByFour;
    bool distinctEyesAndMovement=eye1Accepted&&movedAccepted&&movedStatus.mapUpdates[0]==2&&movedStatus.mapUpdates[1]==1&&movedStatus.providerPolls==2;
    for(size_t i=2;i<4;++i)distinctEyesAndMovement&=results[i].gradient&&!results[i].repeatX&&!results[i].repeatY&&!results[i].equal2x2;
    const bool reduced=full.stats.PSInvocations>0&&coarse.stats.PSInvocations>0&&coarse.stats.PSInvocations<full.stats.PSInvocations;
    const bool owner=accepted&&coarseStatus.coarseAcknowledged&&coarseStatus.magicAbsent&&coarseStatus.mapUpdates[0]>0&&!finalStatus.driverErrors&&!finalStatus.refused;
    edvr::nativeVrsConfigure(true);
    uint64_t witnessFrame=3,resourceSetsAtPause=0,verifiedReactivations=0;
    bool startFrame=true,retained=false,transitionsExact=true;
    using witness_gpu::RateStep;
    auto witnessRate=[&](RateStep step,ID3D11RenderTargetView* c,ID3D11DepthStencilView* d){
        if(step==RateStep::BeginPass || step==RateStep::StrongFull){
            if(step==RateStep::BeginPass)startFrame=true;
            retained=false;
            edvr::nativeVrsFullRate(context.Get());
            const bool strong=edvr::nativeVrsStatus().fullRateAcknowledged;
            transitionsExact=transitionsExact&&strong;return strong;
        }
        if(step==RateStep::PrepareWitness){
            if(startFrame){edvr::nativeVrsTick(context.Get(),++witnessFrame);projection.sequence=witnessFrame;startFrame=false;}
            const auto before=edvr::nativeVrsStatus();
            if(!edvr::nativeVrsPrepareWitness(context.Get(),0,c,d,projection))return false;
            const auto fine=edvr::nativeVrsStatus();
            retained=before.coarseAcknowledged;
            resourceSetsAtPause=before.resourceSets;
            return fine.vendorStateKnown && !fine.coarseAcknowledged &&
                (retained?(!fine.fullRateAcknowledged && fine.resourceSets==resourceSetsAtPause &&
                           fine.rateSets==before.rateSets+1):fine.fullRateAcknowledged);
        }
        if(!edvr::nativeVrsBeforeWorld(context.Get(),0,c,d,&projection))return false;
        if(retained){
            const auto resumed=edvr::nativeVrsStatus();
            transitionsExact=transitionsExact && resumed.coarseAcknowledged && resumed.resourceSets==resourceSetsAtPause;
            ++verifiedReactivations;retained=false;
        }
        return true;
    };
    const bool witnessPixels=witness_gpu::run(device.Get(),context.Get(),compileFn,witnessRate,true);
    edvr::nativeVrsConfigure(false);edvr::nativeVrsTick(context.Get(),++witnessFrame);
    const auto witnessOff=edvr::nativeVrsStatus();
    const bool witness=witnessPixels && transitionsExact && verifiedReactivations>=2 && witnessOff.fullRateAcknowledged &&
        witnessOff.retainedWitnessReactivations-finalStatus.retainedWitnessReactivations==verifiedReactivations;
    std::printf("{\"verified_same_eye_witness_reactivations\":%llu,\"transitions_exact\":%s}\n",verifiedReactivations,transitionsExact?"true":"false");
    std::printf("{\"split_sloped_overlap_coplanar_depth_stencil_slot_skin_exact\":%s}\n",witness?"true":"false");
    const bool success=owner&&topology&&replication&&reduced&&restored&&distinctEyesAndMovement&&ringFootprints&&uniformWitness&&witness;
    std::printf("{\"center_1x1_middle_2x2_outer_4x4_verified\":%s}\n",ringFootprints?"true":"false");
    std::printf("{\"distinct_per_eye_and_moved_eye0_verified\":%s,\"mapUpdates_after_movement\":[%llu,%llu],\"provider_polls\":%llu}\n",distinctEyesAndMovement?"true":"false",static_cast<unsigned long long>(movedStatus.mapUpdates[0]),static_cast<unsigned long long>(movedStatus.mapUpdates[1]),static_cast<unsigned long long>(movedStatus.providerPolls));
    std::printf("{\"success\":%s,\"real_owner_path\":true,\"simulated_provider\":true,\"game_world_claim\":false,\"physical_gaze_claim\":false,\"performance_claim\":false,\"topology_valid\":%s,\"coarse_replication_verified\":%s,\"PSInvocations_reduced\":%s,\"full_after_and_off_restored\":%s}\n",success?"true":"false",topology?"true":"false",replication?"true":"false",reduced?"true":"false",restored?"true":"false");
    return success?0:3;
}
int wmain(int argc,wchar_t** argv){
    try {
        // Default and malformed CLI never reach graphics loads or the owner.
        if(argc==1||(argc==2&&!wcscmp(argv[1],L"--dry-run"))){
            std::puts("{\"mode\":\"DRY\",\"gpu_calls\":0,\"dynamic_dll_loads\":0,\"required_cli\":\"--run --adapter-luid HIGH:LOW\",\"target\":\"1024x1024 R32_FLOAT offscreen\",\"query_timeout_ms\":3000,\"process_timeout_ms\":20000,\"simulated_provider\":true,\"game_world_claim\":false}");return 0;}
        require(argc==4&&!wcscmp(argv[1],L"--run")&&!wcscmp(argv[2],L"--adapter-luid"),"usage: --dry-run | --run --adapter-luid HIGH:LOW");
        require(wcslen(argv[3])==17&&argv[3][8]==L':',"LUID format must be exactly 8hex:8hex");
        for(unsigned i=0;i<17;++i)if(i!=8)require((argv[3][i]>=L'0'&&argv[3][i]<=L'9')||(argv[3][i]>=L'a'&&argv[3][i]<=L'f')||(argv[3][i]>=L'A'&&argv[3][i]<=L'F'),"LUID must be hex");
        unsigned high=0,low=0;require(swscanf_s(argv[3],L"%8x:%8x",&high,&low)==2,"LUID parse");
        return run({low,static_cast<LONG>(high)});
    }catch(const std::exception& error){std::printf("{\"error\":\"%s\",\"success\":false}\n",error.what());return 2;}
}

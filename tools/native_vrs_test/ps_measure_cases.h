#pragma once
#include "../../src/d3d11/native_vrs_ps_measure.h"
#include <d3dcompiler.h>
#include <string>
#include <set>
namespace ps_fixture {
using namespace edvr::native_vrs::ps_measure;
using Microsoft::WRL::ComPtr;
inline std::vector<std::string> rows;
inline void capture(const char* row){rows.emplace_back(row);}
inline size_t lines(const char* marker){size_t n=0;for(const auto& row:rows)if(row.find(marker)!=std::string::npos)++n;return n;}
inline void run(){
    Key key{11,12,13,3,1,0,0,128,128,0,'D'};
    Receipt receipt{1,1,2,3,4,5,0,true};
    Recorder off;check(!off.plan(key,receipt).active,"PS recorder is default OFF");
    Recorder policy;policy.configure(true,capture);
    auto invalid=receipt;invalid.qualified=false;
    check(!policy.plan(key,invalid).active,"unqualified world receipt cannot create a query plan");
    invalid=receipt;invalid.gazeEpoch=0;
    check(!policy.plan(key,invalid).active,"missing gaze epoch refuses measurement");
    auto badKey=key;badKey.count=4;
    check(!policy.plan(badKey,receipt).active,"non-triangle-list work is refused");
    auto changed=receipt;changed.config++;
    check(!sameEpoch(receipt,changed),"config transition cannot qualify a matched triple");
    changed=receipt;changed.eye=1;
    check(!sameEpoch(receipt,changed),"stereo eyes cannot share query evidence");
    changed=receipt;++changed.invalidation;
    check(!sameEpoch(receipt,changed)&&sameSeries(receipt,changed),"mutations invalidate an in-flight query but independently revalidated frames may match");
    changed.config++;
    check(!sameSeries(receipt,changed),"cross-frame matching still refuses a configuration change");
    check(stableFull(1000,1010)&&!stableFull(1000,1011)&&!stableFull(0,0),"full/coarse/full drift and empty work controls");
    D3D11_QUERY_DATA_PIPELINE_STATISTICS work{};work.IAVertices=3;work.IAPrimitives=1;
    auto different=work;different.CInvocations=1;
    check(!sameWork(work,different),"changed clipping work cannot qualify a PS comparison");

    // The actual production recorder is driven on WARP. This verifies query
    // ownership/drain/matching, not hardware VRS or a performance benefit.
    HMODULE d3d=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(d3d?GetProcAddress(d3d,"D3D11CreateDevice"):nullptr);
    auto compile=reinterpret_cast<decltype(&D3DCompile)>(compiler?GetProcAddress(compiler,"D3DCompile"):nullptr);
    check(create&&compile,"System32 D3D11 and shader compiler available for recorder fixture");
    if(!create||!compile)return;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    const HRESULT made=create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context);
    check(SUCCEEDED(made),"WARP device for actual recorder");if(FAILED(made))return;
    const char shader[]="float4 VS(uint id:SV_VertexID):SV_Position { return float4(id==1?3:-1,id==2?3:-1,0,1); } float4 PS():SV_Target { return float4(.2,.3,.4,1); }";
    ComPtr<ID3DBlob> vb,pb,error;
    const HRESULT vc=compile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"VS","vs_5_0",0,0,&vb,&error);
    const HRESULT pc=compile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"PS","ps_5_0",0,0,&pb,&error);
    check(SUCCEEDED(vc)&&SUCCEEDED(pc),"WARP fullscreen triangle shaders compile");if(FAILED(vc)||FAILED(pc))return;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    HRESULT configured=device->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vs);
    if(SUCCEEDED(configured))configured=device->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&ps);
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=128;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target;ComPtr<ID3D11RenderTargetView> rtv;
    if(SUCCEEDED(configured))configured=device->CreateTexture2D(&td,nullptr,&target);
    if(SUCCEEDED(configured))configured=device->CreateRenderTargetView(target.Get(),nullptr,&rtv);
    ComPtr<ID3D11RasterizerState> raster;D3D11_RASTERIZER_DESC rd{};
    rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    if(SUCCEEDED(configured))configured=device->CreateRasterizerState(&rd,&raster);
    check(SUCCEEDED(configured),"WARP draw resources created");if(FAILED(configured))return;
    ID3D11RenderTargetView* raw=rtv.Get();context->OMSetRenderTargets(1,&raw,nullptr);
    context->RSSetState(raster.Get());
    D3D11_VIEWPORT viewport{0,0,128,128,0,1};context->RSSetViewports(1,&viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
    Recorder actual;actual.configure(true,capture);rows.clear();uint64_t frame=1;
    const auto draw=[&](bool changeEpoch,bool disable){
        receipt.frame=receipt.sequence=frame;auto plan=actual.plan(key,receipt);
        if(!plan.active)return false;
        if(!actual.begin(device.Get(),context.Get(),plan,receipt,GetTickCount64()))return false;
        context->Draw(3,0);auto after=receipt;if(changeEpoch)++after.gazeEpoch;
        if(disable)actual.configure(false,capture);
        actual.end(context.Get(),plan,after,true);
        // Submission is a fixture responsibility. Production never Flushes.
        context->Flush();const auto deadline=GetTickCount64()+2000;
        while(actual.pending()&&GetTickCount64()<deadline){actual.collect(context.Get(),++frame,GetTickCount64());Sleep(1);}
        ++receipt.invalidation; // Normal ClearState/Execute activity between frames.
        return !actual.pending();
    };
    const bool tripleDrawn=draw(false,false)&&draw(false,false)&&draw(false,false);
    if(!tripleDrawn)for(const auto& row:rows)std::printf("PS fixture diagnostic: %s\n",row.c_str());
    check(tripleDrawn,"actual query Begin/End/GetData drains a full/coarse/full triple");
    check(lines("native vrs ps sample:")==3&&lines("valid=1 full_before=")==1,"actual pipeline statistics form one qualified matched triple");
    check(lines("PSInvocations=0 ")==0,"actual queried triangle invokes its pixel shader");
    check(lines("performance_claim=0")==1,"WARP recorder result never claims VRS acceleration");
    actual=Recorder{};actual.configure(true,capture);rows.clear();
    check(draw(true,false)&&lines("valid=0 query_hr=")==1,"epoch change while actual query is in flight invalidates its result");
    actual=Recorder{};actual.configure(true,capture);rows.clear();
    check(draw(false,true)&&lines("valid=0 query_hr=")==1,"OFF drains a real pending query without accepting evidence");
    check(!actual.plan(key,receipt).active,"OFF refuses all subsequent query plans");
    // Drive production discovery+recorder through a prefix larger than both
    // candidate and metadata budgets. Only the later key has meaningful PS
    // work. Its startInstance changes midway; triples must never straddle it.
    Recorder screening;screening.configure(true,capture);rows.clear();
    rd.ScissorEnable=TRUE;ComPtr<ID3D11RasterizerState> scissor;
    check(SUCCEEDED(device->CreateRasterizerState(&rd,&scissor)),"selection scissor state");
    context->RSSetState(scissor.Get());
    unsigned queries=0,maxMetadata=0,maxQueries=0;bool drained=true;
    frame=1000;
    for(unsigned iteration=0;iteration<170;++iteration){
        unsigned metadata=0,issued=0;
        for(unsigned ordinal=0;ordinal<40;++ordinal){
            const bool useful=ordinal==24&&iteration>=20;
            Key k{11,12,useful?2u:1u,3,1,0,useful?(iteration<70?500u:iteration<120?700u:500u):uint32_t(iteration*40+ordinal+10000),128,128,0,'N'};
            Receipt r{frame,frame,2,3,uint64_t(iteration+10),5,0,true};
            if(!screening.metadataWanted(k,r))continue;
            ++metadata;const auto plan=screening.plan(k,r);if(!plan.active)continue;
            if(!screening.begin(device.Get(),context.Get(),plan,r,GetTickCount64())){drained=false;continue;}
            const D3D11_RECT clip{0,0,useful?128:1,useful?128:1};context->RSSetScissorRects(1,&clip);
            context->DrawInstanced(3,1,0,k.startInstance);screening.end(context.Get(),plan,r,true);++issued;++queries;
        }
        maxMetadata=(std::max)(maxMetadata,metadata);maxQueries=(std::max)(maxQueries,issued);
        // Fixture-only submission and bounded drain; production never waits.
        context->Flush();const auto deadline=GetTickCount64()+2000;
        while(screening.pending()&&GetTickCount64()<deadline){screening.collect(context.Get(),++frame,GetTickCount64());Sleep(1);}
        drained=drained&&!screening.pending();++frame;
    }
    check(drained&&maxMetadata<=16&&maxQueries<=1&&queries<=170,"rotating discovery preserves metadata/query/ring drain bounds");
    check(lines("valid=1 full_before=")>=5,"useful work after sixteen tiny prefix draws forms real WARP matched triples");
    check(lines("start_instance=500 ")&&lines("start_instance=700 "),"startInstance rollover re-enters useful discovery after unseen expiry");
    check(lines("reason=screen_or_invalid")&&lines("reason=expired"),"resolved tiny and disappeared candidates are recycled with explicit identity provenance");
    std::set<unsigned long long> reusedInstances;
    for(const auto& row:rows)if(row.find("start_instance=500 ")!=std::string::npos){
        const auto at=row.find("candidate_instance=");unsigned long long id=0;
        if(at!=std::string::npos&&std::sscanf(row.c_str()+at,"candidate_instance=%llu",&id)==1)reusedInstances.insert(id);
    }
    check(reusedInstances.size()>=2,"reappearing startInstance has a new candidate identity, never old partial samples");
    unsigned lowTriples=0;for(const auto& row:rows)if(row.find("native vrs ps triple:")!=std::string::npos){
        unsigned long long a=0;const auto at=row.find("full_before=");
        if(at!=std::string::npos&&std::sscanf(row.c_str()+at,"full_before=%llu",&a)==1&&a<kUsefulFullPs)++lowTriples;
    }
    check(lowTriples==0,"tiny full-rate screening never creates a native-map ratio denominator");
    context->ClearState();context->Flush();
}
} // namespace ps_fixture

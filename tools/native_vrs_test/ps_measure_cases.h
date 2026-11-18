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
// Exercise the production selector without a GPU: one successful first
// admission models a resolved tiny query; false canSample models pending slots.
inline void discoveryCases(){
    for(const unsigned count:{1u,15u,16u,17u,32u,40u,48u,64u,96u,256u}){
        Discovery discovery;std::set<unsigned> seen;unsigned maxMetadata=0;
        for(uint64_t frame=1;frame<=count;++frame){
            bool issued=false;unsigned metadata=0;
            for(unsigned ordinal=0;ordinal<count;++ordinal){
                if(!discovery.admit(0,frame,false,!issued))continue;
                ++metadata;seen.insert(ordinal);issued=true;
            }
            maxMetadata=(std::max)(maxMetadata,metadata);
        }
        check(seen.size()==count&&maxMetadata==1,"stable draw counts, including multiples of sixteen, visit every query ordinal");
        if(count==40)check(seen.count(25)==1,"adjacent useful ordinal 25 is reached behind forty stable tiny identities");
    }
    Discovery paused;unsigned resumed=40;
    for(uint64_t frame=1;frame<=7;++frame){
        bool issued=false;
        for(unsigned ordinal=0;ordinal<40;++ordinal){
            if(!paused.admit(0,frame,false,(frame==1||frame==7)&&!issued))continue;
            if(frame==7)resumed=ordinal;
            issued=true;
        }
    }
    check(resumed==1,"pending-query frames preserve the next unexamined ordinal instead of advancing a stride");
    Discovery invalidPrefix;std::set<unsigned> examined;unsigned maxMetadata=0;
    for(uint64_t frame=1;frame<=3;++frame){
        unsigned metadata=0;
        for(unsigned ordinal=0;ordinal<40;++ordinal)if(invalidPrefix.admit(0,frame,false,true)){++metadata;examined.insert(ordinal);}
        maxMetadata=(std::max)(maxMetadata,metadata);
    }
    check(examined.size()==40&&maxMetadata==16,"metadata-invalid prefixes still rotate, with at most sixteen reads per eye/frame");
    Discovery priority;
    for(unsigned ordinal=0;ordinal<40;++ordinal)priority.admit(0,1,false,ordinal==0);
    check(priority.admit(0,2,true,true),"useful candidate priority can precede the discovery cursor");
    unsigned admitted=1;
    for(unsigned ordinal=1;ordinal<40;++ordinal)admitted+=priority.admit(0,2,true,true)?1u:0u;
    check(admitted==16,"priority also respects the sixteen-metadata budget");
    check(priority.admit(1,2,false,true),"the other eye has an independent cursor and metadata budget");
    check(!priority.admit(2,2,true,true)&&!priority.admit(0,0,true,true),"invalid eye/frame cannot admit discovery metadata");
}
inline void run(){
    discoveryCases();
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
    ComPtr<ID3D11Query> completion;D3D11_QUERY_DESC completionDesc{D3D11_QUERY_EVENT,0};
    check(SUCCEEDED(device->CreateQuery(&completionDesc,&completion)),"fixture completion event created");
    if(!completion)return;
    Recorder actual;actual.configure(true,capture);rows.clear();uint64_t frame=1;
    // Submission/waiting belongs only to the fixture. An event after every
    // ended statistics query proves prior GPU completion without charging
    // each CPU poll against the recorder's scene-frame or matching budgets.
    // A failed/timed-out wait leaves the recorder pending; callers stop.
    const auto drain=[&](Recorder& recorder){
        if(!recorder.pending())return true;
        context->End(completion.Get());context->Flush();
        const auto deadline=GetTickCount64()+2000;bool completed=false;
        do {
            BOOL ready=FALSE;
            const HRESULT hr=context->GetData(completion.Get(),&ready,sizeof(ready),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(FAILED(hr))break;
            if(hr==S_OK&&ready){completed=true;break;}
            Sleep(1);
        }while(GetTickCount64()<deadline);
        if(!completed)return false; // Never reuse an unresolved event/query.
        recorder.collect(context.Get(),++frame,GetTickCount64());
        return !recorder.pending();
    };
    const auto draw=[&](bool changeEpoch,bool disable){
        receipt.frame=receipt.sequence=frame;auto plan=actual.plan(key,receipt);
        if(!plan.active)return false;
        if(!actual.begin(device.Get(),context.Get(),plan,receipt,GetTickCount64()))return false;
        context->Draw(3,0);auto after=receipt;if(changeEpoch)++after.gazeEpoch;
        if(disable)actual.configure(false,capture);
        actual.end(context.Get(),plan,after,true);
        const bool drained=drain(actual);
        ++receipt.invalidation; // Normal ClearState/Execute activity between frames.
        return drained;
    };
    const bool tripleDrawn=draw(false,false)&&draw(false,false)&&draw(false,false);
    if(!tripleDrawn)for(const auto& row:rows)std::printf("PS fixture diagnostic: %s\n",row.c_str());
    check(tripleDrawn,"actual query Begin/End/GetData drains a full/coarse/full triple");
    if(!tripleDrawn)return;
    check(lines("native vrs ps sample:")==3&&lines("valid=1 full_before=")==1,"actual pipeline statistics form one qualified matched triple");
    check(lines("PSInvocations=0 ")==0,"actual queried triangle invokes its pixel shader");
    check(lines("performance_claim=0")==1,"WARP recorder result never claims VRS acceleration");
    actual=Recorder{};actual.configure(true,capture);rows.clear();
    const bool epochDrawn=draw(true,false);
    check(epochDrawn&&lines("valid=0 query_hr=")==1,"epoch change while actual query is in flight invalidates its result");
    if(!epochDrawn)return;
    actual=Recorder{};actual.configure(true,capture);rows.clear();
    const bool offDrawn=draw(false,true);
    check(offDrawn&&lines("valid=0 query_hr=")==1,"OFF drains a real pending query without accepting evidence");
    if(!offDrawn)return;
    check(!actual.plan(key,receipt).active,"OFF refuses all subsequent query plans");
    // Drive production discovery+recorder through a prefix larger than both
    // candidate and metadata budgets. Only the later key has meaningful PS
    // work. Each startInstance epoch outlasts candidate expiry plus a full
    // forty-ordinal discovery traversal; triples must never straddle epochs.
    Recorder screening;screening.configure(true,capture);rows.clear();
    rd.ScissorEnable=TRUE;ComPtr<ID3D11RasterizerState> scissor;
    check(SUCCEEDED(device->CreateRasterizerState(&rd,&scissor)),"selection scissor state");
    context->RSSetState(scissor.Get());
    unsigned queries=0,maxMetadata=0,maxQueries=0;bool drained=true;
    frame=1000;
    for(unsigned iteration=0;iteration<230;++iteration){
        unsigned metadata=0,issued=0;
        for(unsigned ordinal=0;ordinal<40;++ordinal){
            const bool useful=ordinal==25&&iteration>=20;
            Key k{11,12,useful?2u:1u,3,1,0,useful?(iteration<70?500u:iteration<140?700u:500u):uint32_t(ordinal+10000),128,128,0,'N'};
            Receipt r{frame,frame,2,3,uint64_t(iteration+10),5,0,true};
            if(!screening.metadataWanted(k,r))continue;
            ++metadata;const auto plan=screening.plan(k,r);if(!plan.active)continue;
            if(!screening.begin(device.Get(),context.Get(),plan,r,GetTickCount64())){drained=false;break;}
            const D3D11_RECT clip{0,0,useful?128:1,useful?128:1};context->RSSetScissorRects(1,&clip);
            context->DrawInstanced(3,1,0,k.startInstance);screening.end(context.Get(),plan,r,true);++issued;++queries;
        }
        maxMetadata=(std::max)(maxMetadata,metadata);maxQueries=(std::max)(maxQueries,issued);
        if(!drained)break;
        drained=drain(screening);if(!drained)break;
        ++frame;
    }
    check(drained&&maxMetadata<=16&&maxQueries<=1&&queries<=230,"rotating discovery preserves metadata/query/ring drain bounds");
    if(!drained)return;
    check(lines("valid=1 full_before=")>=5,"useful ordinal 25 after stable tiny prefix draws forms real WARP matched triples");
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
    // Completed GPU work is still pending ownership until collect resolves
    // the six slots. A full ring must not spend discovery metadata or cursor.
    Recorder pending;pending.configure(true,capture);bool filled=true;auto pendingKey=key;pendingKey.kind='N';
    const D3D11_RECT tiny{0,0,1,1};context->RSSetScissorRects(1,&tiny);
    for(unsigned i=0;i<6;++i){
        ++frame;Receipt r{frame,frame,2,3,4,5,0,true};auto k=pendingKey;k.startInstance=i;
        const auto plan=pending.plan(k,r);
        if(!plan.active||!pending.begin(device.Get(),context.Get(),plan,r,GetTickCount64())){filled=false;break;}
        context->DrawInstanced(3,1,0,k.startInstance);pending.end(context.Get(),plan,r,true);
    }
    check(filled&&pending.pending(),"six real queries retain their slots until owner collection");
    if(!filled)return;
    ++frame;Receipt blocked{frame,frame,2,3,4,5,0,true};unsigned metadataWhilePending=0;
    for(unsigned ordinal=0;ordinal<40;++ordinal){auto k=pendingKey;k.startInstance=ordinal+100;
        metadataWhilePending+=pending.metadataWanted(k,blocked)?1u:0u;}
    check(metadataWhilePending==0,"a pending full query ring pauses discovery metadata and cursor progression");
    const bool ringDrained=drain(pending);
    check(ringDrained,"pending query ring drains without reusing unresolved query objects");
    if(!ringDrained)return;
    ++frame;Receipt resumedReceipt{frame,frame,2,3,4,5,0,true};
    check(pending.metadataWanted(pendingKey,resumedReceipt),"discovery resumes at the first unexamined ordinal after the ring drains");
    context->ClearState();context->Flush();
}
} // namespace ps_fixture

#pragma once
#include "../../src/d3d11/native_vrs_witness.h"
#include "../../src/d3d11/native_vrs_witness_state.h"
#include "../../src/d3d11/dxbc_engine_velocity.h"
#include "witness_pixel_reference.h"
#include "witness_original_depth_cases.h"
#include <d3dcompiler.h>
#include <vector>
#include <cstdio>
#include <thread>

namespace witness_gpu {
using Microsoft::WRL::ComPtr;
enum class RateStep {BeginPass,PrepareWitness,Material,ContinueMaterial,ExpireMaterial,ArmHandoff,AfterHandoff,StrongFull};
inline bool failure(int line){
    std::fprintf(stderr,"witness_gpu_cases.h line %d failed\n",line);return false;
}
inline bool pixelFailure(const char* assertion,unsigned x,unsigned y,float owner,
                         const void* expected,const void* actual,size_t bytes){
    uint32_t e[4]{},a[4]{};
    std::memcpy(e,expected,bytes);std::memcpy(a,actual,bytes);
    float ef[4]{},af[4]{};std::memcpy(ef,e,sizeof(e));std::memcpy(af,a,sizeof(a));
    std::fprintf(stderr,"witness %s at (%u,%u) MRT6 owner %.9g: expected [%08x %08x %08x %08x] (%.9g %.9g %.9g %.9g), actual [%08x %08x %08x %08x] (%.9g %.9g %.9g %.9g)\n",
        assertion,x,y,owner,e[0],e[1],e[2],e[3],ef[0],ef[1],ef[2],ef[3],
        a[0],a[1],a[2],a[3],af[0],af[1],af[2],af[3]);return false;
}
// Both backends drive the same splitWitness and exact production DXBC tail.
// WARP compares material too; the physical GPU must change only that output.
template<class Compile,class Rate>
bool run(ID3D11Device* dev,ID3D11DeviceContext* ctx,Compile compile,Rate rate,bool hardware,bool grouped=false) {
    constexpr UINT side=256;
    struct Targets {
        ComPtr<ID3D11Texture2D> tex[4];
        ComPtr<ID3D11RenderTargetView> color,slot,skin;
        ComPtr<ID3D11DepthStencilView> depth;
    } target[3]; // combined PS, split draw, independently compiled reference PS
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G8X24_TYPELESS};
    for(auto& t:target){
        for(UINT i=0;i<4;++i){
            D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=side;td.ArraySize=td.MipLevels=td.SampleDesc.Count=1;
            td.Format=formats[i];td.BindFlags=i==3?D3D11_BIND_DEPTH_STENCIL:D3D11_BIND_RENDER_TARGET;
            if(FAILED(dev->CreateTexture2D(&td,nullptr,&t.tex[i])))return failure(__LINE__);
        }
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};dd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        if(FAILED(dev->CreateDepthStencilView(t.tex[3].Get(),&dd,&t.depth))||
           FAILED(dev->CreateRenderTargetView(t.tex[0].Get(),nullptr,&t.color))||
           FAILED(dev->CreateRenderTargetView(t.tex[1].Get(),nullptr,&t.slot))||
           FAILED(dev->CreateRenderTargetView(t.tex[2].Get(),nullptr,&t.skin)))return failure(__LINE__);
    }
    const char* hlsl=R"(
struct V { float4 pos:SV_POSITION; nointerpolation uint slot:SLOTID; float2 uv:TEXCOORD; float4 skin:EDVRSKINPREV; };
V vs(uint vertex:SV_VertexID){
 uint triIndex=vertex/3, corner=vertex%3;
 float2 xy=corner==0?float2(-1,-1):corner==1?float2(-1,1):float2(1,-1);
 if(triIndex==1 || triIndex==5 || triIndex==7)xy=float2(-xy.x,-xy.y);
 V o;o.pos=float4(xy,0.375+xy.x*0.0625+xy.y*0.125+(triIndex==2?0.25:triIndex>=4?0.125:0),1);
 o.slot=triIndex>=6?7:triIndex>=4?6:triIndex+1;o.uv=xy;o.skin=float4(xy,triIndex+1,1);return o;
}
cbuffer Material : register(b0) {float bias;};
float ps(float4 pos:SV_POSITION,nointerpolation uint slot:SLOTID,float2 uv:TEXCOORD):SV_TARGET {
 return uv.x+2*uv.y+float(slot)*0.01+pos.z+bias;
}
struct Witness {float2 marker:SV_Target6;float4 skin:SV_Target7;};
Witness referencePs(V i){
 Witness o;o.marker=float2(2*i.slot+1,i.pos.z);o.skin=i.skin;return o;
})";
    ComPtr<ID3DBlob> vsCode,psCode,refCode;
    auto make=[&](const char* entry,const char* profile,ComPtr<ID3DBlob>& blob){
        return SUCCEEDED(compile(hlsl,std::strlen(hlsl),"witness-visibility",nullptr,nullptr,entry,profile,
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,nullptr));
    };
    if(!make("vs","vs_5_0",vsCode)||!make("ps","ps_5_0",psCode)||!make("referencePs","ps_5_0",refCode))return failure(__LINE__);
    edvr::EngineVelocityInputs inputs{};inputs.identityComponent=0;inputs.skinExport=true;
    using namespace edvr::dxbc_engine_velocity_detail;
    for(auto& chunk:parseContainer(psCode->GetBufferPointer(),psCode->GetBufferSize(),kPs50))
        if(chunk.tag==kTagIsgn)for(auto& e:parseSignature(chunk.bytes)){
            if(equalName(e.name,"SLOTID"))inputs.identityRegister=e.registerIndex;
            if(equalName(e.name,"SV_POSITION"))inputs.positionRegister=e.registerIndex;
        }
    for(auto& chunk:parseContainer(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),kVs50))
        if(chunk.tag==kTagOsgn)for(auto& e:parseSignature(chunk.bytes))
            if(equalName(e.name,"EDVRSKINPREV"))inputs.skinRegister=e.registerIndex;
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps,combined,fine,reference;
    std::string why;std::vector<BYTE> bytes;
    if(FAILED(dev->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs))||
       FAILED(dev->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps))||
       FAILED(dev->CreatePixelShader(refCode->GetBufferPointer(),refCode->GetBufferSize(),nullptr,&reference)))return failure(__LINE__);
    if(!edvr::engineVelocityPatchPs(psCode->GetBufferPointer(),psCode->GetBufferSize(),inputs,bytes,why)||
       FAILED(dev->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&combined))||
       !edvr::engineVelocityPatchPs(psCode->GetBufferPointer(),psCode->GetBufferSize(),inputs,bytes,why,false,FlatMarkerKind::None,nullptr,true)||
       FAILED(dev->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&fine)))return failure(__LINE__);
    // Guarded overlays select depth from the snapshot at t3 when the owner
    // matches. Production masks MRT7 to preserve the underlying skin sample.
    ComPtr<ID3D11PixelShader> guardedCombined,guardedFine;
    inputs.skinExport=false;
    if(!edvr::engineVelocityPatchPs(psCode->GetBufferPointer(),psCode->GetBufferSize(),inputs,bytes,why,true)||
       FAILED(dev->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&guardedCombined))||
       !edvr::engineVelocityPatchPs(psCode->GetBufferPointer(),psCode->GetBufferSize(),inputs,bytes,why,true,FlatMarkerKind::None,nullptr,true)||
       FAILED(dev->CreatePixelShader(bytes.data(),bytes.size(),nullptr,&guardedFine)))return failure(__LINE__);
    struct Marker {float code,depth;};
    std::vector<Marker> substrate(side*side);
    for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){
        const float nx=(2.f*x+1.f)/side-1.f,ny=1.f-(2.f*y+1.f)/side;
        const unsigned region=(x+2*y)%3;
        substrate[y*side+x]={region==0?13.f:region==1?15.f:-1.f,.375f+nx*.0625f+ny*.125f};
    }
    D3D11_TEXTURE2D_DESC sd{};target[0].tex[1]->GetDesc(&sd);
    sd.BindFlags=D3D11_BIND_SHADER_RESOURCE;sd.Usage=D3D11_USAGE_DEFAULT;
    std::vector<Marker> replacement=substrate;
    for(auto& pixel:replacement)pixel.code=pixel.code==13.f?15.f:13.f;
    D3D11_SUBRESOURCE_DATA data{substrate.data(),UINT(side*sizeof(Marker)),0};
    ComPtr<ID3D11Texture2D> snapshot,refill,guardOutput[2],beforeRefill[2],skinBefore[2];ComPtr<ID3D11ShaderResourceView> guardSrv;
    if(FAILED(dev->CreateTexture2D(&sd,&data,&snapshot))||
       FAILED(dev->CreateShaderResourceView(snapshot.Get(),nullptr,&guardSrv)))return failure(__LINE__);
    if(grouped){
        data.pSysMem=replacement.data();
        if(FAILED(dev->CreateTexture2D(&sd,&data,&refill)))return failure(__LINE__);
    }
    D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc=D3D11_COMPARISON_LESS;dd.StencilEnable=TRUE;dd.StencilReadMask=dd.StencilWriteMask=255;
    dd.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS;dd.FrontFace.StencilPassOp=D3D11_STENCIL_OP_INCR_SAT;
    dd.FrontFace.StencilFailOp=D3D11_STENCIL_OP_KEEP;dd.FrontFace.StencilDepthFailOp=D3D11_STENCIL_OP_INCR_SAT;dd.BackFace=dd.FrontFace;
    ComPtr<ID3D11DepthStencilState> strict,ties;
    if(FAILED(dev->CreateDepthStencilState(&dd,&strict)))return failure(__LINE__);
    dd.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    dd.FrontFace.StencilFunc=dd.BackFace.StencilFunc=D3D11_COMPARISON_EQUAL;
    if(FAILED(dev->CreateDepthStencilState(&dd,&ties)))return failure(__LINE__);
    dd.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.StencilEnable=FALSE;
    ComPtr<ID3D11DepthStencilState> overlay;if(FAILED(dev->CreateDepthStencilState(&dd,&overlay)))return failure(__LINE__);
    D3D11_BLEND_DESC bd{};bd.IndependentBlendEnable=TRUE;
    for(auto& rt:bd.RenderTarget){rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_ZERO;
        rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=15;}
    ComPtr<ID3D11BlendState> blend;if(FAILED(dev->CreateBlendState(&bd,&blend)))return failure(__LINE__);
    bd.RenderTarget[7].RenderTargetWriteMask=0;
    ComPtr<ID3D11BlendState> guardBlend;if(FAILED(dev->CreateBlendState(&bd,&guardBlend)))return failure(__LINE__);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;if(FAILED(dev->CreateRasterizerState(&rd,&raster)))return failure(__LINE__);
    // D3D11 functional spec 8.16: SV_VertexID restarts at zero on every Draw.
    // DrawIndexed feeds the index-buffer value to the shader, so each later
    // issue actually selects its coplanar or guarded triangles.
    uint16_t indices[24];for(unsigned i=0;i<24;++i)indices[i]=static_cast<uint16_t>(i);
    D3D11_BUFFER_DESC ib{};ib.ByteWidth=sizeof(indices);ib.BindFlags=D3D11_BIND_INDEX_BUFFER;ib.Usage=D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA indexData{indices,0,0};ComPtr<ID3D11Buffer> indexBuffer;
    if(FAILED(dev->CreateBuffer(&ib,&indexData,&indexBuffer)))return failure(__LINE__);
    ctx->ClearState();D3D11_VIEWPORT viewport{0,0,float(side),float(side),0,1};
    ctx->RSSetViewports(1,&viewport);ctx->RSSetState(raster.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetIndexBuffer(indexBuffer.Get(),DXGI_FORMAT_R16_UINT,0);
    ctx->VSSetShader(vs.Get(),nullptr,0);ctx->OMSetBlendState(blend.Get(),nullptr,~0u);
    ComPtr<ID3D11Buffer> materialParams;
    if(grouped){
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=16;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        if(FAILED(dev->CreateBuffer(&desc,nullptr,&materialParams)))return failure(__LINE__);
        ctx->PSSetConstantBuffers(0,1,materialParams.GetAddressOf());
    }
    auto setBias=[&](unsigned stage){
        if(materialParams){const float params[4]={4.f*stage,0,0,0};ctx->UpdateSubresource(materialParams.Get(),0,nullptr,params,0,0);}
    };
    edvr::native_vrs::WitnessMirror mirror;edvr::native_vrs::WitnessCounts counts;
    ComPtr<ID3D11Texture2D> overlap[2];
    ComPtr<ID3D11Texture2D> handoffPixels[2][4];
    for(unsigned pass=0;pass<3;++pass){
        const bool split=pass==1,independent=pass==2;
        auto& t=target[pass];const FLOAT zero[4]{};
        ctx->OMSetBlendState(blend.Get(),nullptr,~0u);
        if(!rate(RateStep::BeginPass,t.color.Get(),t.depth.Get()))return failure(__LINE__);
        for(auto* r:{t.color.Get(),t.slot.Get(),t.skin.Get()})ctx->ClearRenderTargetView(r,zero);
        ctx->ClearDepthStencilView(t.depth.Get(),3,1,0);
        ID3D11RenderTargetView* rt[8]{};rt[0]=t.color.Get();rt[6]=t.slot.Get();rt[7]=t.skin.Get();
        ctx->OMSetRenderTargets(8,rt,t.depth.Get());ctx->PSSetShader(independent?reference.Get():combined.Get(),nullptr,0);
        if(grouped && !independent){
            using namespace edvr::native_vrs;
            ctx->OMSetDepthStencilState(strict.Get(),2);
            setBias(0);
            const WitnessDraw draws[]={WitnessDraw::indexed(3,0,0),WitnessDraw::indexed(3,3,0),WitnessDraw::indexed(3,6,0)};
            if(!split){for(const auto& d:draws)issueWitnessDraw(*ctx,d);}
            else{
                // The immediate context stays on one thread at a time. The
                // notifier forwards its real CB mutation only after every
                // earlier scene draw ran under the owner's strong full rate.
                if(dev->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)return failure(__LINE__);
                WitnessPipeline held;
                if(!held.capture(ctx,t.color.Get(),t.depth.Get(),t.slot.Get(),t.skin.Get(),vs.Get(),combined.Get(),
                                 fine.Get(),ps.Get(),blend.Get()))return failure(__LINE__);
                const bool armed=rate(RateStep::ArmHandoff,t.color.Get(),t.depth.Get());
                WitnessCohort pending;
                const WitnessCohortKey key{ctx,t.color.Get(),t.depth.Get(),&held,1,1,1,0};
                if(!armed)return failure(__LINE__);
                for(const auto& d:draws)if(pending.append(key,d)!=WitnessCohortAppend::Appended)return failure(__LINE__);
                unsigned issued=0;bool preserved=false;const DWORD owner=GetCurrentThreadId();
                std::thread notifier([&]{
                    preserved=GetCurrentThreadId()!=owner && replayPrearmedOriginals(pending,ctx,armed,
                        [&](const WitnessDraw& d){issueWitnessDraw(*ctx,d);++issued;});
                    if(preserved)setBias(23);
                });
                notifier.join();
                if(!preserved || issued!=3 || !pending.empty() || !rate(RateStep::AfterHandoff,t.color.Get(),t.depth.Get()))
                    return failure(__LINE__);
            }
            for(unsigned i=0;i<4;++i){
                D3D11_TEXTURE2D_DESC desc{};t.tex[i]->GetDesc(&desc);
                desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if(FAILED(dev->CreateTexture2D(&desc,nullptr,&handoffPixels[pass][i])))return failure(__LINE__);
                ctx->CopyResource(handoffPixels[pass][i].Get(),t.tex[i].Get());
            }
            for(auto* r:{t.color.Get(),t.slot.Get(),t.skin.Get()})ctx->ClearRenderTargetView(r,zero);
            ctx->ClearDepthStencilView(t.depth.Get(),3,1,0);
        }
        auto fineRate=[&]{return rate(RateStep::PrepareWitness,t.color.Get(),t.depth.Get());};
        struct Api{
            ID3D11DeviceContext* ctx;decltype(fineRate)& restored;ID3D11DepthStencilView* guardOriginal=nullptr;
            unsigned guardBinds=0;bool guardExact=true;
            bool full(){return restored();}
            void pixel(ID3D11PixelShader* p){ctx->PSSetShader(p,nullptr,0);}
            void targets(UINT n,ID3D11RenderTargetView* const* v,ID3D11DepthStencilView* d){
                ctx->OMSetRenderTargets(n,v,d);
                if(guardOriginal){
                    ComPtr<ID3D11DepthStencilView> actual;ctx->OMGetRenderTargets(0,nullptr,&actual);
                    guardExact&=actual.Get()==guardOriginal;++guardBinds;
                }
            }
            void copy(ID3D11Resource* d,ID3D11Resource* s){ctx->CopyResource(d,s);}
        } api{ctx,fineRate};
        for(unsigned step=0;step<4;++step){
            setBias(step); // the previous run must already have consumed its version of b0
            if(step==2)ctx->ClearDepthStencilView(t.depth.Get(),D3D11_CLEAR_STENCIL,1,19);
            ctx->OMSetDepthStencilState(step?ties.Get():strict.Get(),step<2?2:step==2?19:20);
            bool combinedFallback=false;
            using namespace edvr::native_vrs;
            const unsigned length=!grouped?1:step==0?3:step==2?2:1;
            auto descriptor=[&](unsigned i){
                if(grouped && step==0)return WitnessDraw::indexed(3,3*i,0);
                if(grouped && step==2 && i==0)return WitnessDraw::indexed(3,3,0);
                return WitnessDraw::indexed(step?3:9,step?9:0,0);
            };
            auto issue=[&](const WitnessDraw& d,bool material){
                if(split && step==2 && d.start==9 && material){
                    ComPtr<ID3D11PixelShader> actual;ctx->PSGetShader(&actual,nullptr,nullptr);
                    ID3D11RenderTargetView* actualRt[8]{};ComPtr<ID3D11DepthStencilView> actualDepth;
                    ctx->OMGetRenderTargets(8,actualRt,&actualDepth);
                    combinedFallback=actual.Get()==combined.Get() && actualDepth.Get()==t.depth.Get() &&
                        actualRt[6]==t.slot.Get() && actualRt[7]==t.skin.Get();
                    for(auto* r:actualRt)if(r)r->Release();
                }
                issueWitnessDraw(*ctx,d);
            };
            if(!split){for(unsigned i=0;i<length;++i)issue(descriptor(i),true);}
            else if(grouped){
                WitnessPipeline state;
                if(!state.capture(ctx,t.color.Get(),t.depth.Get(),t.slot.Get(),t.skin.Get(),vs.Get(),combined.Get(),
                                  fine.Get(),ps.Get(),blend.Get()))return failure(__LINE__);
                WitnessCohort queue;
                const WitnessCohortKey key{ctx,t.color.Get(),t.depth.Get(),&state,1,step+1,step+1,0};
                for(unsigned i=0;i<length;++i)if(queue.append(key,descriptor(i))!=WitnessCohortAppend::Appended)return failure(__LINE__);
                const uint64_t first[]={1,4,5,7};
                bool strong=true;unsigned fineIssued=0,materials=0;bool ordered=true;
                const bool done=replayWitnessCohort(queue,state,mirror,step>=2?2:1,first[step],counts,api,
                    [&](const WitnessDraw& d,bool material){
                        if(material){ordered=ordered&&fineIssued==length;++materials;}
                        else {ordered=ordered&&materials==0;++fineIssued;}
                        issue(d,material);
                    },[&](unsigned i){
                        if(step==2 && i==1){rate(RateStep::ExpireMaterial,t.color.Get(),t.depth.Get());return false;}
                        return rate(i?RateStep::ContinueMaterial:RateStep::Material,t.color.Get(),t.depth.Get());
                    },[&]{strong=rate(RateStep::StrongFull,t.color.Get(),t.depth.Get());});
                if(!done || !strong || !ordered || fineIssued!=length || materials!=length)return failure(__LINE__);
                queue.retire();
            }
            else if(!edvr::native_vrs::splitWitness(api,ctx,t.color.Get(),t.depth.Get(),t.slot.Get(),t.skin.Get(),
                vs.Get(),combined.Get(),fine.Get(),ps.Get(),blend.Get(),mirror,step>=2?2:1,step+1,true,counts,[&](bool m){issue(descriptor(0),m);},
                [&]{
                    if(step==2){rate(RateStep::StrongFull,t.color.Get(),t.depth.Get());return false;}
                    return rate(RateStep::Material,t.color.Get(),t.depth.Get());
                }))return failure(__LINE__);
            if(split && step==2 && !combinedFallback)return failure(__LINE__);
            if(!independent && step==0){
                D3D11_TEXTURE2D_DESC td{};t.tex[1]->GetDesc(&td);
                td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if(FAILED(dev->CreateTexture2D(&td,nullptr,&overlap[pass])))return failure(__LINE__);
                ctx->CopyResource(overlap[pass].Get(),t.tex[1].Get());
            }
        }
        if(independent)continue;
        setBias(4);
        if(split){
            ComPtr<ID3D11PixelShader> restoredPs;ctx->PSGetShader(&restoredPs,nullptr,nullptr);
            ID3D11RenderTargetView* restored[8]{};ComPtr<ID3D11DepthStencilView> restoredDepth;
            ctx->OMGetRenderTargets(8,restored,&restoredDepth);
            bool restoredAll=restoredPs.Get()==combined.Get() && restoredDepth.Get()==t.depth.Get();
            for(unsigned i=0;i<8;++i){
                restoredAll=restoredAll && rt[i]==restored[i];
                if(restored[i])restored[i]->Release();
            }
            if(!restoredAll)return failure(__LINE__);
            D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> pred;
            if(FAILED(dev->CreatePredicate(&qd,&pred)))return failure(__LINE__);
            ctx->Begin(pred.Get());ctx->End(pred.Get());ctx->SetPredication(pred.Get(),TRUE);
            unsigned unexpectedIssues=0;
            const bool ran=edvr::native_vrs::splitWitness(api,ctx,t.color.Get(),t.depth.Get(),t.slot.Get(),t.skin.Get(),
                vs.Get(),combined.Get(),fine.Get(),ps.Get(),blend.Get(),mirror,2,5,true,counts,
                [&](bool){++unexpectedIssues;},[&]{++unexpectedIssues;return true;});
            ctx->SetPredication(nullptr,FALSE);
            if(ran || unexpectedIssues || counts.stateRefused!=1)return failure(__LINE__);
        }
        // Keep the ordinary MRT6 for its independent depth comparison below.
        // The overlay reuses the actual nonzero E from the preceding skin draw.
        D3D11_TEXTURE2D_DESC od{};t.tex[1]->GetDesc(&od);
        ComPtr<ID3D11RenderTargetView> guardRtv;
        if(FAILED(dev->CreateTexture2D(&od,nullptr,&guardOutput[pass]))||
           FAILED(dev->CreateRenderTargetView(guardOutput[pass].Get(),nullptr,&guardRtv)))return failure(__LINE__);
        t.tex[2]->GetDesc(&od);od.BindFlags=0;od.Usage=D3D11_USAGE_STAGING;od.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(FAILED(dev->CreateTexture2D(&od,nullptr,&skinBefore[pass])))return failure(__LINE__);
        ctx->CopyResource(skinBefore[pass].Get(),t.tex[2].Get());
        rt[6]=guardRtv.Get();ctx->ClearRenderTargetView(guardRtv.Get(),zero);ctx->OMSetRenderTargets(8,rt,t.depth.Get());
        ctx->OMSetDepthStencilState(overlay.Get(),0);ctx->OMSetBlendState(guardBlend.Get(),nullptr,~0u);
        ctx->PSSetShader(guardedCombined.Get(),nullptr,0);
        ctx->PSSetShaderResources(edvr::kEngineVelocityOverlaySnapshotSlot,1,guardSrv.GetAddressOf());
        if(split)api.guardOriginal=t.depth.Get();
        if(grouped){
            using namespace edvr::native_vrs;
            // Preserve the first publication's pixels before another copy
            // into the very same private texture. Both groups overlap fully;
            // reversing even the two descriptors changes the final owner.
            constexpr unsigned slot=edvr::kEngineVelocityOverlaySnapshotSlot;
            ID3D11ShaderResourceView* empty=nullptr;
            WitnessGuardPublication generation;
            if(!generation.replace([]{return true;},[&]{
                ctx->PSSetShaderResources(slot,1,&empty);
                ctx->UpdateSubresource(snapshot.Get(),0,nullptr,substrate.data(),UINT(side*sizeof(Marker)),0);
                ctx->PSSetShaderResources(slot,1,guardSrv.GetAddressOf());
            }))return failure(__LINE__);
            D3D11_TEXTURE2D_DESC read{};guardOutput[pass]->GetDesc(&read);
            read.BindFlags=0;read.Usage=D3D11_USAGE_STAGING;read.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if(FAILED(dev->CreateTexture2D(&read,nullptr,&beforeRefill[pass])))return failure(__LINE__);
            auto replaceBytes=[&]{
                ctx->CopyResource(beforeRefill[pass].Get(),guardOutput[pass].Get());
                ctx->PSSetShaderResources(slot,1,&empty);
                ctx->CopyResource(snapshot.Get(),refill.Get());
                ctx->PSSetShaderResources(slot,1,guardSrv.GetAddressOf());
            };
            for(unsigned publication=1;publication<=2;++publication){
                const WitnessGuardSnapshot stamp{guardSrv.Get(),generation.current()};
                if(stamp.publication!=publication)return failure(__LINE__);
                const WitnessDraw draws[]={WitnessDraw::indexed(6,publication==1?18:12,0),
                                           WitnessDraw::indexed(6,publication==1?12:18,0)};
                if(!split){
                    for(auto& draw:draws)issueWitnessDraw(*ctx,draw);
                    if(publication==1 && !generation.replace([]{return true;},replaceBytes))return failure(__LINE__);
                    continue;
                }
                auto captureState=[&](WitnessPipeline& held,ID3D11BlendState* b){
                    return held.capture(ctx,t.color.Get(),t.depth.Get(),guardRtv.Get(),t.skin.Get(),vs.Get(),
                        guardedCombined.Get(),guardedFine.Get(),ps.Get(),b);
                };
                if(publication==1){
                    D3D11_DEPTH_STENCIL_DESC writes{};overlay->GetDesc(&writes);
                    writes.StencilEnable=TRUE;writes.StencilWriteMask=255;
                    writes.BackFace.StencilPassOp=D3D11_STENCIL_OP_REPLACE;
                    ComPtr<ID3D11DepthStencilState> stencilWriter;
                    if(FAILED(dev->CreateDepthStencilState(&writes,&stencilWriter)))return failure(__LINE__);
                    WitnessPipeline stencilState,skinState,restoredT3;
                    ctx->OMSetDepthStencilState(stencilWriter.Get(),0);
                    if(!captureState(stencilState,guardBlend.Get()) || stencilState.permitsGuard(stamp,slot))return failure(__LINE__);
                    ctx->OMSetDepthStencilState(overlay.Get(),0);ctx->OMSetBlendState(blend.Get(),nullptr,~0u);
                    if(!captureState(skinState,blend.Get()) || skinState.permitsGuard(stamp,slot))return failure(__LINE__);
                    ctx->OMSetBlendState(guardBlend.Get(),nullptr,~0u);ctx->PSSetShaderResources(slot,1,&empty);
                    if(!captureState(restoredT3,guardBlend.Get()) || restoredT3.permitsGuard(stamp,slot))return failure(__LINE__);
                    ctx->PSSetShaderResources(slot,1,guardSrv.GetAddressOf());
                }
                WitnessPipeline held;
                if(!captureState(held,guardBlend.Get()) || !held.permitsGuard(stamp,slot) ||
                   !rate(RateStep::StrongFull,t.color.Get(),t.depth.Get()))return failure(__LINE__);
                WitnessCohort queue;
                const WitnessCohortKey key{ctx,t.color.Get(),t.depth.Get(),&held,1,1,1,0,stamp};
                for(auto& draw:draws)if(queue.append(key,draw)!=WitnessCohortAppend::Appended)return failure(__LINE__);
                auto later=key;++later.guard.publication;
                if(queue.append(later,draws[0])!=WitnessCohortAppend::Drain)return failure(__LINE__);
                unsigned foreignIssues=0;bool refused=false;
                std::thread foreign([&]{
                    refused=!replayPrearmedOriginals(queue,ctx,true,[&](const WitnessDraw&){++foreignIssues;},later.guard);
                });
                foreign.join();
                if(!refused || foreignIssues || queue.size()!=2)return failure(__LINE__);
                auto replay=[&]{
                    unsigned nFine=0,nMaterial=0;bool order=true,strong=true;
                    const bool done=replayWitnessCohort(queue,held,mirror,2+publication,8+(publication-1)*2,counts,api,
                        [&](const WitnessDraw& draw,bool material){
                            if(material){order&=nFine==2 && nMaterial<2 && draw.start==draws[nMaterial].start;++nMaterial;}
                            else{order&=nMaterial==0 && nFine<2 && draw.start==draws[nFine].start;++nFine;}
                            issueWitnessDraw(*ctx,draw);
                        },[&](unsigned i){return rate(i?RateStep::ContinueMaterial:RateStep::Material,t.color.Get(),t.depth.Get());},
                        [&]{strong=rate(RateStep::StrongFull,t.color.Get(),t.depth.Get());});
                    if(!done || !strong || !order || nMaterial!=2)return false;
                    queue.retire();return true;
                };
                if(!(publication==1 ? generation.replace(replay,replaceBytes) : replay()))return failure(__LINE__);
            }
        }else if(!pass)ctx->DrawIndexed(6,12,0);
        else if(!edvr::native_vrs::splitWitness(api,ctx,t.color.Get(),t.depth.Get(),guardRtv.Get(),t.skin.Get(),
            vs.Get(),guardedCombined.Get(),guardedFine.Get(),ps.Get(),guardBlend.Get(),mirror,3,5,true,counts,
            [&](bool){ctx->DrawIndexed(6,12,0);},[&]{return rate(RateStep::Material,t.color.Get(),t.depth.Get());}))return failure(__LINE__);
        if(split && (!api.guardExact || api.guardBinds!=(grouped?6u:3u)))return failure(__LINE__);
        ID3D11ShaderResourceView* empty=nullptr;ctx->PSSetShaderResources(edvr::kEngineVelocityOverlaySnapshotSlot,1,&empty);
    }
    ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->ClearState();
    if(grouped){
        for(unsigned i=0;i<4;++i){
            D3D11_MAPPED_SUBRESOURCE full{},worker{};
            if(FAILED(ctx->Map(handoffPixels[0][i].Get(),0,D3D11_MAP_READ,0,&full)))return failure(__LINE__);
            if(FAILED(ctx->Map(handoffPixels[1][i].Get(),0,D3D11_MAP_READ,0,&worker))){
                ctx->Unmap(handoffPixels[0][i].Get(),0);return failure(__LINE__);
            }
            const unsigned bytes=i==0?4:i==2?16:8;
            bool equal=true,nonzero=false;
            for(unsigned y=0;y<side && equal;++y){
                const auto* a=static_cast<const BYTE*>(full.pData)+y*full.RowPitch;
                const auto* b=static_cast<const BYTE*>(worker.pData)+y*worker.RowPitch;
                for(unsigned x=0;x<side;++x){
                    // The three X24 padding bytes are undefined.
                    const unsigned used=i==3?5:bytes;
                    if(std::memcmp(a+x*bytes,b+x*bytes,used)){equal=false;break;}
                    for(unsigned c=0;c<used;++c)nonzero|=a[x*bytes+c]!=0;
                }
            }
            ctx->Unmap(handoffPixels[1][i].Get(),0);ctx->Unmap(handoffPixels[0][i].Get(),0);
            if(!equal || !nonzero){std::fprintf(stderr,"handoff target %u equal=%d nonzero=%d\n",i,equal,nonzero);return false;}
        }
        std::puts("{\"serialized_handoff_color_depth_stencil_motion_skin_exact\":true}");
    }
    for(auto& first:overlap){
        D3D11_MAPPED_SUBRESOURCE mapped{};if(FAILED(ctx->Map(first.Get(),0,D3D11_MAP_READ,0,&mapped)))return failure(__LINE__);
        unsigned front=0,other=0;
        for(unsigned y=0;y<side;++y){
            const auto* row=reinterpret_cast<const float*>(static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch);
            for(unsigned x=0;x<side;++x){front+=row[x*2]==3.f;other+=row[x*2]==5.f;}
        }
        ctx->Unmap(first.Get(),0);
        if(!front||!other||front+other!=side*side)return failure(__LINE__);
    }
    std::vector<uint32_t> depthWitness(side*side);
    std::vector<float> markerOwner(side*side);
    bool distinctMaterial=false,sawNear=false,sawTie=false,sawStencil=false,sawSlope=false;
    for(unsigned k=0;k<5;++k){
        const unsigned pixelBytes=k==0?4:k==2?16:8;
        const char* const labels[]={"material","MRT6","MRT7","depth/stencil","guarded MRT6"};
        const bool referenceTarget=k==1 || k==2;
        std::vector<uint8_t> pixels[3];
        for(unsigned pass=0;pass<(referenceTarget?3u:2u);++pass){
            auto* output=k==4?guardOutput[pass].Get():target[pass].tex[k].Get();
            D3D11_TEXTURE2D_DESC td{};output->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> read;if(FAILED(dev->CreateTexture2D(&td,nullptr,&read)))return failure(__LINE__);
            ctx->CopyResource(read.Get(),output);D3D11_MAPPED_SUBRESOURCE mapped{};
            if(FAILED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)))return failure(__LINE__);
            pixels[pass].resize(side*side*pixelBytes);
            for(unsigned y=0;y<side;++y)std::memcpy(pixels[pass].data()+y*side*pixelBytes,
                static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch,side*pixelBytes);
            ctx->Unmap(read.Get(),0);
            if(k==2 && pass<2){
                if(FAILED(ctx->Map(skinBefore[pass].Get(),0,D3D11_MAP_READ,0,&mapped)))return failure(__LINE__);
                bool intact=true;for(unsigned y=0;intact && y<side;++y){
                    const auto* before=static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch;
                    const auto* after=pixels[pass].data()+y*side*pixelBytes;
                    for(unsigned x=0;x<side;++x)if(std::memcmp(before+x*pixelBytes,after+x*pixelBytes,pixelBytes)){
                        intact=pixelFailure(pass?"split overlay altered E":"baseline overlay altered E",x,y,markerOwner[size_t(y)*side+x],
                            before+x*pixelBytes,after+x*pixelBytes,pixelBytes);break;
                    }
                }
                ctx->Unmap(skinBefore[pass].Get(),0);
                if(!intact)return false;
            }
            if(k==3)for(size_t pos=0;pos<pixels[pass].size();pos+=8)
                std::memset(pixels[pass].data()+pos+5,0,3); // X24 is not depth or stencil
        }
        if(k==0)distinctMaterial=pixels[0]!=pixels[1];
        for(size_t pos=0;pos<pixels[0].size();pos+=pixelBytes){
            float value[4]{};std::memcpy(value,pixels[0].data()+pos,pixelBytes);
            const size_t i=pos/pixelBytes;const unsigned x=UINT(i%side),y=UINT(i/side);
            if(k==1)markerOwner[i]=value[0];
            if(k && std::memcmp(pixels[0].data()+pos,pixels[1].data()+pos,pixelBytes))
                return pixelFailure(labels[k],x,y,markerOwner[i],pixels[0].data()+pos,pixels[1].data()+pos,pixelBytes);
            if(referenceTarget && std::memcmp(pixels[2].data()+pos,pixels[0].data()+pos,pixelBytes))
                return pixelFailure(k==1?"independent HLSL MRT6":"independent HLSL MRT7",x,y,markerOwner[i],
                    pixels[2].data()+pos,pixels[0].data()+pos,pixelBytes);
            if(k==1){sawNear|=value[0]==5.f;sawTie|=value[0]==9.f;sawSlope|=value[1]>.42f&&value[1]<.54f;
                std::memcpy(&depthWitness[pos/8],pixels[0].data()+pos+4,4);}
            if(k==2){
                // The unequal-depth and stencil passes select the owner per pixel.
                // E.z carries the VS owner as a smooth float. Its exact interpolated
                // bits were checked above against HLSL reading the original varying.
                const float owner=markerOwner[i],slot=(owner-1.f)*.5f;
                const float nx=(2.f*x+1.f)/side-1.f,ny=1.f-(2.f*y+1.f)/side;
                const float expected[4]={nx,ny,slot,1.f};
                if(!fixtureSkinOwner(owner,value,nx,ny))
                    return pixelFailure("skin gradient and decoded owner",x,y,owner,expected,value,sizeof(value));
            }
            if(k==3){sawStencil|=pixels[0][pos+4]==21;
                if(std::memcmp(&depthWitness[i],pixels[0].data()+pos,4))
                    return pixelFailure("MRT6 depth vs scene depth",x,y,markerOwner[i],pixels[0].data()+pos,&depthWitness[i],4);}
            if(k==4){
                const auto& under=grouped?replacement[i]:substrate[i];
                float sceneDepth;std::memcpy(&sceneDepth,&depthWitness[pos/8],4);
                const float owner=grouped?15.f:13.f;
                const float expected=under.code==owner?sceneDepth:sceneDepth+.125f;
                if(under.depth!=sceneDepth)return pixelFailure("guard fixture substrate depth",x,y,under.code,&sceneDepth,&under.depth,4);
                const float marker[]={owner,expected};
                if(value[0]!=owner||value[1]!=expected)return pixelFailure("guarded marker vs substrate",x,y,under.code,marker,value,sizeof(marker));
            }
        }
    }
    if(grouped)for(unsigned pass=0;pass<2;++pass){
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(ctx->Map(beforeRefill[pass].Get(),0,D3D11_MAP_READ,0,&mapped)))return failure(__LINE__);
        bool exact=true;
        for(UINT y=0;y<side && exact;++y)for(UINT x=0;x<side;++x){
            const auto& base=substrate[y*side+x];
            const float expected[]={13.f,base.code==13.f?base.depth:base.depth+.125f};
            const auto* got=static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch+x*sizeof(Marker);
            if(std::memcmp(expected,got,sizeof(expected))){
                exact=pixelFailure("guard before same-SRV refill",x,y,base.code,expected,got,sizeof(expected));break;
            }
        }
        ctx->Unmap(beforeRefill[pass].Get(),0);
        if(!exact)return false;
    }
    if(counts.paired==(grouped?10u:4u) && counts.copies==3 && counts.reused==(grouped?4u:1u) && counts.rateRefused==1 &&
       counts.readOnlyOriginalRuns==(grouped?2u:1u) && counts.readOnlyOriginalDraws==(grouped?4u:1u) &&
       distinctMaterial==hardware && sawNear && sawTie && sawSlope && sawStencil){
        if(grouped){
            if(!rate(RateStep::StrongFull,target[0].color.Get(),target[0].depth.Get()))return failure(__LINE__);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->IASetIndexBuffer(indexBuffer.Get(),DXGI_FORMAT_R16_UINT,0);ctx->VSSetShader(vs.Get(),nullptr,0);
            ctx->RSSetViewports(1,&viewport);ctx->RSSetState(raster.Get());ctx->OMSetBlendState(blend.Get(),nullptr,~0u);
            const bool exact=originalDepthCases(dev,ctx,target,vs.Get(),combined.Get(),fine.Get(),ps.Get(),blend.Get());
            ctx->ClearState();
            std::printf("{\"readonly_original_dsv_transitions_and_pixels_exact\":%s}\n",exact?"true":"false");
            if(!exact)return failure(__LINE__);
        }
        return true;
    }
    std::fprintf(stderr,"witness totals paired=%llu copies=%llu reused=%llu readonly_runs/draws=%llu/%llu rateRefused=%llu materialDiff=%d hardware=%d slot5=%d slot9=%d slope=%d stencil21=%d\n",
        counts.paired,counts.copies,counts.reused,counts.readOnlyOriginalRuns,counts.readOnlyOriginalDraws,
        counts.rateRefused,distinctMaterial,hardware,sawNear,sawTie,sawSlope,sawStencil);
    return false;
}
}

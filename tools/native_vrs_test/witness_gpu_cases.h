#pragma once
#include "../../src/d3d11/native_vrs_witness.h"
#include "../../src/d3d11/dxbc_engine_velocity.h"
#include <d3dcompiler.h>
#include <vector>
#include <cstdio>

namespace witness_gpu {
using Microsoft::WRL::ComPtr;
inline bool failure(int line){
    std::fprintf(stderr,"witness_gpu_cases.h line %d failed\n",line);return false;
}
// Both backends drive the same splitWitness and exact production DXBC tail.
// WARP compares material too; the physical GPU must change only that output.
template<class Compile,class Rate>
bool run(ID3D11Device* dev,ID3D11DeviceContext* ctx,Compile compile,Rate rate,bool hardware) {
    constexpr UINT side=256;
    struct Targets {
        ComPtr<ID3D11Texture2D> tex[4];
        ComPtr<ID3D11RenderTargetView> color,slot,skin;
        ComPtr<ID3D11DepthStencilView> depth;
    } target[2];
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
 if(triIndex==1 || triIndex==5)xy=float2(-xy.x,-xy.y);
 V o;o.pos=float4(xy,0.375+xy.x*0.0625+xy.y*0.125+(triIndex==2?0.25:triIndex>=4?0.125:0),1);
 o.slot=triIndex>=4?6:triIndex+1;o.uv=xy;o.skin=float4(xy,triIndex+1,1);return o;
}
float ps(float4 pos:SV_POSITION,nointerpolation uint slot:SLOTID,float2 uv:TEXCOORD):SV_TARGET {
 return uv.x+2*uv.y+float(slot)*0.01+pos.z;
})";
    ComPtr<ID3DBlob> vsCode,psCode;
    auto make=[&](const char* entry,const char* profile,ComPtr<ID3DBlob>& blob){
        return SUCCEEDED(compile(hlsl,std::strlen(hlsl),"witness-visibility",nullptr,nullptr,entry,profile,
            D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,nullptr));
    };
    if(!make("vs","vs_5_0",vsCode)||!make("ps","ps_5_0",psCode))return failure(__LINE__);
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
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps,combined,fine;
    std::string why;std::vector<BYTE> bytes;
    if(FAILED(dev->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs))||
       FAILED(dev->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)))return failure(__LINE__);
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
    sd.BindFlags=D3D11_BIND_SHADER_RESOURCE;sd.Usage=D3D11_USAGE_IMMUTABLE;
    D3D11_SUBRESOURCE_DATA data{substrate.data(),UINT(side*sizeof(Marker)),0};
    ComPtr<ID3D11Texture2D> snapshot,guardOutput[2],skinBefore[2];ComPtr<ID3D11ShaderResourceView> guardSrv;
    if(FAILED(dev->CreateTexture2D(&sd,&data,&snapshot))||
       FAILED(dev->CreateShaderResourceView(snapshot.Get(),nullptr,&guardSrv)))return failure(__LINE__);
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
    ctx->ClearState();D3D11_VIEWPORT viewport{0,0,float(side),float(side),0,1};
    ctx->RSSetViewports(1,&viewport);ctx->RSSetState(raster.Get());ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(),nullptr,0);ctx->OMSetBlendState(blend.Get(),nullptr,~0u);
    edvr::native_vrs::WitnessMirror mirror;edvr::native_vrs::WitnessCounts counts;
    ComPtr<ID3D11Texture2D> overlap[2];
    for(unsigned pass=0;pass<2;++pass){
        auto& t=target[pass];const FLOAT zero[4]{};
        ctx->OMSetBlendState(blend.Get(),nullptr,~0u);
        if(!rate(false,t.color.Get(),t.depth.Get()))return failure(__LINE__);
        for(auto* r:{t.color.Get(),t.slot.Get(),t.skin.Get()})ctx->ClearRenderTargetView(r,zero);
        ctx->ClearDepthStencilView(t.depth.Get(),3,1,0);
        ID3D11RenderTargetView* rt[8]{};rt[0]=t.color.Get();rt[6]=t.slot.Get();rt[7]=t.skin.Get();
        ctx->OMSetRenderTargets(8,rt,t.depth.Get());ctx->PSSetShader(combined.Get(),nullptr,0);
        auto fineRate=[&]{return rate(false,t.color.Get(),t.depth.Get());};
        struct Api{
            ID3D11DeviceContext* ctx;decltype(fineRate)& restored;
            bool full(){return restored();}
            void pixel(ID3D11PixelShader* p){ctx->PSSetShader(p,nullptr,0);}
            void targets(UINT n,ID3D11RenderTargetView* const* v,ID3D11DepthStencilView* d){ctx->OMSetRenderTargets(n,v,d);}
            void copy(ID3D11Resource* d,ID3D11Resource* s){ctx->CopyResource(d,s);}
        } api{ctx,fineRate};
        for(unsigned step=0;step<4;++step){
            if(step==2)ctx->ClearDepthStencilView(t.depth.Get(),D3D11_CLEAR_STENCIL,1,19);
            ctx->OMSetDepthStencilState(step?ties.Get():strict.Get(),step<2?2:step==2?19:20);
            bool combinedFallback=false;
            auto issue=[&](bool material){
                if(pass && step==2 && material){
                    ComPtr<ID3D11PixelShader> actual;ctx->PSGetShader(&actual,nullptr,nullptr);
                    ID3D11RenderTargetView* actualRt[8]{};ComPtr<ID3D11DepthStencilView> actualDepth;
                    ctx->OMGetRenderTargets(8,actualRt,&actualDepth);
                    combinedFallback=actual.Get()==combined.Get() && actualDepth.Get()==t.depth.Get() &&
                        actualRt[6]==t.slot.Get() && actualRt[7]==t.skin.Get();
                    for(auto* r:actualRt)if(r)r->Release();
                }
                ctx->Draw(step?3:9,step?9:0);
            };
            if(!pass)issue(true);
            else if(!edvr::native_vrs::splitWitness(api,ctx,t.color.Get(),t.depth.Get(),t.slot.Get(),t.skin.Get(),
                vs.Get(),combined.Get(),fine.Get(),ps.Get(),blend.Get(),mirror,step>=2?2:1,step+1,true,counts,issue,
                [&]{return step!=2 && rate(true,t.color.Get(),t.depth.Get());}))return failure(__LINE__);
            if(pass && step==2 && !combinedFallback)return failure(__LINE__);
            if(step==0){
                D3D11_TEXTURE2D_DESC td{};t.tex[1]->GetDesc(&td);
                td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
                if(FAILED(dev->CreateTexture2D(&td,nullptr,&overlap[pass])))return failure(__LINE__);
                ctx->CopyResource(overlap[pass].Get(),t.tex[1].Get());
            }
        }
        if(pass){
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
        if(!pass)ctx->Draw(6,12);
        else if(!edvr::native_vrs::splitWitness(api,ctx,t.color.Get(),t.depth.Get(),guardRtv.Get(),t.skin.Get(),
            vs.Get(),guardedCombined.Get(),guardedFine.Get(),ps.Get(),guardBlend.Get(),mirror,3,5,true,counts,
            [&](bool){ctx->Draw(6,12);},[&]{return rate(true,t.color.Get(),t.depth.Get());}))return failure(__LINE__);
        ID3D11ShaderResourceView* empty=nullptr;ctx->PSSetShaderResources(edvr::kEngineVelocityOverlaySnapshotSlot,1,&empty);
    }
    ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->ClearState();
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
    bool distinctMaterial=false,sawNear=false,sawTie=false,sawStencil=false,sawSlope=false;
    for(unsigned k=0;k<5;++k){
        const unsigned pixelBytes=k==0?4:k==2?16:8;
        std::vector<uint8_t> pixels[2];
        for(unsigned pass=0;pass<2;++pass){
            auto* output=k==4?guardOutput[pass].Get():target[pass].tex[k].Get();
            D3D11_TEXTURE2D_DESC td{};output->GetDesc(&td);td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> read;if(FAILED(dev->CreateTexture2D(&td,nullptr,&read)))return failure(__LINE__);
            ctx->CopyResource(read.Get(),output);D3D11_MAPPED_SUBRESOURCE mapped{};
            if(FAILED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped)))return failure(__LINE__);
            pixels[pass].resize(side*side*pixelBytes);
            for(unsigned y=0;y<side;++y)std::memcpy(pixels[pass].data()+y*side*pixelBytes,
                static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch,side*pixelBytes);
            ctx->Unmap(read.Get(),0);
            if(k==2){
                if(FAILED(ctx->Map(skinBefore[pass].Get(),0,D3D11_MAP_READ,0,&mapped)))return failure(__LINE__);
                bool intact=true;
                for(unsigned y=0;y<side;++y)intact&=std::memcmp(pixels[pass].data()+y*side*pixelBytes,
                    static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch,side*pixelBytes)==0;
                ctx->Unmap(skinBefore[pass].Get(),0);
                if(!intact)return failure(__LINE__);
            }
            if(k==3)for(size_t pos=0;pos<pixels[pass].size();pos+=8)
                std::memset(pixels[pass].data()+pos+5,0,3); // X24 is not depth or stencil
        }
        if(k==0)distinctMaterial=pixels[0]!=pixels[1];else if(pixels[0]!=pixels[1])return failure(__LINE__);
        for(size_t pos=0;pos<pixels[0].size();pos+=pixelBytes){
            float value[4]{};std::memcpy(value,pixels[0].data()+pos,pixelBytes);
            if(k==1){sawNear|=value[0]==5.f;sawTie|=value[0]==9.f;sawSlope|=value[1]>.42f&&value[1]<.54f;
                std::memcpy(&depthWitness[pos/8],pixels[0].data()+pos+4,4);}
            if(k==2 && value[2]!=2.f && value[2]!=4.f)return failure(__LINE__);
            if(k==3){sawStencil|=pixels[0][pos+4]==21;
                if(std::memcmp(&depthWitness[pos/8],pixels[0].data()+pos,4))return failure(__LINE__);}
            if(k==4){
                const auto& under=substrate[pos/8];
                float sceneDepth;std::memcpy(&sceneDepth,&depthWitness[pos/8],4);
                const float expected=under.code==13.f?sceneDepth:sceneDepth+.125f;
                if(under.depth!=sceneDepth||value[0]!=13.f||value[1]!=expected)return failure(__LINE__);
            }
        }
    }
    return counts.paired==4 && counts.copies==4 && counts.reused==1 && counts.rateRefused==1 &&
        distinctMaterial==hardware && sawNear && sawTie && sawSlope && sawStencil;
}
}

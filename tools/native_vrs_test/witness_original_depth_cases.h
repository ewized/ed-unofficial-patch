#pragma once
#include "../../src/d3d11/native_vrs_witness.h"
#include <cstring>
#include <cstdio>
#include <iterator>

namespace witness_gpu {
// The caller has acknowledged full rate. Hold it on both sides of this
// differential so every color sample can be compared alongside motion, skin
// and both depth/stencil planes. The surrounding rig exercises physical VRS.
template<class Targets>
bool originalDepthCases(ID3D11Device* device,ID3D11DeviceContext* ctx,Targets* targets,
                        ID3D11VertexShader* vertex,ID3D11PixelShader* combined,ID3D11PixelShader* fine,
                        ID3D11PixelShader* material,ID3D11BlendState* blend) {
    using namespace edvr::native_vrs;
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable=TRUE;ds.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
    ds.StencilEnable=TRUE;ds.StencilReadMask=ds.StencilWriteMask=255;
    ds.FrontFace.StencilFunc=D3D11_COMPARISON_ALWAYS;
    ds.FrontFace.StencilFailOp=ds.FrontFace.StencilDepthFailOp=ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;
    ds.BackFace=ds.FrontFace;
    WitnessPtr<ID3D11DepthStencilState> readOnly,equalWrites,frontWrites,backWrites,masked;
    auto make=[&](WitnessPtr<ID3D11DepthStencilState>& s){return SUCCEEDED(device->CreateDepthStencilState(&ds,&s));};
    if(!make(readOnly))return false;
    ds.DepthFunc=D3D11_COMPARISON_EQUAL;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    if(!make(equalWrites))return false;
    ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
    ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_INCR_SAT;
    if(!make(frontWrites))return false;
    ds.FrontFace.StencilPassOp=D3D11_STENCIL_OP_KEEP;ds.BackFace.StencilDepthFailOp=D3D11_STENCIL_OP_DECR_SAT;
    if(!make(backWrites))return false;
    ds.StencilWriteMask=0;
    if(!make(masked))return false;

    struct Step {ID3D11DepthStencilState* state;bool original,copy,mutate,refuse;};
    const Step steps[]={
        {readOnly.Get(),true,false,false,false}, // cold read needs neither a mirror nor an allocation
        {nullptr,false,true,false,false},         // null DS state writes depth (LESS)
        {readOnly.Get(),true,false,false,false}, // both faces KEEP, preserve the matched mirror
        {equalWrites.Get(),false,false,false,false},
        {masked.Get(),true,false,true,false},    // clear breaks equivalence; read-only cannot establish it
        {frontWrites.Get(),false,true,false,false},
        {backWrites.Get(),false,false,false,false},
        {readOnly.Get(),true,false,false,true},  // refused coarse issue restores original PS and MRTs
        {nullptr,false,true,false,false},
    };
    struct Api {
        ID3D11DeviceContext* ctx;ID3D11DepthStencilView* original;ID3D11DepthStencilView* last;
        unsigned calls=0,switches=0,copies=0;bool exact=true;
        bool full(){return true;}
        void targets(UINT count,ID3D11RenderTargetView* const* rt,ID3D11DepthStencilView* requested){
            ctx->OMSetRenderTargets(count,rt,requested);
            WitnessPtr<ID3D11DepthStencilView> actual;ctx->OMGetRenderTargets(0,nullptr,&actual);
            exact&=actual.Get()==requested;switches+=actual.Get()!=last;last=actual.Get();++calls;
        }
        void pixel(ID3D11PixelShader* shader){ctx->PSSetShader(shader,nullptr,0);}
        void copy(ID3D11Resource* dst,ID3D11Resource* src){
            WitnessPtr<ID3D11Resource> scene;original->GetResource(&scene);
            exact&=last==nullptr && src==scene.Get() && dst!=src;
            ctx->CopyResource(dst,src);++copies;
        }
    };
    WitnessMirror mirror;WitnessCounts counts;uint64_t epoch=1;
    WitnessPtr<ID3D11Texture2D> read[2][4];
    D3D11_TEXTURE2D_DESC desc{};
    for(unsigned p=0;p<2;++p){
        auto& t=targets[p];
        for(unsigned k=0;k<4;++k){
            t.tex[k]->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            if(FAILED(device->CreateTexture2D(&desc,nullptr,&read[p][k])))return false;
        }
        const float zero[4]{};
        for(auto* r:{t.color.Get(),t.slot.Get(),t.skin.Get()})ctx->ClearRenderTargetView(r,zero);
        ctx->ClearDepthStencilView(t.depth.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1,0);
    }
    for(unsigned i=0;i<std::size(steps);++i){
        const auto& step=steps[i];epoch+=step.mutate;
        for(unsigned p=0;p<2;++p){
            auto& t=targets[p];
            ID3D11RenderTargetView* rt[8]{};rt[0]=t.color.Get();rt[6]=t.slot.Get();rt[7]=t.skin.Get();
            ctx->OMSetRenderTargets(8,rt,t.depth.Get());ctx->PSSetShader(combined,nullptr,0);
            if(step.mutate)ctx->ClearDepthStencilView(t.depth.Get(),D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,.9f,19);
            ctx->OMSetDepthStencilState(step.state,0);
            if(!p){ctx->DrawIndexed(6,0,0);continue;}
            Api api{ctx,t.depth.Get(),t.depth.Get()};
            unsigned fineIssues=0,materialIssues=0;
            auto issue=[&](bool scene){
                WitnessPtr<ID3D11DepthStencilView> actual;
                ID3D11RenderTargetView* bound[8]{};ctx->OMGetRenderTargets(8,bound,&actual);
                WitnessPtr<ID3D11PixelShader> shader;ctx->PSGetShader(&shader,nullptr,nullptr);
                api.exact&=actual.Get()==(scene||step.original?t.depth.Get():mirror.view.Get());
                const bool witnesses=!scene || step.refuse;
                api.exact&=shader.Get()==(!scene?fine:step.refuse?combined:material) &&
                    bound[0]==(scene?rt[0]:nullptr) && bound[6]==(witnesses?rt[6]:nullptr) &&
                    bound[7]==(witnesses?rt[7]:nullptr);
                for(auto* r:bound)if(r)r->Release();
                if(scene)++materialIssues;else ++fineIssues;
                ctx->DrawIndexed(6,0,0);
            };
            const bool ran=splitWitness(api,ctx,rt[0],t.depth.Get(),rt[6],rt[7],vertex,combined,fine,material,
                blend,mirror,epoch,i+1,true,counts,issue,[&]{return !step.refuse;});
            const unsigned switches=step.original?0:step.copy?3:2;
            if(!ran || !api.exact || fineIssues!=1 || materialIssues!=1 || api.copies!=unsigned(step.copy) ||
               api.switches!=switches || api.last!=t.depth.Get() || api.calls!=3u+step.copy+step.refuse ||
               (!i && (mirror.view || mirror.reusable))){
                std::fprintf(stderr,"original depth step %u ran=%d exact=%d copies=%u switches=%u calls=%u fine=%u material=%u\n",
                    i,ran,api.exact,api.copies,api.switches,api.calls,fineIssues,materialIssues);return false;
            }
        }
        ctx->OMSetRenderTargets(0,nullptr,nullptr);
        for(unsigned k=0;k<4;++k){
            D3D11_MAPPED_SUBRESOURCE pixels[2]{};
            for(unsigned p=0;p<2;++p){
                ctx->CopyResource(read[p][k].Get(),targets[p].tex[k].Get());
                if(FAILED(ctx->Map(read[p][k].Get(),0,D3D11_MAP_READ,0,&pixels[p]))){
                    if(p)ctx->Unmap(read[0][k].Get(),0);
                    return false;
                }
            }
            const unsigned bytes=k==0?4:k==2?16:8;
            const unsigned used=k==3?5:bytes; // X24 padding is undefined
            bool same=true,nonzero=false;
            for(unsigned y=0;y<desc.Height && same;++y){
                const auto* a=static_cast<const BYTE*>(pixels[0].pData)+y*pixels[0].RowPitch;
                const auto* b=static_cast<const BYTE*>(pixels[1].pData)+y*pixels[1].RowPitch;
                for(unsigned x=0;x<desc.Width;++x,a+=bytes,b+=bytes){
                    if(std::memcmp(a,b,used)){same=false;break;}
                    for(unsigned c=0;c<used;++c)nonzero|=a[c]!=0;
                }
            }
            for(unsigned p=0;p<2;++p)ctx->Unmap(read[p][k].Get(),0);
            if(!same || !nonzero){std::fprintf(stderr,"original depth step %u target %u equal=%d nonzero=%d\n",i,k,same,nonzero);return false;}
        }
    }
    return counts.readOnlyOriginalRuns==4 && counts.readOnlyOriginalDraws==4 && counts.copies==3 &&
           counts.reused==2 && counts.paired==8 && counts.rateRefused==1;
}
} // namespace witness_gpu

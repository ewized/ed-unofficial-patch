#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <utility>

namespace edvr::native_vrs {
template<class T> using WitnessPtr=Microsoft::WRL::ComPtr<T>;
struct WitnessMirror {
    WitnessPtr<ID3D11DepthStencilView> source,view;
    WitnessPtr<ID3D11Texture2D> texture;
    uint64_t epoch=0,draw=0;
    bool reusable=false;
};
struct WitnessCounts {uint64_t paired=0,copies=0,reused=0,stateRefused=0,shaderRefused=0,queryRefused=0,rateRefused=0;};

// Runs both halves synchronously with unchanged IA, vertex processing, viewport,
// rasterizer, OM blend, depth function and stencil reference. No mutation is
// made until every non-rate prerequisite is proven. After the fine issue the
// material must be issued even if coarse activation fails: its marker exists.
template<class Api,class Draw,class Coarse>
bool splitWitness(Api& api,ID3D11DeviceContext* ctx,ID3D11RenderTargetView* color,ID3D11DepthStencilView* depth,
    ID3D11RenderTargetView* slots,ID3D11RenderTargetView* skin,ID3D11VertexShader* vertex,
    ID3D11PixelShader* combined,ID3D11PixelShader* fine,ID3D11PixelShader* material,ID3D11BlendState* blend,
    WitnessMirror& mirror,uint64_t epoch,uint64_t drawNumber,bool reuseAllowed,WitnessCounts& counts,Draw draw,Coarse coarse) {
    auto refuse=[&]{++counts.stateRefused;return false;};
    if(!ctx||!color||!depth||!slots||!vertex||!combined||!fine||!material||!blend)return refuse();
    WitnessPtr<ID3D11Device> device;ctx->GetDevice(&device);
    WitnessPtr<ID3D11VertexShader> vs;WitnessPtr<ID3D11PixelShader> ps;
    UINT vc=0,pc=0;ctx->VSGetShader(&vs,nullptr,&vc);ctx->PSGetShader(&ps,nullptr,&pc);
    WitnessPtr<ID3D11BlendState> current;ctx->OMGetBlendState(&current,nullptr,nullptr);
    if(vc||pc||vertex!=vs.Get()||combined!=ps.Get()||blend!=current.Get())return refuse();
    D3D11_BLEND_DESC bd{};blend->GetDesc(&bd);
    if(bd.AlphaToCoverageEnable||!bd.IndependentBlendEnable||!(bd.RenderTarget[0].RenderTargetWriteMask&7)||
       bd.RenderTarget[6].BlendEnable||(bd.RenderTarget[6].RenderTargetWriteMask&3)!=3||
       (skin && bd.RenderTarget[7].BlendEnable))return refuse();
    WitnessPtr<ID3D11Predicate> predicate;BOOL pv=FALSE;ctx->GetPredication(&predicate,&pv);
    WitnessPtr<ID3D11HullShader> hs;WitnessPtr<ID3D11DomainShader> ds;WitnessPtr<ID3D11GeometryShader> gs;
    ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);ctx->GSGetShader(&gs,nullptr,nullptr);
    if(predicate||hs||ds||gs)return refuse();
    bool busy=false;ID3D11Buffer* so[4]{};ctx->SOGetTargets(4,so);
    for(auto* buffer:so)if(buffer){buffer->Release();busy=true;}
    ID3D11UnorderedAccessView* uavs[D3D11_1_UAV_SLOT_COUNT]{};
    const UINT limit=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?D3D11_1_UAV_SLOT_COUNT:D3D11_PS_CS_UAV_REGISTER_COUNT;
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,limit,uavs);
    for(auto* uav:uavs)if(uav){uav->Release();busy=true;}
    if(busy)return refuse();
    ID3D11RenderTargetView* rt[8]{};WitnessPtr<ID3D11DepthStencilView> actualDepth;
    WitnessPtr<ID3D11RenderTargetView> held[8];ctx->OMGetRenderTargets(8,rt,&actualDepth);
    for(unsigned i=0;i<8;++i)held[i].Attach(rt[i]);
    if(rt[0]!=color||actualDepth.Get()!=depth||rt[6]!=slots||rt[7]!=skin)return refuse();
    bool fresh=mirror.source.Get()!=depth;
    WitnessPtr<ID3D11Resource> source;depth->GetResource(&source);
    if(fresh){
        WitnessPtr<ID3D11Texture2D> texture;if(FAILED(source.As(&texture)))return refuse();
        D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};depth->GetDesc(&vd);
        if(vd.Flags||vd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||vd.Texture2D.MipSlice||
           td.MipLevels!=1||td.ArraySize!=1||td.SampleDesc.Count!=1||td.Usage!=D3D11_USAGE_DEFAULT||
           td.CPUAccessFlags||td.MiscFlags||(td.BindFlags & ~(D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE)))return refuse();
        WitnessMirror next;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        if(FAILED(device->CreateTexture2D(&td,nullptr,&next.texture))||
           FAILED(device->CreateDepthStencilView(next.texture.Get(),&vd,&next.view)))return refuse();
        next.source=depth;mirror=std::move(next);
    }
    if(!api.full()) {++counts.rateRefused;return false;}
    ID3D11RenderTargetView* outputs[8]{};outputs[6]=rt[6];outputs[7]=rt[7];
    if(!mirror.reusable||!reuseAllowed||mirror.epoch!=epoch||mirror.draw+1!=drawNumber){
        api.targets(0,nullptr,nullptr);
        api.copy(mirror.texture.Get(),source.Get());++counts.copies;
    }else ++counts.reused;
    api.targets(8,outputs,mirror.view.Get());
    struct Restore {
        Api& api;ID3D11RenderTargetView** rt;ID3D11DepthStencilView* depth;ID3D11PixelShader* ps;
        ~Restore(){api.targets(8,rt,depth);api.pixel(ps);}
    } restore{api,rt,depth,combined};
    api.pixel(fine);draw(false);
    api.targets(6,rt,depth);api.pixel(material);
    if(!coarse()){
        // A producer or driver can refuse between the two issues. Reinstate
        // the combined full-rate draw against the untouched original depth.
        // MRT6 and E use replacement writes, so this re-certifies visible pixels.
        ++counts.rateRefused;mirror.reusable=false;
        api.targets(8,rt,depth);api.pixel(combined);draw(true);return true;
    }
    draw(true);++counts.paired;mirror.epoch=epoch;mirror.draw=drawNumber;mirror.reusable=true;return true;
}
}

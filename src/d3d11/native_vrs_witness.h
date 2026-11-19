#pragma once
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <utility>
#include "native_vrs_witness_sampling.h"

namespace edvr::native_vrs {
template<class T> using WitnessPtr=Microsoft::WRL::ComPtr<T>;
struct WitnessMirror {
    WitnessPtr<ID3D11DepthStencilView> source,view;
    WitnessPtr<ID3D11Texture2D> texture;
    uint64_t epoch=0,draw=0;
    bool reusable=false;
    D3D11_TEXTURE2D_DESC textureDesc{};
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN; // actual single-sample DSV format
};
// Non-null only for the exact bound view whose OM state preserves every depth
// and stencil sample. EQUAL with enabled depth writes still needs the mirror.
struct WitnessDepthAccess {
    ID3D11DepthStencilView* original=nullptr;
    WitnessDepthAccess()=default;
    WitnessDepthAccess(ID3D11DepthStencilView* depth,ID3D11DepthStencilState* state) {
        if(!depth || !state)return; // null state uses the depth-writable D3D11 default
        D3D11_DEPTH_STENCIL_DESC ds{};state->GetDesc(&ds);
        if(ds.DepthEnable && ds.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO)return;
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};depth->GetDesc(&vd);
        const bool stencil=vd.Format==DXGI_FORMAT_D24_UNORM_S8_UINT || vd.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        if(vd.Flags || (!stencil && vd.Format!=DXGI_FORMAT_D16_UNORM && vd.Format!=DXGI_FORMAT_D32_FLOAT))return;
        auto writes=[](const D3D11_DEPTH_STENCILOP_DESC& face){
            return face.StencilFailOp!=D3D11_STENCIL_OP_KEEP || face.StencilDepthFailOp!=D3D11_STENCIL_OP_KEEP ||
                   face.StencilPassOp!=D3D11_STENCIL_OP_KEEP;
        };
        if(witnessDepthOnlyReads(ds.DepthEnable!=FALSE,ds.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO,false,
            ds.StencilEnable!=FALSE,ds.StencilWriteMask!=0,writes(ds.FrontFace)||writes(ds.BackFace),false,stencil))original=depth;
    }
    static WitnessDepthAccess bound(ID3D11DeviceContext* ctx,ID3D11DepthStencilView* depth) {
        WitnessPtr<ID3D11DepthStencilState> state;
        if(ctx)ctx->OMGetDepthStencilState(&state,nullptr);
        return {depth,state.Get()};
    }
};
enum class WitnessPhase : unsigned { Prepare, Copy, Fine, CoarseBind, Material, Count };
struct WitnessObserver {
    virtual void validated(const WitnessMirror&,bool copied,bool readOnly) noexcept=0;
    virtual void stage(WitnessPhase) noexcept=0;
};
inline bool witnessNeedsCopy(const WitnessMirror& m,ID3D11DepthStencilView* d,uint64_t epoch,uint64_t draw,bool reuse) noexcept {
    return m.source.Get()!=d || !m.reusable || !reuse || m.epoch!=epoch || m.draw+1!=draw;
}
struct WitnessCounts {uint64_t paired=0,copies=0,reused=0,stateRefused=0,shaderRefused=0,queryRefused=0,rateRefused=0,readOnlyOriginalRuns=0,readOnlyOriginalDraws=0;};

// Replay ordered fine witnesses against the original read-only or private DSV before committing
// any scene material. Every draw in this synchronous span shares its exact
// inputs and state. A refused coarse issue switches the entire remaining
// suffix to combined full rate against the correctly evolved scene DSV.
template<class Api,class Draw,class Coarse>
bool splitWitnessRun(Api& api,ID3D11DeviceContext* ctx,ID3D11RenderTargetView* color,ID3D11DepthStencilView* depth,
    ID3D11RenderTargetView* slots,ID3D11RenderTargetView* skin,ID3D11VertexShader* vertex,
    ID3D11PixelShader* combined,ID3D11PixelShader* fine,ID3D11PixelShader* material,ID3D11BlendState* blend,
    const WitnessDepthAccess& access,WitnessMirror& mirror,uint64_t epoch,uint64_t drawNumber,bool reuseAllowed,WitnessCounts& counts,unsigned length,Draw draw,Coarse coarse,WitnessObserver* observer=nullptr) {
    auto refuse=[&]{++counts.stateRefused;return false;};
    if(!length||!ctx||!color||!depth||!slots||!vertex||!combined||!fine||!material||!blend)return refuse();
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
    const bool readOnly=access.original==depth;
    if(mirror.source.Get()!=depth){
        WitnessPtr<ID3D11Resource> source;depth->GetResource(&source);
        WitnessPtr<ID3D11Texture2D> texture;if(FAILED(source.As(&texture)))return refuse();
        D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};depth->GetDesc(&vd);
        if(vd.Flags||vd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||vd.Texture2D.MipSlice||
           td.MipLevels!=1||td.ArraySize!=1||td.SampleDesc.Count!=1||td.Usage!=D3D11_USAGE_DEFAULT||
           td.CPUAccessFlags||td.MiscFlags||(td.BindFlags & ~(D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE)))return refuse();
        WitnessMirror next;
        next.textureDesc=td;next.textureDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        next.source=depth;next.format=vd.Format;mirror=std::move(next);
    }
    if(!readOnly && !mirror.view){
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=mirror.format;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        WitnessPtr<ID3D11Texture2D> texture;
        if(FAILED(device->CreateTexture2D(&mirror.textureDesc,nullptr,&texture)) ||
           FAILED(device->CreateDepthStencilView(texture.Get(),&vd,&mirror.view)))return refuse();
        mirror.texture=std::move(texture);
    }
    if(!api.full()) {++counts.rateRefused;return false;}
    ID3D11RenderTargetView* outputs[8]{};outputs[6]=rt[6];outputs[7]=rt[7];
    const bool matched=!witnessNeedsCopy(mirror,depth,epoch,drawNumber,reuseAllowed);
    const bool copied=!readOnly && !matched;
    if(observer)observer->validated(mirror,copied,readOnly);
    if(copied){
        if(observer)observer->stage(WitnessPhase::Copy);
        api.targets(0,nullptr,nullptr);
        WitnessPtr<ID3D11Resource> source;depth->GetResource(&source);
        api.copy(mirror.texture.Get(),source.Get());++counts.copies;
    }
    if(readOnly){++counts.readOnlyOriginalRuns;counts.readOnlyOriginalDraws+=length;}
    else counts.reused+=length-unsigned(copied);
    if(observer)observer->stage(WitnessPhase::Fine);
    api.targets(8,outputs,readOnly?depth:mirror.view.Get());
    struct Restore {
        Api& api;ID3D11RenderTargetView** rt;ID3D11DepthStencilView* depth;ID3D11PixelShader* ps;
        ~Restore(){api.targets(8,rt,depth);api.pixel(ps);}
    } restore{api,rt,depth,combined};
    api.pixel(fine);
    for(unsigned i=0;i<length;++i)draw(i,false);
    if(observer)observer->stage(WitnessPhase::CoarseBind);
    api.targets(6,rt,depth);api.pixel(material);
    for(unsigned i=0;i<length;++i){
        if(!coarse(i)){
            counts.rateRefused+=length-i;mirror.reusable=false;
            api.targets(8,rt,depth);api.pixel(combined);
            if(observer && !i)observer->stage(WitnessPhase::Material);
            for(;i<length;++i)draw(i,true);
            return true;
        }
        if(observer && !i)observer->stage(WitnessPhase::Material);
        draw(i,true);++counts.paired;
    }
    // A read-only run can extend an existing identical mirror, never establish one.
    mirror.reusable=!readOnly || matched;
    if(mirror.reusable){mirror.epoch=epoch;mirror.draw=drawNumber+length-1;}
    return true;
}
template<class Api,class Draw,class Coarse>
bool splitWitness(Api& api,ID3D11DeviceContext* ctx,ID3D11RenderTargetView* color,ID3D11DepthStencilView* depth,
    ID3D11RenderTargetView* slots,ID3D11RenderTargetView* skin,ID3D11VertexShader* vertex,
    ID3D11PixelShader* combined,ID3D11PixelShader* fine,ID3D11PixelShader* material,ID3D11BlendState* blend,
    WitnessMirror& mirror,uint64_t epoch,uint64_t drawNumber,bool reuseAllowed,WitnessCounts& counts,Draw draw,Coarse coarse,WitnessObserver* observer=nullptr,const WitnessDepthAccess* knownAccess=nullptr) {
    const auto access=knownAccess?*knownAccess:WitnessDepthAccess::bound(ctx,depth);
    return splitWitnessRun(api,ctx,color,depth,slots,skin,vertex,combined,fine,material,blend,access,mirror,epoch,drawNumber,
        reuseAllowed,counts,1,[&](unsigned,bool m){draw(m);},[&](unsigned){return coarse();},observer);
}
}

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
    WitnessPtr<IUnknown> sourceIdentity,privateIdentity,deviceIdentity;
    uint64_t epoch=0,draw=0;
    bool reusable=false;
    D3D11_TEXTURE2D_DESC textureDesc{};
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN; // actual single-sample DSV format
};
enum class WitnessWrite { Unknown, Matching, Unrelated };
// Called after draining on the verified native immediate-context owner. A
// different interface pointer cannot distinguish aliases. Even distinct COM
// identities need a private texture on the same device with an immutable
// descriptor incompatible with both depth allocations.
inline WitnessWrite witnessWriteRelation(const WitnessMirror& mirror,ID3D11Resource* dst) noexcept {
    if(!dst || !mirror.sourceIdentity || !mirror.privateIdentity || !mirror.deviceIdentity)return WitnessWrite::Unknown;
    WitnessPtr<ID3D11Resource> resource=dst;
    WitnessPtr<IUnknown> id;
    if(FAILED(resource.As(&id)) || !id)return WitnessWrite::Unknown;
    if(id.Get()==mirror.sourceIdentity.Get() || id.Get()==mirror.privateIdentity.Get())return WitnessWrite::Matching;
    WitnessPtr<ID3D11Texture2D> texture;
    if(FAILED(resource.As(&texture)) || !texture)return WitnessWrite::Unknown;
    D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
    if(td.MiscFlags || ((td.BindFlags&D3D11_BIND_DEPTH_STENCIL) && td.Width==mirror.textureDesc.Width &&
       td.Height==mirror.textureDesc.Height && td.Format==mirror.textureDesc.Format))return WitnessWrite::Unknown;
    WitnessPtr<ID3D11Device> device;texture->GetDevice(&device);
    WitnessPtr<IUnknown> owner;
    if(!device || FAILED(device.As(&owner)) || owner.Get()!=mirror.deviceIdentity.Get())return WitnessWrite::Unknown;
    return WitnessWrite::Unrelated;
}
// D3D11.3 Functional Specification §§17.10–13 (depth clamp, comparison and DSV hazards).
// Both routes bind the exact observed view. With fixed-function EQUAL and
// nonwriting stencil, successive accepted fragments stay in the same depth
// comparison class. The ordered material pass writes the final depth bits;
// float equality includes signed zeros, so a private mirror cannot follow it.
struct WitnessDepthAccess {
    ID3D11DepthStencilView* original=nullptr; // truly read-only
    ID3D11DepthStencilView* equalWritable=nullptr; // requires qualified native owner at issue
    // D3D11_COMPARISON_FUNC occupies 1..8. Keep null/default (LESS) distinct
    // from an explicit state, and disabled depth distinct from its unused func.
    static constexpr unsigned unknown=0,defaultState=9,depthDisabled=10,comparisonCount=11;
    unsigned comparison=unknown;
    bool stencilMayWrite=true; // conservative if descriptor unavailable
    WitnessDepthAccess()=default;
    WitnessDepthAccess(ID3D11DepthStencilView* depth,ID3D11DepthStencilState* state) {
        if(!depth)return;
        if(!state){comparison=defaultState;stencilMayWrite=false;return;}
        D3D11_DEPTH_STENCIL_DESC ds{};state->GetDesc(&ds);
        const auto func=static_cast<unsigned>(ds.DepthFunc);
        comparison=!ds.DepthEnable?depthDisabled:(func>=1 && func<=8?func:unknown);
        auto writes=[](const D3D11_DEPTH_STENCILOP_DESC& face){
            return face.StencilFailOp!=D3D11_STENCIL_OP_KEEP || face.StencilDepthFailOp!=D3D11_STENCIL_OP_KEEP ||
                   face.StencilPassOp!=D3D11_STENCIL_OP_KEEP;
        };
        stencilMayWrite=ds.StencilEnable && ds.StencilWriteMask && (writes(ds.FrontFace)||writes(ds.BackFace));
        const bool depthWrites=ds.DepthEnable && ds.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO;
        if(depthWrites && (ds.DepthFunc!=D3D11_COMPARISON_EQUAL || ds.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL))return;
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};depth->GetDesc(&vd);
        const bool stencil=vd.Format==DXGI_FORMAT_D24_UNORM_S8_UINT || vd.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        if(vd.Flags || (!stencil && vd.Format!=DXGI_FORMAT_D16_UNORM && vd.Format!=DXGI_FORMAT_D32_FLOAT))return;
        if(witnessDepthOnlyReads(ds.DepthEnable!=FALSE,depthWrites,false,ds.StencilEnable!=FALSE,
            ds.StencilWriteMask!=0,stencilMayWrite,false,stencil))original=depth;
        else if(depthWrites && vd.Format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT && !stencilMayWrite)equalWritable=depth;
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
struct WitnessCounts {
    uint64_t paired=0,copies=0,reused=0,stateRefused=0,shaderRefused=0,queryRefused=0,rateRefused=0;
    uint64_t readOnlyOriginalRuns=0,readOnlyOriginalDraws=0,equalEligible=0,equalFine=0,equalPaired=0;
    uint64_t copyCauses[WitnessDepthAccess::comparisonCount][2]{};
};

// Replay ordered fine witnesses against the selected original or private DSV before committing
// any scene material. Every draw in this synchronous span shares its exact
// inputs and state. A refused coarse issue switches the entire remaining
// suffix to combined full rate against the correctly evolved scene DSV.
// qualifiedEqual requires the published cohort mutex and all consumer barriers
// through fine draws, ordered materials and binding restoration. An intervening
// copy could observe changed signed-zero bits. Synchronous splits leave it false.
template<class Api,class Draw,class Coarse>
bool splitWitnessRun(Api& api,ID3D11DeviceContext* ctx,ID3D11RenderTargetView* color,ID3D11DepthStencilView* depth,
    ID3D11RenderTargetView* slots,ID3D11RenderTargetView* skin,ID3D11VertexShader* vertex,
    ID3D11PixelShader* combined,ID3D11PixelShader* fine,ID3D11PixelShader* material,ID3D11BlendState* blend,
    const WitnessDepthAccess& access,WitnessMirror& mirror,uint64_t epoch,uint64_t drawNumber,bool reuseAllowed,WitnessCounts& counts,unsigned length,Draw draw,Coarse coarse,WitnessObserver* observer=nullptr,bool qualifiedEqual=false) {
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
    const bool equalWritable=qualifiedEqual && access.equalWritable==depth;
    const bool original=readOnly || equalWritable;
    if(mirror.source.Get()!=depth){
        WitnessPtr<ID3D11Resource> source;depth->GetResource(&source);
        WitnessPtr<ID3D11Texture2D> texture;if(FAILED(source.As(&texture)))return refuse();
        D3D11_TEXTURE2D_DESC td{};texture->GetDesc(&td);
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};depth->GetDesc(&vd);
        if(vd.Flags||vd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D||vd.Texture2D.MipSlice||
           td.MipLevels!=1||td.ArraySize!=1||td.SampleDesc.Count!=1||td.Usage!=D3D11_USAGE_DEFAULT||
           td.CPUAccessFlags||td.MiscFlags||(td.BindFlags & ~(D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE)))return refuse();
        WitnessMirror next;
        source.As(&next.sourceIdentity);device.As(&next.deviceIdentity);
        next.textureDesc=td;next.textureDesc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        next.source=depth;next.format=vd.Format;mirror=std::move(next);
    }
    if(equalWritable){counts.equalEligible+=length;mirror.reusable=false;}
    if(!original && !mirror.view){
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=mirror.format;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
        WitnessPtr<ID3D11Texture2D> texture;
        if(FAILED(device->CreateTexture2D(&mirror.textureDesc,nullptr,&texture)) ||
           FAILED(device->CreateDepthStencilView(texture.Get(),&vd,&mirror.view)))return refuse();
        texture.As(&mirror.privateIdentity);mirror.texture=std::move(texture);
    }
    if(!api.full()) {++counts.rateRefused;return false;}
    ID3D11RenderTargetView* outputs[8]{};outputs[6]=rt[6];outputs[7]=rt[7];
    const bool matched=!witnessNeedsCopy(mirror,depth,epoch,drawNumber,reuseAllowed);
    const bool copied=!original && !matched;
    if(observer)observer->validated(mirror,copied,readOnly);
    if(copied){
        if(observer)observer->stage(WitnessPhase::Copy);
        api.targets(0,nullptr,nullptr);
        WitnessPtr<ID3D11Resource> source;depth->GetResource(&source);
        api.copy(mirror.texture.Get(),source.Get());++counts.copies;
        const bool stencil=mirror.format==DXGI_FORMAT_D24_UNORM_S8_UINT || mirror.format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        ++counts.copyCauses[access.comparison][stencil && access.stencilMayWrite];
    }
    if(readOnly){++counts.readOnlyOriginalRuns;counts.readOnlyOriginalDraws+=length;}
    else if(equalWritable)counts.equalFine+=length;
    else counts.reused+=length-unsigned(copied);
    if(observer)observer->stage(WitnessPhase::Fine);
    api.targets(8,outputs,original?depth:mirror.view.Get());
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
            if(equalWritable)counts.equalPaired+=i;
            counts.rateRefused+=length-i;mirror.reusable=false;
            api.targets(8,rt,depth);api.pixel(combined);
            if(observer && !i)observer->stage(WitnessPhase::Material);
            for(;i<length;++i)draw(i,true);
            return true;
        }
        if(observer && !i)observer->stage(WitnessPhase::Material);
        draw(i,true);++counts.paired;
    }
    if(equalWritable)counts.equalPaired+=length;
    // A read-only run can extend an existing identical mirror, never establish one.
    mirror.reusable=!equalWritable && (!readOnly || matched);
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

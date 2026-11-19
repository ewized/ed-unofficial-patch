#pragma once
#include "native_vrs_witness.h"
#include "native_vrs_replay.h"
#include <array>
#include <iterator>

namespace edvr::native_vrs {
// Getter arrays own the AddRef returned for each occupied slot. Keep all
// input identities and ranges alive until the pre-mutation barrier drains
// this run. An unobserved writer cannot be made safe by holding a reference.
template<class T,UINT N> struct WitnessBindings {
    T* slots[N]{};
    WitnessBindings()=default;
    WitnessBindings(const WitnessBindings&)=delete;
    ~WitnessBindings(){for(auto* p:slots)if(p)p->Release();}
};
using WitnessMappedBuffer=bool (*)(ID3D11Resource*) noexcept;
template<class Object,class Desc> bool witnessResourceFlags(ID3D11Resource* resource,UINT& flags,bool texture,WitnessMappedBuffer mapped=nullptr){
    WitnessPtr<Object> object;
    if(FAILED(resource->QueryInterface(IID_PPV_ARGS(object.GetAddressOf()))))return false;
    Desc desc{};object->GetDesc(&desc);flags=desc.MiscFlags;
    // Texture memory can also be written through IDXGISurface::Map, outside
    // the observed immediate-context Map/Unmap transaction.
    if(texture)return !desc.CPUAccessFlags;
    return !(mapped && (desc.CPUAccessFlags&D3D11_CPU_ACCESS_WRITE) && mapped(resource));
}
inline bool witnessLocalResource(ID3D11Resource* resource,WitnessMappedBuffer mapped=nullptr) {
    if(!resource)return true;
    D3D11_RESOURCE_DIMENSION dim{};resource->GetType(&dim);
    UINT flags=0;
    switch(dim){
    case D3D11_RESOURCE_DIMENSION_BUFFER:if(!witnessResourceFlags<ID3D11Buffer,D3D11_BUFFER_DESC>(resource,flags,false,mapped))return false;break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:if(!witnessResourceFlags<ID3D11Texture1D,D3D11_TEXTURE1D_DESC>(resource,flags,true))return false;break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:if(!witnessResourceFlags<ID3D11Texture2D,D3D11_TEXTURE2D_DESC>(resource,flags,true))return false;break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:if(!witnessResourceFlags<ID3D11Texture3D,D3D11_TEXTURE3D_DESC>(resource,flags,true))return false;break;
    default:return false;
    }
    constexpr UINT ordinary=D3D11_RESOURCE_MISC_GENERATE_MIPS|D3D11_RESOURCE_MISC_TEXTURECUBE|
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS|D3D11_RESOURCE_MISC_BUFFER_STRUCTURED|D3D11_RESOURCE_MISC_RESOURCE_CLAMP;
    return !(flags&~ordinary);
}
inline bool witnessLocalView(ID3D11View* view,WitnessMappedBuffer mapped=nullptr){
    if(!view)return true;
    WitnessPtr<ID3D11Resource> resource;view->GetResource(&resource);
    return resource && witnessLocalResource(resource.Get(),mapped);
}
struct WitnessStageInputs {
    static constexpr UINT buffers=D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
    static constexpr UINT resources=D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
    static constexpr UINT samplers=D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
    WitnessBindings<ID3D11Buffer,buffers> cb;
    UINT first[buffers]{},count[buffers]{};
    WitnessBindings<ID3D11ShaderResourceView,resources> srv;
    WitnessBindings<ID3D11SamplerState,samplers> sampler;
    bool local(WitnessMappedBuffer mapped)const {
        for(auto* b:cb.slots)if(!witnessLocalResource(b,mapped))return false;
        for(auto* v:srv.slots)if(!witnessLocalView(v,mapped))return false;
        return true;
    }
};
struct WitnessPipeline {
    WitnessPtr<ID3D11DeviceContext> context;
    WitnessPtr<ID3D11InputLayout> layout;
    WitnessBindings<ID3D11Buffer,D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertices;
    UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{},offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
    WitnessPtr<ID3D11Buffer> index;
    UINT indexOffset=0;DXGI_FORMAT indexFormat{};D3D11_PRIMITIVE_TOPOLOGY topology{};
    WitnessStageInputs vsInputs,psInputs;
    WitnessPtr<ID3D11VertexShader> vertex;
    WitnessPtr<ID3D11PixelShader> combined,material,fine;
    WitnessPtr<ID3D11RasterizerState> raster;
    D3D11_VIEWPORT viewport[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewportCount=UINT(std::size(viewport)),scissorCount=UINT(std::size(scissors));
    WitnessPtr<ID3D11DepthStencilState> depthState;
    UINT stencil=0,sampleMask=0;FLOAT factors[4]{};
    WitnessPtr<ID3D11BlendState> blend;
    WitnessBindings<ID3D11RenderTargetView,8> rt;
    WitnessPtr<ID3D11DepthStencilView> depth;

    // Capture once per cohort. The owner's complete, pre-change generations
    // compare this snapshot on subsequent appends. A pending cohort leaves
    // this physical pipeline untouched until its synchronous replay.
    bool capture(ID3D11DeviceContext* ctx,ID3D11RenderTargetView* color,ID3D11DepthStencilView* dsv,
        ID3D11RenderTargetView* marker,ID3D11RenderTargetView* skin,ID3D11VertexShader* vs,ID3D11PixelShader* ps,
        ID3D11PixelShader* finePs,ID3D11PixelShader* materialPs,ID3D11BlendState* blendState,WitnessMappedBuffer mapped=nullptr){
        if(!ctx||!color||!dsv||!marker||!vs||!ps||!finePs||!materialPs||!blendState)return false;
        context=ctx;WitnessPtr<ID3D11DeviceContext1> ranged;
        if(FAILED(context.As(&ranged)) || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
        UINT vc=0,pc=0;ctx->VSGetShader(&vertex,nullptr,&vc);ctx->PSGetShader(&combined,nullptr,&pc);
        WitnessPtr<ID3D11HullShader> hs;WitnessPtr<ID3D11DomainShader> ds;WitnessPtr<ID3D11GeometryShader> gs;
        ctx->HSGetShader(&hs,nullptr,nullptr);ctx->DSGetShader(&ds,nullptr,nullptr);ctx->GSGetShader(&gs,nullptr,nullptr);
        WitnessPtr<ID3D11Predicate> predicate;BOOL predicateValue=FALSE;ctx->GetPredication(&predicate,&predicateValue);
        if(vc||pc||vertex.Get()!=vs||combined.Get()!=ps||hs||ds||gs||predicate)return false;
        WitnessBindings<ID3D11Buffer,4> so;ctx->SOGetTargets(4,so.slots);
        for(auto* b:so.slots)if(b)return false;
        WitnessPtr<ID3D11Device> device;ctx->GetDevice(&device);
        const UINT limit=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?D3D11_1_UAV_SLOT_COUNT:D3D11_PS_CS_UAV_REGISTER_COUNT;
        WitnessBindings<ID3D11UnorderedAccessView,D3D11_1_UAV_SLOT_COUNT> uav;
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,limit,uav.slots);
        for(auto* v:uav.slots)if(v)return false;
        ctx->OMGetRenderTargets(8,rt.slots,&depth);
        ctx->OMGetBlendState(&blend,factors,&sampleMask);ctx->OMGetDepthStencilState(&depthState,&stencil);
        if(depth.Get()!=dsv||rt.slots[0]!=color||rt.slots[6]!=marker||rt.slots[7]!=skin||blend.Get()!=blendState)return false;
        for(auto* v:rt.slots)if(!witnessLocalView(v,mapped))return false;
        if(!witnessLocalView(depth.Get(),mapped))return false;
        ctx->IAGetInputLayout(&layout);ctx->IAGetPrimitiveTopology(&topology);
        ctx->IAGetVertexBuffers(0,UINT(std::size(vertices.slots)),vertices.slots,strides,offsets);
        ctx->IAGetIndexBuffer(&index,&indexFormat,&indexOffset);
        for(auto* b:vertices.slots)if(!witnessLocalResource(b,mapped))return false;
        if(!witnessLocalResource(index.Get(),mapped))return false;
        ctx->RSGetState(&raster);ctx->RSGetViewports(&viewportCount,viewport);ctx->RSGetScissorRects(&scissorCount,scissors);
        ranged->VSGetConstantBuffers1(0,vsInputs.buffers,vsInputs.cb.slots,vsInputs.first,vsInputs.count);
        ranged->PSGetConstantBuffers1(0,psInputs.buffers,psInputs.cb.slots,psInputs.first,psInputs.count);
        ctx->VSGetShaderResources(0,vsInputs.resources,vsInputs.srv.slots);ctx->PSGetShaderResources(0,psInputs.resources,psInputs.srv.slots);
        ctx->VSGetSamplers(0,vsInputs.samplers,vsInputs.sampler.slots);ctx->PSGetSamplers(0,psInputs.samplers,psInputs.sampler.slots);
        fine=finePs;material=materialPs;
        return vsInputs.local(mapped) && psInputs.local(mapped);
    }
};

// Once the run has been accepted, every material is owed exactly once.
// A preflight refusal leaves all original attachments intact. Both this
// combined fallback and a partial suffix require an acknowledged full rate
// or explicit invalidation of the affected eye/frame by the caller.
// A serialized immediate-context handoff can issue the original combined
// draws on its current thread. Admission established disabled VRS and a null
// rate map before the first append. No native state call belongs here.
template<class Replay>
bool replayPrearmedOriginals(WitnessCohort& queue,ID3D11DeviceContext* ctx,bool strongFull,Replay replay){
    if(queue.empty())return true;
    if(!strongFull || !ctx || queue.key().context!=ctx)return false;
    for(unsigned i=0;i<queue.size();++i)replay(queue.data()[i]);
    queue.retire();return true;
}

template<class Api,class Issue,class Coarse,class Strong>
bool replayWitnessCohort(const WitnessCohort& queue,WitnessPipeline& s,WitnessMirror& mirror,uint64_t epoch,
    uint64_t first,WitnessCounts& counts,Api& api,Issue issue,Coarse coarse,Strong strong){
    if(queue.empty())return false;
    auto draw=[&](unsigned i,bool m){issue(queue.data()[i],m);};
    const bool replayed=splitWitnessRun(api,s.context.Get(),s.rt.slots[0],s.depth.Get(),s.rt.slots[6],s.rt.slots[7],
        s.vertex.Get(),s.combined.Get(),s.fine.Get(),s.material.Get(),s.blend.Get(),mirror,epoch,first,true,
        counts,queue.size(),draw,[&](unsigned i){if(coarse(i))return true;strong();return false;});
    if(!replayed){
        strong();
        for(unsigned i=0;i<queue.size();++i)draw(i,true);
        mirror.reusable=false;
    }
    return replayed;
}
} // namespace edvr::native_vrs

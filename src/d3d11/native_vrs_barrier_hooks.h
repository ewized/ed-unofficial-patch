// Typed forwarding hooks for state changes without another vscreen observer.
// Slot numbers follow the Windows SDK ID3D11DeviceContext[1-4] COM layouts.
// The installed next pointer may itself be another observer; always forward.
#pragma once
#include <d3d11_4.h>
#include <d3d11on12.h>
#include <dxgi1_5.h>
#include <type_traits>

namespace edvr::native_vrs {
template<unsigned Slot, auto Method, auto Barrier> struct CohortBarrierHook;

template<unsigned Slot, class I, class R, class... A, R (STDMETHODCALLTYPE I::*Method)(A...), auto Barrier>
struct CohortBarrierHook<Slot, Method, Barrier> {
    using Next = R (STDMETHODCALLTYPE*)(I*, A...);
    inline static Next next=nullptr;
    static R STDMETHODCALLTYPE call(I* self,A... args) {
        if(!Barrier(self)){
            if constexpr(std::is_void_v<R>)return;
            else {static_assert(std::is_same_v<R,HRESULT>);return DXGI_ERROR_INVALID_CALL;}
        }
        return next(self,args...);
    }
    template<class Hook> static bool install(const Hook& hook) {
        return hook(Slot,reinterpret_cast<void*>(&call),reinterpret_cast<void**>(&next));
    }
};

// Existing observers keep their own hooks; this list only installs the rest.
// An interface tear-off has its own vtable: never certify it by inspecting
// the owner's vtable length. Unsupported interfaces need no hooks.
template<class I> bool sameCohortContext(ID3D11DeviceContext* ctx,bool& available) {
    I* extended=nullptr;
    const HRESULT hr=ctx->QueryInterface(IID_PPV_ARGS(&extended));
    available=hr==S_OK && extended;
    const bool absent=hr==E_NOINTERFACE && !extended;
    const bool same=available && static_cast<ID3D11DeviceContext*>(extended)==ctx;
    if(extended)extended->Release();
    return absent || same;
}

// GetImmediateContext[1-3] and QueryInterface both expose callable pointers.
// A COM identity match alone is insufficient for an object-vtable hook.
template<class D,class I,auto Get> bool sameImmediateContext(ID3D11Device* device,ID3D11DeviceContext* ctx) {
    D* extended=nullptr;
    const HRESULT hr=device->QueryInterface(IID_PPV_ARGS(&extended));
    if(hr==E_NOINTERFACE && !extended)return true;
    if(hr!=S_OK || !extended){if(extended)extended->Release();return false;}
    I* immediate=nullptr;
    (extended->*Get)(&immediate);
    const bool same=immediate && static_cast<ID3D11DeviceContext*>(immediate)==ctx;
    if(immediate)immediate->Release();
    extended->Release();
    return same;
}

inline bool nativeImmediateAliases(ID3D11Device* device,ID3D11DeviceContext* ctx) {
    if(!ctx || !device || ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    // A wrapped D3D12 resource can be written on another queue without calling
    // a D3D11 context slot. The D3D11-only resource descriptor cannot prove it local.
    ID3D11On12Device* on12=nullptr;
    const HRESULT on12Result=device->QueryInterface(IID_PPV_ARGS(&on12));
    const bool nativeOnly=on12Result==E_NOINTERFACE && !on12;
    if(on12)on12->Release();
    if(!nativeOnly)return false;
    bool have=false;
    return sameCohortContext<ID3D11DeviceContext>(ctx,have) && have &&
        sameCohortContext<ID3D11DeviceContext1>(ctx,have) && have &&
        sameCohortContext<ID3D11DeviceContext2>(ctx,have) &&
        sameCohortContext<ID3D11DeviceContext3>(ctx,have) &&
        sameCohortContext<ID3D11DeviceContext4>(ctx,have) &&
        sameImmediateContext<ID3D11Device,ID3D11DeviceContext,&ID3D11Device::GetImmediateContext>(device,ctx) &&
        sameImmediateContext<ID3D11Device1,ID3D11DeviceContext1,&ID3D11Device1::GetImmediateContext1>(device,ctx) &&
        sameImmediateContext<ID3D11Device2,ID3D11DeviceContext2,&ID3D11Device2::GetImmediateContext2>(device,ctx) &&
        sameImmediateContext<ID3D11Device3,ID3D11DeviceContext3,&ID3D11Device3::GetImmediateContext3>(device,ctx);
}

// Every supported derived DXGI device can call the inherited OfferResources.
// A separate tear-off would need a separate hook and remains ineligible.
template<class I> bool sameDxgiOfferAlias(ID3D11Device* device,IDXGIDevice2* base,bool& available) {
    I* extended=nullptr;
    const HRESULT hr=device->QueryInterface(IID_PPV_ARGS(&extended));
    available=hr==S_OK && extended;
    const bool ok=(hr==E_NOINTERFACE && !extended) ||
        (available && static_cast<IDXGIDevice2*>(extended)==base);
    if(extended)extended->Release();
    return ok;
}

template<auto Pipeline, auto Input, auto QueryRead, class Hook>
bool installCohortBarrierHooks(ID3D11DeviceContext* ctx,const Hook& hook) {
    bool ok=true;
#define EDVR_COHORT_HOOK(i,slot,method,barrier) ok=CohortBarrierHook<slot,&i::method,barrier>::install(hook) && ok
    using C=ID3D11DeviceContext;
    EDVR_COHORT_HOOK(C,10,PSSetSamplers,Pipeline);
    EDVR_COHORT_HOOK(C,17,IASetInputLayout,Pipeline);
    EDVR_COHORT_HOOK(C,18,IASetVertexBuffers,Pipeline);
    EDVR_COHORT_HOOK(C,19,IASetIndexBuffer,Pipeline);
    EDVR_COHORT_HOOK(C,22,GSSetConstantBuffers,Pipeline);
    EDVR_COHORT_HOOK(C,24,IASetPrimitiveTopology,Pipeline);
    EDVR_COHORT_HOOK(C,26,VSSetSamplers,Pipeline);
    EDVR_COHORT_HOOK(C,29,GetData,QueryRead);
    EDVR_COHORT_HOOK(C,31,GSSetShaderResources,Pipeline);
    EDVR_COHORT_HOOK(C,32,GSSetSamplers,Pipeline);
    EDVR_COHORT_HOOK(C,37,SOSetTargets,Pipeline);
    EDVR_COHORT_HOOK(C,41,Dispatch,Input);
    EDVR_COHORT_HOOK(C,42,DispatchIndirect,Input);
    EDVR_COHORT_HOOK(C,43,RSSetState,Pipeline);
    EDVR_COHORT_HOOK(C,45,RSSetScissorRects,Pipeline);
    EDVR_COHORT_HOOK(C,55,SetResourceMinLOD,Input);
    EDVR_COHORT_HOOK(C,59,HSSetShaderResources,Pipeline);
    EDVR_COHORT_HOOK(C,61,HSSetSamplers,Pipeline);
    EDVR_COHORT_HOOK(C,62,HSSetConstantBuffers,Pipeline);
    EDVR_COHORT_HOOK(C,63,DSSetShaderResources,Pipeline);
    EDVR_COHORT_HOOK(C,65,DSSetSamplers,Pipeline);
    EDVR_COHORT_HOOK(C,66,DSSetConstantBuffers,Pipeline);
    EDVR_COHORT_HOOK(C,67,CSSetShaderResources,Pipeline);
    EDVR_COHORT_HOOK(C,68,CSSetUnorderedAccessViews,Pipeline);
    EDVR_COHORT_HOOK(C,69,CSSetShader,Pipeline);
    EDVR_COHORT_HOOK(C,70,CSSetSamplers,Pipeline);
    EDVR_COHORT_HOOK(C,71,CSSetConstantBuffers,Pipeline);
    EDVR_COHORT_HOOK(C,111,Flush,Input);
    EDVR_COHORT_HOOK(C,114,FinishCommandList,Input);
    bool have=false;
    if(!sameCohortContext<ID3D11DeviceContext1>(ctx,have) || !have)return false;
    using C1=ID3D11DeviceContext1;
    EDVR_COHORT_HOOK(C1,120,HSSetConstantBuffers1,Pipeline);
    EDVR_COHORT_HOOK(C1,121,DSSetConstantBuffers1,Pipeline);
    EDVR_COHORT_HOOK(C1,122,GSSetConstantBuffers1,Pipeline);
    EDVR_COHORT_HOOK(C1,123,PSSetConstantBuffers1,Pipeline);
    EDVR_COHORT_HOOK(C1,124,CSSetConstantBuffers1,Pipeline);
    if(!sameCohortContext<ID3D11DeviceContext2>(ctx,have))return false;
    if(have){
        using C2=ID3D11DeviceContext2;
        EDVR_COHORT_HOOK(C2,134,UpdateTileMappings,Input);
        EDVR_COHORT_HOOK(C2,135,CopyTileMappings,Input);
        EDVR_COHORT_HOOK(C2,136,CopyTiles,Input);
        EDVR_COHORT_HOOK(C2,137,UpdateTiles,Input);
        EDVR_COHORT_HOOK(C2,138,ResizeTilePool,Input);
        EDVR_COHORT_HOOK(C2,139,TiledResourceBarrier,Input);
    }
    if(!sameCohortContext<ID3D11DeviceContext3>(ctx,have))return false;
    if(have){
        using C3=ID3D11DeviceContext3;
        EDVR_COHORT_HOOK(C3,144,Flush1,Input);
        EDVR_COHORT_HOOK(C3,145,SetHardwareProtectionState,Input);
    }
    if(!sameCohortContext<ID3D11DeviceContext4>(ctx,have))return false;
    if(have){
        using C4=ID3D11DeviceContext4;
        EDVR_COHORT_HOOK(C4,147,Signal,Input);
        EDVR_COHORT_HOOK(C4,148,Wait,Input);
    }
#undef EDVR_COHORT_HOOK
    return ok;
}
} // namespace edvr::native_vrs

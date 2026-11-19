// Exercise the built proxy's committed barrier thunks on the selected native
// adapter. The generation export only observes the normal dispatch path.
#pragma once
#include <d3d11_4.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

inline bool smokeWitnessBarriers(HMODULE proxy,const char* adapterLuid) {
    using Microsoft::WRL::ComPtr;
    if(!adapterLuid || std::strlen(adapterLuid)!=17 || adapterLuid[8]!=':')return false;
    for(unsigned i=0;i<17;++i)if(i!=8 && !std::isxdigit(static_cast<unsigned char>(adapterLuid[i])))return false;
    unsigned high=0,low=0;
    if(sscanf_s(adapterLuid,"%8x:%8x",&high,&low)!=2)return false;
    const LUID luid{low,static_cast<LONG>(high)};
    const auto dxgi=LoadLibraryExW(L"dxgi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto factoryFn=dxgi?reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(dxgi,"CreateDXGIFactory1")):nullptr;
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(proxy,"D3D11CreateDevice"));
    using Observe=unsigned (*)(ID3D11DeviceContext*,uint64_t*,uint64_t*);
    const auto observe=reinterpret_cast<Observe>(GetProcAddress(proxy,"edvr_selftest_witness_generations"));
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    if(!observe || !create || !factoryFn || FAILED(factoryFn(IID_PPV_ARGS(&factory))) ||
       FAILED(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter))) ||
       FAILED(create(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)))return false;
    uint64_t inputs=0,pipeline=0;
    auto check=[&](const char* name,bool input,unsigned flags=1u){
        const auto before=input?inputs:pipeline;
        const auto status=observe(ctx.Get(),&inputs,&pipeline);
        const bool pass=status==flags && (input?inputs:pipeline)>before;
        printf("  %s  %s input=%llu pipeline=%llu flags=%u\n",pass?"ok  ":"FAIL",name,inputs,pipeline,status);
        return pass;
    };
    if(observe(ctx.Get(),&inputs,&pipeline)!=1u)return false;
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if(!check("IA topology",false))return false;
    ID3D11SamplerState* empty=nullptr;
    ctx->PSSetSamplers(0,1,&empty);
    if(!check("PS sampler",false))return false;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=64;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> buffer;
    D3D11_MAPPED_SUBRESOURCE map{};
    if(FAILED(device->CreateBuffer(&bd,nullptr,&buffer)) || FAILED(ctx->Map(buffer.Get(),0,D3D11_MAP_READ,0,&map)))return false;
    const bool mapped=check("Map",true);
    ctx->Unmap(buffer.Get(),0);
    if(!mapped || !check("Unmap",true))return false;

    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=16;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIResource> resource;
    ComPtr<IDXGIDevice4> offers;
    if(FAILED(device->CreateTexture2D(&td,nullptr,&texture)) || FAILED(texture.As(&resource)) || FAILED(device.As(&offers)))return false;
    IDXGIResource* resources[]={resource.Get()};
    BOOL discarded=FALSE;
    DXGI_RECLAIM_RESOURCE_RESULTS reclaim{};
    if(FAILED(offers->OfferResources(1,resources,DXGI_OFFER_RESOURCE_PRIORITY_LOW)) ||
       !check("DXGI OfferResources",true) || FAILED(offers->ReclaimResources(1,resources,&discarded)))return false;
    if(FAILED(offers->OfferResources1(1,resources,DXGI_OFFER_RESOURCE_PRIORITY_LOW,0)) ||
       !check("DXGI OfferResources1",true) || FAILED(offers->ReclaimResources1(1,resources,&reclaim)))return false;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!event)return false;
    const bool signaled=SUCCEEDED(offers->EnqueueSetEvent(event)) && check("DXGI EnqueueSetEvent",true) &&
        WaitForSingleObject(event,5000)==WAIT_OBJECT_0;
    CloseHandle(event);
    if(!signaled)return false;
    ComPtr<ID3D11Multithread> control;
    if(FAILED(ctx.As(&control)) && FAILED(device.As(&control)))return false;
    if(control->GetMultithreadProtected())return false;
    control->SetMultithreadProtected(FALSE);
    if(observe(ctx.Get(),&inputs,&pipeline)!=3u)return false;
    printf("{\"native_proxy_witness_barriers\":true,\"adapter_luid\":\"%08x:%08x\"}\n",high,low);
    return true;
}

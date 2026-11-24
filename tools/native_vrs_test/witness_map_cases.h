// Run on the fixture's existing WARP device. Only the unbalanced notifications
// below are synthetic; they do not issue invalid D3D11 Map or Unmap calls.
#pragma once
#include "../../src/d3d11/native_vrs_witness_maps.h"
#include "../../src/d3d11/native_vrs_witness_state.h"

namespace witness_map_fixture {
inline edvr::native_vrs::WitnessMappedBuffers* active=nullptr;
inline bool mapped(ID3D11Resource* buffer) noexcept {return active->conflicts(buffer);}
template<class Check> void run(ID3D11Device* device,ID3D11DeviceContext* ctx,Check check){
    using Microsoft::WRL::ComPtr;
    using namespace edvr::native_vrs;
    WitnessMappedBuffers maps;
    active=&maps;
    struct Clear {~Clear(){active=nullptr;}} clear;
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=64;desc.Usage=D3D11_USAGE_DYNAMIC;
    desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    std::array<ComPtr<ID3D11Buffer>,35> buffers;
    for(auto& b:buffers)if(FAILED(device->CreateBuffer(&desc,nullptr,&b))){check(false,"writable map fixtures created");return;}
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    D3D11_MAPPED_SUBRESOURCE data{};
    if(FAILED(device->CreateBuffer(&desc,nullptr,&staging)) ||
       FAILED(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&data))){check(false,"readback map fixture created");return;}
    maps.observe(staging.Get(),0,true);
    check(!maps.needsCheck(),"held staging READ cannot change any retained binding");
    ctx->Unmap(staging.Get(),0);maps.observe(staging.Get(),0,false);
    maps.observe(staging.Get(),0,false); // private raw Map followed by hooked Unmap
    check(!maps.needsCheck() && !maps.unmatched,"unmatched staging Unmap has no retained CPU writer");

    UINT stride=16,offset=0;
    ctx->IASetVertexBuffers(0,1,buffers[0].GetAddressOf(),&stride,&offset);
    unsigned opened=0;
    for(;opened<34;++opened){
        auto* b=buffers[opened].Get();
        if(FAILED(ctx->Map(b,0,D3D11_MAP_WRITE_DISCARD,0,&data)))break;
        maps.observe(b,0,true);
    }
    check(opened==34 && maps.open()==34 && !maps.unknown,"more than 32 distinct open writes stay individually accounted");
    check(witnessLocalResource(buffers.back().Get(),mapped),"held unrelated writes allow this captured buffer");
    ComPtr<ID3D11Buffer> actual;ctx->IAGetVertexBuffers(0,1,&actual,&stride,&offset);
    check(opened && actual.Get()==buffers.front().Get() && !witnessLocalResource(actual.Get(),mapped),
          "IA getter returns the retained writable input and its open write refuses capture");
    while(opened){
        auto* b=buffers[--opened].Get();
        ctx->Unmap(b,0);maps.observe(b,0,false);
    }
    check(!maps.needsCheck() && witnessLocalResource(actual.Get(),mapped),"matching Unmap restores availability");
    ID3D11Buffer* empty=nullptr;ctx->IASetVertexBuffers(0,1,&empty,&stride,&offset);
    maps.observe(buffers[0].Get(),0,false);
    check(maps.unmatched==1 && !maps.unknown && !witnessLocalResource(buffers[0].Get(),mapped) &&
          witnessLocalResource(buffers[1].Get(),mapped),"unmatched writable Unmap quarantines only its resource");
    maps.observe(buffers[1].Get(),0,true);maps.observe(buffers[1].Get(),0,true);maps.observe(buffers[1].Get(),0,false);
    check(maps.duplicates==1 && !maps.unknown && !witnessLocalResource(buffers[1].Get(),mapped) &&
          witnessLocalResource(buffers[2].Get(),mapped),"duplicate writable Map quarantines only its resource");
    maps.observe(nullptr,0,true);
    check(maps.unknown && !witnessLocalResource(buffers[2].Get(),mapped),"unidentifiable writable map fails closed at capture");
}
} // namespace witness_map_fixture

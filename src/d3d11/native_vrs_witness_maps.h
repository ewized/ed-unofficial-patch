// CPU pointers from Map remain writable until Unmap. Track only buffers that
// can also be bound by a certified draw. CPU-accessible textures are rejected
// by WitnessPipeline independently, and staging resources cannot be bound.
#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <new>
#include <vector>
#include <utility>

namespace edvr::native_vrs {
class WitnessMappedBuffers {
    using Identity=Microsoft::WRL::ComPtr<IUnknown>;
    struct Mapping {
        Microsoft::WRL::ComPtr<ID3D11Resource> resource;
        UINT sub;
        mutable Identity identity; // populated only when a second interface needs comparison
        IUnknown* canonical() const noexcept {
            if(!identity)resource->QueryInterface(IID_PPV_ARGS(identity.GetAddressOf()));
            return identity.Get();
        }
    };
    std::vector<Mapping> open_;
    // A marker follows the actual resource across interface aliases and dies
    // with it. An unmatched Unmap cannot permanently retain a game buffer.
    static constexpr GUID uncertainTag{0x11c769e3,0x7c07,0x419a,{0xa5,0x3f,0x47,0x12,0x91,0xa8,0x56,0xcc}};
    bool tagged_=false;
    static bool writableBinding(ID3D11Resource* resource) noexcept {
        D3D11_RESOURCE_DIMENSION kind{};
        resource->GetType(&kind);
        if(kind!=D3D11_RESOURCE_DIMENSION_BUFFER)return false;
        D3D11_BUFFER_DESC desc{};
        static_cast<ID3D11Buffer*>(resource)->GetDesc(&desc);
        constexpr UINT retained=D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_INDEX_BUFFER|
            D3D11_BIND_CONSTANT_BUFFER|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        return (desc.CPUAccessFlags&D3D11_CPU_ACCESS_WRITE) && (desc.BindFlags&retained);
    }
    void mark(ID3D11Resource* resource) noexcept {
        const UINT value=1;
        if(FAILED(resource->SetPrivateData(uncertainTag,sizeof(value),&value))) unknown=true;
        else tagged_=true;
    }
    bool marked(ID3D11Resource* resource) const noexcept {
        if(!tagged_)return false;
        UINT bytes=sizeof(UINT),value=0;
        return resource->GetPrivateData(uncertainTag,&bytes,&value)!=DXGI_ERROR_NOT_FOUND;
    }
public:
    bool unknown=false;
    uint64_t duplicates=0,unmatched=0,storageFailures=0,identityFailures=0;
    size_t peak=0;
    size_t open()const noexcept{return open_.size();}
    bool needsCheck()const noexcept{return unknown || tagged_ || !open_.empty();}

    // Called only on the admitted immediate-context owner. Private replay
    // and serialized foreign handoff never borrow this owner's map ledger.
    void observe(ID3D11Resource* resource,UINT sub,bool mapped) noexcept {
        if(!resource){++identityFailures;unknown=true;return;}
        auto it=std::find_if(open_.begin(),open_.end(),[&](const Mapping& m){return m.resource.Get()==resource && m.sub==sub;});
        if(it==open_.end()){
            if(!writableBinding(resource))return;
            Identity id;
            if(!open_.empty()){
                if(FAILED(resource->QueryInterface(IID_PPV_ARGS(id.GetAddressOf())))){++identityFailures;mark(resource);return;}
                for(it=open_.begin();it!=open_.end();++it){
                    if(it->sub!=sub)continue;
                    auto* other=it->canonical();
                    if(!other){++identityFailures;mark(resource);return;}
                    if(other==id.Get())break;
                }
            }
            if(it==open_.end()){
                if(!mapped || sub!=0){++unmatched;mark(resource);return;}
                try{open_.push_back({resource,sub,std::move(id)});}
                catch(const std::bad_alloc&){++storageFailures;mark(resource);return;}
                peak=(std::max)(peak,open_.size());
                return;
            }
        }
        if(mapped){++duplicates;mark(resource);return;}
        if(it!=open_.end()-1)*it=std::move(open_.back());
        open_.pop_back();
    }

    // WitnessPipeline calls this once per candidate bound writable buffer
    // during initial capture. Every later Map and Unmap drains before changing
    // the mapping, so continuation draws reuse the admitted snapshot.
    bool conflicts(ID3D11Resource* resource) const noexcept {
        if(!needsCheck())return false;
        if(unknown || !resource || marked(resource))return true;
        if(open_.empty())return false;
        for(const auto& item:open_)if(item.resource.Get()==resource)return true;
        Identity id;
        if(FAILED(resource->QueryInterface(IID_PPV_ARGS(id.GetAddressOf()))))return true;
        for(const auto& item:open_){
            auto* other=item.canonical();
            if(!other || other==id.Get())return true;
        }
        return false;
    }
};
} // namespace edvr::native_vrs

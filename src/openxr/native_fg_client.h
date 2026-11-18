#pragma once
#include "../common/native_frame_generation.h"
#include "eye_capture.h"
#include "native_trace.h"
#include "native_fg_import_pins.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cmath>
#include <exception>
#include <thread>
#include <openxr/openxr.h>

namespace edvr::openxr {
// Every method is serialized by the existing XR owner. Imported provider
// resources live only until ALL phases have been copied to private images.
class NativeFgClient final {
 public:
  struct Images {
    EyeCapture phases[EDVR_FG_MAX_PHASES];
    EyeCapture nativeUi;
    EyeCapture nativeWorld;
    EdvrFgBatch metadata{};
    unsigned next=0;
    bool ready=false;
  };
  ~NativeFgClient() {
    close();
    // Like EyeCapture's owner contract: ModuleBackend retains a whole failed
    // Generation. Destruction without a proved close must not drop GPU pins.
    if(importPins_.pending())std::terminate();
  }
  HRESULT acquire(HMODULE provider,ID3D11Device* game,uint64_t generation) {
    if(table_.context)return E_PENDING;
    if(!provider||!game||!generation)return E_INVALIDARG;
    auto entry=GetProcAddress(provider,"edvrAcquireNativeFrameGeneration");
    if(!owned(provider,entry))return E_NOINTERFACE;
    EdvrFgRequest request{sizeof(request),EDVR_NATIVE_FRAME_GENERATION_VERSION_1,game,generation};
    EdvrFgTable candidate{sizeof(candidate),EDVR_NATIVE_FRAME_GENERATION_VERSION_1};
    const HRESULT hr=reinterpret_cast<EdvrAcquireNativeFrameGeneration>(entry)(&request,&candidate);
    if(hr!=S_OK)return FAILED(hr)?hr:E_NOINTERFACE;
    if(candidate.size!=sizeof(candidate)||candidate.version!=EDVR_NATIVE_FRAME_GENERATION_VERSION_1||!candidate.context||
       !owned(provider,candidate.getStatus)||!owned(provider,candidate.beginSource)||!owned(provider,candidate.poll)||
       !owned(provider,candidate.retain)||!owned(provider,candidate.release)||!owned(provider,candidate.retire)||
       !owned(provider,candidate.discard)||!owned(provider,candidate.invalidate)||!owned(provider,candidate.close))return E_NOINTERFACE;
    table_=candidate;owner_=std::this_thread::get_id();return S_OK;
  }
  bool acquired()const{return table_.context!=nullptr;}
  bool healthy()const{return !quarantined_;}
  // Acquisition itself runs through the trusted producer rendezvous. The
  // host transfers the otherwise-unused CPU table to its one XR owner once.
  void bindOwner(){if(!device_&&!pending_.token)owner_=std::this_thread::get_id();}
  HRESULT status(EdvrFgStatus& out) {
    out={sizeof(out),EDVR_NATIVE_FRAME_GENERATION_VERSION_1};
    if(!onOwner()||!table_.context)return S_FALSE;
    const HRESULT collected=collectImportPins();if(FAILED(collected))return collected;
    const HRESULT hr=table_.getStatus(table_.context,&out);
    if(hr==S_OK&&(out.size!=sizeof(out)||out.version!=EDVR_NATIVE_FRAME_GENERATION_VERSION_1||
       (out.requestedMultiplier!=0&&!nativeFgFixedMultiplier(out.requestedMultiplier))))return E_UNEXPECTED;
    return hr;
  }
  HRESULT begin(const EdvrFgSource& source) {return onOwner()&&table_.context?table_.beginSource(table_.context,&source):E_UNEXPECTED;}
  HRESULT initializeConsumer(ID3D11Device* device,ID3D11DeviceContext* context) {
    if(!onOwner()||!device||!context)return E_INVALIDARG;
    if(device_)return device_.Get()==device&&context_.Get()==context?S_OK:E_UNEXPECTED;
    Microsoft::WRL::ComPtr<ID3D11Device5> device5;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
    HRESULT hr=device->QueryInterface(IID_PPV_ARGS(&device5));if(FAILED(hr))return hr;
    hr=context->QueryInterface(IID_PPV_ARGS(&context4));if(FAILED(hr))return hr;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr=device->QueryInterface(IID_PPV_ARGS(&dxgi));if(FAILED(hr))return hr;
    hr=dxgi->GetAdapter(&adapter);if(FAILED(hr))return hr;
    DXGI_ADAPTER_DESC desc{};hr=adapter->GetDesc(&desc);if(FAILED(hr))return hr;
    Microsoft::WRL::ComPtr<ID3D11Fence> retired;
    hr=device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&retired));if(FAILED(hr))return hr;
    HANDLE handle=nullptr;hr=retired->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle);if(FAILED(hr))return hr;
    device_=device;context_=context;device5_=device5;context4_=context4;retired_=retired;retiredHandle_=handle;luid_=desc.AdapterLuid;
    for(auto& image:images_.phases) {hr=image.initialize(device);if(FAILED(hr))return hr;}
    hr=images_.nativeUi.initialize(device);if(FAILED(hr))return hr;
    hr=images_.nativeWorld.initialize(device);if(FAILED(hr))return hr;
    return S_OK;
  }
  // A pending, not-yet-signalled provider fence is held without a GPU wait.
  // Once ready, each aliased source is opened/validated before ANY GPU read.
  HRESULT poll(uint64_t generation,uint64_t reference,uint64_t config,unsigned multiplier,
               uint32_t width,uint32_t height,XrTime now) {
    if(quarantined_)return E_PENDING;
    if(!onOwner()||!table_.context||!device_)return S_FALSE;
    if(!nativeFgFixedMultiplier(multiplier))return E_INVALIDARG;
    const HRESULT collected=collectImportPins();if(collected!=S_OK)return collected;
    if(images_.ready)return S_FALSE;
    if(!pending_.token) {
      pending_={sizeof(pending_),EDVR_FG_BATCH_VERSION_3};
      const HRESULT hr=table_.poll(table_.context,&pending_);
      if(hr!=S_OK){pending_={};return hr;}
    }
    const auto& b=pending_;
    const unsigned full=(1u<<multiplier)-1u;
    if(b.size!=sizeof(b)||b.version!=EDVR_FG_BATCH_VERSION_3||!b.token||
       b.generation!=generation||b.referenceGeneration!=reference||b.configGeneration!=config||!b.historyGeneration||
       !b.currentSequence||(b.generatedCount&&b.currentSequence<=b.previousSequence)||b.multiplier!=multiplier||
       (b.phaseMask!=1u&&b.phaseMask!=full)||b.generatedCount!=((b.phaseMask==1u)?0u:multiplier-1u)||
       b.width!=width||b.height!=height||!width||!height||
       b.adapterLuidLow!=luid_.LowPart||b.adapterLuidHigh!=luid_.HighPart||
       !b.readyFence||!b.readyValue||b.currentPoseDisplayTime<=0||
       now-b.currentPoseDisplayTime>500000000||b.currentPoseDisplayTime-now>500000000||
       (b.generatedCount&&(b.previousPoseDisplayTime<=0||b.previousPoseDisplayTime>=b.currentPoseDisplayTime))||
       !validViews(b.currentViews)|| (b.generatedCount&&!validViews(b.previousViews))||
       (b.uiFlags!=0&&b.uiFlags!=EDVR_FG_UI_NATIVE_COVERAGE)||b.reserved||
       (b.uiFlags&&(!b.generatedCount||!b.uiTextures[0]||!b.uiTextures[1]))||
       (b.uiFlags&&(!b.nativeWorldTextures[0]||!b.nativeWorldTextures[1]))||
       (!b.uiFlags&&(b.uiTextures[0]||b.uiTextures[1]||b.nativeWorldTextures[0]||b.nativeWorldTextures[1])))return reject(E_INVALIDARG);
    if(b.currentPoseDisplayTime>now)return S_FALSE;
    if(retiredValue_>=UINT64_MAX-1)return reject(E_UNEXPECTED);
    Microsoft::WRL::ComPtr<ID3D11Fence> ready;
    HRESULT hr=device5_->OpenSharedFence(b.readyFence,IID_PPV_ARGS(&ready));if(FAILED(hr))return reject(hr);
    const uint64_t completed=ready->GetCompletedValue();
    if(completed==UINT64_MAX)return reject(DXGI_ERROR_DEVICE_REMOVED);
    if(completed<b.readyValue)return S_FALSE;
    if(!importPins_.begin(Microsoft::WRL::ComPtr<IUnknown>(ready.Get())))return E_PENDING;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputs[EDVR_FG_MAX_PHASES][2];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nativeUi[2];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> nativeWorld[2];
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    hr=device_.As(&device1);if(FAILED(hr))return reject(hr);
    for(unsigned phase=0;phase<=b.generatedCount;++phase)for(unsigned eye=0;eye<2;++eye) {
      if(!b.textures[phase][eye])return reject(E_INVALIDARG);
      for(unsigned earlier=0;earlier<phase*2+eye;++earlier)
        if(b.textures[earlier/2][earlier%2]==b.textures[phase][eye])return reject(E_INVALIDARG);
      hr=device1->OpenSharedResource1(b.textures[phase][eye],IID_PPV_ARGS(&inputs[phase][eye]));
      if(FAILED(hr))return reject(hr);
      D3D11_TEXTURE2D_DESC desc{};inputs[phase][eye]->GetDesc(&desc);
      if(desc.Width!=width||desc.Height!=height||uint32_t(desc.Format)!=b.format||desc.MipLevels!=1||
         desc.ArraySize!=1||desc.SampleDesc.Count!=1||desc.SampleDesc.Quality||
         desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM)return reject(E_INVALIDARG);
      if(!importPins_.add(Microsoft::WRL::ComPtr<IUnknown>(inputs[phase][eye].Get())))return reject(E_OUTOFMEMORY);
    }
    if(b.uiFlags)for(unsigned eye=0;eye<2;++eye) {
      for(unsigned phase=0;phase<=b.generatedCount;++phase)for(unsigned e=0;e<2;++e)
        if(b.uiTextures[eye]==b.textures[phase][e])return reject(E_INVALIDARG);
      if(eye&&b.uiTextures[0]==b.uiTextures[1])return reject(E_INVALIDARG);
      hr=device1->OpenSharedResource1(b.uiTextures[eye],IID_PPV_ARGS(&nativeUi[eye]));
      if(FAILED(hr))return reject(hr);
      D3D11_TEXTURE2D_DESC desc{};nativeUi[eye]->GetDesc(&desc);
      if(desc.Width!=width||desc.Height!=height||desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||
         desc.MipLevels!=1||desc.ArraySize!=1||desc.SampleDesc.Count!=1||desc.SampleDesc.Quality)
        return reject(E_INVALIDARG);
      if(!importPins_.add(Microsoft::WRL::ComPtr<IUnknown>(nativeUi[eye].Get())))return reject(E_OUTOFMEMORY);
    }
    if(b.uiFlags)for(unsigned eye=0;eye<2;++eye) {
      for(unsigned phase=0;phase<=b.generatedCount;++phase)for(unsigned e=0;e<2;++e)
        if(b.nativeWorldTextures[eye]==b.textures[phase][e])return reject(E_INVALIDARG);
      for(unsigned e=0;e<2;++e)if(b.nativeWorldTextures[eye]==b.uiTextures[e])return reject(E_INVALIDARG);
      if(eye&&b.nativeWorldTextures[0]==b.nativeWorldTextures[1])return reject(E_INVALIDARG);
      hr=device1->OpenSharedResource1(b.nativeWorldTextures[eye],IID_PPV_ARGS(&nativeWorld[eye]));
      if(FAILED(hr))return reject(hr);
      D3D11_TEXTURE2D_DESC desc{};nativeWorld[eye]->GetDesc(&desc);
      if(desc.Width!=width||desc.Height!=height||desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||
         desc.MipLevels!=1||desc.ArraySize!=1||desc.SampleDesc.Count!=1||desc.SampleDesc.Quality)
        return reject(E_INVALIDARG);
      if(!importPins_.add(Microsoft::WRL::ComPtr<IUnknown>(nativeWorld[eye].Get())))return reject(E_OUTOFMEMORY);
    }
    hr=context4_->Wait(ready.Get(),b.readyValue);if(FAILED(hr))return reject(hr);
    importPins_.readsMayStart();
    bool good=true;
    for(unsigned phase=0;phase<=b.generatedCount&&good;++phase)for(unsigned eye=0;eye<2&&good;++eye) {
      // The publisher explicitly preserves encoded SDR in normalized RGBA8.
      // EyeCapture's Gamma interpretation creates an sRGB SRV; the XR draw
      // then performs exactly the normal encoded-eye decode/encode path.
      const vr::Texture_t texture{inputs[phase][eye].Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
      good=images_.phases[phase].capture(vr::EVREye(eye),&texture)==vr::VRCompositorError_None;
    }
    if(b.uiFlags)for(unsigned eye=0;eye<2&&good;++eye) {
      const vr::Texture_t texture{nativeUi[eye].Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
      good=images_.nativeUi.capture(vr::EVREye(eye),&texture)==vr::VRCompositorError_None;
      if(good){
        const vr::Texture_t worldTexture{nativeWorld[eye].Get(),vr::API_DirectX,vr::ColorSpace_Gamma};
        good=images_.nativeWorld.capture(vr::EVREye(eye),&worldTexture)==vr::VRCompositorError_None;
      }
    }
    // Even a partial copy owns provider GPU reads. Retire, never discard it.
    const uint64_t value=++retiredValue_;
    hr=context4_->Signal(retired_.Get(),value);context_->Flush();
    if(FAILED(hr)) {importPins_.quarantine();quarantined_=true;return hr;}
    if(!importPins_.signal(value)){quarantined_=true;return E_UNEXPECTED;}
    const HRESULT retirement=table_.retire(b.token,retiredHandle_,value);
    if(FAILED(retirement)){importPins_.quarantine();quarantined_=true;return retirement;}
    images_.metadata=b;images_.metadata.token=nullptr;
    for(auto& pair:images_.metadata.textures)for(auto& handle:pair)handle=nullptr;
    for(auto& handle:images_.metadata.uiTextures)handle=nullptr;
    for(auto& handle:images_.metadata.nativeWorldTextures)handle=nullptr;
    images_.metadata.readyFence=nullptr;
    table_.release(b.token);pending_={};
    if(!good)return E_FAIL;
    if(!notedBatch_||(images_.metadata.generatedCount&&!notedGenerated_)) {
      const auto& batch=images_.metadata;
      nativeTracePrintf("native_fg_import,first=%u,first_generated=%u,sequence=%llu,previous_sequence=%llu,multiplier=%u,generated_count=%u,phase_mask=0x%X,kind=%s,output=%ux%u,format=%u,history=%llu,consumer_retire_value=%llu\n",
        unsigned(!notedBatch_),unsigned(batch.generatedCount&&!notedGenerated_),
        (unsigned long long)batch.currentSequence,(unsigned long long)batch.previousSequence,batch.multiplier,
        batch.generatedCount,batch.phaseMask,batch.generatedCount?"generated_ready":"native_only",batch.width,batch.height,batch.format,
        (unsigned long long)batch.historyGeneration,(unsigned long long)value);
      notedBatch_=true;if(batch.generatedCount)notedGenerated_=true;
    }
    images_.next=images_.metadata.generatedCount?1u:0u;images_.ready=true;return S_OK;
  }
  Images& images(){return images_;}
  // Nonblocking owner-side collection, including status calls while FG is Off.
  HRESULT collectImportPins() {
    if(!onOwner())return E_UNEXPECTED;
    if(quarantined_)return E_PENDING;
    if(!importPins_.pending())return S_OK;
    if(!retired_){importPins_.quarantine();quarantined_=true;return E_PENDING;}
    const auto done=retired_->GetCompletedValue();
    if(importPins_.collect(done))return S_OK;
    if(importPins_.quarantined()){quarantined_=true;return DXGI_ERROR_DEVICE_REMOVED;}
    return S_FALSE;
  }
  HRESULT invalidate(uint32_t reason) {
    if(!onOwner())return E_UNEXPECTED;
    importPins_.invalidate();
    images_.ready=false;
    if(pending_.token&&!quarantined_)reject(S_FALSE);
    return table_.context?table_.invalidate(table_.context,reason):S_FALSE;
  }
  HRESULT close() {
    if(!table_.context)return S_FALSE;
    if(!onOwner()||quarantined_)return E_PENDING;
    if(collectImportPins()!=S_OK)return E_PENDING;
    invalidate(1);
    const HRESULT hr=table_.close(table_.context);if(hr!=S_OK)return hr==S_FALSE?E_PENDING:hr;
    for(auto& image:images_.phases)image.shutdown();
    images_.nativeUi.shutdown();
    images_.nativeWorld.shutdown();
    table_={};device5_.Reset();context4_.Reset();retired_.Reset();context_.Reset();device_.Reset();
    if(retiredHandle_){CloseHandle(retiredHandle_);retiredHandle_=nullptr;}return S_OK;
  }
 private:
  bool onOwner()const{return owner_==std::this_thread::get_id();}
  template<class T>static bool owned(HMODULE provider,T address) {
    if(!address)return false;HMODULE actual=nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(address),&actual)&&actual==provider;
  }
  static bool validViews(const EdvrFgView(&views)[2]) {
    for(const auto& view:views) {
      double q=0;for(float v:view.orientation){if(!std::isfinite(v))return false;q+=double(v)*v;}
      if(q<.9||q>1.1)return false;
      for(float v:view.position)if(!std::isfinite(v))return false;
      for(float v:view.fov)if(!std::isfinite(v)||std::fabs(v)>=1.57f)return false;
      if(view.fov[0]>=view.fov[1]||view.fov[3]>=view.fov[2])return false;
    }return true;
  }
  HRESULT reject(HRESULT result) {
    if(!importPins_.cancelBeforeReads()){importPins_.quarantine();quarantined_=true;return E_PENDING;}
    if(pending_.token) {const HRESULT hr=table_.discard(pending_.token);if(FAILED(hr)){quarantined_=true;return hr;}table_.release(pending_.token);}
    pending_={};return result;
  }
  EdvrFgTable table_{};EdvrFgBatch pending_{};Images images_;
  std::thread::id owner_{};LUID luid_{};
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11Device5> device5_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
  Microsoft::WRL::ComPtr<ID3D11Fence> retired_;
  HANDLE retiredHandle_=nullptr;uint64_t retiredValue_=0;bool quarantined_=false;
  bool notedBatch_=false,notedGenerated_=false;
  NativeFgImportPins<Microsoft::WRL::ComPtr<IUnknown>> importPins_;
};
} // namespace edvr::openxr

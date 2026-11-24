#pragma once
#include "../common/native_menu.h"
#include "geometry_snapshot.h"
#include "native_trace.h"
#include <cstring>
#include <wrl/client.h>

namespace edvr::openxr {
// Calls are serialized by the XR owner. Acquisition and eye treatment use
// the host's producer callback. FPS raster reads and close are CPU-only.
class NativeMenuClient final {
 public:
  HRESULT acquire(HMODULE provider, ID3D11Device* device, uint64_t generation, bool fpsLayerSupported=true) {
    if (table_.context) return E_PENDING;
    if (!provider || !device || !generation) return E_INVALIDARG;
    const auto entry = GetProcAddress(provider, "edvrAcquireNativeMenu");
    if (!entry || !owned(provider, entry)) return E_NOINTERFACE;
    const uint32_t version = fpsLayerSupported ? EDVR_NATIVE_MENU_VERSION_2 : EDVR_NATIVE_MENU_VERSION_1;
    const uint32_t size = fpsLayerSupported ? uint32_t(sizeof(EdvrNativeMenuTable)) : EDVR_NATIVE_MENU_TABLE_V1_SIZE;
    EdvrNativeMenuRequest request{sizeof(request), version, device, generation};
    EdvrNativeMenuTable candidate{size, version};
    const auto acquire = reinterpret_cast<decltype(&edvrAcquireNativeMenu)>(entry);
    auto result = acquire(&request, &candidate);
    if (fpsLayerSupported && (result == E_INVALIDARG || result == E_NOINTERFACE)) {
      request.version = EDVR_NATIVE_MENU_VERSION_1;
      candidate = {EDVR_NATIVE_MENU_TABLE_V1_SIZE, EDVR_NATIVE_MENU_VERSION_1};
      result = acquire(&request, &candidate);
    }
    if (result != S_OK) return FAILED(result) ? result : E_NOINTERFACE;
    const bool fps = request.version == EDVR_NATIVE_MENU_VERSION_2;
    if (candidate.size != (fps ? sizeof(candidate) : EDVR_NATIVE_MENU_TABLE_V1_SIZE) || candidate.version != request.version ||
        !candidate.context || !owned(provider, candidate.publishPose) ||
        !owned(provider, candidate.treatEye) || !owned(provider, candidate.close) ||
        (fps && !owned(provider, candidate.readFpsOverlay))) return E_NOINTERFACE;
    table_ = candidate;
    generation_ = generation;
    return S_OK;
  }
  bool acquired() const { return table_.context != nullptr; }
  bool independentFpsOverlay() const {
    return acquired() && table_.version == EDVR_NATIVE_MENU_VERSION_2 && table_.readFpsOverlay;
  }
  HRESULT readFpsOverlay(uint64_t knownRevision, EdvrNativeFpsOverlay& overlay, void* pixels, uint32_t capacity) const {
    if (!independentFpsOverlay()) return S_FALSE;
    return table_.readFpsOverlay(table_.context,knownRevision,&overlay,pixels,capacity);
  }
  HRESULT publish(const GeometryInput& geometry, uint64_t referenceGeneration) {
    if (!acquired()) return S_FALSE;
    GeometrySnapshot snapshot{};
    if (!makeGeometrySnapshot(geometry, snapshot)) return invalidate();
    float eyes[2][12]{}, frusta[2][4]{};
    for (unsigned eye = 0; eye < 2; ++eye) {
      double matrix[4][4]{}; vr::HmdMatrix34_t pose{};
      detail::rigid(geometry.views[eye].pose, matrix);
      if (!detail::narrow(matrix, pose)) return invalidate();
      std::memcpy(eyes[eye], pose.m, sizeof(eyes[eye]));
      const auto& raw = snapshot.raw[eye];
      frusta[eye][0] = raw.left; frusta[eye][1] = raw.right;
      frusta[eye][2] = raw.top; frusta[eye][3] = raw.bottom;
    }
    return table_.publishPose(table_.context, &snapshot.headToLocal.m[0][0], eyes, frusta, generation_, referenceGeneration);
  }
  HRESULT invalidate() {
    return acquired() ? table_.publishPose(table_.context, nullptr, nullptr, nullptr, generation_, 0) : S_FALSE;
  }
  HRESULT treat(unsigned eye, ID3D11Texture2D* source, const vr::VRTextureBounds_t* bounds,
      Microsoft::WRL::ComPtr<ID3D11Texture2D>& output, vr::VRTextureBounds_t& outputBounds) {
    output.Reset(); outputBounds = {};
    if (!acquired()) return S_FALSE;
    const float box[4] = {bounds ? bounds->uMin : 0, bounds ? bounds->vMin : 0,
                         bounds ? bounds->uMax : 1, bounds ? bounds->vMax : 1};
    float outBox[4]{}; ID3D11Texture2D* raw = nullptr;
    const auto result = table_.treatEye(table_.context, eye, source, box, &raw, outBox);
    output.Attach(raw);
    if (result != S_OK) { output.Reset(); return result; }
    if (!output) return E_UNEXPECTED;
    outputBounds = {outBox[0], outBox[1], outBox[2], outBox[3]};
    return S_OK;
  }
  HRESULT close() {
    if (!acquired()) return S_FALSE;
    const auto result = table_.close(table_.context);
    if (SUCCEEDED(result)) table_ = {};
    return result;
  }
 private:
  template<class T> static bool owned(HMODULE provider, T address) {
    if (!address) return false;
    HMODULE implementation = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &implementation)
        && implementation == provider;
  }
  EdvrNativeMenuTable table_{};
  uint64_t generation_ = 0;
};
} // namespace edvr::openxr

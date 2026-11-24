#include "../../src/d3d11/frame_generation_coordinator.h"

// The temporal contract rig exercises disabled frame generation. The sharpen
// contract rig records clean-world capture and the coordinator has its own rig.
namespace edvr {
void nativeFgSetRequestedMultiplier(uint32_t) noexcept {}
bool nativeFgRequested() noexcept { return false; }
HRESULT nativeFgAcquire(const EdvrFgRequest*, EdvrFgTable*) noexcept { return E_NOINTERFACE; }
void nativeFgBeginTemporal(const EdvrNativeTemporalFrame&, bool, bool) noexcept {}
void nativeFgBeforeTemporalEye(uint64_t, unsigned, ID3D11Texture2D*) noexcept {}
void nativeFgAfterTemporalEye(uint64_t, unsigned, ID3D11Texture2D*) noexcept {}
bool nativeFgGuideEye(ID3D11Texture2D*, unsigned&) noexcept { return false; }
void nativeFgCaptureGuides(ID3D11DeviceContext*, const NativeFgGuides&) noexcept {}
void nativeFgCaptureDoor(uint64_t, unsigned, ID3D11Texture2D*, const uint32_t[4], const float[4]) noexcept {}
void nativeFgCaptureWorld(uint64_t, unsigned, ID3D11Texture2D*, const uint32_t[4]) noexcept {}
void nativeFgInvalidateProducer(uint32_t) noexcept {}
}

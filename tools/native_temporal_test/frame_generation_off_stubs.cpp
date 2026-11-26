#include "../../src/d3d11/frame_generation_coordinator.h"

// These existing temporal/sharpen contract rigs exercise the disabled optional
// capability. The integrated coordinator has its own production-connected rig.
namespace edvr {
void nativeFgSetRequestedMultiplier(uint32_t) noexcept {}
bool nativeFgRequested() noexcept { return false; }
bool nativeFgManualOff() noexcept { return true; }
HRESULT nativeFgAcquire(const EdvrFgRequest*, EdvrFgTable*) noexcept { return E_NOINTERFACE; }
void nativeFgBeginTemporal(const EdvrNativeTemporalFrame&, bool, bool) noexcept {}
void nativeFgTemporalUnavailable(uint64_t) noexcept {}
void nativeFgServiceProducer(uint64_t) noexcept {}
void nativeFgBeforeTemporalEye(uint64_t, unsigned, ID3D11Texture2D*) noexcept {}
void nativeFgAfterTemporalEye(uint64_t, unsigned, ID3D11Texture2D*) noexcept {}
bool nativeFgGuideEye(ID3D11Texture2D*, unsigned&) noexcept { return false; }
void nativeFgCaptureGuides(ID3D11DeviceContext*, const NativeFgGuides&) noexcept {}
void nativeFgCaptureDoor(uint64_t, unsigned, ID3D11Texture2D*, const uint32_t[4], const float[4]) noexcept {}
void nativeFgCaptureWorld(uint64_t, unsigned, ID3D11Texture2D*, const uint32_t[4]) noexcept {}
void nativeFgInvalidateProducer(uint32_t) noexcept {}
}

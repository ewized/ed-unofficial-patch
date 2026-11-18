#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stddef.h>
#include <stdint.h>

#define EDVR_NATIVE_MENU_VERSION_1 1u
#define EDVR_NATIVE_MENU_VERSION_2 2u
// Private menu-only capability, independent of the legacy glitch consumer.
// Acquire/publish/treat run on the bound producer thread. Provider owns the
// context for its lifetime; close retires it without GPU work from any thread.
struct EdvrNativeMenuRequest {
  uint32_t size, version;
  ID3D11Device* gameDevice;
  uint64_t generation;
};
// Poses are row-major rigid 3x4 world transforms; frusta are signed tangents
// {left, right, down, up}. Last arguments: runtime and reference generations.
// All pose pointers null and reference generation zero is CPU invalidation,
// allowed on the XR owner. It closes visibility and keyboard capture.
typedef HRESULT(WINAPI *EdvrNativeMenuPublishPose)(void*, const float*, const float (*)[12], const float (*)[4], uint64_t, uint64_t);
// S_OK supplies an AddRef'd texture and full bounds retaining submitted flips.
// S_FALSE is passthrough; failure outputs are null/zero. Input bounds are UVUV.
typedef HRESULT(WINAPI *EdvrNativeMenuTreatEye)(void*, uint32_t, ID3D11Texture2D*, const float*, ID3D11Texture2D**, float*);
typedef HRESULT(WINAPI *EdvrNativeMenuClose)(void*);
struct EdvrNativeFpsOverlay {
  uint64_t revision;
  uint32_t width, height;
  float yawDegrees, pitchDegrees, distanceMeters, widthMeters;
};
// Sample the same published raster revisions on producer and compositor.
constexpr bool edvrTraceFpsOverlayRevision(uint64_t revision) noexcept {
  return (revision & 3u) == 1u;
}
// Version 2. This CPU-only snapshot is safe on the XR owner. A visible
// raster returns S_OK with metadata, and copies tightly packed straight-alpha
// RGBA8 (sRGB colour) when pixels are supplied and revision differs from
// knownRevision. A null buffer queries metadata. A short buffer receives
// metadata and ERROR_INSUFFICIENT_BUFFER as an HRESULT, with no partial copy.
// S_FALSE means hidden or awaiting the first raster.
typedef HRESULT(WINAPI *EdvrNativeMenuReadFpsOverlay)(void*, uint64_t, EdvrNativeFpsOverlay*, void*, uint32_t);
struct EdvrNativeMenuTable {
  uint32_t size, version;
  void* context;
  EdvrNativeMenuPublishPose publishPose;
  EdvrNativeMenuTreatEye treatEye;
  EdvrNativeMenuClose close;
  EdvrNativeMenuReadFpsOverlay readFpsOverlay;
};
constexpr uint32_t EDVR_NATIVE_MENU_TABLE_V1_SIZE = offsetof(EdvrNativeMenuTable, readFpsOverlay);
#ifdef __cplusplus
extern "C" {
#endif
HRESULT WINAPI edvrAcquireNativeMenu(const EdvrNativeMenuRequest*, EdvrNativeMenuTable*);
bool nativeMenuAvailable();
bool nativeMenuActive(); // graphics-local session presence, independent of pose validity
bool nativeMenuFpsOverlaySupported(); // graphics-local, v2 acquired for the active session
uint64_t nativeMenuRevision(); // graphics-local menu model retirement epoch
// graphics-local: the identity of the menu session. It changes at every acquire and at every close of an active session, and not for
// publishes or CPU invalidations, so a verdict latched for one session cannot outlive it. Zero before the first acquire.
uint64_t nativeMenuSessionId();
// graphics-local: copies the two eye poses (row-major 3x4, the layout publishPose carries) of the last valid publish. False, and out
// untouched, when no pose is held: before the first publish and after an invalidation or a close.
bool nativeMenuEyePoses(float out[2][12]);
#ifdef __cplusplus
}
#endif

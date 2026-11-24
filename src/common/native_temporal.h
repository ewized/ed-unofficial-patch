#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stddef.h>
#include <stdint.h>

#define EDVR_NATIVE_TEMPORAL_VERSION_1 1u
// Acquire/table version only. Frame and projection wire shapes stay at V1.
// A V1 consumer cannot withhold VRS failures, so it never enables batching.
#define EDVR_NATIVE_TEMPORAL_VERSION_2 2u

// Private temporal-only capability. Acquire/treat use the bound game producer;
// begin/projection/invalidate/close are CPU-only and can run on the XR owner.
// No method announces the legacy glitch/withhold consumer. The provider owns
// its context; the host retains the device and module until close completes.
struct EdvrNativeTemporalRequest {
  uint32_t size, version;
  ID3D11Device* gameDevice;
  uint64_t generation;
};
struct EdvrNativeTemporalFrame {
  uint32_t size, version;
  uint64_t generation, referenceGeneration, sequence;
  float head[12], eyeToHead[2][12]; // rigid row-major 3x4, exactly as given to Elite (the eye origins moved by the world scale, common/world_scale.h)
  float frusta[2][4];             // unjittered signed {left,right,down,up}
  uint32_t recommendedWidth, recommendedHeight;  // what this frame was rendered for
  // What the game is told now, max over eyes: during a FOV-trim
  // adoption the game re-creates its targets for this size before a frame of
  // it arrives, while the recommendation above is still the previous ask.
  // 0 when the host does not say (the recommendation is then the answer).
  uint32_t askedWidth, askedHeight;
  // Eye 0's TRUE (located, display) frustum's vertical tangents, magnitudes:
  // what the headset shows, before a FOV trim narrows what the game is
  // told. fix.ui_quality's engine-side panel sizing takes
  // its untrimmed k from it. 0 when the host does not say.
  float trueUp, trueDown;
  // B3 (the scope zoom): 1 when eye 0's frustum in `frusta` is the zoomed window this frame, and then eye 0's frustum without it, {left,right,down,up} as `frusta`. The announced
  // frusta (the panels' size, the terrain cull's cover, the menu, the visor) stay these; DLSS, the jitter and the history follow `frusta`. 0 and zeros: eye 0 is not zoomed.
  uint32_t zoomed;
  float preZoom[4];
};
// The size of the frame before B3 appended its fields: a runtime from before them sends it, and begin() takes it (with no zoom). The two DLLs are copied apart by hand.
#define EDVR_NATIVE_TEMPORAL_FRAME_SIZE_NO_ZOOM ((uint32_t)offsetof(EdvrNativeTemporalFrame, zoomed))
struct EdvrNativeTemporalProjection {
  uint32_t size, version;
  float tangentShift[2][2]; // per-eye {dx,dy}, added to both endpoints on each axis
};
typedef HRESULT(WINAPI *EdvrNativeTemporalBegin)(void*, const EdvrNativeTemporalFrame*, EdvrNativeTemporalProjection*);
// The exact successful matrix query's frame/eye and clip planes. CPU only.
typedef HRESULT(WINAPI *EdvrNativeTemporalNoteProjection)(void*, uint64_t, uint32_t, float, float);
// S_OK: AddRef'd unjittered output and UVUV full bounds retaining input flips.
// S_FALSE: unchanged input (AA off or a safe stand-down); the host must submit
// that image with this frame's jittered FOV. Failure: null/zero outputs.
// Only one successful treatment per eye/sequence. History follows successful
// treatments, never merely WaitGetPoses. Invalidate breaks both histories.
typedef HRESULT(WINAPI *EdvrNativeTemporalTreat)(void*, uint64_t, uint32_t, ID3D11Texture2D*, const float*, ID3D11Texture2D**, float*);
typedef HRESULT(WINAPI *EdvrNativeTemporalInvalidate)(void*);
typedef HRESULT(WINAPI *EdvrNativeTemporalClose)(void*);
// Consume an intentionally omitted eye without feeding bad pixels to history.
// Jump-only omissions wait for the transition-flash fix's verdict; other causes reset it.
typedef HRESULT(WINAPI *EdvrNativeTemporalSkip)(void*, uint64_t, uint32_t, uint32_t, uint32_t);
// CPU-only, owner query: bit 0/1 identifies an incomplete left/right eye in
// this exact source sequence. Either bit requires withholding the stereo pair.
typedef uint32_t(WINAPI *EdvrNativeTemporalVrsFailures)(void*, uint64_t);
// Reserved V2 capability slot. Returns zero with this provider.
typedef uint32_t(WINAPI *EdvrNativeTemporalDiagnostics)(void*);
struct EdvrNativeTemporalTable {
  uint32_t size, version;
  void* context;
  EdvrNativeTemporalBegin beginFrame;
  EdvrNativeTemporalNoteProjection noteProjection;
  EdvrNativeTemporalTreat treatEye;
  EdvrNativeTemporalInvalidate invalidate;
  EdvrNativeTemporalClose close;
  EdvrNativeTemporalSkip skipEye;
  EdvrNativeTemporalVrsFailures vrsFailures;
  EdvrNativeTemporalDiagnostics diagnostics;
};
#define EDVR_NATIVE_TEMPORAL_TABLE_V1_SIZE ((uint32_t)offsetof(EdvrNativeTemporalTable, vrsFailures))
extern "C" HRESULT WINAPI edvrAcquireNativeTemporal(const EdvrNativeTemporalRequest*, EdvrNativeTemporalTable*);

namespace edvr {
// Enroll the renderer's exact context and source sequence, only after a V2
// owner negotiated frame withholding. The generation is an acquisition cookie.
uint64_t nativeTemporalVrsOutputGeneration(ID3D11DeviceContext*,uint64_t sequence) noexcept;
// Metadata only. Returns false for another acquisition, context or sequence.
// Safe on a notifying thread under the engine lock; never calls the driver.
bool nativeTemporalInvalidateVrsEye(ID3D11DeviceContext*,unsigned eye,uint64_t generation,uint64_t sequence) noexcept;
}

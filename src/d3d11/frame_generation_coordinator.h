#pragma once
#include "../common/native_frame_generation.h"
#include "../common/native_temporal.h"
#include "frame_generation_auto_policy.h"
#include <cstdint>

namespace edvr {
constexpr uint32_t NativeFgUnsupportedBounds = 0x10001u;
// CPU-only configuration/public capability. The export itself is owned by the
// graphics capability dispatcher; no Config or OpenXR dependency enters here.
void nativeFgSetRequestedMultiplier(uint32_t multiplier) noexcept;
void nativeFgSetAutoMode() noexcept;
bool nativeFgAutoRequested() noexcept;
void nativeFgAutoObserve(const fg_auto::Sample&) noexcept;
fg_auto::Status nativeFgAutoStatus() noexcept;
bool nativeFgRequested() noexcept;
// Explicit Off excludes Auto's native cadence while it measures or recovers.
bool nativeFgManualOff() noexcept;
// Passive CPU snapshot for menu/monitor consumers; no resource creation or worker startup.
// Returns false without a live context, retaining the configured request/state for a waiting UI.
bool nativeFgStatusSnapshot(EdvrFgStatus*) noexcept;
HRESULT nativeFgAcquire(const EdvrFgRequest*, EdvrFgTable*) noexcept;

// These hooks are part of the established native temporal producer. OFF exits
// before COM, allocation, clocks or worker startup. beginTemporal is CPU-only;
// all eye/guide/door hooks run on the validated temporal render owner.
void nativeFgBeginTemporal(const EdvrNativeTemporalFrame&, bool reset, bool supported) noexcept;
void nativeFgTemporalUnavailable(uint64_t generation) noexcept;
// The active temporal render owner services fence-backed producer retirement
// even while the session is presenting untreated native frames.
void nativeFgServiceProducer(uint64_t generation) noexcept;
void nativeFgBeforeTemporalEye(uint64_t sequence, unsigned eye, ID3D11Texture2D* source) noexcept;
void nativeFgAfterTemporalEye(uint64_t sequence, unsigned eye, ID3D11Texture2D* output) noexcept;
bool nativeFgGuideEye(ID3D11Texture2D* exactSource, unsigned& anatomicalEye) noexcept;

struct NativeFgGuides {
    unsigned eye = 2, width = 0, height = 0, outputWidth = 0, outputHeight = 0;
    ID3D11Texture2D* depth = nullptr;
    ID3D11Texture2D* motion = nullptr;
    ID3D11Texture2D* source = nullptr; // exact native before-eye input identity
    unsigned internalEye = 2; // diagnostic only, never anatomical authority
    float jitterPixels[2]{};
    float frustum[4]{}, previousFrustum[4]{}; // signed L,R,D,U; no jitter
    float sceneProjectionA = 0, sceneProjectionB = 0;
    float previousProjectionA = 0, previousProjectionB = 0;
    float cameraNow[12]{}, cameraPrevious[12]{}; // exact mapped game view->world, +Z forward
    bool reset = false, depthAvailable = false, cameraValid = false, originJump = false;
    // Read-only rejection evidence from the actual successful SR boundary.
    // Bits 0..18: depthMotion, depthSrv, headTrans, haveDepth, measuredProjection,
    // worldOn, rowsOwn, curValid, prevValid, headNoted, headFrame, eyesFrame,
    // currentEyeKnown, previousEyeKnown, floorRefused, originJump, worldCandidate,
    // worldValid, rowsFollow. No bit grants admission or substitutes a pose.
    uint32_t provenance = 0, sceneFrame = 0, headFrame = 0, eyesFrame = 0, sceneDraws = 0;
    int selectedEye = -1, previousSelectedEye = -1;
    float rawProjection[2]{};
};
// Called only in the successful full-eye SR branch, before guide reuse.
void nativeFgCaptureGuides(ID3D11DeviceContext*, const NativeFgGuides&) noexcept;
// Exact temporalOut after DoorSeen, before RCAS and before native UI mutation.
void nativeFgCaptureDoor(uint64_t sequence, unsigned eye, ID3D11Texture2D*,
    const uint32_t region[4], const float layerUv[4]) noexcept;
// Actual selected native world color after RCAS (or its real passthrough),
// immediately before uiLayerComposite can mutate the native image in place.
void nativeFgCaptureWorld(uint64_t sequence, unsigned eye, ID3D11Texture2D*,
    const uint32_t region[4]) noexcept;
void nativeFgInvalidateProducer(uint32_t reason) noexcept;
#ifdef EDVR_FRAME_GENERATION_COORDINATOR_TEST
unsigned nativeFgTestProtocol(unsigned* checks = nullptr) noexcept;
#endif
} // namespace edvr

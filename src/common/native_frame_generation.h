#pragma once
#include "native_fg_multiplier.h"
#include <windows.h>
#include <d3d11.h>
#include <stddef.h>
#include <stdint.h>

#define EDVR_NATIVE_FRAME_GENERATION_VERSION_1 1u
#define EDVR_FG_MAX_PHASES 4u
#define EDVR_FG_BATCH_VERSION_2 2u
#define EDVR_FG_BATCH_VERSION_3 3u
#define EDVR_FG_BATCH_VERSION_4 4u
#define EDVR_FG_UI_NATIVE_COVERAGE 1u
// Provider already cut this history on admitted, bilateral protected HUD motion.
// The XR owner may acknowledge that cut after retiring its own import state.
#define EDVR_FG_NATIVE_COVERAGE 0x10002u

// Optional capability of a trusted paired graphics module. No STL or SDK
// objects cross this boundary. All handles below are borrowed, process-local
// NT handles; retaining a batch keeps them valid. The host retains the provider
// module and game device until close and every batch release have completed.
struct EdvrFgView {
  float orientation[4];  // OpenXR quaternion x,y,z,w, in the named reference space
  float position[3];
  float fov[4];          // radians: angleLeft,angleRight,angleUp,angleDown
};
struct EdvrFgSource {
  uint32_t size, version;
  uint64_t generation, referenceGeneration, configGeneration, sequence;
  int64_t poseDisplayTime; // original XrTime; never inferred from a later tick
  EdvrFgView views[2];    // physical located views, not world-scaled game eyes
  uint32_t width, height;
};
struct EdvrFgRequest {
  uint32_t size, version;
  ID3D11Device* gameDevice;
  uint64_t generation;
};
enum EdvrFgState : uint32_t {
  EDVR_FG_OFF=0, EDVR_FG_ARMING=1, EDVR_FG_READY=2,
  EDVR_FG_UNAVAILABLE=3, EDVR_FG_FAILED=4, EDVR_FG_CLOSING=5
};
struct EdvrFgStatus {
  uint32_t size, version;
  uint32_t requestedMultiplier, activeMultiplier; // exactly 0, 2, 3 or 4
  uint32_t state, lastFailure;
  uint64_t generation, configGeneration, historyGeneration;
  uint64_t sourcePairs, generatedPairs, suppressedPairs, refusedPairs, busyPairs;
  uint64_t latestSourceSequence, latestReadySequence;
};
struct EdvrFgBatch {
  uint32_t size, version;
  void* token;
  uint64_t generation, referenceGeneration, configGeneration, historyGeneration;
  uint64_t previousSequence, currentSequence;
  int64_t previousPoseDisplayTime, currentPoseDisplayTime;
  EdvrFgView previousViews[2], currentViews[2];
  uint32_t multiplier, generatedCount, phaseMask;
  uint32_t width, height, format;
  uint32_t adapterLuidLow;
  int32_t adapterLuidHigh;
  // Phase0 is the current native endpoint with original UI. Generated phases
  // are world-only. V2 UI coverage has its own endpoint-B pose, applied by XR.
  // Phases1..generatedCount are
  // interpolations in SDK index order between previous and current. A mask bit
  // is set only for a completely ready stereo pair. Native-only warmup/fallback
  // batches have phaseMask=1; no unavailable generated image is invented.
  HANDLE textures[EDVR_FG_MAX_PHASES][2];
  HANDLE readyFence;
  uint64_t readyValue;
  // Batch v2: native HUD coverage, including its translucent backing, is a
  // separate projection at currentViews. Generated textures contain world only.
  // Native phase0 retains its original complete image for safe fallback.
  HANDLE uiTextures[2];
  uint32_t uiFlags, reserved;
  // V3: world-only native endpoint for the same two-layer projection path
  // as generated phases. textures[0] still contains the full native image
  // for bootstrap/fade/fallback, never a partially reconstructed surrogate.
  HANDLE nativeWorldTextures[2];
  // V4: process-monotonic, nonzero identity of this immutable output-slot
  // texture allocation set. Any added/replaced texture gets a fresh identity.
  // Numeric NT handle reuse never identifies an allocation incarnation.
  uint64_t allocationId;
};

// Legacy callers own only their version's prefix. Never read/write past it.
constexpr uint32_t edvrFgBatchSize(uint32_t version) noexcept {
  return version==EDVR_FG_BATCH_VERSION_2?uint32_t(offsetof(EdvrFgBatch,nativeWorldTextures)):
    version==EDVR_FG_BATCH_VERSION_3?uint32_t(offsetof(EdvrFgBatch,allocationId)):
    version==EDVR_FG_BATCH_VERSION_4?uint32_t(sizeof(EdvrFgBatch)):0u;
}
constexpr bool edvrFgBatchProtocol(uint32_t size,uint32_t version) noexcept {
  return size&&size==edvrFgBatchSize(version);
}

typedef HRESULT (WINAPI *EdvrFgGetStatus)(void*, EdvrFgStatus*);
// CPU-only, XR owner, before releasing the native source credit. This metadata
// must match the subsequent authoritative temporal frame and both eye captures.
typedef HRESULT (WINAPI *EdvrFgBeginSource)(void*, const EdvrFgSource*);
// CPU-only/nonblocking, XR owner. S_OK transfers one retained batch token;
// S_FALSE means no new complete batch. No game-context access or GPU waits.
typedef HRESULT (WINAPI *EdvrFgPoll)(void*, EdvrFgBatch*);
typedef HRESULT (WINAPI *EdvrFgRetain)(void* token);
typedef void (WINAPI *EdvrFgRelease)(void* token);
// Exactly one retirement decision per batch. Retire follows ALL consumer reads,
// including copies of every phase to consumer-owned storage. Discard is only
// for proof that no consumer GPU read was queued. Release is not retirement.
typedef HRESULT (WINAPI *EdvrFgRetire)(void* token, HANDLE consumerFence, uint64_t value);
typedef HRESULT (WINAPI *EdvrFgDiscard)(void* token);
typedef HRESULT (WINAPI *EdvrFgInvalidate)(void*, uint32_t reason);
// The XR owner stops producer admission and drains exported batches before close.
// E_PENDING means the worker or its GPU submissions are still retiring. Retry
// on the same handle and keep the game device and provider pinned until S_OK.
typedef HRESULT (WINAPI *EdvrFgClose)(void*);
struct EdvrFgTable {
  uint32_t size, version;
  void* context;
  EdvrFgGetStatus getStatus;
  EdvrFgBeginSource beginSource;
  EdvrFgPoll poll;
  EdvrFgRetain retain;
  EdvrFgRelease release;
  EdvrFgRetire retire;
  EdvrFgDiscard discard;
  EdvrFgInvalidate invalidate;
  EdvrFgClose close;
};
typedef HRESULT (WINAPI *EdvrAcquireNativeFrameGeneration)(const EdvrFgRequest*, EdvrFgTable*);
extern "C" HRESULT WINAPI edvrAcquireNativeFrameGeneration(const EdvrFgRequest*, EdvrFgTable*);

#ifdef _WIN64
static_assert(sizeof(EdvrFgView)==44, "FG view ABI");
static_assert(sizeof(EdvrFgRequest)==24, "FG request ABI");
static_assert(sizeof(EdvrFgTable)==88, "FG table ABI");
#endif

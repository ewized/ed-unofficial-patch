#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;
struct ID3D11Device;
struct ID3D11Texture2D;

namespace edvr {
enum class NativeVrsFailure:uint32_t {
    None,Disabled,InvalidRadii,MagicPresent,ContextRefused,ThreadRefused,
    FrameRefused,NvapiModule,NvapiPath,NvapiQuery,NvapiOwner,NvapiPin,
    NvapiInitialize,DriverCall,TargetRefused,MapCreate,GazeInvalid,GazeStale,GazeNotReady
};
// Metadata only. No gaze coordinates, sensor timestamps, or rate-map bytes.
struct NativeVrsStatus {
    bool configured=false,driverReady=false,refused=false,magicAbsent=false;
    bool vendorStateKnown=false,fullRateAcknowledged=false,coarseAcknowledged=false;
    bool sensorTimestampUnavailable=true;
    NativeVrsFailure failure=NativeVrsFailure::Disabled;
    int32_t lastDriverStatus=0;
    uint64_t ticks=0,providerPolls=0,validPairs=0,invalidPairs=0,staleRefusals=0;
    uint64_t worldCalls=0,worldAccepted=0,fullRateCalls=0,invalidations=0;
    uint64_t expandedMaterialAccepted[2]{};
    uint64_t mapUpdates[2]{},mapBytes=0,rateSets=0,resourceSets=0,driverErrors=0;
    uint64_t contextRefusals=0,frameRefusals=0,targetRefusals=0;
    float sharpRadius=.15f,transitionRadius=.45f;
};

// Default OFF. May be configured off-thread; a new configuration requires a
// new owning-frame Tick before coarse shading is eligible.
void nativeVrsConfigure(bool enabled,float hqRadius=.15f,float midRadius=.45f) noexcept;
void nativeVrsConfigureFocus(bool enabled,const char* profile) noexcept;
// The live toggle adds a diagnostic only to eligible native eye images.
void nativeVrsConfigureHighlight(bool enabled) noexcept;
bool nativeVrsHighlightRequested() noexcept;
// Borrow the map and its immediate context for this eye on the owning thread.
// Returns false if this frame had no acknowledged native world map, the
// geometry differs, or the context cannot be restored to full rate.
bool nativeVrsHighlightMap(ID3D11Device*,uint64_t sequence,unsigned eye,
    unsigned width,unsigned height,ID3D11DeviceContext*&,ID3D11Texture2D*&) noexcept;
// Borrow the native temporal input geometry for the submitted sequence. Only
// a successful full, unflipped eye is available to the highlight pass.
bool nativeTemporalVrsInput(uint64_t sequence,unsigned eye,unsigned& width,
    unsigned& height,float (&jitter)[2]);
// Returns a private tinted copy with one reference for the caller. A null
// result leaves the clean eye ready for the normal UI and submit paths.
ID3D11Texture2D* nativeVrsHighlightEye(uint64_t sequence,unsigned eye,
    ID3D11Texture2D*,const uint32_t region[4]) noexcept;
// Cheap draw gate. Remains true after OFF once a driver was initialized, so
// full-rate/reset boundaries remain reachable.
extern std::atomic<bool> g_nativeVrsLive;
inline bool nativeVrsLive() noexcept {return g_nativeVrsLive.load(std::memory_order_relaxed);}
// Coarse-preparation hint only. OFF skips native classification/projection
// work, but nativeVrsLive still owns full-rate/reset boundaries.
// nativeVrsBeforeWorld performs authoritative config/frame/provider admission.
bool nativeVrsCoarseRequested() noexcept;
// Exactly one new positive ordinal from the owning immediate Present boundary.
// Duplicate, regressed, zero, foreign-context and foreign-thread calls do not
// poll gaze or advance the frame. No DllMain call is permitted.
void nativeVrsTick(ID3D11DeviceContext*,uint64_t frameOrdinal,bool allowProducer=true) noexcept;
struct NativeVrsProjection {
    uint64_t sequence{};
    float frustum[4]{}; // left,right,down,up tangents, before jitter
    float shift[2]{};
};
// Caller identified an original material draw, a settled scene depth eye and
// a full-eye scene color target. At issue, this validates actual attachments,
// depth test, raster state, viewport, provider and frame. A true result means
// the driver acknowledged the map. expandedMaterial tags newly eligible draws
// for coverage counters only; it does not affect admission.
bool nativeVrsBeforeWorld(ID3D11DeviceContext*,unsigned eye,
    ID3D11RenderTargetView*,ID3D11DepthStencilView*,const NativeVrsProjection* =nullptr,
    bool expandedMaterial=false) noexcept;
// Check the current eye and prepare its map before a full-rate motion issue.
// A false result leaves the original, combined single-draw path in charge.
bool nativeVrsWitnessReady(ID3D11DeviceContext*) noexcept;
bool nativeVrsPrepareWitness(ID3D11DeviceContext*,unsigned eye,
    ID3D11RenderTargetView*,ID3D11DepthStencilView*,const NativeVrsProjection&) noexcept;
// All UI, unknown passes and reissues must pass a full-rate boundary. Invalidate
// on ExecuteCommandList/ClearState/foreign ownership; never trust a prior set.
void nativeVrsFullRate(ID3D11DeviceContext*) noexcept;
void nativeVrsInvalidate(ID3D11DeviceContext*) noexcept;
NativeVrsStatus nativeVrsStatus() noexcept;
// Temporary, default-off, bounded actual-world PS measurement. It never
// enables the legacy observer or installs an NVAPI detour.
extern std::atomic<bool> g_nativeVrsPsMeasureLive;
inline bool nativeVrsPsMeasureLive()noexcept{return g_nativeVrsPsMeasureLive.load(std::memory_order_relaxed);}
using NativeVrsPsSink=void(*)(const char*);
void nativeVrsPsMeasureConfigure(bool,NativeVrsPsSink)noexcept;
struct NativeVrsPsDraw {
    uint64_t vs=0,ps=0;unsigned eye=2;
    uint32_t count=0,instances=0,start=0,startInstance=0;int32_t base=0;char kind='?';
    ID3D11RenderTargetView* target=nullptr;ID3D11DepthStencilView* depth=nullptr;
};
struct NativeVrsPsToken {unsigned candidate=16,slot=6,phase=0;uint64_t id=0,map=0;bool full=false,active=false;};
NativeVrsPsToken nativeVrsPsBefore(ID3D11DeviceContext*,const NativeVrsPsDraw&)noexcept;
void nativeVrsPsAfter(ID3D11DeviceContext*,const NativeVrsPsToken&)noexcept;
#ifdef EDVR_NATIVE_VRS_TEST
namespace native_vrs::testing {
enum class DriverOp:uint8_t {
    Full,Left,Right,Invalidate,NextFrame,FailRate,FailResource,Reconfigure,
    Off,TickNext,TickCurrent,TickZero,TickRegressed,TickForeignContext,TickForeignThread,FlatTickNext
};
struct DriverCall {
    bool rates=false,enabled=false;
    uint32_t version=0,count=0,palette[16]{};
    uintptr_t context=0,resource=0;
    int result=0;
};
struct DriverPublication {
    bool vendorKnown=false,fullKnown=false,coarseKnown=false;
};
struct DriverResult {
    DriverCall calls[64]{};
    DriverPublication publications[64]{};
    size_t publicationCount=0;
    size_t count=0;
    bool fullKnown=false,coarseKnown=false,refused=false;
    bool configured=false,live=false,coarseRequested=false;
    uint64_t frameOrdinal=0,providerPolls=0;
    NativeVrsFailure failure=NativeVrsFailure::None;
};
// CPU callbacks replace only the two driver entry points. Production
// descriptor construction, cache, activation and recovery code execute.
DriverResult runDriverFixture(const DriverOp*,size_t);
}
#endif
} // namespace edvr

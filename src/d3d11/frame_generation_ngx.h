#pragma once
#include <d3d12.h>
#include "frame_generation_camera.h"
#include <cstdint>
#include <memory>
#if defined(EDVR_FRAME_GENERATION_NGX_TEST) && defined(EDVR_HAVE_NGX)
#include "nvsdk_ngx_params_dlssg.h"
#endif

namespace edvr {
enum class FrameGenerationNgxFailure:uint32_t {
    None,NotInitialized,InvalidConfiguration,WrongThread,Capability,MultiplierUnsupported,
    Device,NgxInitialize,NgxParameters,NgxCreate,NgxEvaluate,InvalidGuides,
    Sequence,Busy,Queue,Timeout,DeviceRemoved,OutstandingLease,SdkUnavailable
};
enum class FrameGenerationNgxInitStage:uint32_t {
    NotStarted,NgxInitialize,Capabilities,GraphicsSetup,AllocateParameters,
    ScratchQuery,ScratchAllocate,FeatureCreate,SubmitCreate,Ready
};
enum class FrameGenerationNgxCreationMode:uint32_t {GenericScratchQuery,OfficialFgHelper};
const char* frameGenerationNgxCreationModeName(FrameGenerationNgxCreationMode)noexcept;
enum class FrameGenerationNgxSuppressionLayout:uint32_t {PrivatePerIndex,SharedNativePair};
const char* frameGenerationNgxSuppressionLayoutName(FrameGenerationNgxSuppressionLayout)noexcept;
const char* frameGenerationNgxInitStageName(FrameGenerationNgxInitStage)noexcept;
struct FrameGenerationNgxCreateCall {
    bool scratchCalled=false,createCalled=false,handleReturned=false;
    uint32_t scratchResult=0,createResult=0;
    uint64_t scratchBytes=0;
};
struct FrameGenerationNgxInit {
    ID3D12Device* device=nullptr;
    unsigned width=0,height=0,guideWidth=0,guideHeight=0,multiplier=2;
    DXGI_FORMAT colorFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
    // Explicit diagnostic routes, never an automatic fallback. The generic
    // route preserves every scratch-query error; the official FG helper route
    // does not issue that query and reports CreateFeature independently.
    FrameGenerationNgxCreationMode creationMode=FrameGenerationNgxCreationMode::GenericScratchQuery;
    // Explicit trial: one driver-written native-pair decision per eye, with
    // FF initialization once before index1 and fenced per-index readbacks.
    FrameGenerationNgxSuppressionLayout suppressionLayout=FrameGenerationNgxSuppressionLayout::PrivatePerIndex;
    const wchar_t* featureDirectory=nullptr;
    const wchar_t* dataDirectory=nullptr;
    // Exactly one context mode. Private is the caller's verified ownership of
    // this NGX D3D12 context, not proof inferred from D3D12CreateDevice (which
    // may return the adapter's existing device). Borrowed requires the known
    // owner's successful D3D12 Init. Neither mode permits global NGX Shutdown.
    bool privateNgxDevice=false,ngxAlreadyInitialized=false;
    // Opt in at feature creation. Every later eye must carry both UI inputs.
    bool uiRecomposition=false;
    uint32_t timeoutMs=2000;
};
struct FrameGenerationNgxEye {
    ID3D12Resource* color=nullptr;
    ID3D12Resource* depth=nullptr;
    ID3D12Resource* motion=nullptr;
    D3D12_RESOURCE_STATES colorState=D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES depthState=D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES motionState=D3D12_RESOURCE_STATE_COMMON;
    // Measured metadata, copied into persistent per-evaluation SDK storage.
    // Matrices exclude TAA jitter; jitterOffset is clip-space, mvecScale
    // normalizes the supplied motion encoding. No inferred camera defaults.
    FrameGenerationCamera camera{};
    // Recomposition requires full native color plus distinct, eye-sized
    // HUD-less and premultiplied UI textures from that same source.
    ID3D12Resource* hudless=nullptr;
    ID3D12Resource* ui=nullptr;
    D3D12_RESOURCE_STATES hudlessState=D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES uiState=D3D12_RESOURCE_STATE_COMMON;
};
struct FrameGenerationNgxInput {
    uint64_t sequence=0,historyGeneration=0;
    FrameGenerationNgxEye eyes[2];
    ID3D12Fence* producerFence=nullptr;
    uint64_t producerValue=0;
    std::shared_ptr<void> inputLease;
    // Caller proves full-eye color and paired depth/MV/camera from this exact
    // native sequence. This flag is a contract, not a backend observation.
    bool pairedFullEyeGuides=false;
};
enum class FrameGenerationNgxCompletion {Empty,Pending,Ready,Suppressed,Expired,DeviceRemoved,FailedEvaluation,Stale};
struct FrameGenerationNgxLease;
struct FrameGenerationNgxSuppressionSnapshot {
    bool fenced=false;
    uint64_t sequence=0;
    unsigned generatedCount=0;
    uint32_t historyResetReasons=0;
    FrameGenerationNgxSuppressionLayout layout=FrameGenerationNgxSuppressionLayout::PrivatePerIndex;
    uint8_t raw[2][3][4]{};
    uint32_t evaluationResult[2][3]{};
    uintptr_t resourceIdentity[2][3]{};
};
struct FrameGenerationNgxTicket {
    uint64_t sequence=0,completionValue=0;
    unsigned generatedCount=0;
    bool evaluationSucceeded=false;
    bool uiRecomposed=false; // Ready generated eyes already contain their HUD.
    uint32_t historyResetReasons=0;
    ID3D12Fence* completionFence=nullptr;
    ID3D12Resource* generated[2][3]{};
    D3D12_RESOURCE_STATES outputState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    std::shared_ptr<FrameGenerationNgxLease> lease;
    // A nonempty lease means ownership was accepted, including a failed GPU
    // submission/evaluation. Never cancel the producer lease in that case.
    // A valid completionFence/value also retires failed evaluations safely.
    // Lock-free with respect to the backend worker. Never waits or enters XR.
    FrameGenerationNgxCompletion poll()const noexcept;
    // Diagnostic only: raw four-byte snapshots after the actual completion
    // fence, including suppressed/expired results. Never changes eligibility.
    bool suppressionSnapshot(FrameGenerationNgxSuppressionSnapshot&)const noexcept;
};
struct FrameGenerationNgxStatus {
    bool initialized=false,available=false,failed=false,quarantined=false;
    unsigned knownMaximumMultiplier=0; // 0 unknown, 1 unavailable, 2..4 supported
    unsigned requestedMultiplier=0,maximumGenerated=0;
    FrameGenerationNgxCreationMode creationMode=FrameGenerationNgxCreationMode::GenericScratchQuery;
    FrameGenerationNgxSuppressionLayout suppressionLayout=FrameGenerationNgxSuppressionLayout::PrivatePerIndex;
    FrameGenerationNgxFailure failure=FrameGenerationNgxFailure::NotInitialized;
    uint32_t lastNgxResult=0;
    HRESULT lastHresult=S_OK;
    FrameGenerationNgxInitStage initStage=FrameGenerationNgxInitStage::NotStarted;
    FrameGenerationNgxInitStage failedInitStage=FrameGenerationNgxInitStage::NotStarted;
    unsigned initEye=2,failedInitEye=2;
    FrameGenerationNgxCreateCall creation[2];
    uint64_t submitted=0,evaluations=0,completed=0,busy=0,resets=0;
};
struct FrameGenerationNgxQualification;
#if defined(EDVR_FRAME_GENERATION_NGX_TEST) && defined(EDVR_HAVE_NGX)
struct FrameGenerationNgxQualificationFixture {
    enum class Fault { None, WrongOwner, WrongEvaluation, WrongSequence, WrongGeneration,
        ReusedSlot, Expired, PinnedLatest, PinnedEarlier, Unsafe, MissingReadback };
    Fault fault = Fault::None;
};
struct FrameGenerationNgxQualificationObservation {
    bool reset = false, repeatRefused = false, preservedSlots = false, retiredInputs = false;
    uint64_t sequence = 0, history = 0, completed = 0;
    uint32_t firstLiveResetReasons = 0;
};
#endif
class FrameGenerationNgx {
 public:
    FrameGenerationNgx()noexcept=default;
    ~FrameGenerationNgx();
    FrameGenerationNgx(const FrameGenerationNgx&)=delete;
    FrameGenerationNgx& operator=(const FrameGenerationNgx&)=delete;
    // Generation-thread ownership; creates a private NORMAL direct queue.
    // No DllMain call. Returns after recording/submitting feature creation.
    bool initialize(const FrameGenerationNgxInit&);
    // Nonblocking producer fence wait on the private queue. No native render
    // queue wait, no shader substitute, and no silent multiplier downgrade.
    FrameGenerationNgxTicket submit(const FrameGenerationNgxInput&);
    // Generation thread only, nonblocking. Release completed input pins even
    // when the bounded producer ring cannot supply another submit.
    bool collect()noexcept;
    // Generation owner only. The readback queue must wait on the selected
    // ticket and signal after copying both generated images. Registration
    // binds those fences without keeping the ticket's output slots alive.
    std::shared_ptr<const FrameGenerationNgxQualification> recordQualificationReadback(
        const FrameGenerationNgxTicket&, ID3D12Fence* readback, uint64_t readbackValue) noexcept;
    // The caller must first verify the pixels from this exact readback with
    // its live qualification guard and give up all external synthetic tickets.
    // Refuses stale ownership or unsafe work. Keeps queues, fences and slots.
    bool resetAfterQualification(const std::shared_ptr<const FrameGenerationNgxQualification>&) noexcept;
    // Generation owner only. An unproved qualification consumer can still
    // reference generated output after the NGX evaluation fence completes.
    void quarantineQualification() noexcept;
    // Generation thread only, bounded 0..5000ms. Timeout retains all GPU-owned
    // resources/parameters. No global/shared NGX Shutdown is ever called.
    bool drain(uint32_t timeoutMs=0)noexcept;
    FrameGenerationNgxStatus status()const noexcept;
#if defined(EDVR_FRAME_GENERATION_NGX_TEST) && defined(EDVR_HAVE_NGX)
    // Actual production feature-preparation path, with link-time CPU NGX
    // substitutes. The fixture owns the parameter object returned by its stub.
    static FrameGenerationNgxStatus testFeaturePreparation(unsigned eye=0,
        FrameGenerationNgxCreationMode creationMode=FrameGenerationNgxCreationMode::GenericScratchQuery,
        bool uiRecomposition=false);
    // Actual slot collector with CPU fence values; no device or NGX calls.
    static bool testCollectInputs(uint64_t completed,const uint64_t (&values)[3],
        unsigned expiredMask,std::shared_ptr<void> (&inputs)[3]);
    // Drive terminal drain with an independently retained ticket keeping the
    // input pin visible after the backend releases its own slot reference.
    static bool testDrainInput(ID3D12Fence*, uint64_t required, bool unsafe, bool expired,
        std::shared_ptr<void>& input);
    // CPU substitutes for the two real fence objects, with an actual slot
    // ring, receipt identity and SDK history reset-reason calculation.
    static FrameGenerationNgxQualificationObservation testQualificationReset(
        ID3D12Fence* evaluation, ID3D12Fence* readback, FrameGenerationNgxQualificationFixture fixture);
#endif
 private:
    struct Impl;
    Impl* impl_=nullptr;
};

#if defined(EDVR_FRAME_GENERATION_NGX_TEST) && defined(EDVR_HAVE_NGX)
// Uses the same official helper and persistent option storage as production;
// the test binary supplies CPU-only public NGX entry-point stubs.
unsigned frameGenerationNgxTestContextPaths(const FrameGenerationNgxInit&,NVSDK_NGX_Result&);
FrameGenerationNgxTicket frameGenerationNgxTestTimedTicket(ID3D12Fence*);
bool frameGenerationNgxTestEvaluation(unsigned multiplier,unsigned index,
    NVSDK_NGX_Parameter*,NVSDK_NGX_DLSSG_Opt_Eval_Params* persistent,ID3D12Resource* disableOverride=nullptr,
    const FrameGenerationCamera* camera=nullptr,const FrameGenerationNgxEye* source=nullptr);
#endif
} // namespace edvr

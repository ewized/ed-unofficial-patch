#include "frame_generation_coordinator.h"
#include "frame_generation_color.h"
#include "frame_generation_interop.h"
#include "frame_generation_ngx.h"
#ifdef EDVR_FRAME_GENERATION_COORDINATOR_TEST
#include "frame_generation_ngx_policy.h"
#endif
#include "frame_generation_output.h"
#include "frame_generation_qualification_runner.h"
#include "native_reflex.h"
#include "ui_layer.h"
#include "../common/log.h"
#include "../common/native_fg_coverage.h"
#include "../common/mirror_location.h"
#include "../common/temporal_math.h"
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <cwchar>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef EDVR_FRAME_GENERATION_COORDINATOR_TEST
#include "../openxr/native_fg_admission.h"
#include <cstdio>
#endif

namespace edvr {
namespace {
using Microsoft::WRL::ComPtr;
using InputLease = std::shared_ptr<FrameGenerationInteropLease>;
enum Failure : uint32_t {None, SourceMismatch, TemporalMismatch, Unsupported, MissingPrevious,
    Camera=EDVR_FG_CAMERA_REFUSAL, Guides, Ui, Interop, Busy, Ngx, Output, Device, Closing};
std::atomic<uint32_t> requested{0};
std::atomic<uint64_t> configuration{1};
std::atomic<bool> automatic{false};
constexpr wchar_t kUiRecompositionOverride[] = L"EDUP_TEST_FG_UI_RECOMPOSITION";
// Unfenced qualification commands may outlive the selected configuration or
// coordinator. This process cannot prove safe reuse of that GPU path.
std::atomic<bool> qualificationQuarantined{false};
std::mutex autoMutex;
fg_auto::Policy autoPolicy;
std::mutex ngxContextMutex;
// Exactly the contexts this integration successfully initialized. Retained
// for process life: the backend never performs a global NGX shutdown.
std::array<ID3D12Device*, 16> ownedNgxDevices{};
unsigned ownedNgxCount = 0;

struct Matrix {double m[4][4]{};};
bool finite(const float* values, size_t count) noexcept {
    for (size_t i = 0; i < count; ++i) if (!std::isfinite(values[i])) return false;
    return true;
}
Matrix multiply(const Matrix& a, const Matrix& b) noexcept {
    Matrix out;
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
        for (unsigned k = 0; k < 4; ++k) out.m[r][c] += a.m[r][k] * b.m[k][c];
    return out;
}
bool inverse(const Matrix& input, Matrix& out) noexcept {
    double a[4][8]{};
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) {
        a[r][c] = input.m[r][c]; a[r][c + 4] = r == c ? 1.0 : 0.0;
    }
    for (unsigned c = 0; c < 4; ++c) {
        unsigned best = c;
        for (unsigned r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[best][c])) best = r;
        if (!std::isfinite(a[best][c]) || std::fabs(a[best][c]) < 1e-12) return false;
        if (best != c) for (unsigned k = 0; k < 8; ++k) std::swap(a[best][k], a[c][k]);
        const double divisor = a[c][c];
        for (double& value : a[c]) value /= divisor;
        for (unsigned r = 0; r < 4; ++r) if (r != c) {
            const double factor = a[r][c];
            for (unsigned k = 0; k < 8; ++k) a[r][k] -= factor * a[c][k];
        }
    }
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) {
        if (!std::isfinite(a[r][c + 4]) || std::fabs(a[r][c + 4]) > 1e30) return false;
        out.m[r][c] = a[r][c + 4];
    }
    return true;
}
bool projection(const float tan[4], float a, float b, Matrix& out) noexcept {
    if (!finite(tan, 4) || !std::isfinite(a) || !std::isfinite(b) || b <= 0 || a > 0 ||
        !(tan[0] < tan[1] && tan[2] < tan[3])) return false;
    const double width = double(tan[1]) - tan[0], height = double(tan[3]) - tan[2];
    out.m[0][0] = 2.f / width; out.m[0][2] = (double(tan[1]) + tan[0]) / width;
    out.m[1][1] = 2.f / height; out.m[1][2] = (double(tan[3]) + tan[2]) / height;
    // For RH view Z=-distance, depth=A+B/distance. This is the actual
    // reversed scene encoding, including A=0's measured infinite far plane.
    out.m[2][2] = -a; out.m[2][3] = b; out.m[3][2] = -1.f;
    for (const auto& row : out.m) for (double value : row)
        if (!std::isfinite(value) || std::fabs(value) > (std::numeric_limits<float>::max)()) return false;
    return true;
}
Matrix cameraPose(const float rows[12]) noexcept {
    Matrix out;
    // Existing temporalWorldFromRows owns the game's +Z/OpenXR -Z convention:
    // a pose changes basis with F*pose*F, F=diag(1,1,-1,1).
    for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 4; ++c)
        out.m[r][c] = rows[r * 4 + c] * (r == 2 ? -1.f : 1.f) * (c == 2 ? -1.f : 1.f);
    out.m[3][3] = 1;
    return out;
}
void sdkMatrix(const Matrix& value, float (&out)[4][4]) noexcept {
    // Public NGX constants use row-vector matrices; the owner math above
    // uses column vectors. Retained options copy these exact matrix values.
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) out[r][c] = static_cast<float>(value.m[c][r]);
}
bool camera(const NativeFgGuides& guide, FrameGenerationCamera& out, uint32_t* refusal = nullptr) noexcept {
    // Diagnostic bits preserve every original admission predicate. Stages
    // 512..16384 identify the first failed matrix/projection construction.
    const uint32_t facts = (!guide.cameraValid ? 1u : 0u) | (guide.originJump ? 2u : 0u) |
        (!guide.depthAvailable ? 4u : 0u) | (!guide.width || !guide.height ? 8u : 0u) |
        (!temporalRowsAreRotation(guide.cameraNow) ? 16u : 0u) |
        (!temporalRowsAreRotation(guide.cameraPrevious) ? 32u : 0u) |
        (!finite(guide.cameraNow, 12) ? 64u : 0u) | (!finite(guide.cameraPrevious, 12) ? 128u : 0u) |
        (!finite(guide.jitterPixels, 2) ? 256u : 0u);
    const auto reject = [&](uint32_t value) {if (refusal) *refusal = value; return false;};
    if (refusal) *refusal = 0;
    if (facts) return reject(facts);
    Matrix p, previousP, unproject, relative, clipPrevious, inverseClip;
    if (!projection(guide.frustum, guide.sceneProjectionA, guide.sceneProjectionB, p)) return reject(512);
    if (!projection(guide.previousFrustum, guide.previousProjectionA, guide.previousProjectionB, previousP)) return reject(1024);
    if (!inverse(p, unproject)) return reject(2048);
    // Compose exactly the relative view used by the FG motion shader.
    // Its translation is in the previous eye's local frame, even when
    // both camera positions lie kilometres from the floating origin.
    if (!temporalRelativeEyeView(guide.cameraPrevious, guide.cameraNow, relative.m)) return reject(4096);
    relative.m[3][3] = 1;
    clipPrevious = multiply(multiply(previousP, relative), unproject);
    if (!inverse(clipPrevious, inverseClip)) return reject(8192);
    out = {};
    sdkMatrix(p, out.cameraViewToClip); sdkMatrix(unproject, out.clipToCameraView);
    sdkMatrix(clipPrevious, out.clipToPrevClip); sdkMatrix(inverseClip, out.prevClipToClip);
    // The supplied full-eye native image has not undergone a lens warp. This
    // identity is the proven undistorted-input transform, not a camera default.
    for (unsigned i = 0; i < 4; ++i) out.clipToLensClip[i][i] = 1;
    out.jitterOffset[0] = 2.f * guide.jitterPixels[0] / guide.width;
    out.jitterOffset[1] = -2.f * guide.jitterPixels[1] / guide.height;
    // The temporal FG plane already contains guide-pixel displacement.
    // Use unity scale for direct NGX pixel inputs per the NVIDIA DLSS-FG
    // Programming Guide (SDK 3749594), "Required Input Engine Data" (p. 22).
    out.mvecScale[0] = 1.f;
    out.mvecScale[1] = 1.f;
    const Matrix now = cameraPose(guide.cameraNow);
    for (unsigned i = 0; i < 3; ++i) {
        out.cameraPos[i] = static_cast<float>(now.m[i][3]); out.cameraRight[i] = static_cast<float>(now.m[i][0]);
        out.cameraUp[i] = static_cast<float>(now.m[i][1]); out.cameraFwd[i] = static_cast<float>(-now.m[i][2]);
    }
    out.cameraNear = guide.sceneProjectionB / (1.f - guide.sceneProjectionA);
    // Preserve the actual infinite-far projection in both matrices. The
    // integration supplies max-float as a finite far metadata sentinel;
    // this does not replace the scene's A=0 depth encoding with a finite one.
    out.cameraFar = guide.sceneProjectionA < 0 ? -guide.sceneProjectionB / guide.sceneProjectionA : (std::numeric_limits<float>::max)();
    out.cameraFOV = std::atan(guide.frustum[3]) - std::atan(guide.frustum[2]);
    out.cameraAspectRatio = (guide.frustum[1] - guide.frustum[0]) / (guide.frustum[3] - guide.frustum[2]);
    out.depthInverted = true; out.cameraMotionIncluded = true; out.reset = guide.reset;
    out.motionVectorsDilated = false; // camera and object motion both describe the center texel
    out.motionVectorsInvalidValue = 65504.f; out.minRelativeLinearDepthObjectSeparation = 40.f;
    if (!(std::isfinite(out.cameraNear) && out.cameraNear > 0 && std::isfinite(out.cameraFar) && out.cameraFar > out.cameraNear)) return reject(16384);
    return true;
}

struct Producer {
    FrameGenerationInterop interop;
    bool retired() const noexcept {return interop.retired();}
    FrameGenerationColor color;
    ComPtr<ID3D11DeviceContext> context;
    unsigned width = 0, height = 0, guideWidth = 0, guideHeight = 0;
    uint64_t config = 0, resourceEpoch = 0;
    uint64_t identity = 0; // Assigned once before this producer can enter the worker queue.
    bool colorCaptureNoted[2]{};
};
struct Work {
    std::shared_ptr<Producer> producer;
    InputLease input;
    FrameGenerationCamera cameras[2]{};
    FrameGenerationOutputUi ui[2];
    FrameGenerationOutputSource source;
    EdvrNativeTemporalFrame temporalPrevious{}, temporalCurrent{};
    uint64_t reflexProof = 0;
    bool qualificationOnly = false;
};
struct Publication {
    FrameGenerationOutputTicket ticket;
    uint64_t sequence, config, history, reflexProof;
    uint32_t multiplier;
    bool recomposedCockpit = false;
    uint64_t producerIdentity = 0;
    bool qualifiedRecomposition = false;
};
struct QualificationRoute {
    fg_recomposition::BackendKey backend{};
    fg_recomposition::Mode mode = fg_recomposition::Mode::Legacy;
    uint64_t seededHistory = 0;
    unsigned provenHudEyes = 0;
    bool started = false;
};
enum class TemporalSupport : uint8_t { Awaiting, Supported, Unsupported };
struct Coordinator {
    Coordinator(ID3D11Device* gameDevice, uint64_t sessionGeneration, bool recomposeUi = false)
        : device(gameDevice), generation(sessionGeneration), recomposeUi(recomposeUi) {}
    ID3D11Device* const device; // Immutable identity; host owns its lifetime through close.
    const uint64_t generation;
    const bool recomposeUi;
    std::atomic<bool> closed{false}, workerStarted{false};
    // The host retains the game device until close reports completion.
    // acquireMutex serializes these shutdown fields with the next acquisition.
    bool retired = false, quarantined = false;
    ULONGLONG closeStarted = 0;
    std::atomic<uint32_t> state{EDVR_FG_OFF}, failure{None}, active{0};
    std::atomic<uint64_t> terminalConfig{0};
    std::atomic<uint32_t> maximumMultiplier{0};
    std::atomic<uint32_t> terminalReason{None};
    std::atomic<uint64_t> reflexProof{0}, resourceEpoch{0};
    std::atomic<TemporalSupport> temporalSupport{TemporalSupport::Awaiting};
    std::atomic<uint64_t> history{1}, sourcePairs{0}, generatedPairs{0}, suppressedPairs{0}, refusedPairs{0}, busyPairs{0}, latestSource{0}, latestReady{0}, attemptedSource{0};
    std::atomic<uint64_t> latestExported{0}, coverageCutHistory{0}, coverageCutConfig{0}, coverageCutProof{0};
    std::mutex metadataMutex;
    EdvrFgSource source{}, previousSource{};
    EdvrNativeTemporalFrame temporal{}, previousTemporal{};
    bool haveSource = false, havePrevious = false;
    uint64_t sourceProof = 0, temporalProof = 0, temporalConfig = 0;
    std::mutex queueMutex;
    std::condition_variable wake;
    std::deque<Work> queued;
    std::deque<Publication> ready;
    QualificationRoute qualification; // queueMutex, including render-side eligibility reads.
    uint64_t reportedRecomposedHistory = 0; // queueMutex
    // The thread identity may be checked by a foreign caller. Only the
    // elected render owner reads or changes the capture fields below it.
    std::atomic<DWORD> renderThread{0};
    std::shared_ptr<Producer> producer;
    uint64_t lastProducerIdentity = 0; // Render owner only.
    std::atomic<uint64_t> liveProducerIdentity{0}; // Cross-owner check without borrowing the render pointer.
    // At most one displaced ring can remain alive solely for its GPU work.
    fg_interop::Retirement<Producer> retiringProducer;
    Work capture;
    uint64_t captureSequence = 0, captureHistory = 0, partialCheckpoints = 0, lastCheckpointReportMs = 0;
    std::atomic<uint64_t> resetSequence{0};
    unsigned activeEye = 2, guideMask = 0, uiMask = 0, worldMask = 0, temporalMask = 0;
    bool transaction = false, rejected = false;
    // Only the render owner observes the two immutable door facts. This
    // permission opens capture for a later source, never an output phase.
    uint64_t metadataHistory = 0, metadataSequence = 0, coverageHistory = 0, lastEligible = 0, lastCoverageLog = 0;
    uint64_t coverageBlocked = 0, coverageUnknown = 0, coverageEligible = 0, captureStarts = 0, guideCaptures = 0;
    unsigned eligibleStreak = 0, doorMask = 0, coveredMask = 0, previousHudEyes = 0;
    unsigned submittedGuideWidth = 0, submittedGuideHeight = 0;
    bool captureAllowed = false, observedAdjacent = false;
    ID3D11Texture2D* submitted[2]{};
    ID3D11Texture2D* temporalOutput[2]{};
    std::wstring features, data;
};
std::atomic<Coordinator*> current{nullptr};
std::mutex acquireMutex;
std::array<std::atomic<Coordinator*>, 16> contexts{};
unsigned contextCount = 0;
Coordinator* identify(void* pointer) noexcept {
    for (auto& slot : contexts) if (slot.load(std::memory_order_acquire) == pointer) return static_cast<Coordinator*>(pointer);
    return nullptr;
}
void refuse(Coordinator& c, Failure failure) noexcept {
    if (failure != Busy) c.coverageCutHistory = 0;
    c.failure.store(failure, std::memory_order_relaxed);
    const auto count = ++c.refusedPairs;
    if (failure != Busy && !c.closed.load() && c.state.load() != EDVR_FG_FAILED)
        c.state = EDVR_FG_UNAVAILABLE;
    if (count <= 12 || count % 600 == 0)
        Log::get().note("native fg: source refused reason=%u sequence=%llu history=%llu count=%llu; native fallback required.",
            uint32_t(failure), (unsigned long long)c.attemptedSource.load(),
            (unsigned long long)c.history.load(), (unsigned long long)count);
}
bool terminalFor(const Coordinator& c,uint64_t config) noexcept {
    return qualificationQuarantined.load(std::memory_order_acquire) ||
        (config && c.terminalConfig.load(std::memory_order_acquire)==config);
}
// queueMutex protects the bounded queues and serializes their invalidation
// with a ready publication. Queued inputs have not entered any GPU consumer.
void clearWorkLocked(Coordinator& c, uint32_t state, bool releaseResources = false) noexcept {
    c.coverageCutHistory = 0;
    ++c.history;
    c.active = 0;
    c.state = state;
    c.latestSource = 0;
    c.latestReady = 0;
    c.latestExported = 0;
    for (auto& work : c.queued) if (work.input) work.input->cancel();
    c.queued.clear();
    c.ready.clear();
    if (c.qualification.mode == fg_recomposition::Mode::Pending && !c.qualification.started) c.qualification = {};
    if (releaseResources) ++c.resourceEpoch;
}
void clearWork(Coordinator& c, uint32_t state, bool releaseResources = false) noexcept {
    {
        std::lock_guard<std::mutex> lock(c.queueMutex);
        if (c.closed.load()) return;
        clearWorkLocked(c, state, releaseResources);
    }
    c.wake.notify_all();
}

uint64_t liveReflexProof(const Coordinator& c) noexcept {
    return native_reflex::confirmedFrameGenerationProof(c.device, c.generation);
}

uint64_t syncReflexProof(Coordinator& c) noexcept {
    auto proof = liveReflexProof(c);
    if (proof == c.reflexProof.load(std::memory_order_acquire)) return proof;
    {
        std::lock_guard<std::mutex> lock(c.queueMutex);
        if (c.closed.load()) return 0;
        proof = liveReflexProof(c);
        const auto previous = c.reflexProof.load(std::memory_order_relaxed);
        if (previous == proof) return proof;
        clearWorkLocked(c, proof ? EDVR_FG_ARMING : EDVR_FG_UNAVAILABLE, previous != 0);
        c.reflexProof.store(proof, std::memory_order_release);
    }
    c.wake.notify_all();
    return proof;
}

void terminalFault(Coordinator& c,uint64_t config,Failure reason) noexcept {
    if (!config || c.closed.load()) return;
    // A fault belongs to the backend's configuration. A racing user selection
    // has a different generation and must be allowed to create a new backend.
    {
        std::lock_guard<std::mutex> lock(c.queueMutex);
        if (c.closed.load() || config != configuration.load() || terminalFor(c, config)) return;
        c.terminalReason.store(reason,std::memory_order_relaxed);
        c.terminalConfig.store(config,std::memory_order_release);
        clearWorkLocked(c, EDVR_FG_FAILED, true);
    }
    ++c.refusedPairs;
    c.wake.notify_all();
}
void quarantineTrial(Coordinator& c) noexcept {
    {
        std::lock_guard<std::mutex> lock(c.queueMutex);
        qualificationQuarantined.store(true, std::memory_order_release);
        if (!c.closed.load()) clearWorkLocked(c, EDVR_FG_FAILED, true);
    }
    ++c.refusedPairs;
    c.wake.notify_all();
}
bool terminalNgx(FrameGenerationNgxCompletion completion,const FrameGenerationNgxStatus& status) noexcept {
    return completion==FrameGenerationNgxCompletion::Expired ||
        completion==FrameGenerationNgxCompletion::DeviceRemoved || status.failed || status.quarantined;
}
void refuseOutput(Coordinator& c,const FrameGenerationOutputStatus& status) noexcept {
    // A full, healthy output ring needs the consumer to keep draining leases.
    // Preserve the existing backend retirement; only classify this exact
    // nonfatal publisher result as backpressure instead of unavailability.
    const bool busy = status.initialized && !status.failed && !status.quarantined &&
        status.failure == FrameGenerationOutputFailure::Busy && status.lastHresult == S_FALSE;
    if (busy) ++c.busyPairs;
    refuse(c,busy ? Busy : Output);
}
void refuseInterop(Coordinator& c,const FrameGenerationInteropStatus& status,uint64_t config) noexcept {
    if(status.quarantined||status.failure==FrameGenerationInteropFailure::DeviceRemoved) {
        Log::get().note("native fg: producer terminal failure=%u hr=0x%08x config=%llu.",
            unsigned(status.failure),unsigned(status.lastHresult),(unsigned long long)config);
        terminalFault(c,config,Interop);
    } else if(status.failure==FrameGenerationInteropFailure::Busy) {
        ++c.busyPairs;refuse(c,Busy);
    } else refuse(c,Interop);
}
void discardCapture(Coordinator& c) noexcept {
    if (c.transaction && c.capture.producer)
        c.capture.producer->interop.abort(c.capture.producer->context.Get(), c.captureSequence, c.captureHistory);
    c.transaction = false; c.rejected = true; c.activeEye = 2; c.capture = {};
}
void suspendCoverage(Coordinator&, bool protectedMotion) noexcept;
void abortCapture(Coordinator& c, Failure failure) noexcept {
    if (failure != Busy && c.captureAllowed) suspendCoverage(c,false);
    discardCapture(c);
    if(c.producer) {
        const auto status=c.producer->interop.status();
        if(status.quarantined||status.failure==FrameGenerationInteropFailure::DeviceRemoved) {
            refuseInterop(c,status,c.producer->config);return;
        }
    }
    refuse(c, failure);
}
bool selected(Coordinator* c) noexcept {
    return c && !c->closed.load(std::memory_order_acquire) &&
        requested.load(std::memory_order_acquire) != 0 && !terminalFor(*c,configuration.load());
}
bool enabled(Coordinator* c) noexcept {
    return selected(c) && syncReflexProof(*c) &&
        c->temporalSupport.load(std::memory_order_acquire) == TemporalSupport::Supported;
}
bool currentAdmission(const Coordinator& c, uint64_t config, uint64_t history, uint64_t proof, uint32_t multiplier) noexcept {
    return !c.closed.load() && multiplier && requested.load() == multiplier && config == configuration.load() &&
        !terminalFor(c, config) && c.temporalSupport.load() == TemporalSupport::Supported &&
        proof && proof == liveReflexProof(c) && history == c.history.load();
}
bool currentWork(const Coordinator& c, const Work& work) noexcept {
    return currentAdmission(c, work.source.current.configGeneration, work.source.historyGeneration,
        work.reflexProof, work.source.multiplier) &&
        (!work.producer || work.producer->identity == c.liveProducerIdentity.load(std::memory_order_acquire));
}
bool currentPublication(const Coordinator& c, const Publication& publication) noexcept {
    return currentAdmission(c, publication.config, publication.history, publication.reflexProof, publication.multiplier) &&
        publication.producerIdentity == c.liveProducerIdentity.load(std::memory_order_acquire) &&
        (!publication.qualifiedRecomposition || (c.qualification.mode == fg_recomposition::Mode::Verified &&
            c.qualification.backend.producer == publication.producerIdentity &&
            c.qualification.seededHistory == publication.history));
}

bool wantsQualification(const Coordinator& c, unsigned multiplier) noexcept {
    return c.recomposeUi && multiplier == 2;
}
fg_recomposition::BackendKey qualificationKey(const Coordinator& c, const Producer& producer, unsigned multiplier) noexcept {
    return {c.generation, producer.config, producer.resourceEpoch, static_cast<uintptr_t>(producer.identity),
        reinterpret_cast<uintptr_t>(producer.interop.device12()), producer.width, producer.height,
        producer.guideWidth, producer.guideHeight, DXGI_FORMAT_R8G8B8A8_UNORM, multiplier};
}
bool currentQualification(const Coordinator& c, const fg_recomposition::BackendKey& key) noexcept {
    return key.valid() && c.qualification.backend == key && key.sessionGeneration == c.generation &&
        key.configGeneration == configuration.load() && !terminalFor(c,key.configGeneration) &&
        key.resourceEpoch == c.resourceEpoch.load() && key.producer == c.liveProducerIdentity.load(std::memory_order_acquire) &&
        wantsQualification(c, requested.load()) && !c.closed.load();
}
enum class QualificationWork { Ordinary, Probe, Defer };

// Claim at most one immutable stereo pair for a worker attempt. The pair
// supplies its producer contract for the probe, never game pixels or cadence.
QualificationWork claimQualification(Coordinator& c, const fg_recomposition::BackendKey& key) noexcept {
    if (!key.valid() || !wantsQualification(c, key.multiplier)) return QualificationWork::Ordinary;
    if (!(c.qualification.backend == key)) {
        c.qualification = {key, fg_recomposition::Mode::Pending, 0};
        return QualificationWork::Probe;
    }
    return c.qualification.mode == fg_recomposition::Mode::Pending ? QualificationWork::Defer : QualificationWork::Ordinary;
}

void dropUnstartedQualification(Coordinator& c, const fg_recomposition::BackendKey& key) noexcept {
    std::lock_guard<std::mutex> lock(c.queueMutex);
    if (c.qualification.backend == key && c.qualification.mode == fg_recomposition::Mode::Pending) c.qualification = {};
}

// Under queueMutex. READY certifies an exportable current-epoch GPU result,
// with an actual generated phase. A seed is displayed by ordinary native
// pacing; only the later poll transfers a token.
void promoteReadyLocked(Coordinator& c) noexcept {
    bool exportable = false;
    for (auto item = c.ready.begin(); item != c.ready.end();) {
        const auto result = currentPublication(c, *item) ? item->ticket.exportStatus() : E_ABORT;
        if (FAILED(result)) {item = c.ready.erase(item); continue;}
        if (result == S_OK && item->ticket.generatedCount) {
            exportable = true;
            if (item->recomposedCockpit && c.reportedRecomposedHistory != item->history) {
                c.reportedRecomposedHistory = item->history;
                Log::get().note("native fg: recomposed stereo ready generation=%llu config=%llu history=%llu sequence=%llu hud_eyes=3.",
                    (unsigned long long)c.generation, (unsigned long long)item->config,
                    (unsigned long long)item->history, (unsigned long long)item->sequence);
            }
            if (item->sequence > c.latestReady.load()) {
                c.latestReady = item->sequence;
                c.active = item->multiplier;
                c.state = EDVR_FG_READY;
            }
        }
        ++item;
    }
    if (!exportable && !c.latestExported.load() && c.state.load() == EDVR_FG_READY) {
        c.latestReady = 0;
        c.active = 0;
        c.state = EDVR_FG_ARMING;
    }
}
void promoteReady(Coordinator& c) noexcept {
    std::lock_guard<std::mutex> lock(c.queueMutex);
    promoteReadyLocked(c);
}
bool owner(const Coordinator& c) noexcept {return c.renderThread.load(std::memory_order_acquire) == GetCurrentThreadId();}

// Only the render owner seals partial D3D11 copies and drops the producer's
// last reference. The worker retires its consumers on its own queue first.
bool serviceProducer(Coordinator& c) noexcept {
    c.retiringProducer.collect();
    if (!c.producer || c.producer->resourceEpoch == c.resourceEpoch.load()) return true;
    discardCapture(c);
    if (c.producer.use_count() != 1 || !c.producer->interop.retired()) return false;
    if (auto* context = c.producer->context.Get(); context && !uiLayerFgRetireStorage(context)) return false;
    c.liveProducerIdentity.store(0, std::memory_order_release);
    c.producer.reset();
    return true;
}
enum class ProducerInstall { Installed, Replaced, Superseded, Busy };

// Render owner only. Commit identity and revoke the displaced producer's
// ready images together, including a resize that did not bump resourceEpoch.
ProducerInstall installProducer(Coordinator& c, std::shared_ptr<Producer> candidate) noexcept {
    std::lock_guard<std::mutex> lock(c.queueMutex);
    if (!currentWork(c, c.capture) || candidate->resourceEpoch != c.resourceEpoch.load()) return ProducerInstall::Superseded;
    if (c.lastProducerIdentity == UINT64_MAX) return ProducerInstall::Busy;
    const bool replacing = bool(c.producer);
    candidate->identity = c.lastProducerIdentity + 1;
    if (!c.retiringProducer.replace(c.producer, std::move(candidate))) return ProducerInstall::Busy;
    c.liveProducerIdentity.store(++c.lastProducerIdentity, std::memory_order_release);
    if (!replacing) return ProducerInstall::Installed;
    clearWorkLocked(c, EDVR_FG_ARMING);
    c.wake.notify_all();
    return ProducerInstall::Replaced;
}
void temporalUnsupported(Coordinator& c) noexcept {
    if (c.temporalSupport.exchange(TemporalSupport::Unsupported) != TemporalSupport::Unsupported)
        clearWork(c, EDVR_FG_UNAVAILABLE);
}
bool paths(Coordinator& c) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&nativeFgAcquire), &module)) return false;
    wchar_t file[32768]{};
    const DWORD length = GetModuleFileNameW(module, file, 32768);
    if (!length || length >= 32768) return false;
    auto* slash = wcsrchr(file, L'\\'); if (!slash) return false; *slash = 0;
    c.features = file;
    wchar_t local[32768]{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
    if (!count || count >= 32700) return false;
    c.data = std::wstring(local) + L"\\" + kAppDataDirNameW;
    if (!CreateDirectoryW(c.data.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    c.data += L"\\NGX-FG";
    return CreateDirectoryW(c.data.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
bool retireBackendOnly(Work& work, const FrameGenerationNgxTicket& ticket,
    const FrameGenerationRecompositionTicket& prepared) noexcept {
    if (ticket.completionFence && ticket.completionValue) return work.input->retire(ticket.completionFence, ticket.completionValue);
    if (ticket.lease || ticket.evaluationSucceeded) return false;
    // Preparation can read the source even when NGX declined it. The
    // original producer must still wait for the last accepted GPU stage.
    if (prepared.accepted)
        return prepared.readyFence && prepared.readyValue && work.input->retire(prepared.readyFence, prepared.readyValue);
    return work.input->cancel();
}
// The GPU finished this source, but its presentation lifetime expired while
// the worker was off CPU. Retire the actual read, drop that history, and leave
// the backend available for the next qualified native source.
void retireStale(Coordinator& c,Work& work,const FrameGenerationNgxTicket& ticket) noexcept {
    if(!ticket.completionFence||!ticket.completionValue||!work.input->retire(ticket.completionFence,ticket.completionValue)) {
        terminalFault(c,work.producer->config,Interop);return;
    }
    std::lock_guard<std::mutex> lock(c.queueMutex);
    if(currentWork(c,work))clearWorkLocked(c,EDVR_FG_ARMING);
}
void clearUnexported(Coordinator& c) {
    // Tickets still in this deque have never transferred an ABI token. Their
    // publisher consumer state is Discarded, so dropping them is safe and
    // necessary before the publisher can prove that all output slots drained.
    std::lock_guard<std::mutex> lock(c.queueMutex);
    c.ready.clear();
}
bool drainOutput(FrameGenerationOutput& output) noexcept {
    const auto deadline = GetTickCount64() + 2000;
    do {
        if (output.drain(0)) return true;
        if (output.status().quarantined) return false;
        // An exported consumer may already have queued its retirement fence;
        // let it complete without blocking either native rendering or XR.
        Sleep(1);
    } while (GetTickCount64() < deadline);
    return output.drain(0);
}
bool drainBackend(FrameGenerationNgx* ngx, FrameGenerationOutput* output) noexcept {
    // NGX holds preparation leases through its last read. Fencing those
    // reads first lets output drain both its prep and publication rings.
    return (!ngx || ngx->drain(2000)) && (!output || drainOutput(*output));
}
bool pendingRetirement(const FrameGenerationNgx* ngx, const FrameGenerationOutput* output) noexcept {
    if (ngx) {
        const auto status = ngx->status();
        if (status.submitted > status.completed) return true;
    }
    if (output) {
        const auto status = output->status();
        if (status.submitted > status.completed || status.recompositionSubmitted > status.recompositionRetired) return true;
    }
    return false;
}
void waitForWorkerActivity(Coordinator& c, std::unique_lock<std::mutex>& lock,
    uint64_t releasedEpoch, bool retiring) {
    const auto changed = [&] {return c.closed.load() || !c.queued.empty() || c.resourceEpoch.load() != releasedEpoch;};
    // With all producer slots pinned, fence completion must wake collection
    // without another capture. A drained worker stays parked until signaled.
    if (retiring) c.wake.wait_for(lock, std::chrono::milliseconds(2), changed);
    else c.wake.wait(lock, changed);
}
enum class EvaluationRoute { Seed, Interpolate, NativeCoverage };

bool requiresNativeOutput(EvaluationRoute route,const FrameGenerationNgxTicket& ticket) noexcept {
    return route!=EvaluationRoute::Interpolate||ticket.historyResetReasons!=0;
}

// A native endpoint always uses the current source. A fresh history receives
// that source in both NGX eyes even if there is no previous physical pose.
template<class Backend>
FrameGenerationNgxTicket evaluateSource(Backend& backend,FrameGenerationNgxInput& input,
    const FrameGenerationOutputSource& source,const FrameGenerationOutputUi (&ui)[2],
    bool haveHistory,EvaluationRoute& route,bool qualifiedTranslation = false) {
    if(!haveHistory||source.previous.sequence==source.current.sequence) {
        route=EvaluationRoute::Seed;
        for(auto& eye:input.eyes)eye.camera.reset=true;
    } else {
        route=EvaluationRoute::Interpolate;
        const bool recompositionInputs = qualifiedTranslation && input.eyes[0].hudless && input.eyes[0].ui &&
            input.eyes[1].hudless && input.eyes[1].ui;
        if (!recompositionInputs && (!ui[0].empty || !ui[1].empty) &&
            !fg_coverage::anyFits(source.previous.views,source.current.views,source.multiplier)) {
            route=EvaluationRoute::NativeCoverage;
            return {}; // No NGX commands or reads were submitted for this pair.
        }
    }
    return backend.submit(input);
}
void worker(Coordinator* c) noexcept {
    std::unique_ptr<FrameGenerationNgx> ngx;
    std::unique_ptr<FrameGenerationOutput> output;
    std::shared_ptr<Producer> configured;
    std::unique_ptr<fg_recomposition::Activation> activation;
    fg_recomposition::Activation::Attempt activationAttempt;
    FrameGenerationQualificationResult qualificationProof;
    unsigned multiplier = 0, diagnosticFailures = 0;
    uint64_t releasedEpoch = 0, firstSeed = 0;
    bool notedAdjacent=false;
    uint64_t evaluatedHistory=0,coverageSkipped=0,lastCoverageReportMs=0;
    unsigned previousHudEyes = 0;
    const auto resetBackend = [&] {
        if (activation && configured) {
            const auto key = qualificationKey(*c, *configured, multiplier);
            std::lock_guard<std::mutex> lock(c->queueMutex);
            if (c->qualification.mode == fg_recomposition::Mode::Verified && c->qualification.backend == key)
                c->qualification = {};
        }
        output.reset(); ngx.reset(); configured.reset(); multiplier=0;
        activation.reset(); activationAttempt = {}; qualificationProof = {}; previousHudEyes = 0;
    };
    const auto releaseBackend = [&] {
        clearUnexported(*c);
        drainBackend(ngx.get(), output.get());
        resetBackend();
    };
    const auto discardBackend = [&](uint64_t config,Failure why) {
        terminalFault(*c,config,why);
        releaseBackend();
    };
    const auto quarantineBackend = [&] {
        if (activation && activationAttempt) activation->quarantine(activationAttempt);
        ngx->quarantineQualification();
        quarantineTrial(*c);
        releaseBackend();
    };
    const auto retireTrial = [&](const Work& trial, const fg_recomposition::BackendKey& key, bool unsupported) {
        if (!drainBackend(ngx.get(), output.get()) ||
            trial.producer->interop.device12()->GetDeviceRemovedReason() != S_OK) {
            quarantineBackend(); return;
        }
        if (activation && activationAttempt && !activation->retire(activationAttempt,
            {reinterpret_cast<uintptr_t>(qualificationProof.readbackFence.Get()), qualificationProof.completed})) {
            discardBackend(key.configGeneration, Ngx); return;
        }
        {
            std::lock_guard<std::mutex> lock(c->queueMutex);
            if (c->qualification.backend == key && c->qualification.mode == fg_recomposition::Mode::Pending) {
                const bool routeCurrent = currentQualification(*c, key);
                if (unsupported && routeCurrent && currentWork(*c, trial))
                    c->qualification.mode = fg_recomposition::Mode::Legacy;
                else c->qualification = {};
                if (routeCurrent) clearWorkLocked(*c, EDVR_FG_ARMING);
            }
        }
        resetBackend();
        c->wake.notify_all();
    };
    try {
        for (;;) {
            if (ngx && !ngx->collect() && terminalNgx(FrameGenerationNgxCompletion::Empty,ngx->status())) {
                const auto s=ngx->status();
                Log::get().note("native fg: terminal NGX collection fault, failure=%u hr=0x%08x; native cadence until reselected.",
                    uint32_t(s.failure),unsigned(s.lastHresult));
                discardBackend(configured->config,Ngx);
            }
            if (output && !output->collect()) {
                const auto s=output->status();
                if (s.failed || s.quarantined) discardBackend(configured->config,Output);
            }
            promoteReady(*c);
            Work work;
            const bool retiring = pendingRetirement(ngx.get(), output.get());
            {
                std::unique_lock<std::mutex> lock(c->queueMutex);
                waitForWorkerActivity(*c, lock, releasedEpoch, retiring);
                if (c->closed.load()) break;
                const auto epoch = c->resourceEpoch.load();
                if (epoch != releasedEpoch) {
                    releasedEpoch = epoch;
                    lock.unlock();
                    releaseBackend();
                    continue;
                }
                if (c->queued.empty()) continue;
                work = std::move(c->queued.front()); c->queued.pop_front();
            }
            if (!work.producer || !work.input) continue;
            const auto key = qualificationKey(*c, *work.producer, work.source.multiplier);
            if (!currentWork(*c, work)) {
                if (work.qualificationOnly) dropUnstartedQualification(*c, key);
                work.input->cancel(); continue;
            }
            if (work.qualificationOnly) {
                std::lock_guard<std::mutex> lock(c->queueMutex);
                if (!currentWork(*c, work) || !currentQualification(*c, key) ||
                    c->qualification.mode != fg_recomposition::Mode::Pending) {
                    if (c->qualification.backend == key && c->qualification.mode == fg_recomposition::Mode::Pending)
                        c->qualification = {};
                    work.input->cancel(); continue;
                }
                c->qualification.started = true;
            }
            if (work.qualificationOnly || configured != work.producer || multiplier != work.source.multiplier) {
                clearUnexported(*c);
                if (!drainBackend(ngx.get(), output.get())) {
                    work.input->cancel(); discardBackend(work.producer->config,Output); continue;
                }
                resetBackend(); configured = work.producer; multiplier = work.source.multiplier;
                firstSeed=0;notedAdjacent=false;evaluatedHistory=0;
                ngx = std::make_unique<FrameGenerationNgx>(); output = std::make_unique<FrameGenerationOutput>();
                FrameGenerationNgxInit init;
                init.device = configured->interop.device12(); init.width = configured->width; init.height = configured->height;
                init.guideWidth = configured->guideWidth; init.guideHeight = configured->guideHeight; init.multiplier = multiplier;
                init.creationMode = FrameGenerationNgxCreationMode::OfficialFgHelper;
                init.suppressionLayout = FrameGenerationNgxSuppressionLayout::SharedNativePair;
                init.uiRecomposition = work.qualificationOnly;
                init.featureDirectory = c->features.c_str(); init.dataDirectory = c->data.c_str();
                bool initialized = false;
                {
                    std::lock_guard<std::mutex> contextLock(ngxContextMutex);
                    bool known = false;
                    for (unsigned i = 0; i < ownedNgxCount; ++i) if (ownedNgxDevices[i] == init.device) known = true;
                    init.ngxAlreadyInitialized = known; init.privateNgxDevice = !known;
                    if (known || ownedNgxCount < ownedNgxDevices.size()) {
                        initialized = ngx->initialize(init);
                        c->maximumMultiplier=ngx->status().knownMaximumMultiplier;
                        if (!known && ngx->status().initStage >= FrameGenerationNgxInitStage::Capabilities) {
                            init.device->AddRef(); ownedNgxDevices[ownedNgxCount++] = init.device;
                        }
                    }
                }
                if (!initialized) {
                    const auto s = ngx->status();
                    Log::get().note("native fg: NGX initialization failed failure=%u stage=%u failed_stage=%u eye=%u ngx=0x%08x hr=0x%08x available=%u maximum_generated=%u private_init=%u borrowed_init=%u.",
                        uint32_t(s.failure),uint32_t(s.initStage),uint32_t(s.failedInitStage),s.failedInitEye,
                        s.lastNgxResult,unsigned(s.lastHresult),unsigned(s.available),s.maximumGenerated,
                        unsigned(init.privateNgxDevice),unsigned(init.ngxAlreadyInitialized));
                    work.input->cancel();
                    if (work.qualificationOnly && (s.quarantined || init.device->GetDeviceRemovedReason() != S_OK))
                        quarantineBackend();
                    else if (work.qualificationOnly && s.failure == FrameGenerationNgxFailure::NgxCreate) retireTrial(work, key, true);
                    else discardBackend(work.producer->config,Ngx);
                    continue;
                }
                if (!output->initialize(true, init.device, init.width, init.height)) {
                    const auto s = output->status();
                    Log::get().note("native fg: output initialization failed failure=%u hr=0x%08x quarantined=%u.",
                        uint32_t(s.failure),unsigned(s.lastHresult),unsigned(s.quarantined));
                    work.input->cancel(); discardBackend(work.producer->config,Output); continue;
                }
                Log::get().note("native fg: worker initialized multiplier=%u source=%ux%u guides=%ux%u route=official-helper/shared-native-pair ui_recomposition=%u mv_units=input-pixels mv_scale_left=(%.9g,%.9g) mv_scale_right=(%.9g,%.9g) jitter_units=clip-space",
                    multiplier, init.width, init.height, init.guideWidth, init.guideHeight, unsigned(init.uiRecomposition),
                    double(work.cameras[0].mvecScale[0]), double(work.cameras[0].mvecScale[1]),
                    double(work.cameras[1].mvecScale[0]), double(work.cameras[1].mvecScale[1]));
                if (work.qualificationOnly) {
                    if (!work.input->cancel()) {discardBackend(work.producer->config, Interop);continue;}
                    if (!currentWork(*c, work)) {retireTrial(work, key, false);continue;}
                    activation = std::make_unique<fg_recomposition::Activation>();
                    activationAttempt = activation->start(key);
                    if (!activationAttempt) {retireTrial(work, key, false);continue;}
                    qualificationProof = qualifyFrameGeneration(*ngx, init);
                    const auto status = ngx->status();
                    Log::get().note("native fg: UI qualification producer=%llu verdict=%u reason=%u matched_midpoints=%u.",
                        (unsigned long long)key.producer, unsigned(qualificationProof.verdict),
                        unsigned(qualificationProof.reason), qualificationProof.passingMidpoints);
                    if (qualificationProof.verdict == FrameGenerationQualificationVerdict::Unsafe || status.quarantined ||
                        init.device->GetDeviceRemovedReason() != S_OK) {
                        quarantineBackend();
                        continue;
                    }
                    if (status.failed) {retireTrial(work, key, true);continue;}
                    const auto fence = reinterpret_cast<uintptr_t>(qualificationProof.readbackFence.Get());
                    const bool terminal = qualificationProof.hasTerminalProof() && activation->registerProof(activationAttempt,
                        {fence, qualificationProof.submitted}, qualificationProof.coveredEyes);
                    const bool verified = terminal && qualificationProof.verdict == FrameGenerationQualificationVerdict::Passed;
                    if (!verified || !currentWork(*c, work)) {retireTrial(work, key, !verified);continue;}
                    if (!ngx->resetAfterQualification(qualificationProof.resetReceipt)) {
                        retireTrial(work, key, true);continue;
                    }
                    bool committed = false;
                    {
                        std::lock_guard<std::mutex> lock(c->queueMutex);
                        if (currentWork(*c, work) && currentQualification(*c, key) && activation->finish(activationAttempt,
                            key, {fence, qualificationProof.completed}, c->history.load(), qualificationProof.pixels) &&
                            activation->verified(activationAttempt,key)) {
                            c->qualification.mode = fg_recomposition::Mode::Verified;
                            c->qualification.provenHudEyes = qualificationProof.coveredEyes;
                            clearWorkLocked(*c, EDVR_FG_ARMING);
                            committed = true;
                        }
                    }
                    if (!committed) retireTrial(work, key, false);
                    else c->wake.notify_all();
                    continue;
                }
            }
            if (!currentWork(*c, work)) {work.input->cancel(); continue;}
            FrameGenerationNgxInput input;
            input.sequence = work.input->sequence(); input.historyGeneration = work.input->historyGeneration();
            input.producerFence = work.input->producerFence(); input.producerValue = work.input->producerValue();
            input.inputLease = work.input; input.pairedFullEyeGuides = true;
            for (unsigned eye = 0; eye < 2; ++eye) {
                auto& value = input.eyes[eye];
                value.color = work.input->plane(eye, FrameGenerationPlane::Color)->resource12;
                value.depth = work.input->plane(eye, FrameGenerationPlane::Depth)->resource12;
                value.motion = work.input->plane(eye, FrameGenerationPlane::Motion)->resource12;
                value.camera = work.cameras[eye];
            }
            FrameGenerationRecompositionTicket prepared;
            if (activation) {
                std::lock_guard<std::mutex> lock(c->queueMutex);
                if (!currentQualification(*c, key) || c->qualification.mode != fg_recomposition::Mode::Verified) {
                    work.input->cancel(); continue;
                }
            }
            if (activation) {
                prepared = output->prepareRecomposition(work.input, work.ui, work.source);
                if (!prepared.accepted) {
                    work.input->cancel();
                    const auto status = output->status();
                    if (status.failed || status.quarantined) discardBackend(work.producer->config, Output);
                    else refuseOutput(*c, status);
                    continue;
                }
                if (prepared.unsafe || !prepared.readyFence || !prepared.readyValue) {
                    prepared.lease.reset();
                    discardBackend(work.producer->config, Output);
                    continue;
                }
                if (!currentWork(*c, work)) {
                    const bool retired = retireBackendOnly(work, {}, prepared);
                    prepared.lease.reset();
                    if (!retired) discardBackend(work.producer->config, Interop);
                    continue;
                }
                input.producerFence = prepared.readyFence;
                input.producerValue = prepared.readyValue;
                input.inputLease = prepared.lease;
                for (unsigned eye = 0; eye < 2; ++eye) {
                    input.eyes[eye].color = prepared.eyes[eye].color;
                    input.eyes[eye].hudless = prepared.eyes[eye].hudless;
                    input.eyes[eye].ui = prepared.eyes[eye].ui;
                }
            }
            EvaluationRoute evaluation{};
            const unsigned hudEyes = (work.ui[0].empty ? 0u : 1u) | (work.ui[1].empty ? 0u : 2u);
            const bool haveHistory = evaluatedHistory == input.historyGeneration;
            const auto priorHudEyes = haveHistory ? previousHudEyes : 0u;
            const bool translated = activation && activation->canPublish(activationAttempt,key,input.historyGeneration) &&
                hudEyes == qualificationProof.coveredEyes && priorHudEyes == hudEyes;
            auto ticket=evaluateSource(*ngx,input,work.source,work.ui,haveHistory,evaluation,translated);
            input.inputLease.reset();
            prepared.lease.reset();
            const bool nativeBootstrap=evaluation==EvaluationRoute::Seed;
            const bool skipNgx=evaluation==EvaluationRoute::NativeCoverage;
            if(skipNgx) {
                ++coverageSkipped;
                const auto now=GetTickCount64();
                if(now-lastCoverageReportMs>=1000) {
                    lastCoverageReportMs=now;
                    Log::get().note("native fg: covered translation skips generation=%llu coverage_skipped=%llu.",
                        (unsigned long long)c->generation,(unsigned long long)coverageSkipped);
                }
            }
            auto completion = ticket.poll();
            while (completion == FrameGenerationNgxCompletion::Pending) {
                if (output) output->collect();
                Sleep(1); completion = ticket.poll();
            }
            const auto ngxStatus=ngx->status();
            if (terminalNgx(completion,ngxStatus)) {
                retireBackendOnly(work,ticket,prepared);
                Log::get().note("native fg: terminal NGX evaluation fault, completion=%u failure=%u ngx=0x%08x hr=0x%08x; native cadence until reselected.",
                    unsigned(completion),uint32_t(ngxStatus.failure),ngxStatus.lastNgxResult,unsigned(ngxStatus.lastHresult));
                discardBackend(work.producer->config,Ngx);continue;
            }
            if (!currentWork(*c, work)) {
                retireBackendOnly(work, ticket, prepared); continue;
            }
            if(completion==FrameGenerationNgxCompletion::Stale) {
                retireStale(*c,work,ticket);continue;
            }
            if (completion == FrameGenerationNgxCompletion::FailedEvaluation &&
                (++diagnosticFailures <= 12 || diagnosticFailures % 600 == 0)) {
                const auto s = ngx->status();
                Log::get().note("native fg: NGX evaluation failed; publishing native-only sequence=%llu failure=%u ngx=0x%08x hr=0x%08x.",
                    (unsigned long long)input.sequence,uint32_t(s.failure),s.lastNgxResult,unsigned(s.lastHresult));
            }
            if (!skipNgx && completion != FrameGenerationNgxCompletion::Ready && completion != FrameGenerationNgxCompletion::Suppressed &&
                completion != FrameGenerationNgxCompletion::FailedEvaluation) {
                if (++diagnosticFailures <= 12 || diagnosticFailures % 600 == 0) {
                    const auto s = ngx->status();
                    Log::get().note("native fg: NGX submit rejected sequence=%llu completion=%u lease=%u failure=%u ngx=0x%08x hr=0x%08x quarantined=%u.",
                        (unsigned long long)input.sequence,uint32_t(completion),unsigned(bool(ticket.lease)),
                        uint32_t(s.failure),s.lastNgxResult,unsigned(s.lastHresult),unsigned(s.quarantined));
                }
                retireBackendOnly(work, ticket, prepared); refuse(*c, Ngx); continue;
            }
            if (activation && completion == FrameGenerationNgxCompletion::Ready && !ticket.uiRecomposed) {
                retireBackendOnly(work, ticket, prepared);
                discardBackend(work.producer->config, Ngx);
                continue;
            }
            if (nativeBootstrap && evaluatedHistory != input.historyGeneration) {firstSeed=0;notedAdjacent=false;}
            if(!skipNgx) {
                evaluatedHistory=ticket.evaluationSucceeded?input.historyGeneration:0;
                previousHudEyes=ticket.evaluationSucceeded?hudEyes:0;
            }
            // A reset after a skipped sequence seeds the current source. Even
            // an unsuppressed SDK result then has no contiguous previous image.
            // An opaque bilateral witness does not certify a transition to or
            // from one clear eye, even when the optical centers coincide.
            const bool nativeOnly=requiresNativeOutput(evaluation,ticket) ||
                (activation && (hudEyes || priorHudEyes) && !translated);
            // While ordinary native owns presentation it has already drawn
            // this endpoint. Seed the actual backend without occupying a
            // fenced output slot the XR consumer cannot yet poll.
            if ((nativeOnly || completion!=FrameGenerationNgxCompletion::Ready) && !c->active.load()) {
                if (!retireBackendOnly(work,ticket,prepared) || (ticket.evaluationSucceeded && (!ticket.completionFence || !ticket.completionValue))) {
                    discardBackend(work.producer->config,Ngx); continue;
                }
                if (activation && nativeOnly && ticket.evaluationSucceeded &&
                    (completion == FrameGenerationNgxCompletion::Ready || completion == FrameGenerationNgxCompletion::Suppressed)) {
                    std::lock_guard<std::mutex> lock(c->queueMutex);
                    if (currentWork(*c,work) && currentQualification(*c,key) &&
                        activation->noteLiveSeed(activationAttempt,key,input.historyGeneration,ticket.historyResetReasons))
                        c->qualification.seededHistory = input.historyGeneration;
                }
                if (nativeBootstrap && ticket.evaluationSucceeded && !firstSeed) {
                    firstSeed=input.sequence;
                    Log::get().note("native fg: history seed sequence=%llu history=%llu completion=%u generated_count=0.",
                        (unsigned long long)firstSeed,(unsigned long long)input.historyGeneration,unsigned(completion));
                }
                if(currentWork(*c,work)) ++c->suppressedPairs;
                continue;
            }
            auto published = output->submit(work.input,ticket,work.ui,work.source,nativeOnly);
            if (!published.accepted) {
                const auto outputStatus = output->status();
                if(outputStatus.failure==FrameGenerationOutputFailure::Stale) {retireStale(*c,work,ticket);continue;}
                if (++diagnosticFailures <= 12 || diagnosticFailures % 600 == 0) {
                    Log::get().note("native fg: output submit rejected sequence=%llu failure=%u hr=0x%08x quarantined=%u backend_completion=%u.",
                        (unsigned long long)input.sequence,uint32_t(outputStatus.failure),unsigned(outputStatus.lastHresult),
                        unsigned(outputStatus.quarantined),uint32_t(completion));
                }
                retireBackendOnly(work, ticket, prepared);
                if (outputStatus.failed || outputStatus.quarantined) discardBackend(work.producer->config,Output);
                else refuseOutput(*c, outputStatus);
                continue;
            }
            if (!published.finalFence || !published.finalValue || !work.input->retire(published.finalFence, published.finalValue)) {
                Log::get().note("native fg: source retirement failed sequence=%llu final_fence=%u final_value=%llu; source remains quarantined.",
                    (unsigned long long)input.sequence,unsigned(published.finalFence!=nullptr),(unsigned long long)published.finalValue);
                discardBackend(work.producer->config,Output);continue;
            }
            if(nativeBootstrap&&!firstSeed) {
                firstSeed=input.sequence;
                Log::get().note("native fg: history seed sequence=%llu history=%llu completion=%u generated_count=%u.",
                    (unsigned long long)firstSeed,(unsigned long long)input.historyGeneration,unsigned(completion),published.generatedCount);
            } else if(firstSeed&&!notedAdjacent&&input.sequence==firstSeed+1) {
                notedAdjacent=true;
                Log::get().note("native fg: first post-seed pair previous=%llu current=%llu history=%llu completion=%u generated_count=%u.",
                    (unsigned long long)work.source.previous.sequence,(unsigned long long)input.sequence,
                    (unsigned long long)input.historyGeneration,unsigned(completion),published.generatedCount);
            }
            {
                std::lock_guard<std::mutex> lock(c->queueMutex);
                const bool qualifiedRecomposition = activation && published.generatedCount;
                if (!currentWork(*c, work) || (qualifiedRecomposition &&
                    (!currentQualification(*c,key) || !activation->canPublish(activationAttempt,key,input.historyGeneration)))) continue;
                if(published.generatedCount)++c->generatedPairs;else ++c->suppressedPairs;
                if (c->ready.size() >= 3) {++c->busyPairs;}
                else {
                    c->ready.push_back({std::move(published), work.source.current.sequence,
                        work.source.current.configGeneration, work.source.historyGeneration, work.reflexProof, multiplier,
                        ticket.uiRecomposed && !work.ui[0].empty && !work.ui[1].empty, work.producer->identity,
                        qualifiedRecomposition});
                }
            }
            c->wake.notify_all();
        }
    } catch (...) {terminalFault(*c,configured?configured->config:configuration.load(),Output);}
    // Destruction remains on the component owner thread. Only after every
    // destructor has finished may a nonblocking close report completion.
    releaseBackend();
    c->active = 0;
    c->workerStarted = false;
}
template<class Launch> bool launchWorker(Coordinator& c, Launch&& launch) {
    c.workerStarted = true;
    try { launch(); return true; }
    catch (...) { c.workerStarted = false; return false; }
}
bool startWorker(Coordinator& c) {
    if (c.workerStarted.load()) return true;
    if (!paths(c)) return false;
    return launchWorker(c, [&] { std::thread(worker, &c).detach(); });
}
HRESULT WINAPI status(void* pointer, EdvrFgStatus* out) {
    auto* c = identify(pointer);
    if (!c || !out || out->size != sizeof(*out) || out->version != 1) return E_INVALIDARG;
    *out = {}; out->size = sizeof(*out); out->version = 1;
    out->configGeneration = configuration.load();
    const bool fault=terminalFor(*c,out->configGeneration);
    out->requestedMultiplier = requested.load();
    const bool isClosed = c->closed.load();
    const bool qualified = out->requestedMultiplier && !isClosed && !fault && syncReflexProof(*c);
    const auto temporal = c->temporalSupport.load();
    if (qualified && temporal == TemporalSupport::Supported && c->latestReady.load() && !c->latestExported.load()) {
        // The first unclaimed batch may have expired while the host was
        // blocked. Recheck it before using READY to enable independent pacing.
        std::unique_lock<std::mutex> lock(c->queueMutex, std::try_to_lock);
        if (lock.owns_lock()) promoteReadyLocked(*c);
    }
    out->activeMultiplier = qualified && temporal == TemporalSupport::Supported ? c->active.load() : 0;
    out->state = !out->requestedMultiplier ? (automatic.load() ? EDVR_FG_ARMING : EDVR_FG_OFF) : isClosed ? EDVR_FG_CLOSING : fault ? EDVR_FG_FAILED :
        (!qualified || temporal == TemporalSupport::Unsupported) ? EDVR_FG_UNAVAILABLE : c->state.load();
    out->lastFailure = qualificationQuarantined.load() ? uint32_t(Ngx) :
        (fault ? c->terminalReason.load() : c->failure.load());
    out->generation = c->generation; out->historyGeneration = c->history.load();
    if (out->state == EDVR_FG_UNAVAILABLE && qualified && temporal == TemporalSupport::Supported &&
        !out->activeMultiplier && out->historyGeneration == c->coverageCutHistory.load() &&
        out->configGeneration == c->coverageCutConfig.load() && c->coverageCutProof.load() == liveReflexProof(*c) &&
        out->historyGeneration == c->history.load())
        out->lastFailure = EDVR_FG_NATIVE_COVERAGE;
    out->sourcePairs = c->sourcePairs.load(); out->generatedPairs = c->generatedPairs.load();
    out->suppressedPairs = c->suppressedPairs.load(); out->refusedPairs = c->refusedPairs.load(); out->busyPairs = c->busyPairs.load();
    out->latestSourceSequence = c->latestSource.load();
    // The XR handoff's wire watermark advances when poll transfers the batch.
    out->latestReadySequence = c->latestExported.load();
    return S_OK;
}
bool validSource(const EdvrFgSource& value, const Coordinator& c) noexcept {
    if (value.size != sizeof(value) || value.version != 1 || value.generation != c.generation ||
        !value.sequence || !value.referenceGeneration || value.configGeneration != configuration.load() ||
        !value.width || !value.height || value.width > 16384 || value.height > 16384 || !value.poseDisplayTime) return false;
    for (const auto& view : value.views) {
        if (!finite(view.orientation, 4) || !finite(view.position, 3) || !finite(view.fov, 4) ||
            !(view.fov[0] < view.fov[1] && view.fov[3] < view.fov[2])) return false;
        const double norm = double(view.orientation[0]) * view.orientation[0] + double(view.orientation[1]) * view.orientation[1] +
            double(view.orientation[2]) * view.orientation[2] + double(view.orientation[3]) * view.orientation[3];
        if (std::fabs(norm - 1) > .02) return false;
    }
    return true;
}
HRESULT WINAPI beginSource(void* pointer, const EdvrFgSource* value) {
    auto* c = identify(pointer); if (!c || !value) return E_INVALIDARG;
    if (!selected(c)) return S_FALSE;
    const auto proof = syncReflexProof(*c);
    if (!proof) return S_FALSE;
    if (!validSource(*value, *c)) {refuse(*c, SourceMismatch); return E_INVALIDARG;}
    std::unique_lock<std::mutex> lock(c->metadataMutex, std::try_to_lock); if (!lock.owns_lock()) return E_PENDING;
    if (proof != liveReflexProof(*c) || !selected(c)) return S_FALSE;
    if (c->haveSource && value->sequence <= c->source.sequence) return E_INVALIDARG;
    c->havePrevious = c->haveSource && c->sourceProof == proof && value->sequence == c->source.sequence + 1 &&
        value->generation == c->source.generation && value->referenceGeneration == c->source.referenceGeneration &&
        value->configGeneration == c->source.configGeneration && value->width == c->source.width && value->height == c->source.height;
    if (c->haveSource && c->source.referenceGeneration != value->referenceGeneration)
        clearWork(*c, EDVR_FG_ARMING);
    if (c->haveSource) c->previousSource = c->source;
    c->source = *value; c->sourceProof = proof; c->haveSource = true; c->attemptedSource = value->sequence;
    return S_OK;
}
HRESULT WINAPI poll(void* pointer, EdvrFgBatch* out) {
    auto* c = identify(pointer);
    if (!c || !out || !edvrFgBatchProtocol(out->size,out->version)) return E_INVALIDARG;
    if (!enabled(c) || !c->active.load()) return S_FALSE;
    std::unique_lock<std::mutex> lock(c->queueMutex, std::try_to_lock); if (!lock.owns_lock()) return S_FALSE;
    const auto proof = c->reflexProof.load();
    if (!selected(c) || !c->active.load() || !proof || proof != liveReflexProof(*c) || c->temporalSupport.load() != TemporalSupport::Supported) return S_FALSE;
    while (!c->ready.empty()) {
        auto& publication = c->ready.front();
        if (!currentPublication(*c, publication)) {c->ready.pop_front(); continue;}
        // Validate in full local storage; an old caller may provide only its
        // original ABI prefix. No token or other mutation escapes on rejection.
        EdvrFgBatch batch{};batch.version=out->version;batch.size=out->size;
        const auto result = publication.ticket.acquireBatch(&batch);
        if (result == S_FALSE) return S_FALSE;
        c->ready.pop_front();
        if (FAILED(result)) {refuse(*c, Output); continue;}
        if (proof != liveReflexProof(*c) || !selected(c) || c->temporalSupport.load() != TemporalSupport::Supported ||
            batch.generation != c->generation || batch.configGeneration != configuration.load() ||
            batch.historyGeneration != c->history.load()) {
            frameGenerationOutputDiscard(batch.token); frameGenerationOutputRelease(batch.token);
            continue;
        }
        c->latestExported = batch.currentSequence;
        std::memcpy(out,&batch,batch.size);
        return S_OK;
    }
    return S_FALSE;
}
HRESULT WINAPI invalidate(void* pointer, uint32_t reason) {
    auto* c = identify(pointer); if (!c) return E_INVALIDARG;
    // The announcing provider already invalidated this history. Caller
    // invalidation still retired the XR consumer's local tokens and pins.
    if (reason != EDVR_FG_NATIVE_COVERAGE && !c->closed.load()) {clearWork(*c, EDVR_FG_ARMING); c->failure = reason;}
    return S_OK;
}
HRESULT WINAPI close(void* pointer) {
    auto* c = identify(pointer); if (!c) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(acquireMutex);
    if (c->retired) return S_OK;
    if (!c->closed.load()) {
        {
            std::lock_guard<std::mutex> queue(c->queueMutex);
            c->closed.store(true);
            c->closeStarted = GetTickCount64();
            clearWorkLocked(*c, EDVR_FG_CLOSING);
        }
        c->wake.notify_all();
    }
    if (c->workerStarted.load(std::memory_order_acquire)) return E_PENDING;

    // NativeModule closes its producer gates before the XR owner gets here.
    // An exceptional worker exit can leave work it never submitted in the queue.
    {
        std::lock_guard<std::mutex> queue(c->queueMutex);
        for (auto& work : c->queued) if (work.input) work.input->cancel();
        c->queued.clear();
        c->ready.clear();
    }
    if(c->transaction&&c->producer&&c->producer->interop.cancelQuiesced())c->transaction=false;
    c->capture = {};
    if (!c->retiringProducer.collect() || (c->producer && !c->producer->interop.retired())) {
        // Checkpointed partial copies can retire after the producer gates close.
        // Keep ownership if a producer signal failed or no proof was recorded.
        if (!c->transaction && GetTickCount64() - c->closeStarted < 2000) return E_PENDING;
        c->quarantined = true;
        Log::get().note("native fg: producer retirement unproven; retaining its ring and disabling further acquisitions this process.");
    } else {
        c->liveProducerIdentity.store(0, std::memory_order_release);
        c->producer.reset();
    }
    c->retired = true;
    return S_OK;
}
} // namespace

namespace {
void setEffectiveMultiplier(uint32_t multiplier, bool userConfiguration, bool manualOff = false) noexcept {
    if (!nativeFgFixedMultiplier(multiplier)) multiplier = 0;
    const bool changed = requested.exchange(multiplier, std::memory_order_acq_rel) != multiplier;
    if (userConfiguration) ++configuration;
    if (auto* c = current.load(std::memory_order_acquire)) {
        if ((changed||userConfiguration) && !c->closed.load()) {
            c->failure = None;
            clearWork(*c, multiplier ? EDVR_FG_ARMING : EDVR_FG_OFF, manualOff);
        }
    }
}
}
void nativeFgSetRequestedMultiplier(uint32_t multiplier) noexcept {
    std::lock_guard<std::mutex> lock(autoMutex);
    const bool wasAutomatic=automatic.load(std::memory_order_acquire);
    const unsigned effective=nativeFgFixedMultiplier(multiplier)?multiplier:0;
    setEffectiveMultiplier(effective, wasAutomatic || requested.load()!=effective, effective == 0);
    automatic.store(false,std::memory_order_release);
}
void nativeFgSetAutoMode() noexcept {
    if(automatic.load(std::memory_order_acquire))return;
    std::lock_guard<std::mutex> lock(autoMutex);
    if(automatic.load(std::memory_order_acquire))return;
    autoPolicy.reset();
    automatic.store(true,std::memory_order_release);
    setEffectiveMultiplier(0,true);
    Log::get().note("native fg auto: selected, effective=1x, native-first; work measurement required before 2x admission.");
}
bool nativeFgAutoRequested() noexcept {return automatic.load(std::memory_order_acquire);}
fg_auto::Status nativeFgAutoStatus() noexcept {
    std::lock_guard<std::mutex> lock(autoMutex);return autoPolicy.status();
}
void nativeFgAutoObserve(const fg_auto::Sample& sample) noexcept {
    if(!nativeFgAutoRequested())return;
    std::lock_guard<std::mutex> lock(autoMutex);
    if(!nativeFgAutoRequested())return;
    auto qualified=sample;
    if(auto* c=current.load(std::memory_order_acquire)){
        // UNAVAILABLE is the existing recoverable native-fallback route. The
        // XR owner keeps announcing sources until a supported pair recovers;
        // toggling Auto Off here would fight that recovery on every warmup.
        const bool terminal=terminalFor(*c,configuration.load())||c->state.load()==EDVR_FG_FAILED;
        qualified.fault=qualified.fault||(requested.load()!=0&&terminal);
        qualified.fatal=qualified.fatal||c->closed.load()||terminal;
    }
    const auto before=requested.load();const auto selected=autoPolicy.observe(qualified);
    if(selected!=before){
        // The user's Auto configuration stays fixed. Only its internal cadence
        // changes, retaining the producer/NGX allocations at identical sizes.
        setEffectiveMultiplier(selected,false);
        const auto status=autoPolicy.status();
        Log::get().note("native fg auto: effective=%ux reason=%s cpu_work_ms=%.3f application_gpu_ms=%.3f period_ms=%.3f transitions=%llu; independent recent work estimates, pose wait excluded.",
            selected?selected:1,fg_auto::reasonName(status.reason),status.cpuMs,status.gpuMs,status.periodMs,
            (unsigned long long)status.transitions);
    }
}
uint32_t nativeFgMaximumMultiplier() noexcept {
    auto* c=current.load(std::memory_order_acquire);
    return c&&!c->closed.load(std::memory_order_acquire)?c->maximumMultiplier.load(std::memory_order_acquire):0;
}
bool nativeFgRequested() noexcept {return requested.load(std::memory_order_relaxed) != 0;}
bool nativeFgManualOff() noexcept {
    return !requested.load(std::memory_order_acquire) && !nativeFgAutoRequested() &&
           !requested.load(std::memory_order_acquire);
}
bool nativeFgStatusSnapshot(EdvrFgStatus* out) noexcept {
    if(!out)return false;
    *out={sizeof(*out),EDVR_NATIVE_FRAME_GENERATION_VERSION_1};
    out->requestedMultiplier=requested.load(std::memory_order_acquire);
    out->state=out->requestedMultiplier||nativeFgAutoRequested()?EDVR_FG_ARMING:EDVR_FG_OFF;
    auto* c=current.load(std::memory_order_acquire);
    return c&&!c->closed.load(std::memory_order_acquire)&&status(c,out)==S_OK;
}
HRESULT nativeFgAcquire(const EdvrFgRequest* request, EdvrFgTable* table) noexcept {
    if (!request || !table || request->size != sizeof(*request) || request->version != 1 || !request->gameDevice ||
        !request->generation || table->size != sizeof(*table) || table->version != 1) return E_INVALIDARG;
    try {
        std::lock_guard<std::mutex> lock(acquireMutex);
        if (auto* old = current.load()) {
            if (!old->retired) return E_PENDING;
            if (old->quarantined) return E_NOINTERFACE;
        }
        if (contextCount == contexts.size()) return E_OUTOFMEMORY;
        // An ordinary launch can qualify recomposition at 2x. The process-local
        // override keeps legacy available for controls. A present empty or
        // malformed value cannot enable qualification.
        wchar_t qualification[2]{};
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(kUiRecompositionOverride, qualification, 2);
        const bool recomposeUi = (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) ||
            (length == 1 && qualification[0] == L'1');
        auto* c = new Coordinator(request->gameDevice, request->generation, recomposeUi);
        contexts[contextCount++].store(c, std::memory_order_release); current.store(c, std::memory_order_release);
        *table = {}; table->size = sizeof(*table); table->version = 1; table->context = c;
        table->getStatus = status; table->beginSource = beginSource; table->poll = poll;
        table->retain = frameGenerationOutputRetain; table->release = frameGenerationOutputRelease;
        table->retire = frameGenerationOutputRetire; table->discard = frameGenerationOutputDiscard;
        table->invalidate = invalidate; table->close = close;
        return S_OK;
    } catch (...) {return E_OUTOFMEMORY;}
}
void nativeFgBeginTemporal(const EdvrNativeTemporalFrame& frame, bool reset, bool supported) noexcept {
    auto* c = current.load(std::memory_order_acquire);
    if (!c || c->closed.load() || c->generation != frame.generation ||
        (!nativeFgRequested() && !nativeFgAutoRequested())) return;
    if (!supported) {temporalUnsupported(*c); return;}
    const bool validSize = frame.size == sizeof(frame) || frame.size == EDVR_NATIVE_TEMPORAL_FRAME_SIZE_NO_ZOOM;
    if (!validSize) {temporalUnsupported(*c); return;}
    const auto proof = syncReflexProof(*c);
    try {
        std::lock_guard<std::mutex> lock(c->metadataMutex);
        const bool newlySupported = c->temporalSupport.load() != TemporalSupport::Supported;
        c->previousTemporal = !newlySupported && c->temporalProof == proof ? c->temporal : EdvrNativeTemporalFrame{};
        c->temporal = {};
        // NO_ZOOM is the normal cockpit wire size. Never read the optional
        // tail of an older caller, even when its current compiler knows it.
        std::memcpy(&c->temporal, &frame, frame.size);
        c->temporalProof = proof;
        c->temporalConfig = configuration.load();
        if (newlySupported || (reset && c->resetSequence != frame.sequence)) {
            clearWork(*c, EDVR_FG_ARMING);
            if (reset) c->resetSequence = frame.sequence;
        }
        c->temporalSupport.store(TemporalSupport::Supported, std::memory_order_release);
    } catch (...) {refuse(*c, TemporalMismatch);}
}
void nativeFgTemporalUnavailable(uint64_t generation) noexcept {
    auto* c = current.load(std::memory_order_acquire);
    if (c && !c->closed.load() && generation == c->generation && (nativeFgRequested() || nativeFgAutoRequested()))
        temporalUnsupported(*c);
}
void nativeFgServiceProducer(uint64_t generation) noexcept {
    auto* c = current.load(std::memory_order_acquire);
    if (c && !c->closed.load() && generation == c->generation && owner(*c)) {
        if (selected(c)) syncReflexProof(*c);
        serviceProducer(*c);
    }
}
namespace {
bool captureMayInterpolate(Coordinator& c, const EdvrFgSource& previous, const EdvrFgSource& currentSource,
    unsigned multiplier, unsigned previousHudEyes, unsigned currentHudEyes, bool observing = false) noexcept {
    if (wantsQualification(c, multiplier)) {
        std::lock_guard<std::mutex> lock(c.queueMutex);
        const auto& state = c.qualification;
        if (!currentQualification(c, state.backend) || state.backend.configGeneration != currentSource.configGeneration ||
            state.backend.width != currentSource.width || state.backend.height != currentSource.height ||
            (c.submittedGuideWidth && c.submittedGuideHeight &&
                (state.backend.guideWidth != c.submittedGuideWidth || state.backend.guideHeight != c.submittedGuideHeight))) return true;
        // Observing continues while the one private pair is in flight. Cutting
        // coverage history here would invalidate the very proof being measured.
        if (state.mode == fg_recomposition::Mode::Pending) return observing;
        if (state.mode == fg_recomposition::Mode::Verified)
            return !(previousHudEyes | currentHudEyes) ||
                (previousHudEyes == state.provenHudEyes && currentHudEyes == state.provenHudEyes);
        if (state.mode == fg_recomposition::Mode::Unsafe) return false;
    }
    return !currentHudEyes || fg_coverage::anyFits(previous.views, currentSource.views, multiplier);
}

// One generation cut per loss of capture permission. Sealing partial GPU
// commands remains render-owner work even when another thread already reset.
void suspendCoverage(Coordinator& c, bool protectedMotion) noexcept {
    const bool coverageCut = c.captureHistory && c.coverageCutHistory.load() == c.captureHistory;
    if (c.eligibleStreak >= 2 || protectedMotion != coverageCut) {
        std::lock_guard<std::mutex> lock(c.queueMutex);
        const bool qualified = selected(&c) && c.temporalSupport.load() == TemporalSupport::Supported &&
            c.reflexProof.load() && c.reflexProof.load() == liveReflexProof(c);
        if (c.captureHistory == c.history.load() && qualified && (!protectedMotion || currentWork(c,c.capture))) {
            clearWorkLocked(c, protectedMotion ? EDVR_FG_UNAVAILABLE : EDVR_FG_ARMING);
            if (protectedMotion) {
                c.coverageCutConfig = c.capture.source.current.configGeneration;
                c.coverageCutProof = c.capture.reflexProof;
                c.coverageCutHistory = c.history.load();
            }
        }
        c.wake.notify_all();
    }
    c.eligibleStreak = 0;
    c.lastEligible = 0;
    c.captureAllowed = false;
    if (c.transaction) {
        discardCapture(c);
        const auto st = c.producer->interop.status();
        if (st.quarantined || st.failure == FrameGenerationInteropFailure::DeviceRemoved)
            refuseInterop(c,st,c.producer->config);
    }
}

void observeDoor(Coordinator& c, unsigned eye, bool admitted, bool hasUi) noexcept {
    const unsigned bit = 1u << eye;
    if (!admitted || (c.doorMask & bit)) {
        ++c.coverageUnknown;
        c.doorMask |= bit;
        suspendCoverage(c,false);
        c.rejected = true;
        return;
    }
    c.doorMask |= bit;
    if (hasUi) c.coveredMask |= bit;
    if (c.doorMask != 3) return;
    const auto& pair = c.capture.source;
    const bool adjacent = c.observedAdjacent;
    const auto previousHudEyes = c.previousHudEyes;
    c.previousHudEyes = c.coveredMask;
    if (!adjacent) {
        // The first native endpoint across a history cut establishes the new
        // anchor. It cannot qualify A/B or supersede the preceding cut.
        ++c.coverageUnknown;
        c.eligibleStreak = 0; c.lastEligible = 0;
    } else if (!captureMayInterpolate(c, pair.previous, pair.current, pair.multiplier,
        previousHudEyes, c.coveredMask, true)) {
        ++c.coverageBlocked;
        suspendCoverage(c,true);
    } else {
        ++c.coverageEligible;
        c.eligibleStreak = c.lastEligible == pair.previous.sequence ? (std::min)(2u,c.eligibleStreak+1) : 1;
        c.lastEligible = pair.current.sequence;
        if (c.eligibleStreak == 2 && c.coverageCutHistory.load() == c.captureHistory) {
            std::lock_guard<std::mutex> lock(c.queueMutex);
            if (currentWork(c,c.capture) && c.coverageCutHistory.load() == c.captureHistory) {
                c.coverageCutHistory = 0;
                c.state = EDVR_FG_ARMING;
            }
        }
    }
    const auto now = GetTickCount64();
    if (now-c.lastCoverageLog >= 1000) {
        c.lastCoverageLog=now;
        Log::get().note("native fg: coverage generation=%llu config=%llu history=%llu requested=%u auto=%u known_blocked=%llu unknown=%llu eligible=%llu capture_sources=%llu guide_captures=%llu partial_signals=%llu.",
            (unsigned long long)c.generation,(unsigned long long)configuration.load(),(unsigned long long)c.history.load(),
            requested.load(),unsigned(automatic.load()),(unsigned long long)c.coverageBlocked,(unsigned long long)c.coverageUnknown,
            (unsigned long long)c.coverageEligible,(unsigned long long)c.captureStarts,(unsigned long long)c.guideCaptures,(unsigned long long)c.partialCheckpoints);
    }
}
} // namespace
void nativeFgBeforeTemporalEye(uint64_t sequence, unsigned eye, ID3D11Texture2D* source,
    unsigned guideWidth, unsigned guideHeight) noexcept {
    auto* c = current.load(std::memory_order_acquire); if (!enabled(c) || eye > 1) return;
    try {
        DWORD unclaimed = 0;
        c->renderThread.compare_exchange_strong(unclaimed, GetCurrentThreadId());
        if (!owner(*c)) {refuse(*c, TemporalMismatch); return;}
        if (!serviceProducer(*c)) return;
        if (c->captureSequence != sequence || c->captureHistory != c->history.load()) {
            c->submittedGuideWidth = guideWidth;
            c->submittedGuideHeight = guideHeight;
            if (c->coverageHistory != c->history.load()) {
                c->eligibleStreak = 0; c->lastEligible = 0; c->previousHudEyes = 0;
            } else if (c->doorMask != 3 || sequence != c->captureSequence + 1) {
                ++c->coverageUnknown;
                suspendCoverage(*c,false);
            }
            if (c->transaction) abortCapture(*c, TemporalMismatch);
            c->doorMask = c->coveredMask = 0;
            c->captureAllowed = c->observedAdjacent = false;
            c->coverageHistory = c->history.load();
            c->capture = {}; c->guideMask = c->uiMask = c->worldMask = c->temporalMask = 0;
            c->captureSequence = sequence; c->captureHistory = c->history.load(); c->rejected = false;
            std::lock_guard<std::mutex> lock(c->metadataMutex);
            const auto proof = c->reflexProof.load();
            if (!proof || proof != liveReflexProof(*c) || c->temporalSupport.load() != TemporalSupport::Supported ||
                c->sourceProof != proof || c->temporalProof != proof || !c->haveSource || c->temporal.sequence != sequence ||
                c->source.sequence != sequence || c->source.referenceGeneration != c->temporal.referenceGeneration ||
                c->source.configGeneration != c->temporalConfig || c->temporalConfig != configuration.load()) {
                suspendCoverage(*c,false);
                c->rejected = true; refuse(*c, TemporalMismatch);
            } else {
                const bool haveExactPrevious = c->havePrevious && c->previousTemporal.sequence == c->previousSource.sequence &&
                    c->previousTemporal.referenceGeneration == c->previousSource.referenceGeneration &&
                    c->previousTemporal.generation == c->previousSource.generation;
                c->capture.source.previous = haveExactPrevious ? c->previousSource : c->source;
                c->capture.source.current = c->source;
                c->capture.temporalPrevious = haveExactPrevious ? c->previousTemporal : c->temporal;
                c->capture.temporalCurrent = c->temporal;
                c->capture.reflexProof = proof;
                c->capture.source.historyGeneration = c->captureHistory; c->capture.source.multiplier = requested.load();
                c->observedAdjacent = haveExactPrevious && c->metadataHistory == c->captureHistory &&
                    c->metadataSequence == c->previousSource.sequence;
                c->metadataSequence = sequence; c->metadataHistory = c->captureHistory;
                c->captureAllowed = c->observedAdjacent && c->eligibleStreak >= 2 &&
                    c->lastEligible == c->previousSource.sequence &&
                    captureMayInterpolate(*c, c->previousSource, c->source, c->capture.source.multiplier,
                        c->previousHudEyes, c->previousHudEyes);
                if (c->captureAllowed) ++c->captureStarts;
            }
        }
        c->activeEye = c->rejected ? 2u : eye; c->submitted[eye] = source;
    } catch (...) {c->rejected = true; refuse(*c, TemporalMismatch);}
}
void nativeFgCaptureGuides(ID3D11DeviceContext* context, const NativeFgGuides& guides) noexcept {
    auto* c = current.load(std::memory_order_acquire);
    if (!enabled(c) || !owner(*c) || c->rejected || !c->captureAllowed || !currentWork(*c, c->capture) || guides.eye != c->activeEye) return;
    try {
        const auto resourceEpoch = c->resourceEpoch.load();
        if (!context || guides.eye > 1 || !guides.source || guides.source != c->submitted[guides.eye] ||
            !guides.depth || !guides.motion || !guides.width || !guides.height ||
            guides.outputWidth != c->capture.source.current.width || guides.outputHeight != c->capture.source.current.height ||
            c->capture.source.current.configGeneration != configuration.load() || (c->guideMask & (1u << guides.eye))) {
            abortCapture(*c, Guides); return;
        }
        if (guides.reset && c->resetSequence != c->captureSequence) {
            c->resetSequence = c->captureSequence;
            clearWork(*c,EDVR_FG_ARMING);
            suspendCoverage(*c,false);
            return;
        }
        uint32_t cameraRefusal = 0;
        if (!camera(guides, c->capture.cameras[guides.eye], &cameraRefusal)) {
            const auto count = c->refusedPairs.load() + 1;
            if (count <= 12 || count % 600 == 0)
                Log::get().note("native fg: camera refused sequence=%llu eye=%u slot=%u mask=0x%08x provenance=0x%08x scene_frame=%u head_frame=%u eyes_frame=%u selected_eye=%d previous_eye=%d draws=%u scene_projection=(%.9g,%.9g) raw_projection=(%.9g,%.9g) previous_projection=(%.9g,%.9g) jitter=(%.6g,%.6g) size=%ux%u.",
                    (unsigned long long)c->captureSequence,guides.eye,guides.internalEye,cameraRefusal,guides.provenance,
                    guides.sceneFrame,guides.headFrame,guides.eyesFrame,guides.selectedEye,guides.previousSelectedEye,guides.sceneDraws,
                    double(guides.sceneProjectionA),double(guides.sceneProjectionB),double(guides.rawProjection[0]),double(guides.rawProjection[1]),
                    double(guides.previousProjectionA),double(guides.previousProjectionB),double(guides.jitterPixels[0]),double(guides.jitterPixels[1]),guides.width,guides.height);
            abortCapture(*c, Camera); return;
        }
        if (!c->producer || c->producer->config != configuration.load() || c->producer->width != guides.outputWidth ||
            c->producer->height != guides.outputHeight || c->producer->guideWidth != guides.width || c->producer->guideHeight != guides.height) {
            if (c->transaction) {abortCapture(*c, Guides); return;}
            if(!c->retiringProducer.collect()) {++c->busyPairs;abortCapture(*c,Busy);return;}
            auto made = std::make_shared<Producer>(); made->context = context;
            made->width = guides.outputWidth; made->height = guides.outputHeight;
            made->guideWidth = guides.width; made->guideHeight = guides.height; made->config = configuration.load();
            made->resourceEpoch = resourceEpoch;
            FrameGenerationInteropInit init;
            init.enabled = true; init.device11 = c->device; init.context = context; init.renderThreadId = c->renderThread.load();
            init.colorWidth = made->width; init.colorHeight = made->height; init.guideWidth = made->guideWidth; init.guideHeight = made->guideHeight;
            if (!made->interop.initialize(init) || !made->color.initialize(true, c->device, context, c->renderThread)) {
                abortCapture(*c, Interop); return;
            }
            // Validate both UI capture caches against the render owner before
            // the new producer starts recording copies on this context.
            if (!uiLayerFgRetireStorage(context)) {abortCapture(*c, Ui); return;}
            const auto installed = installProducer(*c, std::move(made));
            if (installed == ProducerInstall::Busy) {++c->busyPairs;abortCapture(*c, Busy);return;}
            if (installed != ProducerInstall::Installed) {discardCapture(*c);return;}
        }
        if (!currentWork(*c, c->capture)) {discardCapture(*c); return;}
        if (!startWorker(*c)) {abortCapture(*c, Interop); return;}
        if (!c->transaction) {
            c->capture.producer = c->producer;
            if (!c->producer->interop.begin(context, c->captureSequence, c->captureHistory)) {
                const auto status=c->producer->interop.status();
                discardCapture(*c);refuseInterop(*c,status,c->producer->config);return;
            }
            c->transaction = true;
        }
        const auto identity = (c->captureSequence << 3) | (uint64_t(guides.eye) << 2) | 1;
        if (!c->producer->interop.capture(context, c->captureSequence, c->captureHistory, guides.eye, FrameGenerationPlane::Depth, guides.depth, identity) ||
            !c->producer->interop.capture(context, c->captureSequence, c->captureHistory, guides.eye, FrameGenerationPlane::Motion, guides.motion, identity + 1)) {
            abortCapture(*c, Interop); return;
        }
        c->guideMask |= 1u << guides.eye;
        ++c->guideCaptures;
    } catch (...) {abortCapture(*c, Guides);}
}
bool nativeFgGuideEye(ID3D11Texture2D* exactSource, unsigned& anatomicalEye) noexcept {
    auto* c = current.load(std::memory_order_acquire);
    if (!enabled(c) || !owner(*c) || c->rejected || !c->captureAllowed || !currentWork(*c, c->capture) || c->activeEye > 1 || !exactSource ||
        c->submitted[c->activeEye] != exactSource) return false;
    anatomicalEye = c->activeEye;
    return true;
}
void nativeFgCheckpointPartial(uint64_t sequence,unsigned eye) noexcept {
    // The producer route remains owned here even if admission was revoked.
    auto* c=current.load(std::memory_order_acquire);
    if(!c||!owner(*c)||!c->transaction||eye>1||c->captureSequence!=sequence)return;
    const auto prior=c->producer->interop.status().partialCheckpoints;
    if(!c->producer->interop.checkpoint(c->producer->context.Get())) {
        const auto status=c->producer->interop.status();
        discardCapture(*c);refuseInterop(*c,status,c->producer->config);
    } else if(c->producer->interop.status().partialCheckpoints!=prior) {
        ++c->partialCheckpoints;
        const auto now=GetTickCount64();
        if(now-c->lastCheckpointReportMs>=1000) {
            c->lastCheckpointReportMs=now;
            Log::get().note("native fg: producer handoff generation=%llu partial_signals=%llu source_pairs=%llu.",
                (unsigned long long)c->generation,(unsigned long long)c->partialCheckpoints,(unsigned long long)c->sourcePairs.load());
        }
    }
}
void nativeFgAfterTemporalEye(uint64_t sequence, unsigned eye, ID3D11Texture2D* output) noexcept {
    auto* c = current.load(std::memory_order_acquire); if (!c || !owner(*c) || eye > 1 || c->captureSequence != sequence) return;
    c->activeEye = 2;
    if (!output || !enabled(c) || c->captureHistory != c->history.load()) {if (c->transaction) abortCapture(*c, TemporalMismatch); return;}
    c->temporalOutput[eye] = output; c->temporalMask |= 1u << eye;
    nativeFgCheckpointPartial(sequence,eye);
}
void nativeFgCaptureDoor(uint64_t sequence, unsigned eye, ID3D11Texture2D* source,
    const uint32_t region[4], const float layerUv[4]) noexcept {
    auto* c = current.load(std::memory_order_acquire);
    if (!c || !owner(*c) || c->rejected || eye > 1 || c->captureSequence != sequence) return;
    try {
        if (!enabled(c) || !currentWork(*c,c->capture)) {if(c->transaction) abortCapture(*c,Ui);return;}
        bool hasUi = false;
        const bool admitted = source && source == c->temporalOutput[eye] && (c->temporalMask & (1u << eye)) &&
            uiLayerFgObserveCoverage(sequence,eye,source,region,layerUv,hasUi);
        observeDoor(*c,eye,admitted,hasUi);
        if (!c->captureAllowed || !c->transaction || !currentWork(*c,c->capture)) return;
        struct Submission { Coordinator& coordinator; Failure refusal = Ui; } submission{*c};
        const auto submit = +[](void* cookie, const UiLayerFgPlanes& planes) noexcept {
            auto& output = *static_cast<Submission*>(cookie);
            auto& coordinator = output.coordinator;
            try {
                const auto& info = planes.info;
                const auto targetEye = info.eye;
                auto& producer = *coordinator.producer;
                auto* context = producer.context.Get();
                auto* layer = planes.layer;
                auto* multiply = planes.multiply;
                if (info.empty && !producer.color.emptyUi(context, targetEye, info.width, info.height, false, true, &layer)) return false;
                if (!planes.useMult && !producer.color.emptyUi(context, targetEye, info.width, info.height, true, true, &multiply)) return false;
                const auto identity = (info.sequence << 3) | (uint64_t(targetEye) << 2) | 1;
                // Capture retains each source before recording its copy. Only
                // the sealed interop destinations are queued for the worker.
                if (!layer || !multiply || !producer.interop.capture(context, info.sequence, coordinator.captureHistory, targetEye,
                        FrameGenerationPlane::UiColorTransmittance, layer, identity) ||
                    !producer.interop.capture(context, info.sequence, coordinator.captureHistory, targetEye,
                        FrameGenerationPlane::UiMultiply, multiply, identity + 1)) {
                    output.refusal = Interop;
                    return false;
                }
                auto& ui = coordinator.capture.ui[targetEye];
                ui.empty = info.empty;
                ui.useMult = planes.useMult;
                std::memcpy(ui.uv, planes.layerUv, sizeof(ui.uv));
                return true;
            } catch (...) {return false;}
        };
        if (!uiLayerFgCaptureTo(c->producer->context.Get(), sequence, eye, source, region, layerUv, submit, &submission)) {
            abortCapture(*c, submission.refusal);
            return;
        }
        if (c->capture.ui[eye].empty == hasUi) {abortCapture(*c,Ui); return;}
        c->uiMask |= 1u << eye;
    } catch (...) {abortCapture(*c, Ui);}
}
void nativeFgCaptureWorld(uint64_t sequence, unsigned eye, ID3D11Texture2D* source, const uint32_t region[4]) noexcept {
    auto* c = current.load(std::memory_order_acquire);
    if (!c || !owner(*c) || c->rejected || !c->transaction || eye > 1 || c->captureSequence != sequence) return;
    try {
        if (!enabled(c)) {abortCapture(*c, Unsupported); return;}
        D3D11_TEXTURE2D_DESC desc{}; if (source) source->GetDesc(&desc);
        if (c->captureHistory != c->history.load() || !source || !region || region[0] || region[1] ||
            region[2] != desc.Width || region[3] != desc.Height || !(c->uiMask & (1u << eye)) || (c->worldMask & (1u << eye))) {
            abortCapture(*c, Unsupported); return;
        }
        ID3D11Texture2D* normalized = nullptr;
        auto* context = c->producer->context.Get();
        // Compatible RGBA8 color may borrow source directly. Enqueue the
        // immutable interop copy now, before its caller can reuse that source.
        if (!c->producer->color.normalize(context, eye, source, &normalized) || !c->producer->interop.capture(context,
            sequence, c->captureHistory, eye, FrameGenerationPlane::Color, normalized, (sequence << 3) | (uint64_t(eye) << 2) | 3)) {
            abortCapture(*c, Interop); return;
        }
        if (!c->producer->colorCaptureNoted[eye]) {
            D3D11_TEXTURE2D_DESC prepared{}; normalized->GetDesc(&prepared);
            const char* path = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? "borrowed-typeless" :
                desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? "borrowed-unorm" : "converted";
            Log::get().note("native fg: color capture eye=%u source_format=%u normalization=%s prepared_format=%u immutable_format=%u size=%ux%u.",
                eye, unsigned(desc.Format), path, unsigned(prepared.Format),
                unsigned(fg_interop::captureFormat(FrameGenerationPlane::Color, prepared.Format)), desc.Width, desc.Height);
            c->producer->colorCaptureNoted[eye] = true;
        }
        c->worldMask |= 1u << eye;
        if (c->worldMask != 3 || c->uiMask != 3 || c->guideMask != 3 || c->temporalMask != 3) return;
        c->capture.input = c->producer->interop.publish(context, sequence, c->captureHistory, fg_interop::allPlanes);
        c->transaction = false;
        if (!c->capture.input) {abortCapture(*c, Interop); return;}
        {
            std::lock_guard<std::mutex> lock(c->queueMutex);
            if (!currentWork(*c, c->capture)) {c->capture.input->cancel(); c->capture = {}; return;}
            if (c->queued.size() >= 3) {c->capture.input->cancel(); ++c->busyPairs; c->capture = {}; return;}
            const auto key = qualificationKey(*c, *c->producer, c->capture.source.multiplier);
            const auto qualification = claimQualification(*c, key);
            if (qualification == QualificationWork::Defer) {c->capture.input->cancel(); c->capture = {}; return;}
            c->capture.qualificationOnly = qualification == QualificationWork::Probe;
            try { c->queued.push_back(std::move(c->capture)); }
            catch (...) {
                if (qualification == QualificationWork::Probe) c->qualification = {};
                throw;
            }
            if (qualification == QualificationWork::Ordinary) {++c->sourcePairs; c->latestSource = sequence;}
        }
        c->wake.notify_all();
    } catch (...) {abortCapture(*c, Interop);}
}
void nativeFgInvalidateProducer(uint32_t reason) noexcept {
    if (auto* c = current.load(std::memory_order_acquire)) {
        if (c->closed.load()) return;
        if (reason == NativeFgUnsupportedBounds) temporalUnsupported(*c);
        else clearWork(*c, EDVR_FG_ARMING);
        c->failure = reason;
        if (reason == NativeFgUnsupportedBounds && owner(*c)) {
            if (c->transaction) abortCapture(*c, Unsupported);
            else {c->rejected = true; refuse(*c, Unsupported);}
            c->failure = reason;
        }
        c->wake.notify_all();
    }
}
#ifdef EDVR_FRAME_GENERATION_COORDINATOR_TEST
void nativeFgTestSetReflexProof(uint64_t proof) noexcept;
void nativeFgTestDoors(unsigned admitted, unsigned covered) noexcept;

namespace {
class FixtureOutputFence final : public ID3D12Fence {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override {return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef() override {return 2;}
    ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override {return E_NOTIMPL;}
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override {return value_.load();}
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override {return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override {value_.store(value);return S_OK;}
private:
    std::atomic<UINT64> value_{0};
};

template<class Check>
void testProducerReplacement(const Coordinator& source, Check check) {
    Coordinator c(source.device, source.generation);
    c.temporalSupport = TemporalSupport::Supported;
    c.capture.source.current.configGeneration = configuration.load();
    c.capture.source.historyGeneration = c.history.load();
    c.capture.source.multiplier = requested.load();
    c.capture.reflexProof = liveReflexProof(c);
    const auto historyBeforeResize = c.history.load();
    const auto resourceEpoch = c.resourceEpoch.load();
    auto first = std::make_shared<Producer>();
    first->resourceEpoch = resourceEpoch;
    first->width = 1280;
    check(installProducer(c, first) == ProducerInstall::Installed && c.history == historyBeforeResize && first->identity != 0);

    Work displaced = c.capture;
    displaced.producer = first;
    Publication stale{};
    stale.config = configuration.load();
    stale.history = historyBeforeResize;
    stale.reflexProof = c.capture.reflexProof;
    stale.multiplier = c.capture.source.multiplier;
    stale.producerIdentity = first->identity;
    check(currentWork(c, displaced) && currentPublication(c, stale));
    c.queued.push_back(displaced);
    c.ready.push_back(stale);
    c.active = stale.multiplier;
    c.state = EDVR_FG_READY;

    auto resized = std::make_shared<Producer>();
    resized->resourceEpoch = resourceEpoch;
    resized->width = 1440;
    check(installProducer(c, resized) == ProducerInstall::Replaced);
    check(c.resourceEpoch == resourceEpoch && c.history > historyBeforeResize && c.queued.empty() && c.ready.empty());
    check(c.active == 0 && c.state == EDVR_FG_ARMING && resized->identity != first->identity);

    displaced.source.historyGeneration = stale.history = c.history.load();
    check(!currentWork(c, displaced) && !currentPublication(c, stale));
    displaced.producer = resized;
    stale.producerIdentity = resized->identity;
    check(currentWork(c, displaced) && currentPublication(c, stale));
    check(installProducer(c, std::make_shared<Producer>()) == ProducerInstall::Superseded && c.producer == resized);
}

template<class Check>
void testPendingQualification(const Coordinator& source, Check check) {
    Coordinator c(source.device, source.generation, true);
    c.temporalSupport = TemporalSupport::Supported;
    const auto config = configuration.load();
    const fg_recomposition::BackendKey key{c.generation, config, c.resourceEpoch.load(),
        1, 2, 1280, 720, 960, 540, DXGI_FORMAT_R8G8B8A8_UNORM, 2};
    c.liveProducerIdentity = key.producer;
    EdvrFgSource previous{}, next{};
    previous.configGeneration = next.configGeneration = config;
    previous.width = next.width = key.width;
    previous.height = next.height = key.height;
    for (unsigned eye = 0; eye < 2; ++eye) {
        previous.views[eye].orientation[3] = next.views[eye].orientation[3] = 1;
        next.views[eye].position[0] = 1.f;
    }
    check(claimQualification(c, key) == QualificationWork::Probe);
    check(currentQualification(c, key) && claimQualification(c, key) == QualificationWork::Defer);
    check(captureMayInterpolate(c, previous, next, key.multiplier, 3, 3, true));
    check(!captureMayInterpolate(c, previous, next, key.multiplier, 3, 3));
    clearWorkLocked(c, EDVR_FG_ARMING);
    check(!currentQualification(c, key) && claimQualification(c, key) == QualificationWork::Probe);
    c.qualification.started = true;
    clearWorkLocked(c, EDVR_FG_ARMING);
    check(currentQualification(c, key) && claimQualification(c, key) == QualificationWork::Defer);
    c.qualification.mode = fg_recomposition::Mode::Legacy;
    check(!captureMayInterpolate(c, previous, next, key.multiplier, 3, 3) &&
        claimQualification(c, key) == QualificationWork::Ordinary);
    c.submittedGuideWidth = key.guideWidth + 1;
    c.submittedGuideHeight = key.guideHeight;
    check(captureMayInterpolate(c, previous, next, key.multiplier, 3, 3));
    c.submittedGuideWidth = key.guideWidth;

    Publication result{};
    result.qualifiedRecomposition = true;
    result.config = config;
    result.history = c.history.load();
    result.reflexProof = liveReflexProof(c);
    result.multiplier = key.multiplier;
    result.producerIdentity = key.producer;
    c.qualification.mode = fg_recomposition::Mode::Verified;
    c.qualification.provenHudEyes = 3;
    check(captureMayInterpolate(c, previous, next, key.multiplier, 3, 3));
    check(captureMayInterpolate(c, previous, next, key.multiplier, 0, 0));
    check(!captureMayInterpolate(c, previous, next, key.multiplier, 3, 0, true));
    check(!captureMayInterpolate(c, previous, next, key.multiplier, 1, 1, true));
    check(!currentPublication(c, result));
    c.qualification.seededHistory = result.history;
    check(currentPublication(c, result));
    ++c.liveProducerIdentity;
    check(!currentPublication(c, result));
    quarantineTrial(c);
    check(!selected(&c) && c.state == EDVR_FG_FAILED && terminalFor(c, config + 1));
    qualificationQuarantined.store(false, std::memory_order_release); // CPU fixture only.
}

template<class Check>
void testFencedBootstrap(Coordinator& c, const EdvrFgTable& table, Check check) {
    clearWork(c, EDVR_FG_ARMING);
    FixtureOutputFence fence;
    FrameGenerationOutput output;
    std::shared_ptr<FrameGenerationInteropLease> pins[3];
    EdvrFgBatch batches[3]{};
    FrameGenerationOutputTicket tickets[3];
    const auto epoch = c.resourceEpoch.load();
    const auto config = configuration.load(), history = c.history.load(), proof = liveReflexProof(c);
    const auto multiplier = requested.load();
    for (unsigned i=0;i<3;++i) {
        // Only the shared ownership is used. The actual output collector
        // releases its strong input pin as it does with a real producer slot.
        auto identity = std::make_shared<int>(0);
        pins[i] = std::shared_ptr<FrameGenerationInteropLease>(identity,
            reinterpret_cast<FrameGenerationInteropLease*>(identity.get()));
        auto& batch = batches[i];
        batch.size=sizeof(batch);batch.version=EDVR_FG_BATCH_VERSION_4;batch.allocationId=i+1;batch.generation=c.generation;
        batch.configGeneration=config;batch.historyGeneration=history;batch.currentSequence=c.captureSequence+i+1;
        batch.previousSequence=batch.currentSequence-1;
        batch.multiplier=multiplier;batch.generatedCount=i ? multiplier-1 : 0;
        batch.phaseMask=i ? (1u<<multiplier)-1u : 1u;
    }
    const bool seeded = output.testSeedRetirement(&fence,pins,batches,tickets);
    check(seeded);
    if (!seeded) return;
    for (unsigned i=0;i<3;++i)
        c.ready.push_back({std::move(tickets[i]),batches[i].currentSequence,config,history,proof,multiplier});
    EdvrFgStatus report{sizeof(report),1};
    promoteReady(c);
    check(table.getStatus(table.context,&report)==S_OK && report.state==EDVR_FG_ARMING &&
        !openxr::fgadmission::providerReady(report,c.generation));
    check(pins[0].use_count()==2 && pins[1].use_count()==2 && pins[2].use_count()==2 && c.queued.empty());

    // Even a completed zero-generated seed cannot open independent XR
    // ownership. The two actual positive publications still await the fence.
    fence.Signal(1);
    promoteReady(c);
    check(table.getStatus(table.context,&report)==S_OK && !openxr::fgadmission::providerReady(report,c.generation));
    EdvrFgBatch tooSoon{sizeof(tooSoon),EDVR_FG_BATCH_VERSION_4};
    check(table.poll(table.context,&tooSoon)==S_FALSE && !tooSoon.token);
    c.ready.pop_front(); // Ordinary native already presented this seed.

    // Fence completion creates no Work and sends no coordinator notification.
    // The watchdog only unblocks a broken unconditional wait to fail the rig.
    std::mutex signalMutex;
    std::condition_variable signalWake;
    bool finished = false;
    std::atomic<bool> watchdog{false};
    std::thread signal([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        fence.Signal(3);
        std::unique_lock<std::mutex> lock(signalMutex);
        if (signalWake.wait_for(lock,std::chrono::seconds(1),[&]{return finished;})) return;
        watchdog.store(true);
        {
            std::lock_guard<std::mutex> queue(c.queueMutex);
            ++c.resourceEpoch;
        }
        c.wake.notify_all();
    });
    while (pendingRetirement(nullptr,&output) && !watchdog.load()) {
        {
            std::unique_lock<std::mutex> lock(c.queueMutex);
            waitForWorkerActivity(c,lock,epoch,true);
        }
        if (!output.collect()) break;
        promoteReady(c);
    }
    {
        std::lock_guard<std::mutex> lock(signalMutex);
        finished=true;
    }
    signalWake.notify_all();
    signal.join();

    check(!watchdog.load() && !pendingRetirement(nullptr,&output) && c.queued.empty() &&
        pins[0].use_count()==1 && pins[1].use_count()==1 && pins[2].use_count()==1);
    check(table.getStatus(table.context,&report)==S_OK && openxr::fgadmission::providerReady(report,c.generation) &&
        c.latestReady.load()==batches[2].currentSequence && !report.latestReadySequence);
    EdvrFgBatch unsupported{sizeof(EdvrFgBatch),EDVR_FG_BATCH_VERSION_3};
    check(table.poll(table.context,&unsupported)==E_INVALIDARG&&!unsupported.token&&c.ready.size()==2&&!c.latestExported.load());
    EdvrFgBatch claimed{sizeof(claimed),EDVR_FG_BATCH_VERSION_4};
    check(table.poll(table.context,&claimed)==S_OK && claimed.token && claimed.generatedCount==multiplier-1 && claimed.phaseMask==batches[1].phaseMask);
    check(table.getStatus(table.context,&report)==S_OK && report.latestReadySequence==batches[1].currentSequence &&
        report.latestReadySequence==claimed.currentSequence);
    if (claimed.token) {table.discard(claimed.token);table.release(claimed.token);}
    clearUnexported(c);
    check(output.drain(0));
}

// Acquire the real coordinator for each process override without starting
// the GPU worker. Restore the rig's original environment before returning.
template<class Check>
void testUiRecompositionSelection(EdvrFgRequest request, Check check) {
    wchar_t previous[32768]{};
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableW(kUiRecompositionOverride, previous, _countof(previous));
    const DWORD error = GetLastError();
    const bool readable = length < _countof(previous) &&
        (length != 0 || error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND);
    check(readable);
    if (!readable) return;
    const bool absent = length == 0 && error == ERROR_ENVVAR_NOT_FOUND;
    const auto setOverride = [](const wchar_t* value) {
        return SetEnvironmentVariableW(kUiRecompositionOverride, value) ||
            (!value && GetLastError() == ERROR_ENVVAR_NOT_FOUND);
    };
    struct Selection { const wchar_t* value; bool allow; };
    for (const auto& selection : {Selection{nullptr, true}, Selection{L"1", true}, Selection{L"0", false},
        Selection{L"", false}, Selection{L"x", false}, Selection{L"10", false}}) {
        const bool set = setOverride(selection.value);
        check(set);
        if (!set) continue;
        ++request.generation;
        EdvrFgTable table{sizeof(table), 1};
        const bool acquired = nativeFgAcquire(&request, &table) == S_OK;
        check(acquired);
        if (!acquired) continue;
        const auto* coordinator = identify(table.context);
        check(coordinator && wantsQualification(*coordinator, 2) == selection.allow);
        check(coordinator && !wantsQualification(*coordinator, 0) &&
            !wantsQualification(*coordinator, 3) && !wantsQualification(*coordinator, 4));
        check(table.close(table.context) == S_OK);
    }
    check(setOverride(absent ? nullptr : previous));
}
} // namespace

unsigned nativeFgTestProtocol(unsigned* checksOut) noexcept {
    unsigned failures = 0, checks = 0;
    const auto check = [&](bool pass) {++checks; if (!pass) ++failures;};
    const auto closeFloat = [](float a, float b) {return std::fabs(a - b) < 1e-5f;};
    {
        struct Recorder {
            unsigned count=0;bool reset[2][2]{};uint64_t sequences[2]{};
            FrameGenerationNgxTicket submit(const FrameGenerationNgxInput& input) {
                if(count<2){sequences[count]=input.sequence;for(unsigned eye=0;eye<2;++eye)reset[count][eye]=input.eyes[eye].camera.reset;}
                ++count;return {};
            }
        } recorder;
        FrameGenerationNgxInput input{};FrameGenerationOutputSource pair{};FrameGenerationOutputUi ui[2]{};
        EvaluationRoute route{};pair.multiplier=4;
        input.sequence=pair.previous.sequence=pair.current.sequence=11;
        evaluateSource(recorder,input,pair,ui,false,route);
        check(route==EvaluationRoute::Seed&&recorder.count==1&&recorder.sequences[0]==11&&recorder.reset[0][0]&&recorder.reset[0][1]);
        input={};input.sequence=pair.current.sequence=12;
        pair.current.views[1].position[0]=1.f;ui[1].empty=false;
        auto ticket=evaluateSource(recorder,input,pair,ui,true,route);
        check(route==EvaluationRoute::NativeCoverage&&recorder.count==1&&!ticket.lease&&!ticket.completionFence);
        ui[0].empty=ui[1].empty=true;
        evaluateSource(recorder,input,pair,ui,true,route);
        check(route==EvaluationRoute::Interpolate&&recorder.count==2&&recorder.sequences[1]==12&&!recorder.reset[1][0]&&!recorder.reset[1][1]);
        ui[1].empty=false;pair.previous.views[1].position[0]=1.f;
        pair.current.views[1].position[0]=std::nextafter(1.f,2.f);
        evaluateSource(recorder,input,pair,ui,true,route);
        check(!fg_coverage::fits(pair.previous.views,pair.current.views,4,1)&&fg_coverage::fits(pair.previous.views,pair.current.views,4,3)&&
            route==EvaluationRoute::Interpolate&&recorder.count==3);
        ticket.historyResetReasons=fg_ngx::kResetSequenceGap;
        check(requiresNativeOutput(route,ticket)); // Newly eligible after skipped B: re-seed C without presenting B->C.
        ticket.historyResetReasons=0;check(!requiresNativeOutput(route,ticket));
        pair.current.views[1].position[0]=100.f;
        evaluateSource(recorder,input,pair,ui,false,route);
        check(route==EvaluationRoute::Seed&&recorder.count==4);
        auto* captured = reinterpret_cast<ID3D12Resource*>(static_cast<uintptr_t>(1));
        input.eyes[0].hudless = input.eyes[0].ui = captured;
        evaluateSource(recorder,input,pair,ui,true,route,true);
        check(route==EvaluationRoute::NativeCoverage&&recorder.count==4);
        input.eyes[1].hudless = input.eyes[1].ui = captured;
        evaluateSource(recorder,input,pair,ui,true,route);
        check(route==EvaluationRoute::NativeCoverage&&recorder.count==4);
        evaluateSource(recorder,input,pair,ui,true,route,true);
        check(route==EvaluationRoute::Interpolate&&recorder.count==5);
        Coordinator legacyCapture(nullptr, 1), recomposedCapture(nullptr, 1, true);
        check(!captureMayInterpolate(legacyCapture,pair.previous,pair.current,pair.multiplier,3,3));
        check(!captureMayInterpolate(recomposedCapture,pair.previous,pair.current,pair.multiplier,3,3));
        check(captureMayInterpolate(recomposedCapture,pair.previous,pair.current,2,3,3));
    }
    NativeFgGuides guide;
    guide.width = 2133; guide.height = 1600;
    guide.outputWidth = 3200; guide.outputHeight = 2400;
    guide.cameraValid = guide.depthAvailable = true;
    const float bounds[4] = {-1.2f,1.f,-1.1f,1.3f};
    const float previousBounds[4] = {-1.1f,1.1f,-1.f,1.2f};
    std::memcpy(guide.frustum,bounds,sizeof(bounds));
    std::memcpy(guide.previousFrustum,previousBounds,sizeof(previousBounds));
    guide.sceneProjectionB = guide.previousProjectionB = .025f;
    guide.previousProjectionA = -.000001f;
    guide.cameraNow[0] = guide.cameraNow[5] = guide.cameraNow[10] = 1;
    guide.cameraPrevious[0] = guide.cameraPrevious[5] = guide.cameraPrevious[10] = 1;
    guide.cameraNow[3] = 1; guide.cameraNow[7] = 2; guide.cameraNow[11] = 3;
    guide.cameraPrevious[3] = .5f; guide.cameraPrevious[7] = 1.75f; guide.cameraPrevious[11] = 2.5f;
    guide.jitterPixels[0] = .25f; guide.jitterPixels[1] = -.3f;
    FrameGenerationCamera options{};
    check(camera(guide, options));
    check(closeFloat(options.cameraPos[0],1) && closeFloat(options.cameraPos[1],2) && closeFloat(options.cameraPos[2],-3));
    check(closeFloat(options.cameraFwd[2],-1) && options.depthInverted && options.cameraMotionIncluded);
    check(closeFloat(options.jitterOffset[0],.5f/2133) && closeFloat(options.jitterOffset[1],.6f/1600));
    check(options.mvecScale[0] == 1.f && options.mvecScale[1] == 1.f);
    check(closeFloat(options.cameraNear,.025f));
    // Known asymmetric-frustum point with a nonzero camera translation. The
    // previous point is (.3,.35,-5.5), independent of matrix helper inversion.
    const float currentClip[4] = {2.f/2.2f*-.2f + (-.2f/2.2f)*-5.f,
        2.f/2.4f*.1f + (.2f/2.4f)*-5.f, .025f, 5.f};
    float previousClip[4]{};
    for (unsigned col=0;col<4;++col) for(unsigned row=0;row<4;++row)
        previousClip[col] += currentClip[row]*options.clipToPrevClip[row][col];
    check(closeFloat(previousClip[0],2.f/2.2f*.3f));
    check(closeFloat(previousClip[1],2.f/2.2f*.35f + .2f/2.2f*-5.5f));
    check(closeFloat(previousClip[2],-.000001f*5.5f+.025f) && closeFloat(previousClip[3],5.5f));
    // A stationary camera at a large world offset cannot move a point at
    // either near distance when its current clip is mapped to previous clip.
    NativeFgGuides stationary = guide;
    std::memcpy(stationary.previousFrustum, stationary.frustum, sizeof(stationary.frustum));
    stationary.previousProjectionA = stationary.sceneProjectionA;
    const float cYaw = std::cos(.65f), sYaw = std::sin(.65f);
    const float pose[12] = {cYaw,0,sYaw,4000, 0,1,0,2750, -sYaw,0,cYaw,1100};
    std::memcpy(stationary.cameraNow, pose, sizeof(pose));
    std::memcpy(stationary.cameraPrevious, pose, sizeof(pose));
    check(camera(stationary, options) && !options.motionVectorsDilated);
    for (float distance : {.3f, 1.f}) {
        const auto& m = options.clipToPrevClip;
        const float x = .025f * m[2][0] + distance * m[3][0];
        const float y = .025f * m[2][1] + distance * m[3][1];
        const float w = .025f * m[2][3] + distance * m[3][3];
        check(std::fabs(x / w * 1920.f) < .0001f && std::fabs(y / w * 1080.f) < .0001f);
    }
    guide.originJump=true; check(!camera(guide,options)); guide.originJump=false;
    guide.depthAvailable=false; check(!camera(guide,options)); guide.depthAvailable=true;
    guide.cameraNow[0]=2; check(!camera(guide,options)); guide.cameraNow[0]=1;
    guide.sceneProjectionA=.1f; check(!camera(guide,options)); guide.sceneProjectionA=0;
    guide.jitterPixels[0]=(std::numeric_limits<float>::infinity)(); check(!camera(guide,options));

    nativeFgSetRequestedMultiplier(0);
    EdvrFgRequest request{sizeof(request),1,reinterpret_cast<ID3D11Device*>(uintptr_t(1)),7};
    EdvrFgTable table{};table.size=sizeof(table);table.version=1;
    check(nativeFgAcquire(&request,&table)==S_OK);
    check(nativeFgMaximumMultiplier()==0);
    current.load()->maximumMultiplier=2;check(nativeFgMaximumMultiplier()==2);
    EdvrFgStatus report{};report.size=sizeof(report);report.version=1;
    check(table.getStatus(table.context,&report)==S_OK && report.state==EDVR_FG_OFF);
    EdvrFgSource source{}; source.size=sizeof(source);source.version=1;source.generation=7;
    source.referenceGeneration=2;source.sequence=1;source.poseDisplayTime=1000000;source.width=source.height=3200;
    for(auto& view:source.views){view.orientation[3]=1;view.fov[0]=view.fov[3]=-.8f;view.fov[1]=view.fov[2]=.8f;}
    check(table.beginSource(table.context,&source)==S_FALSE);
    nativeFgSetRequestedMultiplier(2);
    table.getStatus(table.context,&report);source.configGeneration=report.configGeneration;
    check(report.state==EDVR_FG_UNAVAILABLE && !report.activeMultiplier && table.beginSource(table.context,&source)==S_FALSE);
    nativeFgTestSetReflexProof(1);
    source.views[0].orientation[3]=2;check(table.beginSource(table.context,&source)==E_INVALIDARG);
    source.views[0].orientation[3]=1;check(table.beginSource(table.context,&source)==S_OK);
    check(table.beginSource(table.context,&source)==E_INVALIDARG);
    auto* c=identify(table.context);
    // A real inaccessible trailing page catches any unbounded read of the
    // optional zoom tail from the ordinary NO_ZOOM wire-size caller.
    SYSTEM_INFO info{};GetSystemInfo(&info);
    auto* pages=static_cast<unsigned char*>(VirtualAlloc(nullptr,size_t(info.dwPageSize)*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    check(pages!=nullptr);
    if(pages){
        DWORD protection=0;
        const bool guarded=VirtualProtect(pages+info.dwPageSize,info.dwPageSize,PAGE_NOACCESS,&protection)!=FALSE;
        check(guarded);
        if(guarded){
            auto* frame=reinterpret_cast<EdvrNativeTemporalFrame*>(pages+info.dwPageSize-EDVR_NATIVE_TEMPORAL_FRAME_SIZE_NO_ZOOM);
            std::memset(frame,0,EDVR_NATIVE_TEMPORAL_FRAME_SIZE_NO_ZOOM);
            frame->size=EDVR_NATIVE_TEMPORAL_FRAME_SIZE_NO_ZOOM;frame->version=1;frame->generation=7;frame->referenceGeneration=2;frame->sequence=1;
            nativeFgBeginTemporal(*frame,true,true);
            check(c->temporalSupport.load()==TemporalSupport::Supported && c->temporal.sequence==1 && c->temporal.zoomed==0);
            auto* exact=reinterpret_cast<ID3D11Texture2D*>(uintptr_t(2));
            nativeFgBeforeTemporalEye(1,1,exact);
            unsigned anatomical=2;
            check(!nativeFgGuideEye(exact,anatomical) && c->activeEye==1);
            check(!nativeFgGuideEye(reinterpret_cast<ID3D11Texture2D*>(uintptr_t(3)),anatomical));
            check(c->capture.source.previous.sequence==1 && c->capture.source.current.sequence==1);
            NativeFgGuides invalid;invalid.eye=1;invalid.source=exact;
            nativeFgCaptureGuides(nullptr,invalid);
            table.getStatus(table.context,&report);check(report.state==EDVR_FG_ARMING && !c->workerStarted.load() && !c->rejected);
            source.sequence=2;source.poseDisplayTime+=1000000;
            check(table.beginSource(table.context,&source)==S_OK && c->havePrevious);
            frame->sequence=2;nativeFgBeginTemporal(*frame,false,true);
            nativeFgBeforeTemporalEye(2,0,exact);
            check(c->capture.source.previous.sequence==1 && c->capture.source.current.sequence==2 && !c->rejected);

            testProducerReplacement(*c,check);
            testPendingQualification(*c,check);
            testFencedBootstrap(*c,table,check);
            nativeFgBeforeTemporalEye(2,0,exact);
            check(currentWork(*c,c->capture));
            c->queued.emplace_back();c->ready.emplace_back();
            const auto provedHistory=c->history.load();
            nativeFgTestSetReflexProof(0);
            check(table.getStatus(table.context,&report)==S_OK && report.state==EDVR_FG_UNAVAILABLE && !report.activeMultiplier &&
                report.historyGeneration>provedHistory && c->queued.empty() && c->ready.empty());
            check(!openxr::fgadmission::providerReady(report,c->generation));
            check(!nativeFgGuideEye(exact,anatomical) && !currentWork(*c,c->capture) && table.beginSource(table.context,&source)==S_FALSE);

            nativeFgTestSetReflexProof(2);
            check(table.getStatus(table.context,&report)==S_OK && report.state==EDVR_FG_ARMING && !report.activeMultiplier);
            source.sequence=3;source.poseDisplayTime+=1000000;
            check(table.beginSource(table.context,&source)==S_OK && !c->havePrevious);
            frame->sequence=3;nativeFgBeginTemporal(*frame,false,false);
            check(table.getStatus(table.context,&report)==S_OK && report.state==EDVR_FG_UNAVAILABLE && !nativeFgGuideEye(exact,anatomical));
            const auto unsupportedHistory=report.historyGeneration;
            source.sequence=4;source.poseDisplayTime+=1000000;
            check(table.beginSource(table.context,&source)==S_OK);
            frame->sequence=4;nativeFgBeginTemporal(*frame,true,true);
            nativeFgBeforeTemporalEye(4,0,exact);
            check(table.getStatus(table.context,&report)==S_OK && report.state==EDVR_FG_ARMING && report.historyGeneration>unsupportedHistory &&
                !nativeFgGuideEye(exact,anatomical) && c->capture.source.previous.sequence==4 && c->capture.reflexProof==2);
            testFencedBootstrap(*c,table,check);
        }
        VirtualFree(pages,0,MEM_RELEASE);
    }
    const auto before=c->history.load();check(table.invalidate(table.context,9)==S_OK && c->history.load()==before+1);
    EdvrFgBatch batch{};batch.size=sizeof(batch);batch.version=EDVR_FG_BATCH_VERSION_4;check(table.poll(table.context,&batch)==S_FALSE && !batch.token);
    FrameGenerationOutputStatus outputStatus;outputStatus.initialized=true;
    outputStatus.failure=FrameGenerationOutputFailure::Busy;outputStatus.lastHresult=S_FALSE;
    for (const auto state : {EDVR_FG_ARMING,EDVR_FG_READY}) {
        c->state=state;
        const auto busyBefore=c->busyPairs.load(),refusedBefore=c->refusedPairs.load(),historyBefore=c->history.load();
        const auto sourceBefore=c->sourcePairs.load(),generatedBefore=c->generatedPairs.load(),suppressedBefore=c->suppressedPairs.load();
        refuseOutput(*c,outputStatus);
        check(table.getStatus(table.context,&report)==S_OK && report.state==state && report.lastFailure==Busy);
        check(report.busyPairs==busyBefore+1 && report.refusedPairs==refusedBefore+1 && report.historyGeneration==historyBefore);
        check(report.sourcePairs==sourceBefore && report.generatedPairs==generatedBefore && report.suppressedPairs==suppressedBefore);
    }
    // Only the observed healthy Busy/S_FALSE result may preserve admission.
    for (unsigned variant=0;variant<7;++variant) {
        auto rejected=outputStatus;
        if(variant==0)rejected.initialized=false;
        if(variant==1)rejected.failed=true;
        if(variant==2)rejected.quarantined=true;
        if(variant==3)rejected.lastHresult=S_OK;
        if(variant==4)rejected.lastHresult=E_FAIL;
        if(variant==5)rejected.failure=FrameGenerationOutputFailure::Pending;
        if(variant==6)rejected.failure=FrameGenerationOutputFailure::Allocation;
        c->state=EDVR_FG_READY;const auto busyBefore=c->busyPairs.load();
        refuseOutput(*c,rejected);
        check(table.getStatus(table.context,&report)==S_OK && report.state==EDVR_FG_UNAVAILABLE &&
            report.lastFailure==Output && report.busyPairs==busyBefore);
    }
    c->state=EDVR_FG_FAILED;refuseOutput(*c,outputStatus);
    check(table.getStatus(table.context,&report)==S_OK && report.state==EDVR_FG_FAILED);
    auto fatalOutput=outputStatus;fatalOutput.quarantined=true;refuseOutput(*c,fatalOutput);
    check(table.getStatus(table.context,&report)==S_OK && report.state==EDVR_FG_FAILED);
    nativeFgSetRequestedMultiplier(5);table.getStatus(table.context,&report);check(report.requestedMultiplier==0 && report.state==EDVR_FG_OFF);
    nativeFgSetRequestedMultiplier(3);table.getStatus(table.context,&report);
    check(report.requestedMultiplier==3 && report.state==EDVR_FG_ARMING && !report.activeMultiplier);
    FrameGenerationNgxStatus ngxStatus;
    check(!terminalNgx(FrameGenerationNgxCompletion::Stale,ngxStatus));
    check(!terminalNgx(FrameGenerationNgxCompletion::Suppressed,ngxStatus));
    check(!terminalNgx(FrameGenerationNgxCompletion::FailedEvaluation,ngxStatus));
    check(terminalNgx(FrameGenerationNgxCompletion::Expired,ngxStatus));
    check(terminalNgx(FrameGenerationNgxCompletion::DeviceRemoved,ngxStatus));
    ngxStatus.failed=true;
    check(terminalNgx(FrameGenerationNgxCompletion::Empty,ngxStatus)); // sticky collect/submit refusal
    const auto failedConfig=report.configGeneration;
    source.configGeneration=failedConfig; ++source.sequence; source.poseDisplayTime+=1000000;
    check(table.beginSource(table.context,&source)==S_OK);
    EdvrNativeTemporalFrame terminalFrame{};
    terminalFrame.size=sizeof(terminalFrame);terminalFrame.version=1;terminalFrame.generation=source.generation;
    terminalFrame.referenceGeneration=source.referenceGeneration;terminalFrame.sequence=source.sequence;
    nativeFgBeginTemporal(terminalFrame,true,true);
    auto* terminalTexture=reinterpret_cast<ID3D11Texture2D*>(uintptr_t(2));
    nativeFgBeforeTemporalEye(source.sequence,0,terminalTexture);
    const auto verifyCoverage=[&](int line,bool pass) {
        if(!pass) std::fprintf(stderr,"FAIL coordinator coverage at line %d\n",line);
        check(pass);
    };
    // Exercise the real source, temporal and HUD calls with a metadata-only
    // door stub. All GPU copy/NGX stubs still abort if observation enters one.
    const auto observe=[&](float rightX,unsigned accepted,unsigned covered,unsigned eyes=3u) {
        source.views[1].position[0]=rightX;
        ++source.sequence;source.poseDisplayTime+=1000000;
        verifyCoverage(__LINE__,table.beginSource(table.context,&source)==S_OK);
        terminalFrame.sequence=source.sequence;
        nativeFgBeginTemporal(terminalFrame,false,true);
        nativeFgTestDoors(accepted,covered);
        const uint32_t rect[4]={0,0,source.width,source.height};
        const float uv[4]={0,0,1,1};
        unsigned allowed=0;
        for(unsigned eye=0;eye<2;++eye) {
            if(!(eyes&(1u<<eye)))continue;
            auto* pixel=reinterpret_cast<ID3D11Texture2D*>(uintptr_t(2+eye));
            nativeFgBeforeTemporalEye(source.sequence,eye,pixel);
            unsigned actual=2;
            if(nativeFgGuideEye(pixel,actual)){verifyCoverage(__LINE__,actual==eye);allowed|=1u<<eye;}
            else {
                NativeFgGuides missing;missing.eye=eye;missing.source=pixel;
                nativeFgCaptureGuides(nullptr,missing); // suspended even on a direct guide callback
            }
            nativeFgAfterTemporalEye(source.sequence,eye,pixel);
            nativeFgCaptureDoor(source.sequence,eye,pixel,rect,uv);
            nativeFgCaptureWorld(source.sequence,eye,pixel,rect);
            nativeFgCheckpointPartial(source.sequence,eye);
        }
        return allowed;
    };
    verifyCoverage(__LINE__,observe(1.f,3,2)==0);
    verifyCoverage(__LINE__,table.getStatus(table.context,&report)==S_OK && report.state==EDVR_FG_UNAVAILABLE &&
        report.lastFailure==EDVR_FG_NATIVE_COVERAGE && !report.activeMultiplier && report.requestedMultiplier==3);
    const auto cut=report.historyGeneration;
    verifyCoverage(__LINE__,table.invalidate(table.context,EDVR_FG_NATIVE_COVERAGE)==S_OK && c->history.load()==cut);
    for(unsigned n=2;n<9;++n)verifyCoverage(__LINE__,observe(float(n),3,2)==0);
    verifyCoverage(__LINE__,c->history.load()==cut && c->captureStarts==0 && !c->guideCaptures &&
        !c->partialCheckpoints && !c->workerStarted && c->queued.empty() && c->ready.empty());
    verifyCoverage(__LINE__,observe(9.f,1,0)==0); // right eye unknown
    verifyCoverage(__LINE__,table.getStatus(table.context,&report)==S_OK && report.lastFailure!=EDVR_FG_NATIVE_COVERAGE);
    verifyCoverage(__LINE__,observe(10.f,3,0)==0); // two confirmed empty eyes allow translated A/B
    verifyCoverage(__LINE__,observe(11.f,3,0)==0);
    verifyCoverage(__LINE__,observe(12.f,3,0)==0);
    verifyCoverage(__LINE__,observe(12.5f,3,0)==3);
    verifyCoverage(__LINE__,observe(13.f,3,2)==3); // new coverage known only at the current door
    verifyCoverage(__LINE__,table.getStatus(table.context,&report)==S_OK && report.lastFailure==EDVR_FG_NATIVE_COVERAGE && !report.activeMultiplier);
    verifyCoverage(__LINE__,observe(13.f,3,2,1)==0); // incomplete pair cannot rearm
    verifyCoverage(__LINE__,observe(13.f,3,2)==0);
    verifyCoverage(__LINE__,observe(13.f,3,2)==0);
    verifyCoverage(__LINE__,observe(13.f,3,2)==0);
    verifyCoverage(__LINE__,observe(13.f,3,2)==3);
    const auto oldProof=liveReflexProof(*c);
    nativeFgTestSetReflexProof(0);verifyCoverage(__LINE__,table.getStatus(table.context,&report)==S_OK&&!report.activeMultiplier);
    nativeFgTestSetReflexProof(oldProof+1);verifyCoverage(__LINE__,table.getStatus(table.context,&report)==S_OK);
    verifyCoverage(__LINE__,observe(13.f,3,2)==0); // first source in new proof cannot pair with old source
    verifyCoverage(__LINE__,observe(13.f,3,2)==0);
    verifyCoverage(__LINE__,observe(13.f,3,2)==0);
    verifyCoverage(__LINE__,observe(13.f,3,2)==3);
    nativeFgBeforeTemporalEye(source.sequence,0,terminalTexture);
    unsigned terminalEye=2;
    verifyCoverage(__LINE__,nativeFgGuideEye(terminalTexture,terminalEye));
    NativeFgGuides invalidGuide;invalidGuide.eye=0;invalidGuide.source=terminalTexture;
    nativeFgCaptureGuides(nullptr,invalidGuide);
    verifyCoverage(__LINE__,table.getStatus(table.context,&report)==S_OK && report.state==EDVR_FG_UNAVAILABLE && report.lastFailure==Guides);
    verifyCoverage(__LINE__,observe(13.f,3,2)==0); // fresh qualification after the rejected GPU guide
    verifyCoverage(__LINE__,observe(13.f,3,2)==0);
    verifyCoverage(__LINE__,observe(13.f,3,2)==0);
    verifyCoverage(__LINE__,observe(13.f,3,2)==3);
    nativeFgBeforeTemporalEye(source.sequence,0,terminalTexture);
    verifyCoverage(__LINE__,nativeFgGuideEye(terminalTexture,terminalEye));
    terminalFault(*c,failedConfig,Ngx);
    verifyCoverage(__LINE__,table.getStatus(table.context,&report)==S_OK&&report.state==EDVR_FG_FAILED&&
        report.lastFailure==Ngx&&!report.activeMultiplier);
    verifyCoverage(__LINE__,table.beginSource(table.context,&source)==S_FALSE&&table.poll(table.context,&batch)==S_FALSE);
    verifyCoverage(__LINE__,!nativeFgGuideEye(terminalTexture,terminalEye) && report.requestedMultiplier==3);
    nativeFgSetRequestedMultiplier(4);
    verifyCoverage(__LINE__,table.getStatus(table.context,&report)==S_OK);
    source.configGeneration=report.configGeneration;
    verifyCoverage(__LINE__,observe(1.f,3,2)==0);
    verifyCoverage(__LINE__,observe(1.f,3,2)==0);
    verifyCoverage(__LINE__,observe(1.f,3,2)==0);
    const float next=std::nextafter(1.f,2.f);
    verifyCoverage(__LINE__,observe(next,3,2)==3); // original phase 3 alone fits this covered 4x pair
    verifyCoverage(__LINE__,!fg_coverage::fits(c->capture.source.previous.views,c->capture.source.current.views,4,1) &&
        fg_coverage::fits(c->capture.source.previous.views,c->capture.source.current.views,4,3));
    verifyCoverage(__LINE__,observe(next,3,2,1)==1);
    const auto partialEpoch=c->history.load();
    ++source.referenceGeneration;terminalFrame.referenceGeneration=source.referenceGeneration;
    verifyCoverage(__LINE__,observe(next,3,2,2)==0 && c->history.load()>partialEpoch && c->doorMask==2 && !c->eligibleStreak);
    nativeFgSetRequestedMultiplier(0);nativeFgSetRequestedMultiplier(3);
    terminalFault(*c,failedConfig,Ngx); // a late result from the old owner
    check(table.getStatus(table.context,&report)==S_OK&&report.state==EDVR_FG_ARMING&&
        report.configGeneration!=failedConfig);
    FrameGenerationInteropStatus ringStatus;
    ringStatus.failure=FrameGenerationInteropFailure::Busy;
    const auto beforeBusy=c->busyPairs.load(),beforeEpoch=c->resourceEpoch.load();
    c->state=EDVR_FG_READY;refuseInterop(*c,ringStatus,report.configGeneration);
    check(c->busyPairs==beforeBusy+1&&c->state==EDVR_FG_READY&&c->resourceEpoch==beforeEpoch);
    ringStatus.failure=FrameGenerationInteropFailure::DeviceRemoved;ringStatus.lastHresult=DXGI_ERROR_DEVICE_HUNG;
    refuseInterop(*c,ringStatus,report.configGeneration);
    check(table.getStatus(table.context,&report)==S_OK&&report.state==EDVR_FG_FAILED&&report.lastFailure==Interop&&
        !report.activeMultiplier&&c->busyPairs==beforeBusy+1&&c->resourceEpoch>beforeEpoch);
    nativeFgSetRequestedMultiplier(0);nativeFgSetRequestedMultiplier(3);
    nativeFgSetAutoMode();table.getStatus(table.context,&report);
    const auto autoConfiguration=report.configGeneration;
    c->producer=std::make_shared<Producer>(); c->producer->resourceEpoch=c->resourceEpoch.load();
    const auto* sameProducer=c->producer.get();
    check(nativeFgAutoRequested()&&report.requestedMultiplier==0&&report.state==EDVR_FG_ARMING);
    fg_auto::Sample load;load.valid=true;load.session=7;load.cpuMs=14;load.gpuMs=13;load.periodMs=1000./90;
    for(unsigned i=1;i<=250;++i){load.nowMs=i*11;load.cpuSequence=load.gpuSequence=i;nativeFgAutoObserve(load);}
    table.getStatus(table.context,&report);
    check(report.requestedMultiplier==2&&report.configGeneration==autoConfiguration&&c->producer.get()==sameProducer);
    c->state=EDVR_FG_UNAVAILABLE;load.nowMs+=22;++load.cpuSequence;++load.gpuSequence;
    nativeFgAutoObserve(load);table.getStatus(table.context,&report);
    check(report.requestedMultiplier==2&&report.configGeneration==autoConfiguration);
    c->state=EDVR_FG_READY;load.nowMs+=22;++load.cpuSequence;++load.gpuSequence;
    nativeFgAutoObserve(load);table.getStatus(table.context,&report);
    check(report.requestedMultiplier==2&&report.configGeneration==autoConfiguration);
    load.cpuMs=4;load.gpuMs=7;
    for(unsigned i=0;i<400;++i){load.nowMs+=22;++load.cpuSequence;++load.gpuSequence;nativeFgAutoObserve(load);}
    table.getStatus(table.context,&report);
    check(report.requestedMultiplier==0&&report.configGeneration==autoConfiguration&&c->producer.get()==sameProducer);
    c->active=2;c->queued.emplace_back();c->ready.emplace_back();
    const auto beforeManualOff=c->resourceEpoch.load();
    std::weak_ptr<Producer> manualProducer=c->producer;
    nativeFgSetRequestedMultiplier(0);table.getStatus(table.context,&report);
    nativeFgServiceProducer(c->generation);
    check(!nativeFgAutoRequested()&&report.configGeneration==autoConfiguration+1 &&
        report.state==EDVR_FG_OFF && !report.activeMultiplier && c->resourceEpoch.load()==beforeManualOff+1 &&
        c->queued.empty() && c->ready.empty() && manualProducer.expired());
    // An Off worker with no graphics allocations still must wake for close.
    Coordinator idle(request.gameDevice, request.generation);
    idle.workerStarted=true;
    std::thread parked(worker,&idle);
    clearWork(idle, EDVR_FG_OFF, true);
    {
        std::lock_guard<std::mutex> lock(idle.queueMutex);
        idle.closed.store(true);
    }
    idle.wake.notify_all();
    parked.join();
    check(!idle.workerStarted.load() && !idle.active.load());
    // An exception constructing the worker cannot keep close permanently pending.
    check(!launchWorker(*c, [] { throw std::bad_alloc(); }) && !c->workerStarted);
    c->producer=std::make_shared<Producer>(); c->capture.producer=c->producer;
    std::weak_ptr<Producer> producerLease=c->producer;
    c->workerStarted=true;
    check(table.close(table.context)==E_PENDING && !producerLease.expired());
    EdvrFgTable retry{}; retry.size=sizeof(retry); retry.version=1;
    check(nativeFgAcquire(&request,&retry)==E_PENDING);
    c->workerStarted=false;
    check(table.close(table.context)==S_OK && producerLease.expired() && c->retired);
    refuseOutput(*c,outputStatus);
    check(c->state.load()==EDVR_FG_CLOSING && table.getStatus(table.context,&report)==S_OK &&
        report.state==EDVR_FG_OFF && report.requestedMultiplier==0);
    refuseOutput(*c,fatalOutput);
    check(c->state.load()==EDVR_FG_CLOSING && table.getStatus(table.context,&report)==S_OK &&
        report.state==EDVR_FG_OFF && report.requestedMultiplier==0);
    check(table.close(table.context)==S_OK);
    check(nativeFgMaximumMultiplier()==0);
    ++request.generation;
    retry={sizeof(retry),1};
    check(nativeFgAcquire(&request,&retry)==S_OK&&retry.close(retry.context)==S_OK);
    testUiRecompositionSelection(request, check);
    if (checksOut) *checksOut = checks;
    return failures;
}
#endif
} // namespace edvr

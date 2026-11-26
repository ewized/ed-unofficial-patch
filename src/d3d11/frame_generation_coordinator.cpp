#include "frame_generation_coordinator.h"
#include "frame_generation_color.h"
#include "frame_generation_interop.h"
#include "frame_generation_ngx.h"
#include "frame_generation_output.h"
#include "native_reflex.h"
#include "ui_layer.h"
#include "../common/log.h"
#include "../common/mirror_location.h"
#include "../common/temporal_math.h"
#include <wrl/client.h>
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
#endif

namespace edvr {
namespace {
using Microsoft::WRL::ComPtr;
using InputLease = std::shared_ptr<FrameGenerationInteropLease>;
enum Failure : uint32_t {None, SourceMismatch, TemporalMismatch, Unsupported, MissingPrevious,
    Camera, Guides, Ui, Interop, Busy, Ngx, Output, Device, Closing};
std::atomic<uint32_t> requested{0};
std::atomic<uint64_t> configuration{1};
std::atomic<bool> automatic{false};
std::mutex autoMutex;
fg_auto::Policy autoPolicy;
std::mutex ngxContextMutex;
// Exactly the contexts this integration successfully initialized. Retained
// for process life: the backend never performs a global NGX shutdown.
std::array<ID3D12Device*, 16> ownedNgxDevices{};
unsigned ownedNgxCount = 0;

struct Matrix {float m[4][4]{};};
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
        out.m[r][c] = static_cast<float>(a[r][c + 4]);
    }
    return true;
}
bool projection(const float tan[4], float a, float b, Matrix& out) noexcept {
    if (!finite(tan, 4) || !std::isfinite(a) || !std::isfinite(b) || b <= 0 || a > 0 ||
        !(tan[0] < tan[1] && tan[2] < tan[3])) return false;
    const float width = tan[1] - tan[0], height = tan[3] - tan[2];
    out.m[0][0] = 2.f / width; out.m[0][2] = (tan[1] + tan[0]) / width;
    out.m[1][1] = 2.f / height; out.m[1][2] = (tan[3] + tan[2]) / height;
    // For RH view Z=-distance, depth=A+B/distance. This is the actual
    // reversed scene encoding, including A=0's measured infinite far plane.
    out.m[2][2] = -a; out.m[2][3] = b; out.m[3][2] = -1.f;
    return finite(&out.m[0][0], 16);
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
    for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c) out[r][c] = value.m[c][r];
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
    Matrix p, previousP, unproject, previousView, clipPrevious, inverseClip;
    if (!projection(guide.frustum, guide.sceneProjectionA, guide.sceneProjectionB, p)) return reject(512);
    if (!projection(guide.previousFrustum, guide.previousProjectionA, guide.previousProjectionB, previousP)) return reject(1024);
    if (!inverse(p, unproject)) return reject(2048);
    const Matrix now = cameraPose(guide.cameraNow), before = cameraPose(guide.cameraPrevious);
    if (!inverse(before, previousView)) return reject(4096);
    clipPrevious = multiply(multiply(multiply(previousP, previousView), now), unproject);
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
    for (unsigned i = 0; i < 3; ++i) {
        out.cameraPos[i] = now.m[i][3]; out.cameraRight[i] = now.m[i][0];
        out.cameraUp[i] = now.m[i][1]; out.cameraFwd[i] = -now.m[i][2];
    }
    out.cameraNear = guide.sceneProjectionB / (1.f - guide.sceneProjectionA);
    // Preserve the actual infinite-far projection in both matrices. The
    // integration supplies max-float as a finite far metadata sentinel;
    // this does not replace the scene's A=0 depth encoding with a finite one.
    out.cameraFar = guide.sceneProjectionA < 0 ? -guide.sceneProjectionB / guide.sceneProjectionA : (std::numeric_limits<float>::max)();
    out.cameraFOV = std::atan(guide.frustum[3]) - std::atan(guide.frustum[2]);
    out.cameraAspectRatio = (guide.frustum[1] - guide.frustum[0]) / (guide.frustum[3] - guide.frustum[2]);
    out.depthInverted = true; out.cameraMotionIncluded = true; out.reset = guide.reset;
    out.motionVectorsInvalidValue = 65504.f; out.minRelativeLinearDepthObjectSeparation = 40.f;
    if (!(std::isfinite(out.cameraNear) && out.cameraNear > 0 && std::isfinite(out.cameraFar) && out.cameraFar > out.cameraNear)) return reject(16384);
    return true;
}

struct Producer {
    FrameGenerationInterop interop;
    FrameGenerationColor color;
    ComPtr<ID3D11DeviceContext> context;
    unsigned width = 0, height = 0, guideWidth = 0, guideHeight = 0;
    uint64_t config = 0, resourceEpoch = 0;
    bool colorCaptureNoted[2]{};
};
struct Work {
    std::shared_ptr<Producer> producer;
    InputLease input;
    FrameGenerationCamera cameras[2]{};
    FrameGenerationOutputUi ui[2];
    FrameGenerationOutputSource source;
    EdvrNativeTemporalFrame temporalPrevious{}, temporalCurrent{};
    std::shared_ptr<const UiLayerFgSnapshot> snapshots[2];
    uint64_t reflexProof = 0;
};
struct Publication {
    FrameGenerationOutputTicket ticket;
    uint64_t sequence, config, history, reflexProof;
    uint32_t multiplier;
};
enum class TemporalSupport : uint8_t { Awaiting, Supported, Unsupported };
struct Coordinator {
    Coordinator(ID3D11Device* gameDevice, uint64_t sessionGeneration) : device(gameDevice), generation(sessionGeneration) {}
    ID3D11Device* const device; // Immutable identity; host owns its lifetime through close.
    const uint64_t generation;
    std::atomic<bool> closed{false}, workerStarted{false};
    // The host retains the game device until close reports completion.
    // acquireMutex serializes these shutdown fields with the next acquisition.
    bool retired = false, quarantined = false;
    ULONGLONG closeStarted = 0;
    std::atomic<uint32_t> state{EDVR_FG_OFF}, failure{None}, active{0};
    std::atomic<uint64_t> terminalConfig{0};
    std::atomic<uint32_t> terminalReason{None};
    std::atomic<uint64_t> reflexProof{0}, resourceEpoch{0};
    std::atomic<TemporalSupport> temporalSupport{TemporalSupport::Awaiting};
    std::atomic<uint64_t> history{1}, sourcePairs{0}, generatedPairs{0}, suppressedPairs{0}, refusedPairs{0}, busyPairs{0}, latestSource{0}, latestReady{0}, attemptedSource{0};
    std::atomic<uint64_t> latestExported{0};
    std::mutex metadataMutex;
    EdvrFgSource source{}, previousSource{};
    EdvrNativeTemporalFrame temporal{}, previousTemporal{};
    bool haveSource = false, havePrevious = false;
    uint64_t sourceProof = 0, temporalProof = 0, temporalConfig = 0;
    std::mutex queueMutex;
    std::condition_variable wake;
    std::deque<Work> queued;
    std::deque<Publication> ready;
    // The thread identity may be checked by a foreign caller. Only the
    // elected render owner reads or changes the capture fields below it.
    std::atomic<DWORD> renderThread{0};
    std::shared_ptr<Producer> producer;
    Work capture;
    uint64_t captureSequence = 0, captureHistory = 0;
    std::atomic<uint64_t> resetSequence{0};
    unsigned activeEye = 2, guideMask = 0, uiMask = 0, worldMask = 0, temporalMask = 0;
    bool transaction = false, rejected = false;
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
    return config && c.terminalConfig.load(std::memory_order_acquire)==config;
}
// queueMutex protects the bounded queues and serializes their invalidation
// with a ready publication. Queued inputs have not entered any GPU consumer.
void clearWorkLocked(Coordinator& c, uint32_t state, bool releaseResources = false) noexcept {
    ++c.history;
    c.active = 0;
    c.state = state;
    c.latestSource = 0;
    c.latestReady = 0;
    c.latestExported = 0;
    for (auto& work : c.queued) if (work.input) work.input->cancel();
    c.queued.clear();
    c.ready.clear();
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
void discardCapture(Coordinator& c) noexcept {
    if (c.transaction && c.capture.producer)
        c.capture.producer->interop.abort(c.capture.producer->context.Get(), c.captureSequence, c.captureHistory);
    c.transaction = false; c.rejected = true; c.activeEye = 2; c.capture = {};
}
void abortCapture(Coordinator& c, Failure failure) noexcept {
    discardCapture(c);
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
        work.reflexProof, work.source.multiplier);
}
bool currentPublication(const Coordinator& c, const Publication& publication) noexcept {
    return currentAdmission(c, publication.config, publication.history, publication.reflexProof, publication.multiplier);
}

// Under queueMutex. READY certifies an exportable current-epoch GPU result,
// including a fenced native-only bootstrap. Only the later poll transfers a
// token. Generated work still requires the actual batch's generated phases.
void promoteReadyLocked(Coordinator& c) noexcept {
    bool exportable = false;
    for (auto item = c.ready.begin(); item != c.ready.end();) {
        const auto result = currentPublication(c, *item) ? item->ticket.exportStatus() : E_ABORT;
        if (FAILED(result)) {item = c.ready.erase(item); continue;}
        if (result == S_OK) {
            exportable = true;
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
    if (!c.producer || c.producer->resourceEpoch == c.resourceEpoch.load()) return true;
    discardCapture(c);
    if (c.producer.use_count() != 1 || !c.producer->interop.retired()) return false;
    if (auto* context = c.producer->context.Get(); context && !uiLayerFgRetireStorage(context)) return false;
    c.producer.reset();
    return true;
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
void retireBackendOnly(Work& work, const FrameGenerationNgxTicket& ticket) noexcept {
    if (ticket.completionFence && ticket.completionValue) work.input->retire(ticket.completionFence, ticket.completionValue);
    else if (!ticket.lease) work.input->cancel();
    // A nonempty ticket without a completion proof remains quarantined in the
    // bounded interop ring. Never convert that uncertainty into a cancellation.
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
bool pendingRetirement(const FrameGenerationNgx* ngx, const FrameGenerationOutput* output) noexcept {
    if (ngx) {
        const auto status = ngx->status();
        if (status.submitted > status.completed) return true;
    }
    if (output) {
        const auto status = output->status();
        if (status.submitted > status.completed) return true;
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
void worker(Coordinator* c) noexcept {
    std::unique_ptr<FrameGenerationNgx> ngx;
    std::unique_ptr<FrameGenerationOutput> output;
    std::shared_ptr<Producer> configured;
    unsigned multiplier = 0, diagnosticFailures = 0;
    uint64_t releasedEpoch = 0;
    const auto releaseBackend = [&] {
        clearUnexported(*c);
        if (output) drainOutput(*output);
        if (ngx) ngx->drain(2000);
        output.reset(); ngx.reset(); configured.reset(); multiplier=0;
    };
    const auto discardBackend = [&](uint64_t config,Failure why) {
        terminalFault(*c,config,why);
        releaseBackend();
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
            if (!currentWork(*c, work)) {
                work.input->cancel(); continue;
            }
            if (configured != work.producer || multiplier != work.source.multiplier) {
                clearUnexported(*c);
                if ((output && !drainOutput(*output)) || (ngx && !ngx->drain(2000))) {
                    work.input->cancel(); discardBackend(work.producer->config,Output); continue;
                }
                output.reset(); ngx.reset(); configured = work.producer; multiplier = work.source.multiplier;
                ngx = std::make_unique<FrameGenerationNgx>(); output = std::make_unique<FrameGenerationOutput>();
                FrameGenerationNgxInit init;
                init.device = configured->interop.device12(); init.width = configured->width; init.height = configured->height;
                init.guideWidth = configured->guideWidth; init.guideHeight = configured->guideHeight; init.multiplier = multiplier;
                init.creationMode = FrameGenerationNgxCreationMode::OfficialFgHelper;
                init.suppressionLayout = FrameGenerationNgxSuppressionLayout::SharedNativePair;
                init.featureDirectory = c->features.c_str(); init.dataDirectory = c->data.c_str();
                bool initialized = false;
                {
                    std::lock_guard<std::mutex> contextLock(ngxContextMutex);
                    bool known = false;
                    for (unsigned i = 0; i < ownedNgxCount; ++i) if (ownedNgxDevices[i] == init.device) known = true;
                    init.ngxAlreadyInitialized = known; init.privateNgxDevice = !known;
                    if (known || ownedNgxCount < ownedNgxDevices.size()) {
                        initialized = ngx->initialize(init);
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
                    work.input->cancel(); discardBackend(work.producer->config,Ngx); continue;
                }
                if (!output->initialize(true, init.device, init.width, init.height)) {
                    const auto s = output->status();
                    Log::get().note("native fg: output initialization failed failure=%u hr=0x%08x quarantined=%u.",
                        uint32_t(s.failure),unsigned(s.lastHresult),unsigned(s.quarantined));
                    work.input->cancel(); discardBackend(work.producer->config,Output); continue;
                }
                Log::get().note("native fg: worker initialized multiplier=%u source=%ux%u guides=%ux%u route=official-helper/shared-native-pair mv_units=input-pixels mv_scale_left=(%.9g,%.9g) mv_scale_right=(%.9g,%.9g) jitter_units=clip-space",
                    multiplier, init.width, init.height, init.guideWidth, init.guideHeight,
                    double(work.cameras[0].mvecScale[0]), double(work.cameras[0].mvecScale[1]),
                    double(work.cameras[1].mvecScale[0]), double(work.cameras[1].mvecScale[1]));
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
            // A first source has no fabricated previous physical pose. Publish
            // its actual current endpoint alone; arm NGX on the next true pair.
            auto ticket = work.source.previous.sequence == work.source.current.sequence
                ? FrameGenerationNgxTicket{} : ngx->submit(input);
            auto completion = ticket.poll();
            while (completion == FrameGenerationNgxCompletion::Pending) {
                if (output) output->collect();
                Sleep(1); completion = ticket.poll();
            }
            const auto ngxStatus=ngx->status();
            if (terminalNgx(completion,ngxStatus)) {
                retireBackendOnly(work,ticket);
                Log::get().note("native fg: terminal NGX evaluation fault, completion=%u failure=%u ngx=0x%08x hr=0x%08x; native cadence until reselected.",
                    unsigned(completion),uint32_t(ngxStatus.failure),ngxStatus.lastNgxResult,unsigned(ngxStatus.lastHresult));
                discardBackend(work.producer->config,Ngx);continue;
            }
            if (!currentWork(*c, work)) {
                retireBackendOnly(work, ticket); continue;
            }
            const bool nativeBootstrap = work.source.previous.sequence == work.source.current.sequence && !ticket.lease;
            if (completion == FrameGenerationNgxCompletion::FailedEvaluation &&
                (++diagnosticFailures <= 12 || diagnosticFailures % 600 == 0)) {
                const auto s = ngx->status();
                Log::get().note("native fg: NGX evaluation failed; publishing native-only sequence=%llu failure=%u ngx=0x%08x hr=0x%08x.",
                    (unsigned long long)input.sequence,uint32_t(s.failure),s.lastNgxResult,unsigned(s.lastHresult));
            }
            if (!nativeBootstrap && completion != FrameGenerationNgxCompletion::Ready && completion != FrameGenerationNgxCompletion::Suppressed &&
                completion != FrameGenerationNgxCompletion::FailedEvaluation) {
                if (++diagnosticFailures <= 12 || diagnosticFailures % 600 == 0) {
                    const auto s = ngx->status();
                    Log::get().note("native fg: NGX submit rejected sequence=%llu completion=%u lease=%u failure=%u ngx=0x%08x hr=0x%08x quarantined=%u.",
                        (unsigned long long)input.sequence,uint32_t(completion),unsigned(bool(ticket.lease)),
                        uint32_t(s.failure),s.lastNgxResult,unsigned(s.lastHresult),unsigned(s.quarantined));
                }
                retireBackendOnly(work, ticket); refuse(*c, Ngx); continue;
            }
            auto published = output->submit(work.input, ticket, work.ui, work.source);
            if (!published.accepted) {
                const auto outputStatus = output->status();
                if (++diagnosticFailures <= 12 || diagnosticFailures % 600 == 0) {
                    Log::get().note("native fg: output submit rejected sequence=%llu failure=%u hr=0x%08x quarantined=%u backend_completion=%u.",
                        (unsigned long long)input.sequence,uint32_t(outputStatus.failure),unsigned(outputStatus.lastHresult),
                        unsigned(outputStatus.quarantined),uint32_t(completion));
                }
                retireBackendOnly(work, ticket);
                if (outputStatus.failed || outputStatus.quarantined) discardBackend(work.producer->config,Output);
                else refuseOutput(*c, outputStatus);
                continue;
            }
            if (!published.finalFence || !published.finalValue || !work.input->retire(published.finalFence, published.finalValue)) {
                Log::get().note("native fg: source retirement failed sequence=%llu final_fence=%u final_value=%llu; source remains quarantined.",
                    (unsigned long long)input.sequence,unsigned(published.finalFence!=nullptr),(unsigned long long)published.finalValue);
                discardBackend(work.producer->config,Output);continue;
            }
            {
                std::lock_guard<std::mutex> lock(c->queueMutex);
                if (!currentWork(*c, work)) continue;
                if (completion == FrameGenerationNgxCompletion::Ready) ++c->generatedPairs; else ++c->suppressedPairs;
                if (c->ready.size() >= 3) {++c->busyPairs;}
                else {
                    c->ready.push_back({std::move(published), work.source.current.sequence,
                        work.source.current.configGeneration, work.source.historyGeneration, work.reflexProof, multiplier});
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
    out->lastFailure = fault?c->terminalReason.load():c->failure.load();
    out->generation = c->generation; out->historyGeneration = c->history.load();
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
    if (c->haveSource) c->previousSource = c->source;
    c->source = *value; c->sourceProof = proof; c->haveSource = true; c->attemptedSource = value->sequence;
    return S_OK;
}
HRESULT WINAPI poll(void* pointer, EdvrFgBatch* out) {
    auto* c = identify(pointer);
    if (!c || !out || out->size != sizeof(*out) || out->version != EDVR_FG_BATCH_VERSION_3) return E_INVALIDARG;
    if (!enabled(c)) return S_FALSE;
    std::unique_lock<std::mutex> lock(c->queueMutex, std::try_to_lock); if (!lock.owns_lock()) return S_FALSE;
    const auto proof = c->reflexProof.load();
    if (!selected(c) || !proof || proof != liveReflexProof(*c) || c->temporalSupport.load() != TemporalSupport::Supported) return S_FALSE;
    while (!c->ready.empty()) {
        auto& publication = c->ready.front();
        if (!currentPublication(*c, publication)) {c->ready.pop_front(); continue;}
        const auto result = publication.ticket.acquireBatch(out);
        if (result == S_FALSE) return S_FALSE;
        c->ready.pop_front();
        if (FAILED(result)) {refuse(*c, Output); continue;}
        if (proof != liveReflexProof(*c) || !selected(c) || c->temporalSupport.load() != TemporalSupport::Supported ||
            out->generation != c->generation || out->configGeneration != configuration.load() ||
            out->historyGeneration != c->history.load()) {
            frameGenerationOutputDiscard(out->token); frameGenerationOutputRelease(out->token);
            *out = {}; out->size = sizeof(*out); out->version = EDVR_FG_BATCH_VERSION_3;
            continue;
        }
        c->latestExported = out->currentSequence;
        return S_OK;
    }
    return S_FALSE;
}
HRESULT WINAPI invalidate(void* pointer, uint32_t reason) {
    auto* c = identify(pointer); if (!c) return E_INVALIDARG;
    if (!c->closed.load()) {clearWork(*c, EDVR_FG_ARMING); c->failure = reason;}
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
    c->capture = {};
    if (c->producer && !c->producer->interop.retired()) {
        // A half-copied stereo pair cannot be sealed from the XR thread.
        // Other submitted copies may still retire without touching that context.
        if (!c->transaction && GetTickCount64() - c->closeStarted < 2000) return E_PENDING;
        c->quarantined = true;
        Log::get().note("native fg: producer retirement unproven; retaining its ring and disabling further acquisitions this process.");
    } else {
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
        auto* c = new Coordinator(request->gameDevice, request->generation);
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
void nativeFgBeforeTemporalEye(uint64_t sequence, unsigned eye, ID3D11Texture2D* source) noexcept {
    auto* c = current.load(std::memory_order_acquire); if (!enabled(c) || eye > 1) return;
    try {
        DWORD unclaimed = 0;
        c->renderThread.compare_exchange_strong(unclaimed, GetCurrentThreadId());
        if (!owner(*c)) {refuse(*c, TemporalMismatch); return;}
        if (!serviceProducer(*c)) return;
        if (c->captureSequence != sequence || c->captureHistory != c->history.load()) {
            if (c->transaction) abortCapture(*c, TemporalMismatch);
            c->capture = {}; c->guideMask = c->uiMask = c->worldMask = c->temporalMask = 0;
            c->captureSequence = sequence; c->captureHistory = c->history.load(); c->rejected = false;
            std::lock_guard<std::mutex> lock(c->metadataMutex);
            const auto proof = c->reflexProof.load();
            if (!proof || proof != liveReflexProof(*c) || c->temporalSupport.load() != TemporalSupport::Supported ||
                c->sourceProof != proof || c->temporalProof != proof || !c->haveSource || c->temporal.sequence != sequence ||
                c->source.sequence != sequence || c->source.referenceGeneration != c->temporal.referenceGeneration ||
                c->source.configGeneration != c->temporalConfig || c->temporalConfig != configuration.load()) {
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
            }
        }
        c->activeEye = c->rejected ? 2u : eye; c->submitted[eye] = source;
    } catch (...) {c->rejected = true; refuse(*c, TemporalMismatch);}
}
void nativeFgCaptureGuides(ID3D11DeviceContext* context, const NativeFgGuides& guides) noexcept {
    auto* c = current.load(std::memory_order_acquire);
    if (!enabled(c) || !owner(*c) || c->rejected || !currentWork(*c, c->capture) || guides.eye != c->activeEye) return;
    try {
        const auto resourceEpoch = c->resourceEpoch.load();
        if (!context || guides.eye > 1 || !guides.source || guides.source != c->submitted[guides.eye] ||
            !guides.depth || !guides.motion || !guides.width || !guides.height ||
            guides.outputWidth != c->capture.source.current.width || guides.outputHeight != c->capture.source.current.height ||
            c->capture.source.current.configGeneration != configuration.load() || (c->guideMask & (1u << guides.eye))) {
            abortCapture(*c, Guides); return;
        }
        if (guides.reset && c->resetSequence != c->captureSequence) {
            ++c->history; c->resetSequence = c->captureSequence;
            if (c->transaction) {abortCapture(*c, Guides); return;}
            c->captureHistory = c->history.load(); c->capture.source.historyGeneration = c->captureHistory;
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
            // The worker may retain the previous producer until new work is
            // queued. Its retired ring still owns UI snapshots, so release only
            // the global cache's references before this producer's first capture.
            if (!uiLayerFgRetireStorage(context)) {abortCapture(*c, Ui); return;}
            if (!currentWork(*c, c->capture) || resourceEpoch != c->resourceEpoch.load()) {discardCapture(*c); return;}
            c->producer = std::move(made);
        }
        if (!currentWork(*c, c->capture)) {discardCapture(*c); return;}
        if (!startWorker(*c)) {abortCapture(*c, Interop); return;}
        if (!c->transaction) {
            c->capture.producer = c->producer;
            if (!c->producer->interop.begin(context, c->captureSequence, c->captureHistory)) {++c->busyPairs; abortCapture(*c, Busy); return;}
            c->transaction = true;
        }
        const auto identity = (c->captureSequence << 3) | (uint64_t(guides.eye) << 2) | 1;
        if (!c->producer->interop.capture(context, c->captureSequence, c->captureHistory, guides.eye, FrameGenerationPlane::Depth, guides.depth, identity) ||
            !c->producer->interop.capture(context, c->captureSequence, c->captureHistory, guides.eye, FrameGenerationPlane::Motion, guides.motion, identity + 1)) {
            abortCapture(*c, Interop); return;
        }
        c->guideMask |= 1u << guides.eye;
    } catch (...) {abortCapture(*c, Guides);}
}
bool nativeFgGuideEye(ID3D11Texture2D* exactSource, unsigned& anatomicalEye) noexcept {
    auto* c = current.load(std::memory_order_acquire);
    if (!enabled(c) || !owner(*c) || c->rejected || !currentWork(*c, c->capture) || c->activeEye > 1 || !exactSource ||
        c->submitted[c->activeEye] != exactSource) return false;
    anatomicalEye = c->activeEye;
    return true;
}
void nativeFgAfterTemporalEye(uint64_t sequence, unsigned eye, ID3D11Texture2D* output) noexcept {
    auto* c = current.load(std::memory_order_acquire); if (!c || !owner(*c) || eye > 1 || c->captureSequence != sequence) return;
    c->activeEye = 2;
    if (!output || !enabled(c) || c->captureHistory != c->history.load()) {if (c->transaction) abortCapture(*c, TemporalMismatch); return;}
    c->temporalOutput[eye] = output; c->temporalMask |= 1u << eye;
}
void nativeFgCaptureDoor(uint64_t sequence, unsigned eye, ID3D11Texture2D* source,
    const uint32_t region[4], const float layerUv[4]) noexcept {
    auto* c = current.load(std::memory_order_acquire);
    if (!c || !owner(*c) || c->rejected || !c->transaction || eye > 1 || c->captureSequence != sequence) return;
    try {
        if (!enabled(c) || c->captureHistory != c->history.load() || source != c->temporalOutput[eye] || !(c->temporalMask & (1u << eye)) ||
            (c->uiMask & (1u << eye))) {abortCapture(*c, Ui); return;}
        UiLayerFgInfo info;
        auto snapshot = uiLayerFgCapture(c->producer->context.Get(), sequence, eye, source, region, layerUv, &info);
        UiLayerFgPlanes planes;
        if (!snapshot || !uiLayerFgGetPlanes(snapshot, &planes)) {abortCapture(*c, Ui); return;}
        auto& ui = c->capture.ui[eye]; ui.empty = info.empty; ui.useMult = planes.useMult;
        std::memcpy(ui.uv, planes.layerUv, sizeof(ui.uv)); c->capture.snapshots[eye] = snapshot;
        ID3D11Texture2D *layer = planes.layer, *multiplyPlane = planes.multiply;
        auto* context = c->producer->context.Get();
        if (info.empty && !c->producer->color.emptyUi(context, eye, info.width, info.height, false, true, &layer)) {abortCapture(*c, Ui); return;}
        if (!planes.useMult && !c->producer->color.emptyUi(context, eye, info.width, info.height, true, true, &multiplyPlane)) {abortCapture(*c, Ui); return;}
        const auto identity = (sequence << 3) | (uint64_t(eye) << 2) | 1;
        if (!layer || !multiplyPlane || !c->producer->interop.capture(context, sequence, c->captureHistory, eye,
            FrameGenerationPlane::UiColorTransmittance, layer, identity, snapshot) || !c->producer->interop.capture(context, sequence, c->captureHistory, eye,
            FrameGenerationPlane::UiMultiply, multiplyPlane, identity + 1, snapshot)) {abortCapture(*c, Interop); return;}
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
            c->queued.push_back(std::move(c->capture));
            ++c->sourcePairs; c->latestSource = sequence;
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
        batch.size=sizeof(batch);batch.version=EDVR_FG_BATCH_VERSION_3;batch.generation=c.generation;
        batch.configGeneration=config;batch.historyGeneration=history;batch.currentSequence=c.captureSequence+i+1;
        batch.multiplier=multiplier;batch.phaseMask=1;
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
    EdvrFgBatch claimed{sizeof(claimed),EDVR_FG_BATCH_VERSION_3};
    check(table.poll(table.context,&claimed)==S_OK && claimed.token && claimed.phaseMask==1);
    check(table.getStatus(table.context,&report)==S_OK && report.latestReadySequence==batches[0].currentSequence &&
        report.latestReadySequence==claimed.currentSequence);
    if (claimed.token) {table.discard(claimed.token);table.release(claimed.token);}
    clearUnexported(c);
    check(output.drain(0));
}
} // namespace

unsigned nativeFgTestProtocol(unsigned* checksOut) noexcept {
    unsigned failures = 0, checks = 0;
    const auto check = [&](bool pass) {++checks; if (!pass) ++failures;};
    const auto closeFloat = [](float a, float b) {return std::fabs(a - b) < 1e-5f;};
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
    guide.originJump=true; check(!camera(guide,options)); guide.originJump=false;
    guide.depthAvailable=false; check(!camera(guide,options)); guide.depthAvailable=true;
    guide.cameraNow[0]=2; check(!camera(guide,options)); guide.cameraNow[0]=1;
    guide.sceneProjectionA=.1f; check(!camera(guide,options)); guide.sceneProjectionA=0;
    guide.jitterPixels[0]=(std::numeric_limits<float>::infinity)(); check(!camera(guide,options));

    nativeFgSetRequestedMultiplier(0);
    EdvrFgRequest request{sizeof(request),1,reinterpret_cast<ID3D11Device*>(uintptr_t(1)),7};
    EdvrFgTable table{};table.size=sizeof(table);table.version=1;
    check(nativeFgAcquire(&request,&table)==S_OK);
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
            check(nativeFgGuideEye(exact,anatomical) && anatomical==1);
            check(!nativeFgGuideEye(reinterpret_cast<ID3D11Texture2D*>(uintptr_t(3)),anatomical));
            check(c->capture.source.previous.sequence==1 && c->capture.source.current.sequence==1);
            NativeFgGuides invalid;invalid.eye=1;invalid.source=exact;
            nativeFgCaptureGuides(nullptr,invalid);
            table.getStatus(table.context,&report);check(report.state==EDVR_FG_UNAVAILABLE && !c->workerStarted.load());
            source.sequence=2;source.poseDisplayTime+=1000000;
            check(table.beginSource(table.context,&source)==S_OK && c->havePrevious);
            frame->sequence=2;nativeFgBeginTemporal(*frame,false,true);
            nativeFgBeforeTemporalEye(2,0,exact);
            check(c->capture.source.previous.sequence==1 && c->capture.source.current.sequence==2 && !c->rejected);

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
                nativeFgGuideEye(exact,anatomical) && c->capture.source.previous.sequence==4 && c->capture.reflexProof==2);
            testFencedBootstrap(*c,table,check);
        }
        VirtualFree(pages,0,MEM_RELEASE);
    }
    const auto before=c->history.load();check(table.invalidate(table.context,9)==S_OK && c->history.load()==before+1);
    EdvrFgBatch batch{};batch.size=sizeof(batch);batch.version=EDVR_FG_BATCH_VERSION_3;check(table.poll(table.context,&batch)==S_FALSE && !batch.token);
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
    unsigned terminalEye=2;
    check(nativeFgGuideEye(terminalTexture,terminalEye));
    terminalFault(*c,failedConfig,Ngx);
    check(table.getStatus(table.context,&report)==S_OK&&report.state==EDVR_FG_FAILED&&
        report.lastFailure==Ngx&&!report.activeMultiplier);
    check(table.beginSource(table.context,&source)==S_FALSE&&table.poll(table.context,&batch)==S_FALSE);
    check(!nativeFgGuideEye(terminalTexture,terminalEye) && report.requestedMultiplier==3);
    nativeFgSetRequestedMultiplier(0);nativeFgSetRequestedMultiplier(3);
    terminalFault(*c,failedConfig,Ngx); // a late result from the old owner
    check(table.getStatus(table.context,&report)==S_OK&&report.state==EDVR_FG_ARMING&&
        report.configGeneration!=failedConfig);
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
    if (checksOut) *checksOut = checks;
    return failures;
}
#endif
} // namespace edvr

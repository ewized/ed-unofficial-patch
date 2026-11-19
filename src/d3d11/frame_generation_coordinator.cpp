#include "frame_generation_coordinator.h"
#include "frame_generation_color.h"
#include "frame_generation_interop.h"
#include "frame_generation_ngx.h"
#include "frame_generation_output.h"
#include "ui_layer.h"
#include "../common/log.h"
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
    out.mvecScale[0] = 1.f / guide.width; out.mvecScale[1] = 1.f / guide.height;
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
    uint64_t config = 0;
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
};
struct Coordinator {
    ID3D11Device* device = nullptr; // Provider capability's host retains it.
    uint64_t generation = 0;
    std::atomic<bool> closed{false}, workerStarted{false};
    bool devicePinned = false;
    std::atomic<uint32_t> state{EDVR_FG_OFF}, failure{None}, active{0};
    std::atomic<uint64_t> terminalConfig{0};
    std::atomic<uint32_t> terminalReason{None};
    std::atomic<uint64_t> history{1}, sourcePairs{0}, generatedPairs{0}, suppressedPairs{0}, refusedPairs{0}, busyPairs{0}, latestSource{0}, latestReady{0}, attemptedSource{0};
    std::mutex metadataMutex;
    EdvrFgSource source{}, previousSource{};
    EdvrNativeTemporalFrame temporal{}, previousTemporal{};
    bool haveSource = false, havePrevious = false, temporalSupported = false;
    uint64_t temporalConfig = 0;
    std::mutex queueMutex;
    std::condition_variable wake;
    std::deque<Work> queued;
    std::deque<FrameGenerationOutputTicket> ready;
    // Render-owner fields: no worker accesses the game context or mutable UI.
    DWORD renderThread = 0;
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
void terminalFault(Coordinator& c,uint64_t config,Failure reason) noexcept {
    if (!config || c.closed.load()) return;
    // A fault belongs to the backend's configuration. A racing user selection
    // has a different generation and must be allowed to create a new backend.
    c.terminalReason.store(reason,std::memory_order_relaxed);
    c.terminalConfig.store(config,std::memory_order_release);
    ++c.refusedPairs;
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
void abortCapture(Coordinator& c, Failure failure) noexcept {
    if (c.transaction && c.capture.producer)
        c.capture.producer->interop.abort(c.capture.producer->context.Get(), c.captureSequence, c.captureHistory);
    c.transaction = false; c.rejected = true; c.capture = {}; refuse(c, failure);
}
bool enabled(Coordinator* c) noexcept {
    return c && !c->closed.load(std::memory_order_acquire) &&
        requested.load(std::memory_order_acquire) != 0 && !terminalFor(*c,configuration.load());
}
bool owner(Coordinator& c) noexcept {return c.renderThread && c.renderThread == GetCurrentThreadId();}
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
    c.data = std::wstring(local) + L"\\EDVR";
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
void worker(Coordinator* c) noexcept {
    std::unique_ptr<FrameGenerationNgx> ngx;
    std::unique_ptr<FrameGenerationOutput> output;
    std::shared_ptr<Producer> configured;
    unsigned multiplier = 0, diagnosticFailures = 0;
    const auto discardBackend = [&](uint64_t config,Failure why) {
        terminalFault(*c,config,why);
        c->active=0;
        clearUnexported(*c);
        if (output) drainOutput(*output);
        if (ngx) ngx->drain(2000);
        output.reset(); ngx.reset(); configured.reset(); multiplier=0;
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
            Work work;
            {
                std::unique_lock<std::mutex> lock(c->queueMutex);
                c->wake.wait_for(lock, std::chrono::milliseconds(2), [&]{return c->closed.load() || !c->queued.empty();});
                if (c->queued.empty()) {if (c->closed.load()) break; else continue;}
                work = std::move(c->queued.front()); c->queued.pop_front();
            }
            if (!enabled(c) || work.source.current.configGeneration != configuration.load() ||
                work.source.historyGeneration != c->history.load()) {
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
                c->active = multiplier; c->state = EDVR_FG_ARMING;
                Log::get().note("native fg: worker initialized multiplier=%u source=%ux%u guides=%ux%u route=official-helper/shared-native-pair mv_units=input-pixels mv_scale=reciprocal-guide-size jitter_units=clip-space", multiplier, init.width, init.height, init.guideWidth, init.guideHeight);
            }
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
            if (work.source.current.configGeneration != configuration.load() || work.source.historyGeneration != c->history.load() ||
                c->closed.load() || !requested.load()) {
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
            if (completion == FrameGenerationNgxCompletion::Ready) ++c->generatedPairs; else ++c->suppressedPairs;
            {
                std::lock_guard<std::mutex> lock(c->queueMutex);
                if (c->ready.size() >= 3) {++c->busyPairs;}
                else c->ready.push_back(std::move(published));
            }
            c->wake.notify_all();
        }
        clearUnexported(*c);
        if (output) drainOutput(*output);
        if (ngx) ngx->drain(2000);
    } catch (...) {terminalFault(*c,configured?configured->config:configuration.load(),Output);}
    // Destruction remains on the component owner thread. Only after every
    // destructor has finished may a nonblocking close report completion.
    output.reset(); ngx.reset(); configured.reset();
    c->active = 0;
    c->workerStarted = false;
}
bool startWorker(Coordinator& c) {
    if (c.workerStarted.load()) return true;
    if (!paths(c)) return false;
    if (!c.devicePinned) {c.device->AddRef(); c.devicePinned = true;}
    c.workerStarted = true;
    try {std::thread(worker, &c).detach(); return true;}
    catch (...) {c.workerStarted = false; return false;}
}
HRESULT WINAPI status(void* pointer, EdvrFgStatus* out) {
    auto* c = identify(pointer);
    if (!c || !out || out->size != sizeof(*out) || out->version != 1) return E_INVALIDARG;
    *out = {}; out->size = sizeof(*out); out->version = 1;
    out->configGeneration = configuration.load();
    const bool fault=terminalFor(*c,out->configGeneration);
    out->requestedMultiplier = requested.load(); out->activeMultiplier = fault?0:c->active.load();
    out->state = !out->requestedMultiplier ? EDVR_FG_OFF : fault?EDVR_FG_FAILED:c->state.load();
    out->lastFailure = fault?c->terminalReason.load():c->failure.load();
    out->generation = c->generation; out->historyGeneration = c->history.load();
    out->sourcePairs = c->sourcePairs.load(); out->generatedPairs = c->generatedPairs.load();
    out->suppressedPairs = c->suppressedPairs.load(); out->refusedPairs = c->refusedPairs.load(); out->busyPairs = c->busyPairs.load();
    out->latestSourceSequence = c->latestSource.load(); out->latestReadySequence = c->latestReady.load();
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
    if (!enabled(c)) return S_FALSE;
    if (!validSource(*value, *c)) {refuse(*c, SourceMismatch); return E_INVALIDARG;}
    std::unique_lock<std::mutex> lock(c->metadataMutex, std::try_to_lock); if (!lock.owns_lock()) return E_PENDING;
    if (c->haveSource && value->sequence <= c->source.sequence) return E_INVALIDARG;
    c->havePrevious = c->haveSource && value->sequence == c->source.sequence + 1 &&
        value->generation == c->source.generation && value->referenceGeneration == c->source.referenceGeneration &&
        value->configGeneration == c->source.configGeneration && value->width == c->source.width && value->height == c->source.height;
    if (c->haveSource) c->previousSource = c->source;
    c->source = *value; c->haveSource = true; c->attemptedSource = value->sequence;
    c->state = c->state.load() == EDVR_FG_FAILED ? EDVR_FG_FAILED : EDVR_FG_ARMING;
    return S_OK;
}
HRESULT WINAPI poll(void* pointer, EdvrFgBatch* out) {
    auto* c = identify(pointer);
    if (!c || !out || out->size != sizeof(*out) || out->version != EDVR_FG_BATCH_VERSION_3) return E_INVALIDARG;
    if (!enabled(c)) return S_FALSE;
    std::unique_lock<std::mutex> lock(c->queueMutex, std::try_to_lock); if (!lock.owns_lock()) return S_FALSE;
    while (!c->ready.empty()) {
        auto& ticket = c->ready.front();
        const auto result = ticket.acquireBatch(out);
        if (result == S_FALSE) return S_FALSE;
        c->ready.pop_front();
        if (FAILED(result)) {refuse(*c, Output); continue;}
        if (out->generation != c->generation || out->configGeneration != configuration.load() ||
            out->historyGeneration != c->history.load()) {
            frameGenerationOutputDiscard(out->token); frameGenerationOutputRelease(out->token); continue;
        }
        c->latestReady = out->currentSequence; c->state = EDVR_FG_READY;
        return S_OK;
    }
    return S_FALSE;
}
HRESULT WINAPI invalidate(void* pointer, uint32_t reason) {
    auto* c = identify(pointer); if (!c) return E_INVALIDARG;
    ++c->history; c->failure = reason; c->wake.notify_all(); return S_OK;
}
HRESULT WINAPI close(void* pointer) {
    auto* c = identify(pointer); if (!c) return E_INVALIDARG;
    if (!c->closed.exchange(true)) ++c->history;
    c->state = EDVR_FG_CLOSING; c->wake.notify_all();
    return c->workerStarted.load(std::memory_order_acquire) ? E_PENDING : S_OK;
}
} // namespace

namespace {
void setEffectiveMultiplier(uint32_t multiplier,bool userConfiguration) noexcept {
    if (!nativeFgFixedMultiplier(multiplier)) multiplier = 0;
    const bool changed = requested.exchange(multiplier, std::memory_order_acq_rel) != multiplier;
    if (userConfiguration) ++configuration;
    if (auto* c = current.load(std::memory_order_acquire)) {
        if ((changed||userConfiguration) && !c->closed.load()) {
            ++c->history; c->failure = None;c->state = multiplier ? EDVR_FG_ARMING : EDVR_FG_OFF;
            // These batches were never exported. Do not leave their leases
            // filling the bounded rings throughout native probation.
            if(!multiplier)clearUnexported(*c);
        }
        c->wake.notify_all();
    }
}
}
void nativeFgSetRequestedMultiplier(uint32_t multiplier) noexcept {
    std::lock_guard<std::mutex> lock(autoMutex);
    const bool wasAutomatic=automatic.exchange(false,std::memory_order_acq_rel);
    const unsigned effective=nativeFgFixedMultiplier(multiplier)?multiplier:0;
    setEffectiveMultiplier(effective,wasAutomatic||requested.load()!=effective);
}
void nativeFgSetAutoMode() noexcept {
    if(automatic.load(std::memory_order_acquire))return;
    std::lock_guard<std::mutex> lock(autoMutex);
    if(automatic.load(std::memory_order_acquire))return;
    autoPolicy.reset();
    setEffectiveMultiplier(0,true);automatic.store(true,std::memory_order_release);
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
bool nativeFgStatusSnapshot(EdvrFgStatus* out) noexcept {
    if(!out)return false;
    *out={sizeof(*out),EDVR_NATIVE_FRAME_GENERATION_VERSION_1};
    out->requestedMultiplier=requested.load(std::memory_order_acquire);
    out->state=out->requestedMultiplier?EDVR_FG_ARMING:EDVR_FG_OFF;
    auto* c=current.load(std::memory_order_acquire);
    return c&&!c->closed.load(std::memory_order_acquire)&&status(c,out)==S_OK;
}
HRESULT nativeFgAcquire(const EdvrFgRequest* request, EdvrFgTable* table) noexcept {
    if (!request || !table || request->size != sizeof(*request) || request->version != 1 || !request->gameDevice ||
        !request->generation || table->size != sizeof(*table) || table->version != 1) return E_INVALIDARG;
    try {
        std::lock_guard<std::mutex> lock(acquireMutex);
        if (auto* old = current.load()) if (!old->closed.load()) return E_PENDING;
        if (contextCount == contexts.size()) return E_OUTOFMEMORY;
        auto* c = new Coordinator; c->device = request->gameDevice; c->generation = request->generation;
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
    auto* c = current.load(std::memory_order_acquire); if (!enabled(c)) return;
    try {
        std::lock_guard<std::mutex> lock(c->metadataMutex);
        c->previousTemporal = c->temporal;
        const bool validSize = frame.size == sizeof(frame) || frame.size == EDVR_NATIVE_TEMPORAL_FRAME_SIZE_NO_ZOOM;
        c->temporalSupported = supported && validSize && frame.generation == c->generation;
        c->temporal = {};
        // NO_ZOOM is the normal cockpit wire size. Never read the optional
        // tail of an older caller, even when its current compiler knows it.
        if (validSize) std::memcpy(&c->temporal, &frame, frame.size);
        c->temporalConfig = configuration.load();
        if (reset && c->resetSequence != frame.sequence) {++c->history; c->resetSequence = frame.sequence;}
    } catch (...) {refuse(*c, TemporalMismatch);}
}
void nativeFgBeforeTemporalEye(uint64_t sequence, unsigned eye, ID3D11Texture2D* source) noexcept {
    auto* c = current.load(std::memory_order_acquire); if (!enabled(c) || eye > 1) return;
    try {
        if (!c->renderThread) c->renderThread = GetCurrentThreadId();
        if (!owner(*c)) {refuse(*c, TemporalMismatch); return;}
        if (c->captureSequence != sequence) {
            if (c->transaction) abortCapture(*c, TemporalMismatch);
            c->capture = {}; c->guideMask = c->uiMask = c->worldMask = c->temporalMask = 0;
            c->captureSequence = sequence; c->captureHistory = c->history.load(); c->rejected = false;
            std::lock_guard<std::mutex> lock(c->metadataMutex);
            if (!c->temporalSupported || !c->haveSource || c->temporal.sequence != sequence ||
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
                c->capture.source.historyGeneration = c->captureHistory; c->capture.source.multiplier = requested.load();
            }
        }
        c->activeEye = c->rejected ? 2u : eye; c->submitted[eye] = source;
    } catch (...) {c->rejected = true; refuse(*c, TemporalMismatch);}
}
void nativeFgCaptureGuides(ID3D11DeviceContext* context, const NativeFgGuides& guides) noexcept {
    auto* c = current.load(std::memory_order_acquire); if (!enabled(c) || !owner(*c) || c->rejected || guides.eye != c->activeEye) return;
    try {
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
            FrameGenerationInteropInit init;
            init.enabled = true; init.device11 = c->device; init.context = context; init.renderThreadId = c->renderThread;
            init.colorWidth = made->width; init.colorHeight = made->height; init.guideWidth = made->guideWidth; init.guideHeight = made->guideHeight;
            if (!made->interop.initialize(init) || !made->color.initialize(true, c->device, context, c->renderThread)) {
                abortCapture(*c, Interop); return;
            }
            // The worker may retain the previous producer until new work is
            // queued. Its retired ring still owns UI snapshots, so release only
            // the global cache's references before this producer's first capture.
            if (!uiLayerFgRetireStorage(context)) {abortCapture(*c, Ui); return;}
            c->producer = std::move(made);
        }
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
    if (!enabled(c) || !owner(*c) || c->rejected || c->activeEye > 1 || !exactSource ||
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
            if (c->queued.size() >= 3) {c->capture.input->cancel(); ++c->busyPairs; c->capture = {}; return;}
            c->queued.push_back(std::move(c->capture));
        }
        ++c->sourcePairs; c->latestSource = sequence; c->wake.notify_all();
    } catch (...) {abortCapture(*c, Interop);}
}
void nativeFgInvalidateProducer(uint32_t reason) noexcept {
    if (auto* c = current.load(std::memory_order_acquire)) {
        ++c->history; c->failure = reason;
        if (reason == NativeFgUnsupportedBounds && owner(*c)) {
            if (c->transaction) abortCapture(*c, Unsupported);
            else {c->rejected = true; refuse(*c, Unsupported);}
            c->failure = reason;
        }
        c->wake.notify_all();
    }
}
#ifdef EDVR_FRAME_GENERATION_COORDINATOR_TEST
unsigned nativeFgTestProtocol(unsigned* checksOut) noexcept {
    unsigned failures = 0, checks = 0;
    const auto check = [&](bool pass) {++checks; if (!pass) ++failures;};
    const auto closeFloat = [](float a, float b) {return std::fabs(a - b) < 1e-5f;};
    NativeFgGuides guide;
    guide.width = guide.height = 2133; guide.outputWidth = guide.outputHeight = 3200;
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
    check(closeFloat(options.jitterOffset[0],.5f/2133) && closeFloat(options.jitterOffset[1],.6f/2133));
    check(closeFloat(options.mvecScale[0],1.f/2133) && closeFloat(options.cameraNear,.025f));
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
            check(c->temporalSupported && c->temporal.sequence==1 && c->temporal.zoomed==0);
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
    terminalFault(*c,failedConfig,Ngx);
    check(table.getStatus(table.context,&report)==S_OK&&report.state==EDVR_FG_FAILED&&
        report.lastFailure==Ngx&&!report.activeMultiplier);
    check(table.beginSource(table.context,&source)==S_FALSE&&table.poll(table.context,&batch)==S_FALSE);
    nativeFgSetRequestedMultiplier(0);nativeFgSetRequestedMultiplier(3);
    terminalFault(*c,failedConfig,Ngx); // a late result from the old owner
    check(table.getStatus(table.context,&report)==S_OK&&report.state==EDVR_FG_ARMING&&
        report.configGeneration!=failedConfig);
    nativeFgSetAutoMode();table.getStatus(table.context,&report);
    const auto autoConfiguration=report.configGeneration;
    const auto* sameProducer=c->producer.get();
    check(nativeFgAutoRequested()&&report.requestedMultiplier==0);
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
    nativeFgSetRequestedMultiplier(0);table.getStatus(table.context,&report);
    check(!nativeFgAutoRequested()&&report.configGeneration==autoConfiguration+1);
    check(table.close(table.context)==S_OK && !c->devicePinned && !c->workerStarted.load());
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

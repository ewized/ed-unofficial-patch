// Build 334925, observation only. The early worker can Present before its
// issuing tick consumes input. Record the actual scheduler job, resource and
// native session identities independently. No NVAPI call belongs in this file.
#include "native_reflex_engine_hook.h"
#include "native_reflex_trace_buffer.h"
#include "../common/native_reflex_submit.h"
#include "../openvr/compat/openvr_v0_9_20.h"
#include "../common/code_hook.h"
#include "../common/config.h"
#include "../common/game_build.h"
#include "../common/log.h"
#include <d3d11.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace edvr::native_reflex {
namespace {
constexpr uint32_t imageSize = 0x640C000;
constexpr uint32_t buildStamp = 0x6ADA8048;
constexpr uint32_t capacity = 16384;
constexpr uint32_t windowPresents = 24;
constexpr uint64_t unknown = UINT64_MAX;
uintptr_t imageBase = 0;

// Borrowed within their original callback only. A helper executing a nested
// task must restore its outer worker context, even on the tick's own thread.
struct Scope {
    uintptr_t app = 0, group = 0;
    uint64_t tick = 0, worker = 0, groupId = unknown;
};
thread_local Scope scope;
struct Job {
    uintptr_t group = 0, executor = 0, payload = 0;
    uint64_t id = unknown;
};
struct Jobs {
    Job head, app;
    int matches = -1;
};
std::atomic<uintptr_t> observedApp{0};
struct Row {
    TraceSample sample;
    Scope context;
    Jobs jobs;
    uint64_t session, resourceId, epoch;
    int64_t qpc;
    DWORD thread;
    uint64_t projection = unknown, viewDigest = 0;
};
TraceBuffer<Row, capacity> rows;
enum class Window { Waiting, Recording, Draining, Done };
Window window = Window::Waiting; // validated Present owner only
uint32_t presents = 0;
uint64_t sealedCount = 0;
int64_t started = 0;
std::wstring outputPath;

uint32_t origin(const void* returnAddress) noexcept {
    const auto value = reinterpret_cast<uintptr_t>(returnAddress);
    return value >= imageBase && value - imageBase < imageSize ? uint32_t(value - imageBase) : 0;
}

// Only call with pinned same-build layouts while the game owns the receiver or
// retained job. A missing or inaccessible optional field is explicitly unknown.
template<class T> bool readField(uintptr_t address, T& value) noexcept {
    if (!address) return false;
    __try { std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T)); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
uint64_t appEpoch() noexcept {
    uint32_t epoch = 0;
    return readField(imageBase + 0x5F2ADF4, epoch) ? epoch : unknown;
}
bool receiver(uintptr_t object, uint32_t vtable) noexcept {
    uintptr_t value = 0;
    return readField(object, value) && value == imageBase + vtable;
}
// Read only this thread's active jobs. The scheduler retains each group until
// its callback returns. The original queue payload is an identity value only;
// its bytes can already belong to another job.
Jobs activeJobs() noexcept {
    uint32_t index = 0;
    uint8_t initialized = 0;
    uintptr_t block = 0, thread = 0, node = 0;
    const auto vector = uintptr_t(__readgsqword(0x58));
    if (!readField(imageBase + 0x60484E8, index) || !vector || index > 65535 ||
        !readField(vector + sizeof(void*) * index, block) || !block ||
        !readField(block + 0x44BC, initialized) || !initialized ||
        !readField(block + 0x4480, thread) || !thread || !readField(thread + 0x90, node)) return {};
    const auto app = observedApp.load(std::memory_order_acquire);
    uintptr_t previous = thread + 0x90;
    Jobs jobs;
    jobs.matches = 0;
    for (unsigned depth = 0; node != thread + 0xA8; ++depth) {
        uintptr_t next = 0, back = 0;
        uint16_t kind = 0;
        uint64_t bytes = 0;
        uint32_t id = 0;
        Job job;
        if (depth == 8 || !node || !readField(node, next) || !readField(node + 8, back) || back != previous ||
            !readField(node + 0x18, kind) || !readField(node + 0x20, job.executor) ||
            !readField(node + 0x28, bytes) || !readField(node + 0x30, job.group) || !job.group ||
            !readField(job.group + 0x30, id) || !readField(node + 0x40, job.payload)) return {};
        job.id = id;
        if (depth == 0) jobs.head = job;
        if (app && !kind && bytes == 16 && job.executor == app + 0x58) {
            if (++jobs.matches == 1) jobs.app = job;
            else jobs.app = {};
        }
        previous = node;
        node = next;
    }
    return jobs;
}
uint64_t objectSerial(ID3D11DeviceChild* object) noexcept {
    if (!object) return 0;
    // A scalar in an EDVR-owned private-data slot lives exactly as long as its
    // COM object. It neither holds another resource nor blocks ResizeBuffers.
    static constexpr GUID key = {0x2f18b1c1,0x2c72,0x4f21,{0x84,0xd4,0xf0,0x03,0x84,0x6e,0x9f,0x4a}};
    static SRWLOCK lock = SRWLOCK_INIT;
    static uint64_t next = 1;
    uint64_t id = 0;
    UINT bytes = sizeof(id);
    HRESULT hr = object->GetPrivateData(key, &bytes, &id);
    if (SUCCEEDED(hr)) return bytes == sizeof(id) ? id : 0;
    if (hr != DXGI_ERROR_NOT_FOUND) return 0;
    AcquireSRWLockExclusive(&lock);
    bytes = sizeof(id);
    hr = object->GetPrivateData(key, &bytes, &id);
    if (hr == DXGI_ERROR_NOT_FOUND) {
        id = next++;
        if (FAILED(object->SetPrivateData(key, sizeof(id), &id))) id = 0;
    } else if (FAILED(hr) || bytes != sizeof(id)) id = 0;
    ReleaseSRWLockExclusive(&lock);
    return id;
}
Row makeRow(TraceSample event, uint64_t serial) noexcept {
    return {event, scope, activeJobs(), traceNativeSession.load(std::memory_order_acquire),
        serial, appEpoch(), qpcNow(), GetCurrentThreadId()};
}
uint64_t record(TraceSample event, uint64_t serial = 0) noexcept {
    return rows.append(makeRow(event, serial));
}
void sample(TraceSample event) noexcept {
    uint64_t serial = 0;
    if (event.event == TraceEvent::EyeInput)
        serial = objectSerial(reinterpret_cast<ID3D11Texture2D*>(event.resource));
    if (event.event == TraceEvent::CommandList)
        serial = objectSerial(reinterpret_cast<ID3D11CommandList*>(event.resource));
    record(event, serial);
}
void targets(ID3D11DeviceContext* context, unsigned count, ID3D11RenderTargetView* const* views) noexcept {
    if (!views || count > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) return;
    // Record the first binding per context/epoch/worker and color resource.
    // Elite can rebind the same surface hundreds of times before one eye.
    struct Seen {
        ID3D11DeviceContext* context{};
        uint64_t epoch{}, worker{};
        std::array<uint64_t, 64> ids{};
        unsigned used{};
    };
    thread_local Seen seen;
    const auto epoch = appEpoch();
    if (seen.context != context || seen.epoch != epoch || seen.worker != scope.worker) {
        seen.context = context; seen.epoch = epoch; seen.worker = scope.worker; seen.used = 0;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (!views[i]) continue;
        ID3D11Resource* resource = nullptr;
        views[i]->GetResource(&resource);
        if (!resource) continue;
        const auto id = objectSerial(resource);
        const auto end = seen.ids.begin() + seen.used;
        if (!id || std::find(seen.ids.begin(), end, id) == end) {
            record({TraceEvent::RenderTarget, reinterpret_cast<uintptr_t>(context),
                    reinterpret_cast<uintptr_t>(resource), 0, i}, id);
            if (id && seen.used < seen.ids.size()) seen.ids[seen.used++] = id;
        }
        resource->Release();
    }
}
bool sameJob(const Job& a, const Job& b) noexcept {
    return a.group == b.group && a.id == b.id && a.executor == b.executor && a.payload == b.payload;
}
void drawIssued(ID3D11DeviceContext* ctx, const void* caller, unsigned count, unsigned instances) noexcept {
    const uint32_t rva = origin(caller);
    if (!ctx || !rva || !count || !instances) return;
    // Query the real OM state after the ordinary game draw returns, before any
    // scope restores its state. The preceding game OM bind can have been
    // superseded. Slot zero covers the native color target in this capture.
    ID3D11RenderTargetView* view = nullptr;
    ctx->OMGetRenderTargets(1, &view, nullptr);
    if (!view) return;
    ID3D11Resource* output = nullptr;
    view->GetResource(&output);
    view->Release();
    if (!output) return;
    const uint64_t serial = objectSerial(output);
    const Row row = makeRow({TraceEvent::GpuDraw, reinterpret_cast<uintptr_t>(ctx),
        reinterpret_cast<uintptr_t>(output), 0, (uint64_t(instances) << 32) | count, rva}, serial);
    output->Release();
    // Collapse adjacent draws from one real job into the same color resource.
    // Another recorded command, rebind, copy, eye, thread or job breaks the
    // run. This preserves the first issued draw in the observed run.
    struct Last { Row row{}; uint64_t ticket = 0; };
    thread_local Last last;
    const auto& prior = last.row;
    if (serial && last.ticket && rows.count() == last.ticket && prior.sample.actor == row.sample.actor &&
        prior.sample.caller == rva && prior.epoch == row.epoch && prior.session == row.session &&
        prior.context.tick == row.context.tick && prior.context.worker == row.context.worker && prior.resourceId == serial &&
        prior.jobs.matches == row.jobs.matches && sameJob(prior.jobs.head, row.jobs.head) && sameJob(prior.jobs.app, row.jobs.app)) return;
    last.row = row;
    last.ticket = rows.append(row);
}
void copyIssued(ID3D11DeviceContext* ctx, const void* caller, ID3D11Resource* dst, ID3D11Resource* src) noexcept {
    const uint32_t rva = origin(caller);
    if (!rva || !ctx || !dst || !src || src == dst) return;
    D3D11_RESOURCE_DIMENSION type{};
    dst->GetType(&type);
    if (type != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    src->GetType(&type);
    if (type != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    // CopyResource submitted a whole-resource transfer. Keep both lifetimes
    // so an intermediate color target can be related to a submitted eye.
    record({TraceEvent::GpuCopy, reinterpret_cast<uintptr_t>(ctx), reinterpret_cast<uintptr_t>(dst),
            objectSerial(src), 0, rva}, objectSerial(dst));
}
void clearIssued(ID3D11DeviceContext* ctx, const void* caller, ID3D11RenderTargetView* view) noexcept {
    const uint32_t rva = origin(caller);
    if (!rva || !ctx || !view) return;
    ID3D11Resource* output = nullptr;
    view->GetResource(&output);
    if (!output) return;
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    view->GetDesc(&desc);
    // A clear writes the view's extent. Only single-slice, single-mip
    // Texture2D views are marked as whole-resource writes.
    bool whole = false;
    if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && desc.Texture2D.MipSlice == 0) {
        D3D11_TEXTURE2D_DESC texture{};
        static_cast<ID3D11Texture2D*>(output)->GetDesc(&texture);
        whole = texture.MipLevels == 1 && texture.ArraySize == 1;
    }
    record({TraceEvent::GpuClear, reinterpret_cast<uintptr_t>(ctx), reinterpret_cast<uintptr_t>(output),
            0, uint64_t(whole), rva}, objectSerial(output));
    output->Release();
}
// Persistent camera-record addresses observed in this capture. Retaining the
// numeric address permits recording a competing copy into it from another
// caller. It grants no lifetime or generation beyond a bracketed operation.
TraceAddresses<32> viewRecords;
bool watchView(uintptr_t address, bool add) noexcept {
    const bool watched = viewRecords.find(address, add);
    if (address && add && !watched) record({TraceEvent::ViewWatchFull, 0, address});
    return watched;
}

// The first four rows are the camera basis and translation. Their digest can
// reject a changed view. The two exact projection fields are consumed by both
// eye builders and can stay equal over many distinct camera generations.
uint64_t basisDigest(uintptr_t address) noexcept {
    std::array<uint8_t, 64> basis{};
    if (!readField(address, basis)) return 0;
    uint64_t hash = 14695981039346656037ull;
    for (auto byte : basis) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}
uint64_t viewRow(TraceSample event, uintptr_t cameraRecord) noexcept {
    Row row = makeRow(event, 0);
    if (cameraRecord) {
        uint32_t farBits = 0;
        uint8_t flags = 0;
        if (readField(cameraRecord + 0x238, farBits) && readField(cameraRecord + 0x248, flags))
            row.projection = (uint64_t(flags) << 32) | farBits;
        row.viewDigest = basisDigest(cameraRecord);
    }
    return rows.append(row);
}
uint64_t viewConsume(uintptr_t camera, int32_t mode, const void* caller) noexcept {
    uintptr_t world = 0;
    uint32_t rva = origin(caller);
    if (!camera || !rva || !readField(camera + 0x50, world) || !world) return 0;
    uintptr_t source = world + 0x4A0;
    watchView(source, true);
    const auto ticket = viewRow({TraceEvent::ViewConsumeBegin, camera, source, 0, uint32_t(mode), rva}, source);
    if (ticket) {
        Row mailbox = makeRow({TraceEvent::ViewMailbox, camera, world + 0x3330, ticket, 0, rva}, 0);
        mailbox.viewDigest = basisDigest(mailbox.sample.resource);
        rows.append(mailbox);
    }
    return ticket;
}
void viewRefresh(uintptr_t ctx, uintptr_t camera, uintptr_t returnSlot) noexcept {
    // Only the real persistent nodes previously written by an eye consumer
    // are interesting here. Many constant refresh calls serve other cameras.
    const uintptr_t source = camera + 0x20;
    if (!camera || !watchView(source, false)) return;
    const void* caller = nullptr;
    readField(returnSlot, caller);
    viewRow({TraceEvent::ViewRefresh, ctx, source, 0, 0, origin(caller)}, source);
}
const TraceObserver observer{sample, targets, drawIssued, copyIssued, clearIssued, viewConsume, viewRefresh};

struct Entry {
    uint32_t rva;
    const char* pin;
    std::atomic<void*> original{nullptr};
};
Entry tick{0x7F33D0, "488bc45553565741564157488d68a148"};
Entry input{0x8F0BA0, "4c8bdc554154498dab28feffff4881ec"};
Entry allocation{0x5D3EA0, "48895c240848896c2410488974241848"};
Entry early{0x7FE710, "48895c2410574883ec50f30f10891004"};
Entry late{0x7FDCB0, "40534883ec30f30f1089100400000f57"};
Entry render{0x200DE0, "48895c2408574883ec204c8b89080b00"};
Entry hmd{0x4E3730, "405553565741564157488dac2478ffff"};

Entry viewCopy{0x59F2A0, "48895c2408574883ec200f1002488bda"};
using ViewCopyFn = uintptr_t(*)(uintptr_t, uintptr_t);

using PhaseFn = void(*)(uintptr_t, float);
using WorkerFn = void(*)(uintptr_t);
using AllocateFn = void*(*)(uintptr_t, uintptr_t, size_t, uint32_t);
using HmdFn = void*(*)(uintptr_t, void*, double, uint8_t*, uint8_t);
template<class Fn> Fn original(Entry& entry) noexcept {
    return reinterpret_cast<Fn>(entry.original.load(std::memory_order_acquire));
}
// This copier also serves unrelated cameras. Record exact source/destination
// at the two simulation publishes, computed eye records and final eye-node
// copies, plus other calls into a persistent record already being watched.
uintptr_t hookedViewCopy(uintptr_t destination, uintptr_t source) {
    const auto forward = original<ViewCopyFn>(viewCopy);
    if (!traceEnabled()) return forward(destination, source);
    const auto rva = origin(_ReturnAddress());
    const bool postInputView = rva == 0x281E1DD || rva == 0x281E537 ||
        rva == 0x281E203 || rva == 0x281E23C || rva == 0x281E229 || rva == 0x281E262;
    const bool directEye = rva == 0x2845C3E || rva == 0x2845C62;
    const bool retainedEye = rva == 0x287D828 || rva == 0x287D89C || rva == 0x287D853 || rva == 0x287D8C8;
    const bool persistent = postInputView || directEye || retainedEye;
    const bool watched = watchView(destination, persistent);
    const bool observe = watched || persistent || rva == 0x287B4F1;
    uint64_t ticket = 0;
    if (observe) ticket = viewRow({TraceEvent::ViewCopyBegin, source, destination, 0, 0, rva}, source);
    if (ticket && postInputView)
        viewRow({TraceEvent::ViewCopyPrior, source, destination, ticket, 0, rva}, destination);
    const auto result = forward(destination, source);
    // Also expose a copy that was already running when the destination became
    // interesting. Its missing begin receipt makes that interval ambiguous.
    if (observe || watchView(destination, false))
        viewRow({TraceEvent::ViewCopyEnd, source, destination, ticket, result == destination, rva}, destination);
    return result;
}
void hookedTick(uintptr_t app, float dt) {
    const auto caller = origin(_ReturnAddress());
    const bool observe = traceEnabled() && (caller == 0x7F95D8 || caller == 0x7F96DF) && receiver(app, 0x4DDF490);
    if (!observe) return original<PhaseFn>(tick)(app, dt);
    const Scope previous = scope;
    scope = {};
    observedApp.store(app, std::memory_order_release);
    scope.app = app;
    scope.tick = record({TraceEvent::TickBegin, app, 0, 0, 0, caller});
    original<PhaseFn>(tick)(app, dt);
    record({TraceEvent::TickEnd, app, 0, scope.tick, 0, caller});
    scope = previous;
}
void hookedInput(uintptr_t manager, float dt) {
    const auto caller = origin(_ReturnAddress());
    if (!traceEnabled()) return original<PhaseFn>(input)(manager, dt);
    const bool matches = caller == 0x1EB2FC && receiver(manager, 0x512FD90);
    const auto ticket = record({TraceEvent::InputBegin, manager, 0, 0, matches, caller});
    original<PhaseFn>(input)(manager, dt);
    record({TraceEvent::InputEnd, manager, 0, ticket, matches, caller});
}
struct JobGroup {
    uintptr_t address = 0;
    uint64_t id = unknown;
};
JobGroup jobGroup(uintptr_t field) noexcept {
    JobGroup result;
    uint32_t id = 0;
    if (readField(field, result.address) && result.address && readField(result.address + 0x30, id)) result.id = id;
    return result;
}
void* hookedAllocate(uintptr_t scheduler, uintptr_t executor, size_t size, uint32_t flags) {
    void* payload = original<AllocateFn>(allocation)(scheduler, executor, size, flags);
    if (!traceEnabled() || !scope.tick || !payload) return payload;
    const auto caller = origin(_ReturnAddress());
    if ((caller != 0x7F3545 && caller != 0x7F3882) || executor != scope.app + 0x58 || size != 16 || flags) return payload;
    const auto group = jobGroup(reinterpret_cast<uintptr_t>(payload) - sizeof(void*));
    const Scope previous = scope;
    scope.group = group.address;
    scope.groupId = group.id;
    record({TraceEvent::JobIssue, scheduler, reinterpret_cast<uintptr_t>(payload), 0,
            caller == 0x7F3545 ? early.rva : late.rva, caller});
    scope = previous;
    return payload;
}

void worker(Entry& entry, TraceEvent kind, uintptr_t app, uint32_t caller) {
    if (!traceEnabled()) return original<WorkerFn>(entry)(app);
    const auto previous = scope;
    scope = {};
    scope.app = app;
    const bool matched = caller == 0x5D6B81 && receiver(app, 0x4DDF490);
    scope.worker = record({kind, app, 0, 0, matched, caller});
    original<WorkerFn>(entry)(app);
    scope = previous;
}
void hookedEarly(uintptr_t app) { worker(early, TraceEvent::EarlyWorker, app, origin(_ReturnAddress())); }
void hookedLate(uintptr_t app) { worker(late, TraceEvent::LateWorker, app, origin(_ReturnAddress())); }

void hookedRender(uintptr_t app) {
    if (!traceEnabled()) return original<WorkerFn>(render)(app);
    const auto caller = origin(_ReturnAddress());
    uintptr_t ring = 0, priorResource = 0;
    uint32_t priorTag = UINT32_MAX;
    const auto epoch = appEpoch();
    if (caller == 0x7FE8B2 && receiver(app, 0x4DDF490) && epoch != unknown &&
        readField(app + 0xB08, ring) && ring) {
        // This ring accepts any non-sentinel prior tag. Keep the stored tag
        // and resource verbatim, including stale tags and missing resources.
        const auto slot = ring + 40 * ((uint32_t(epoch) - 1u) % 6);
        readField(slot + 0x20, priorTag);
        if (priorTag != UINT32_MAX) readField(slot + 0x10, priorResource);
    }
    const auto ticket = record({TraceEvent::RenderBegin, app, priorResource, ring, priorTag, caller});
    original<WorkerFn>(render)(app);
    record({TraceEvent::RenderEnd, app, 0, ticket, 0, caller});
}
void* hookedHmd(uintptr_t service, void* matrix, double prediction, uint8_t* valid, uint8_t waitRequested) {
    if (!traceEnabled()) return original<HmdFn>(hmd)(service, matrix, prediction, valid, waitRequested);
    const auto caller = origin(_ReturnAddress());
    uint8_t cached = 0;
    const uint64_t cache = service && readField(service + 0x111, cached) ? cached : 256;
    const uint64_t selector = (cache << 8) | waitRequested;
    const auto ticket = record({TraceEvent::HmdRequest, service, 0, 0, selector, caller});
    void* result = original<HmdFn>(hmd)(service, matrix, prediction, valid, waitRequested);
    uint8_t validByte = 0;
    const auto resultFlag = readField(reinterpret_cast<uintptr_t>(valid), validByte) ? uint64_t(validByte) : unknown;
    Row returned = makeRow({TraceEvent::HmdReturn, service, 0, ticket, resultFlag, caller}, 0);
    if (result && result == matrix && resultFlag == 1)
        returned.viewDigest = basisDigest(reinterpret_cast<uintptr_t>(matrix));
    rows.append(returned);
    return result;
}
bool pinMatches(uint32_t rva, const char* hex) noexcept {
    for (unsigned n = 0; *hex; ++n, hex += 2) {
        auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        uint8_t value = 0;
        if (!readField(imageBase + rva + n, value) || value != ((digit(hex[0]) << 4) | digit(hex[1]))) return false;
    }
    return true;
}
bool publishOriginal(void* trampoline, void* opaque) noexcept {
    static_cast<Entry*>(opaque)->original.store(trampoline, std::memory_order_release);
    return true;
}
bool install() {
    uint32_t stamp = 0, size = 0;
    if (!gameBuildKnown() || !gameBuildIdentity(&stamp, &size) || stamp != buildStamp || size != imageSize) return false;
    imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    struct Target { Entry* entry; void* fn; const char* name; };
    const Target targetsToInstall[] = {
        {&tick, reinterpret_cast<void*>(&hookedTick), "frame trace app tick"},
        {&input, reinterpret_cast<void*>(&hookedInput), "frame trace input"},
        {&allocation, reinterpret_cast<void*>(&hookedAllocate), "frame trace job allocation"},
        {&early, reinterpret_cast<void*>(&hookedEarly), "frame trace early worker"},
        {&late, reinterpret_cast<void*>(&hookedLate), "frame trace late worker"},
        {&render, reinterpret_cast<void*>(&hookedRender), "frame trace render"},
        {&hmd, reinterpret_cast<void*>(&hookedHmd), "frame trace HMD service"},
        {&viewCopy, reinterpret_cast<void*>(&hookedViewCopy), "frame trace camera record copy"}
    };
    for (const auto& target : targetsToInstall)
        if (!pinMatches(target.entry->rva, target.entry->pin)) return false;
    struct Pin { uint32_t rva; const char* bytes; };
    // Verify the observed indirect call origins and the TLS list protocol as
    // well as the entry bytes. Refuse a modified path before installing any tap.
    static constexpr Pin sites[] = {
        {0x7F340A, "e8512fdeff"}, {0x7F395C, "ff90c8000000"}, {0x1EB2F9, "ff5018"},
        {0x7FE8AC, "ff90b8000000"}, {0x50F5FF, "ff5040"}, {0x4E37B5, "ff5010"},
        {0x4E391F, "ffd3"}, {0x4E1AED, "ff5028"}, {0x4E1B34, "ff5028"},
        {0x7F95D3, "e8f89dffff"}, {0x7F96DA, "e8f19cffff"}, {0x7F3540, "e85b09deff"},
        {0x7F387D, "e81e06deff"}, {0x5D41EA, "65488b042558000000"}, {0x5D3D96, "895930"},
        {0x5D6BBD, "488b4d188b4510418987"}, {0x5D6B7E, "ff5008"}, {0x5D570F, "488d5308"},
        {0x281E1D8, "e8c310d8fd"}, {0x281E532, "e8690dd8fd"}, {0x287B4EC, "e8af3dd2fd"},
        {0x2845C39, "e86296d5fd"}, {0x2845C5D, "e83e96d5fd"}, {0x286B1EB, "e840a5fdff"},
        {0x284583A, "f3410f109ed8060000"}, {0x284586A, "410fb686e8060000"}, {0x284587F, "e89ca1ffff"},
        {0x281E1FE, "e89d10d8fd"}, {0x281E237, "e86410d8fd"}, {0x281E224, "e87710d8fd"}, {0x281E25D, "e83e10d8fd"},
        {0x287D823, "e8781ad2fd"}, {0x287D897, "e8041ad2fd"}, {0x287D84E, "e84d1ad2fd"}, {0x287D8C3, "e8d819d2fd"},
        {0x594FA6, "e845d3ffff"}, {0x59F40E, "898738020000"}, {0x59F43F, "888748020000"}, {0x592560, "f3410f108758020000"}
    };
    for (const auto& site : sites) if (!pinMatches(site.rva, site.bytes)) return false;
    // The opt-in capture leaves forwarding entry hooks until the game exits.
    // Keep their code mapped even if a harness explicitly unloads the proxy.
    HMODULE retained = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&hookedTick), &retained)) return false;
    for (const auto& target : targetsToInstall) {
        // As with the other process-lifetime game entry hooks, preserve the
        // forward trampoline if another installation refuses or a window ends.
        auto hook = std::make_unique<CodeHook>();
        void* originalFn = nullptr;
        if (!hook->install(reinterpret_cast<void*>(imageBase + target.entry->rva), target.fn, &originalFn,
                           target.name, publishOriginal, target.entry, true)) return false;
        hook.release();
    }
    return true;
}

const char* eventName(TraceEvent type) noexcept {
    static constexpr const char* names[] = {"tick_begin","tick_end","input_begin","input_end","early_worker","late_worker","job_issue",
        "render_begin","render_end","command_list","render_target","native_pose","eye_input",
        "present_begin","present_end","hmd_request","hmd_return","game_eye_accepted","game_eye_rejected",
        "gpu_draw","gpu_copy","gpu_clear","view_copy_begin","view_copy_end",
        "view_consume_begin","view_consume_end","view_refresh","view_watch_full","view_copy_prior","view_mailbox"};
    return names[static_cast<unsigned>(type)];
}
bool writeTrace(uint32_t kept) {
    HANDLE file = CreateFileW(outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    // The window is sealed before disk I/O. A fixed buffer bounds the export
    // memory and short/failed writes leave an identifiable incomplete file.
    char batch[16384];
    size_t used = 0;
    auto flush = [&] {
        DWORD written = 0;
        bool ok = WriteFile(file, batch, DWORD(used), &written, nullptr) && written == used;
        used = 0;
        return ok;
    };
    auto line = [&](const char* text) {
        const size_t len = std::strlen(text);
        if (used + len > sizeof(batch) && !flush()) return false;
        std::memcpy(batch + used, text, len);
        used += len;
        return true;
    };
    char buf[1000];
    std::snprintf(buf, sizeof(buf), "# build=334925,qpc_frequency=%lld,kept=%u,overflow=%llu,window_presents=%u\n",
                  static_cast<long long>(qpcFrequency()), kept, sealedCount - kept, presents);
    bool ok = line(buf) && line("ordinal,qpc,thread,session,event,epoch,tick,worker,app,issue_group,issue_group_id,actor,resource,resource_id,sequence,detail,caller_rva,head_group,head_id,head_executor,head_payload,app_jobs,app_job_group,app_job_id,app_job_executor,app_job_payload,projection_bits,view_digest\n");
    for (uint32_t i = 0; ok && i < kept; ++i) {
        const auto& row = rows.row(i);
        const auto& e = row.sample;
        const auto& c = row.context;
        const auto& j = row.jobs;
        std::snprintf(buf, sizeof(buf),
            "%u,%lld,%lu,%llu,%s,%llu,%llu,%llu,%llx,%llx,%llu,%llx,%llx,%llu,%llu,%llu,%x,%llx,%llu,%llx,%llx,%d,%llx,%llu,%llx,%llx,%llu,%llu\n",
            i + 1, static_cast<long long>(row.qpc), row.thread, row.session, eventName(e.event), row.epoch,
            c.tick, c.worker, c.app, c.group, c.groupId, e.actor, e.resource, row.resourceId, e.sequence, e.detail, e.caller,
            j.head.group, j.head.id, j.head.executor, j.head.payload, j.matches, j.app.group, j.app.id, j.app.executor, j.app.payload, row.projection, row.viewDigest);
        ok = line(buf);
    }
    if (ok) ok = line("# complete\n") && flush();
    if (!CloseHandle(file)) ok = false;
    return ok;
}
} // namespace

uint64_t tracePresentBegin(IDXGISwapChain* chain, const void* caller, unsigned flags, bool proxy) {
    if (!chain || !traceEnabled()) return 0;
    ID3D11Texture2D* texture = nullptr;
    chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    const auto ticket = record({TraceEvent::PresentBegin, reinterpret_cast<uintptr_t>(chain),
        reinterpret_cast<uintptr_t>(texture), 0, (uint64_t(proxy) << 32) | flags, origin(caller)}, objectSerial(texture));
    if (texture) texture->Release();
    return ticket;
}
void tracePresentEnd(IDXGISwapChain* chain, uint64_t ticket, long result, bool owner) {
    if (ticket && traceEnabled())
        record({TraceEvent::PresentEnd, reinterpret_cast<uintptr_t>(chain), 0, ticket, (uint64_t(owner) << 32) | uint32_t(result)});
}
void updateFrameTrace() {
    static uint32_t configGeneration = 0;
    static bool readConfig = false, requested = false;
    if (window == Window::Done) return;
    auto& config = Config::get();
    const auto current = config.generation();
    if (!readConfig || current != configGeneration) {
        requested = config.getBool("log.reflex_frame_trace", false);
        configGeneration = current;
        readConfig = true;
    }
    if (window == Window::Waiting) {
        if (!requested) return;
        if (!install()) {
            Log::get().note("Reflex frame trace: build, site or hook unavailable; recording declined.");
            window = Window::Done;
            return;
        }
        started = qpcNow();
        outputPath = Log::get().dir() + L"\\reflex_frame_trace_" + std::to_wstring(GetCurrentProcessId()) +
                     L"_" + std::to_wstring(started) + L".csv";
        window = Window::Recording;
        traceObserver.store(&observer, std::memory_order_release);
        Log::get().note("Reflex frame trace: armed for %u owned Presents on build 334925.", windowPresents);
        return;
    }
    if (window == Window::Recording) {
        ++presents;
        if (requested && presents < windowPresents && rows.count() < capacity && qpcNow() - started < 5 * qpcFrequency()) return;
        traceObserver.store(nullptr, std::memory_order_release);
        sealedCount = rows.seal();
        window = Window::Draining;
    }
    const uint32_t kept = uint32_t((std::min)(sealedCount, uint64_t(capacity)));
    for (uint32_t i = 0; i < kept; ++i) if (!rows.readable(i)) return;
    window = Window::Done;
    const bool written = writeTrace(kept);
    Log::get().note("Reflex frame trace: %s %ls, kept=%u overflow=%llu owner_presents=%u.",
                    written ? "wrote" : "write failed for", outputPath.c_str(), kept, sealedCount - kept, presents);
}
} // namespace edvr::native_reflex

// No device operation for rejected input or an unrecognized producer. Keep
// the rejection and its actual origin so observers can distinguish missing
// pairs from an ownership mismatch without dereferencing arbitrary handles.
extern "C" void WINAPI edvrNoteNativeReflexSubmit(const EdvrNativeReflexSubmit* receipt) {
    using namespace edvr::native_reflex;
    if (!traceEnabled() || !receipt || receipt->size != sizeof(*receipt) || receipt->version != EDVR_NATIVE_REFLEX_SUBMIT_VERSION_1) return;
    const auto& s = *receipt;
    const auto caller = origin(reinterpret_cast<const void*>(s.caller));
    const bool pinned = (s.eye == 0 && caller == 0x4E1AF0) || (s.eye == 1 && caller == 0x4E1B37);
    const bool sameSession = s.runtimeGeneration && s.runtimeGeneration == traceNativeSession.load(std::memory_order_acquire);
    vr::Texture_t descriptor{};
    const bool accepted = !s.result && pinned && sameSession && s.compositorGeneration && s.poseSequence &&
        readField(s.textureDescriptor, descriptor) && descriptor.eType == vr::API_DirectX && descriptor.handle;
    const auto serial = accepted ? objectSerial(static_cast<ID3D11Texture2D*>(descriptor.handle)) : 0;
    const auto detail = uint64_t(s.result) << 32 | s.eye;
    record({accepted ? TraceEvent::GameEyeAccepted : TraceEvent::GameEyeRejected,
            uintptr_t(s.compositorGeneration), reinterpret_cast<uintptr_t>(descriptor.handle), s.poseSequence, detail, caller}, serial);
}

// Explicit standalone GPU correctness fixture for the REAL output publisher,
// exact production HUD shader, shared import and retirement callbacks. NGX
// admission alone is link-time substituted: phase inputs are synthetic, named
// as such in the receipt, and this rig never claims an NGX evaluation or XR flight.
#include "../../src/d3d11/frame_generation_output.h"
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace edvr;
using Microsoft::WRL::ComPtr;
namespace {
FrameGenerationNgxCompletion admission = FrameGenerationNgxCompletion::Ready;
unsigned checks = 0;
constexpr unsigned width = 64, height = 32;
void check(bool value, const char* label) {
    ++checks;
    if (!value) throw std::runtime_error(label);
}
void hr(HRESULT value, const char* label) {
    if (FAILED(value)) std::printf("HRESULT %s=0x%08lx\n", label, static_cast<unsigned long>(value));
    check(SUCCEEDED(value), label);
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
HMODULE systemModule(const wchar_t* leaf) {
    wchar_t path[32768]{};
    const UINT length = GetSystemDirectoryW(path, 32768);
    check(length && length < 32000 && !wcscat_s(path, L"\\") && !wcscat_s(path, leaf), "System32 path");
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    wchar_t actual[32768]{};
    const DWORD count = module ? GetModuleFileNameW(module, actual, 32768) : 0;
    check(count && count < 32768 && !_wcsicmp(path, actual), "verified System32 graphics module");
    return module;
}
struct Runtime {
    using Factory = HRESULT (WINAPI*)(REFIID, void**);
    using Create = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    Create create = nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> producer, consumer;
    ComPtr<ID3D11DeviceContext> producerContext, consumerContext;
    Runtime() {
        auto factory = reinterpret_cast<Factory>(GetProcAddress(systemModule(L"dxgi.dll"), "CreateDXGIFactory1"));
        create = reinterpret_cast<Create>(GetProcAddress(systemModule(L"d3d11.dll"), "D3D11CreateDevice"));
        check(factory && create, "System32 entry points");
        ComPtr<IDXGIFactory1> dxgi; hr(factory(IID_PPV_ARGS(&dxgi)), "DXGI factory");
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto result = dxgi->EnumAdapters1(i, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            hr(result, "adapter enumeration");
            DXGI_ADAPTER_DESC1 desc{}; hr(candidate->GetDesc1(&desc), "adapter descriptor");
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter = candidate; break; }
        }
        check(bool(adapter), "real NVIDIA adapter required, no WARP substitution");
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        hr(create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
            &producer, nullptr, &producerContext), "standalone producer D3D11 device");
        hr(create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
            &consumer, nullptr, &consumerContext), "separate same-adapter XR-style D3D11 device");
        check(producer.Get() != consumer.Get(), "consumer does not borrow producer context");
    }
};
using Pixel = std::array<unsigned char, 4>;
Pixel world(unsigned phase, unsigned eye, unsigned x, unsigned y) {
    return Pixel{static_cast<unsigned char>(20 + eye * 60 + phase * 12 + (x + y) % 16),
        static_cast<unsigned char>(40 + eye * 10 + phase * 7 + x % 8),
        static_cast<unsigned char>(60 + phase * 5 + y % 8), static_cast<unsigned char>(173 + eye * 24)};
}
Pixel layer(unsigned eye, unsigned x, unsigned y) {
    if(x<width/4)return Pixel{0,0,0,255};
    return Pixel{static_cast<unsigned char>(12 + eye * 8 + x % 8), static_cast<unsigned char>(5 + y % 4),
        static_cast<unsigned char>(3 + eye), static_cast<unsigned char>(128 + eye * 16)};
}
Pixel multiply(unsigned eye) { return Pixel{static_cast<unsigned char>(128 + eye * 32), 255, 64, 255}; }
Pixel multiplyAt(unsigned eye,unsigned x){return x<width/4?Pixel{255,255,255,255}:multiply(eye);}
ComPtr<ID3D11Texture2D> image(ID3D11Device* device, DXGI_FORMAT format, const void* pixels, UINT pitch,
                             bool shared = false) {
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1; desc.Format = format;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (shared) { desc.BindFlags |= D3D11_BIND_RENDER_TARGET; desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED; }
    D3D11_SUBRESOURCE_DATA data{pixels, pitch, 0};
    ComPtr<ID3D11Texture2D> result; hr(device->CreateTexture2D(&desc, pixels ? &data : nullptr, &result), "fixture texture");
    return result;
}
struct Shared {
    ComPtr<ID3D11Texture2D> alias;
    ComPtr<ID3D12Resource> resource, privateResource;
    Handle handle;
};
struct SyntheticOwner {
    ComPtr<ID3D12Resource> resources[2][3];
    ComPtr<ID3D12Fence> fence;
};
void openImage(ID3D12Device* device12, ID3D11Device* device11, Shared& target, const std::vector<Pixel>& pixels) {
    target.alias = image(device11, DXGI_FORMAT_R8G8B8A8_UNORM, pixels.data(), width * sizeof(Pixel), true);
    ComPtr<IDXGIResource1> sharing; hr(target.alias.As(&sharing), "generated fixture NT resource");
    hr(sharing->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
        &target.handle.value), "generated fixture NT handle");
    hr(device12->OpenSharedHandle(target.handle.value, IID_PPV_ARGS(&target.resource)), "generated fixture D3D12 import");
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    auto desc = target.resource->GetDesc(); desc.Alignment = 0; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&target.privateResource)), "synthetic private NGX-shaped output resource");
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* image,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {image, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after}; list->ResourceBarrier(1, &barrier);
}
bool wait(ID3D11Fence* fence, uint64_t value) {
    const auto start = GetTickCount64();
    while (GetTickCount64() - start < 3000) {
        const auto done = fence->GetCompletedValue();
        if (done == UINT64_MAX) return false;
        if (done >= value) return true;
        Sleep(1);
    }
    return false;
}
struct Gate {
    ComPtr<ID3D12Fence> fence;
    ~Gate() { if (fence) fence->Signal(1); }
};
EdvrFgSource metadata(uint64_t sequence) {
    EdvrFgSource source{}; source.size = sizeof(source); source.version = EDVR_NATIVE_FRAME_GENERATION_VERSION_1;
    source.generation = source.referenceGeneration = source.configGeneration = 1; source.sequence = sequence;
    source.poseDisplayTime = static_cast<int64_t>(sequence * 10000000); source.width = width; source.height = height;
    for (unsigned e = 0; e < 2; ++e) {
        source.views[e].orientation[3] = 1; source.views[e].position[0] = e ? .03f : -.03f;
        source.views[e].fov[0] = source.views[e].fov[3] = -.8f;
        source.views[e].fov[1] = source.views[e].fov[2] = .8f;
    }
    return source;
}
void run() {
    Runtime runtime;
    FrameGenerationInterop interop;
    FrameGenerationInteropInit init; init.enabled = true; init.device11 = runtime.producer.Get();
    init.context = runtime.producerContext.Get(); init.renderThreadId = GetCurrentThreadId();
    init.colorWidth = init.guideWidth = width; init.colorHeight = init.guideHeight = height;
    check(interop.initialize(init), "actual immutable input adapter initialized");
    auto* device12 = interop.device12(); check(device12 != nullptr, "same-adapter D3D12 device");
    // Kept until successful final drain; any exception intentionally retains
    // the actual GPU objects until process exit, including setup-time failures.
    auto* keepAlive = new std::vector<ComPtr<IUnknown>>;
    keepAlive->reserve(256);
    const auto keep = [&](auto* object) { keepAlive->emplace_back(object); };
    keep(device12); keep(runtime.producer.Get()); keep(runtime.producerContext.Get());
    keep(runtime.consumer.Get()); keep(runtime.consumerContext.Get());
    auto* output = new FrameGenerationOutput;
    check(output->initialize(true, device12, width, height), "actual private D3D12 HUD/output publisher initialized");
    ComPtr<ID3D11Device1> consumer1; ComPtr<ID3D11Device5> consumer5; ComPtr<ID3D11DeviceContext4> context4;
    hr(runtime.consumer.As(&consumer1), "consumer NT resource interface");
    hr(runtime.consumer.As(&consumer5), "consumer shared fence interface");
    hr(runtime.consumerContext.As(&context4), "consumer private context fence interface");
    ComPtr<ID3D11Fence> retired;
    hr(consumer5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&retired)), "consumer retirement fence");
    keep(retired.Get());
    Handle retiredHandle; hr(retired->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &retiredHandle.value), "consumer NT fence handle");
    Shared generated[2][3];
    for (unsigned e = 0; e < 2; ++e) for (unsigned p = 0; p < 3; ++p) {
        std::vector<Pixel> pixels(width * height);
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) pixels[y * width + x] = world(p + 1, e, x, y);
        openImage(device12, runtime.producer.Get(), generated[e][p], pixels);
        keep(generated[e][p].resource.Get()); keep(generated[e][p].privateResource.Get());
    }
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> syntheticReady;
    hr(device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "synthetic input private queue");
    hr(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "synthetic input allocator");
    hr(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "synthetic input command list");
    hr(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&syntheticReady)), "synthetic input completion");
    keep(queue.Get()); keep(allocator.Get()); keep(list.Get()); keep(syntheticReady.Get());
    for (auto& eye : generated) for (auto& item : eye) {
        transition(list.Get(), item.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(item.privateResource.Get(), item.resource.Get());
        transition(list.Get(), item.privateResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(list.Get(), item.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    hr(list->Close(), "synthetic input close");
    ComPtr<ID3D11Device5> producer5; ComPtr<ID3D11DeviceContext4> producer4;
    ComPtr<ID3D11Fence> initialized11; ComPtr<ID3D12Fence> initialized12; Handle initializedHandle;
    hr(runtime.producer.As(&producer5), "producer setup fence interface");
    hr(runtime.producerContext.As(&producer4), "producer setup context fence interface");
    hr(producer5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&initialized11)), "producer setup fence");
    hr(initialized11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &initializedHandle.value), "producer setup NT fence");
    hr(device12->OpenSharedHandle(initializedHandle.value, IID_PPV_ARGS(&initialized12)), "producer setup D3D12 fence import");
    keep(initialized12.Get()); keep(initialized11.Get());
    hr(producer4->Signal(initialized11.Get(), 1), "producer initialization signal"); producer4->Flush();
    hr(queue->Wait(initialized12.Get(), 1), "synthetic input barriers wait actual D3D11 initialization");
    ID3D12CommandList* lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists);
    hr(queue->Signal(syntheticReady.Get(), 1), "synthetic input signal");
    const auto syntheticStart = GetTickCount64();
    while (syntheticReady->GetCompletedValue() < 1 && GetTickCount64() - syntheticStart < 3000) Sleep(1);
    check(syntheticReady->GetCompletedValue() == 1, "synthetic inputs ready before admission stub");
    for (unsigned route = 0; route < 6; ++route) {
        const uint64_t sequence = route + 2;
        admission = route == 1 ? FrameGenerationNgxCompletion::Suppressed :
            route == 2 ? FrameGenerationNgxCompletion::FailedEvaluation : FrameGenerationNgxCompletion::Ready;
        const unsigned multiplier = route == 5 ? 3 : route >= 3 ? 2 : 4;
        check(interop.begin(runtime.producerContext.Get(), sequence, 1), "begin immutable stereo source");
        std::array<std::array<ComPtr<ID3D11Texture2D>, 5>, 2> sources;
        for (unsigned e = 0; e < 2; ++e) {
            std::vector<Pixel> colors(width * height), layers(width * height), mults(width * height, multiply(e));
            for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
                colors[y * width + x] = world(0, e, x, y); layers[y * width + x] = layer(e, x, y);
                mults[y*width+x]=multiplyAt(e,x);
                if(route==4&&e==1){layers[y*width+x]={0,0,0,255};mults[y*width+x]={255,255,255,255};}
            }
            std::vector<float> depth(width * height, .5f); std::vector<uint32_t> motion(width * height, 0);
            sources[e][0] = image(runtime.producer.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, colors.data(), width * sizeof(Pixel));
            sources[e][1] = image(runtime.producer.Get(), DXGI_FORMAT_R32_FLOAT, depth.data(), width * sizeof(float));
            sources[e][2] = image(runtime.producer.Get(), DXGI_FORMAT_R16G16_FLOAT, motion.data(), width * sizeof(uint32_t));
            sources[e][3] = image(runtime.producer.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, layers.data(), width * sizeof(Pixel));
            sources[e][4] = image(runtime.producer.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, mults.data(), width * sizeof(Pixel));
            for (unsigned p = 0; p < 5; ++p) check(interop.capture(runtime.producerContext.Get(), sequence, 1, e,
                FrameGenerationPlane(p), sources[e][p].Get(), sequence * 100 + e * 10 + p + 1), "capture actual immutable plane");
        }
        auto captured = interop.publish(runtime.producerContext.Get(), sequence, 1, 31);
        check(bool(captured), "publish exact paired source");
        hr(queue->Wait(captured->producerFence(), captured->producerValue()), "synthetic backend completion covers current immutable producer");
        hr(queue->Signal(syntheticReady.Get(), sequence), "synthetic backend source completion signal");
        const auto sourceStart = GetTickCount64();
        while (syntheticReady->GetCompletedValue() < sequence && GetTickCount64() - sourceStart < 3000) Sleep(1);
        check(syntheticReady->GetCompletedValue() == sequence, "synthetic source completion is actually fenced");
        FrameGenerationNgxTicket synthetic;
        synthetic.sequence = sequence; synthetic.generatedCount = multiplier - 1; synthetic.evaluationSucceeded = route != 2;
        synthetic.completionFence = syntheticReady.Get(); synthetic.completionValue = sequence;
        auto syntheticOwner = std::make_shared<SyntheticOwner>(); syntheticOwner->fence = syntheticReady;
        for (unsigned e = 0; e < 2; ++e) for (unsigned p = 0; p < 3; ++p) syntheticOwner->resources[e][p] = generated[e][p].privateResource;
        synthetic.lease = std::shared_ptr<FrameGenerationNgxLease>(syntheticOwner,
            reinterpret_cast<FrameGenerationNgxLease*>(syntheticOwner.get()));
        for (unsigned e = 0; e < 2; ++e) for (unsigned p = 0; p < multiplier - 1; ++p) synthetic.generated[e][p] = generated[e][p].privateResource.Get();
        FrameGenerationOutputSource source; source.previous = metadata(sequence - 1); source.current = metadata(sequence);
        source.historyGeneration = 1; source.multiplier = multiplier;
        FrameGenerationOutputUi ui[2]; ui[0].useMult = true;
        ui[1].empty=route==4;
        auto ticket = output->submit(captured, synthetic, ui, source);
        check(ticket.accepted && !ticket.unsafe && ticket.finalFence && ticket.finalValue, "actual output command submission and final fence");
        check(captured->retire(ticket.finalFence, ticket.finalValue), "source retires ONCE after final HUD fence, never NGX-only");
        EdvrFgBatch batch{}; batch.size = sizeof(batch); batch.version = EDVR_FG_BATCH_VERSION_3;
        HRESULT acquired = S_FALSE; const auto started = GetTickCount64();
        while (acquired == S_FALSE && GetTickCount64() - started < 3000) { acquired = ticket.acquireBatch(&batch); if (acquired == S_FALSE) Sleep(1); }
        hr(acquired, "acquire actually fenced output"); check(acquired == S_OK && batch.token, "one retained ABI batch transferred");
        check(ticket.acquireBatch(&batch) == S_FALSE, "batch cannot transfer a second initial token");
        const bool hasGenerated = route == 0 || route >= 3;
        check(batch.uiFlags==(hasGenerated?EDVR_FG_UI_NATIVE_COVERAGE:0u),"only generated batches carry independent native-pose UI");
        check(batch.phaseMask == (hasGenerated ? (1u << multiplier) - 1 : 1u), "suppression/error never invents generated readiness");
        check(batch.generatedCount == (hasGenerated ? multiplier - 1 : 0u), "native-only batches advertise zero available generated frames");
        if (multiplier == 3) check(!batch.textures[3][0] && !batch.textures[3][1],
            "3x publishes both intermediate stereo phases without a fourth phase");
        check(!std::memcmp(batch.currentViews, source.current.views, sizeof(batch.currentViews)) &&
            batch.currentPoseDisplayTime == source.current.poseDisplayTime, "physical endpoint pose metadata copied exactly");
        ComPtr<ID3D11Fence> ready; hr(consumer5->OpenSharedFence(batch.readyFence, IID_PPV_ARGS(&ready)), "XR-style shared ready fence import");
        check(ready->GetCompletedValue() >= batch.readyValue, "phase mask exposed only after actual ready completion");
        Gate gate; Handle gateHandle; ComPtr<ID3D11Fence> gate11;
        hr(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate.fence)), "consumer gate fence");
        keep(gate.fence.Get());
        hr(device12->CreateSharedHandle(gate.fence.Get(), nullptr, GENERIC_ALL, nullptr, &gateHandle.value), "consumer gate NT handle");
        hr(consumer5->OpenSharedFence(gateHandle.value, IID_PPV_ARGS(&gate11)), "consumer gate import");
        hr(context4->Wait(gate11.Get(), 1), "private consumer queue blocked for early-reuse falsifier");
        hr(context4->Wait(ready.Get(), batch.readyValue), "private consumer waits actual publication fence");
        std::array<std::array<ComPtr<ID3D11Texture2D>, 2>, 6> staging;
        const unsigned phases = hasGenerated ? multiplier : 1;
        for (unsigned p = 0; p < 6; ++p) for (unsigned e = 0; e < 2; ++e) {
            if(p>=phases&&!(hasGenerated&&p>=4))continue;
            ComPtr<ID3D11Texture2D> imported;
            const HANDLE image=p==5?batch.nativeWorldTextures[e]:p==4?batch.uiTextures[e]:batch.textures[p][e];
            hr(consumer1->OpenSharedResource1(image, IID_PPV_ARGS(&imported)), "same-adapter output texture import");
            keep(imported.Get());
            D3D11_TEXTURE2D_DESC desc{}; imported->GetDesc(&desc);
            check(desc.Width == width && desc.Height == height && desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                desc.SampleDesc.Count == 1 && desc.ArraySize == 1 && desc.MipLevels == 1, "full-eye typed UNORM import, no sRGB decode");
            desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            hr(runtime.consumer->CreateTexture2D(&desc, nullptr, &staging[p][e]), "consumer-owned readback texture");
            keep(staging[p][e].Get());
            runtime.consumerContext->CopyResource(staging[p][e].Get(), imported.Get());
        }
        hr(context4->Signal(retired.Get(), sequence), "consumer completion follows ALL phase reads"); context4->Flush();
        hr(frameGenerationOutputRetire(batch.token, retiredHandle.value, sequence), "production batch retirement callback imports consumer fence");
        check(frameGenerationOutputDiscard(batch.token) == E_UNEXPECTED, "retired batch cannot be discarded");
        frameGenerationOutputRelease(batch.token); batch.token = nullptr;
        ticket = {}; captured.reset();
        check(!output->drain(0), "release does not permit reuse while consumer GPU is blocked");
        hr(gate.fence->Signal(1), "release private consumer gate on bounded path");
        check(wait(retired.Get(), sequence), "bounded consumer completion");
        std::printf("route=%u consumerFenceTarget=%llu consumerFenceCompleted=%llu\n", route,
            static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(retired->GetCompletedValue()));
        const auto readbackStarted = GetTickCount64(); unsigned readbackRetries = 0;
        for (unsigned p = 0; p < 6; ++p) for (unsigned e = 0; e < 2; ++e) {
            if(p>=phases&&!(hasGenerated&&p>=4))continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT mappedResult = DXGI_ERROR_WAS_STILL_DRAWING;
            do {
                mappedResult = runtime.consumerContext->Map(staging[p][e].Get(), 0, D3D11_MAP_READ,
                    D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
                if (mappedResult != DXGI_ERROR_WAS_STILL_DRAWING) break;
                ++readbackRetries;
                if (GetTickCount64() - readbackStarted >= 3000) break;
                Sleep(1);
            } while (true);
            hr(mappedResult, "bounded fenced nonblocking pixel readback");
            for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
                const auto expectedWorld = world(p>=4?0:p, e, x, y);
                const bool emptyUi=route==4&&e==1;
                const auto expectedLayer = emptyUi?Pixel{0,0,0,255}:layer(e, x, y); const auto mult = multiplyAt(e,x);
                const auto* actual = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                for (unsigned c = 0; c < 3; ++c) {
                    float result = expectedLayer[c] + expectedWorld[c] * (expectedLayer[3] / 255.f) * (e == 0 ? mult[c] / 255.f : 1.f);
                    if((p>0&&p<4)||p==5)result=expectedWorld[c];
                    if(p==4&&(x<width/4||emptyUi))result=0;
                    check(std::abs(int(actual[c]) - int(std::lround(result))) <= 1,
                        p==5?"raw native world supports matching HUD composition":p==4?"native-pose UI coverage retains exact production color":p?"generated world contains no native HUD":"native endpoint preserves original UI blend");
                }
                check(actual[3] == (p==4?((x<width/4||emptyUi)?0:255):expectedWorld[3]), "native UI footprint alpha is opaque or transparent; world alpha is preserved");
            }
            runtime.consumerContext->Unmap(staging[p][e].Get(), 0);
        }
        check(output->collect() && output->drain(0), "final fence + consumer completion + released leases permit safe drain");
        std::printf("route=%u multiplier=%u phaseMask=%u stereoPhases=%u mapBusyRetries=%u HUD=exact-production-shader syntheticNGXAdmission=1 PASS\n",
            route, multiplier, batch.phaseMask, phases, readbackRetries);
    }
    check(output->drain(2000), "final bounded drain"); delete output; delete keepAlive;
}
} // namespace
namespace edvr {
FrameGenerationNgxCompletion FrameGenerationNgxTicket::poll() const noexcept { return admission; }
}
int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--gpu")) {
        std::puts("DRY: use --gpu explicitly for real NVIDIA output/HUD/shared-consumer correctness; synthetic NGX admission only.");
        return argc == 1 || (argc == 2 && !std::strcmp(argv[1], "--self-test")) ? 0 : 2;
    }
    try { run(); std::printf("FG output GPU correctness PASS checks=%u NGXEvaluations=0 XR=0\n", checks); return 0; }
    catch (const std::exception& error) { std::printf("FG output GPU correctness FAIL checks=%u reason=%s\n", checks, error.what()); return 1; }
}

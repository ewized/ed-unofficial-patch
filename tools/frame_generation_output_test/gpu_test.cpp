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
#include <thread>
#include <utility>
#include <vector>

using namespace edvr;
using Microsoft::WRL::ComPtr;
namespace {
FrameGenerationNgxCompletion admission = FrameGenerationNgxCompletion::Ready;
unsigned checks = 0;
constexpr unsigned width = 64, height = 32;
constexpr unsigned supersampledWidth = width * 3 / 2, supersampledHeight = height * 3 / 2;
constexpr unsigned hudEdge = width / 4, supersampledHudEdge = hudEdge * 3 / 2 + 1;
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
constexpr Pixel emptyLayer{0, 0, 0, 255}, emptyMultiply{255, 255, 255, 255};
// Cover a supersampled HUD with multiply on the left, an empty right eye,
// then a supersampled HUD with a scene-sized neutral multiply on the right.
bool supersampledHud(unsigned route, unsigned eye) {
    return (route == 4 && eye == 0) || (route == 5 && eye == 1);
}
unsigned hudBoundary(bool supersampled) { return supersampled ? supersampledHudEdge : hudEdge; }
Pixel layer(unsigned eye, unsigned x, unsigned y, bool supersampled) {
    if (x < hudBoundary(supersampled)) return emptyLayer;
    return Pixel{static_cast<unsigned char>(12 + eye * 8 + x % 8), static_cast<unsigned char>(5 + y % 4),
        static_cast<unsigned char>(3 + eye), static_cast<unsigned char>(128 + eye * 16)};
}
Pixel multiply(unsigned eye) { return Pixel{static_cast<unsigned char>(128 + eye * 32), 255, 64, 255}; }
Pixel multiplyAt(unsigned eye, unsigned x, bool supersampled) {
    return x < hudBoundary(supersampled) ? emptyMultiply : multiply(eye);
}
using FilteredPixel = std::array<float, 4>;
// At 150%, each output pixel straddles two source texels per axis. The
// full and half texel swap weights every pixel, including at the HUD edge.
template<class Sample>
FilteredPixel filterHud(unsigned x, unsigned y, bool supersampled, Sample sample) {
    if (!supersampled) {
        const auto pixel = sample(x, y);
        return {float(pixel[0]), float(pixel[1]), float(pixel[2]), float(pixel[3])};
    }
    const unsigned firstX = x * 3 / 2, firstY = y * 3 / 2;
    const unsigned xWeight[] = {2 - x % 2, 1 + x % 2}, yWeight[] = {2 - y % 2, 1 + y % 2};
    FilteredPixel filtered{};
    for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx) {
        const auto pixel = sample(firstX + dx, firstY + dy);
        for (unsigned c = 0; c < pixel.size(); ++c) filtered[c] += float(pixel[c] * xWeight[dx] * yWeight[dy]);
    }
    for (auto& channel : filtered) channel /= 9.f;
    return filtered;
}
ComPtr<ID3D11Texture2D> image(ID3D11Device* device, DXGI_FORMAT format, const void* pixels, UINT pitch,
                             bool shared = false, unsigned imageWidth = width, unsigned imageHeight = height) {
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = imageWidth; desc.Height = imageHeight;
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
void checkRecomposition(FrameGenerationOutput& output, ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12Fence* producerGate, const std::shared_ptr<FrameGenerationInteropLease>& captured,
    const FrameGenerationOutputUi (&ui)[2], const FrameGenerationOutputSource& source,
    std::vector<ComPtr<IUnknown>>& keepAlive) {
    const auto initialOwners = captured.use_count();
    const auto initialStatus = output.status();
    check(captured->producerFence()->GetCompletedValue() < captured->producerValue(),
        "the real producer queue is blocked ahead of source publication");
    FrameGenerationRecompositionTicket tickets[3];
    for (auto& ticket : tickets) {
        ticket = output.prepareRecomposition(captured, ui, source);
        check(ticket.accepted && !ticket.unsafe && ticket.lease && ticket.readyFence && ticket.readyValue,
            "private recomposition prepares both eyes behind a GPU fence");
        check(ticket.readyFence->GetCompletedValue() < ticket.readyValue,
            "private readiness cannot outrun the unsignalled input producer");
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& surfaces = ticket.eyes[eye];
            check(surfaces.color && surfaces.ui && surfaces.color != surfaces.ui &&
                surfaces.color != surfaces.hudless && surfaces.ui != surfaces.hudless &&
                surfaces.hudless == captured->plane(eye, FrameGenerationPlane::Color)->resource12,
                "native RGBA and UI are private; HUD-less aliases the immutable source");
        }
    }
    check(captured.use_count() == initialOwners + 3, "each preparation owns its source through downstream reads");
    auto mismatched = source;
    ++mismatched.historyGeneration;
    const auto badHistory = output.prepareRecomposition(captured, ui, mismatched);
    check(!badHistory.accepted && !badHistory.lease &&
        output.status().failure == FrameGenerationOutputFailure::InvalidMetadata,
        "history-mismatched sources cannot enter the private preparation ring");
    bool wrongOwnerAccepted = true;
    std::thread foreign([&] { wrongOwnerAccepted = output.prepareRecomposition(captured, ui, source).accepted; });
    foreign.join();
    check(!wrongOwnerAccepted && output.status().failure == FrameGenerationOutputFailure::WrongThread,
        "preparation uses the existing output-worker owner");
    check(!output.prepareRecomposition(captured, ui, source).accepted &&
        output.status().failure == FrameGenerationOutputFailure::Busy,
        "pending or externally held preparations backpressure the bounded ring");

    // Model NGX refusing before queueing anything. Dropping its planned
    // input must leave the original source pinned until the prep fence.
    auto* abandonedFence = tickets[2].readyFence;
    const auto abandonedValue = tickets[2].readyValue;
    tickets[2] = {};
    check(output.collect() && captured.use_count() == initialOwners + 3 &&
        output.status().recompositionSubmitted == initialStatus.recompositionSubmitted + 3 &&
        output.status().recompositionRetired == initialStatus.recompositionRetired,
        "an accepted prep without NGX stays outstanding until its GPU fence");
    hr(producerGate->Signal(1), "release the real source producer queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> consumer;
    hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "recomposition reader allocator");
    hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
        "separate recomposition consumer list");
    hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumer)), "recomposition consumer completion");
    ComPtr<ID3D12Resource> readback;
    constexpr UINT rowPitch = (width * sizeof(Pixel) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    constexpr UINT imageStride = (rowPitch * height + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
        ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readDesc{};
    readDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; readDesc.Width = 6 * imageStride;
    readDesc.Height = readDesc.DepthOrArraySize = readDesc.MipLevels = 1;
    readDesc.SampleDesc.Count = 1;
    readDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "private recomposition readback");
    keepAlive.emplace_back(allocator.Get()); keepAlive.emplace_back(list.Get());
    keepAlive.emplace_back(consumer.Get()); keepAlive.emplace_back(readback.Get());
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto& surfaces = tickets[0].eyes[eye];
        ID3D12Resource* planes[] = {surfaces.color, surfaces.hudless, surfaces.ui};
        for (unsigned p = 0; p < 3; ++p) {
            auto* plane = planes[p];
            keepAlive.emplace_back(plane);
            transition(list.Get(), plane, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = {UINT64((eye * 3 + p) * imageStride), {DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, rowPitch}};
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = plane; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            transition(list.Get(), plane, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
    }
    hr(list->Close(), "recomposition reader list close");
    hr(queue->Wait(abandonedFence, abandonedValue), "downstream queue waits every private preparation");
    ID3D12CommandList* lists[] = {list.Get()}; queue->ExecuteCommandLists(1, lists);
    hr(queue->Signal(consumer.Get(), 1), "downstream readback fence");
    const auto started = GetTickCount64();
    while (consumer->GetCompletedValue() == 0 && GetTickCount64() - started < 3000) Sleep(1);
    check(consumer->GetCompletedValue() == 1 && output.collect() && captured.use_count() == initialOwners + 2 &&
        output.status().recompositionRetired == initialStatus.recompositionRetired + 1,
        "fenced prep without NGX retires while consumer-owned source pins remain");
    check(!output.drain(0) && output.status().failure == FrameGenerationOutputFailure::OutstandingLease,
        "completed preparations with downstream owners block terminal drain");
    void* opaque = nullptr;
    const D3D12_RANGE reads{0, SIZE_T(readDesc.Width)};
    hr(readback->Map(0, &reads, &opaque), "map fenced private RGB planes");
    const auto* data = static_cast<const unsigned char*>(opaque);
    for (unsigned eye = 0; eye < 2; ++eye) for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const auto* color = data + eye * 3 * imageStride + y * rowPitch + x * sizeof(Pixel);
        const auto* hudless = color + imageStride;
        const auto* coverage = hudless + imageStride;
        const Pixel scene = world(0, eye, x, y);
        const auto nativeHud = filterHud(x, y, eye == 0, [&](unsigned sx, unsigned sy) {
            return eye == 0 ? layer(eye, sx, sy, true) : emptyLayer;
        });
        const auto mult = filterHud(x, y, eye == 0, [&](unsigned sx, unsigned) {
            return eye == 0 ? multiplyAt(eye, sx, true) : emptyMultiply;
        });
        const bool covered = nativeHud != FilteredPixel{0, 0, 0, 255} || mult != FilteredPixel{255, 255, 255, 255};
        check(std::memcmp(hudless, scene.data(), sizeof(scene)) == 0, "borrowed HUD-less is the exact stereo world");
        for (unsigned c = 0; c < 3; ++c) {
            const float expected = nativeHud[c] + scene[c] * nativeHud[3] * mult[c] / (255.f * 255.f);
            check(std::abs(int(color[c]) - int(std::lround(expected))) <= 1, "full native color uses the shared supersampled blend");
            check(coverage[c] == (covered ? color[c] : 0),
                "opaque coverage preserves exact native blend and translucent backing");
        }
        check(color[3] == scene[3] && coverage[3] == (covered ? 255 : 0),
            "native alpha preserved, opaque on covered texels and exact transparent peer");
    }
    const D3D12_RANGE noWrites{}; readback->Unmap(0, &noWrites);
    for (auto& ticket : tickets) ticket = {};
    check(output.collect() && captured.use_count() == initialOwners && output.drain(0) &&
        output.status().recompositionRetired == initialStatus.recompositionRetired + 3 &&
        output.status().submitted == initialStatus.submitted,
        "released consumers unpin the original source and allow complete drain");
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
    uint64_t previousAllocation=0;
    for (unsigned route = 0; route < 11; ++route) {
        const uint64_t sequence = route + 2;
        admission = route == 1 ? FrameGenerationNgxCompletion::Suppressed :
            route == 2 ? FrameGenerationNgxCompletion::FailedEvaluation :
            route == 9 ? FrameGenerationNgxCompletion::Stale : FrameGenerationNgxCompletion::Ready;
        const unsigned multiplier = route == 5 ? 3 : route >= 6 ? 4 : route >= 3 ? 2 : 4;
        const bool nativeOnly=route==0||route==8;
        const bool recomposed = route == 10;
        Gate producerGate;
        ComPtr<ID3D11Fence> producerGate11;
        Handle producerGateHandle;
        if (route == 4) {
            hr(producer5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&producerGate11)), "real producer gate");
            hr(producerGate11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &producerGateHandle.value),
                "producer gate NT handle");
            hr(device12->OpenSharedHandle(producerGateHandle.value, IID_PPV_ARGS(&producerGate.fence)),
                "same-adapter producer gate control");
            hr(producer4->Wait(producerGate11.Get(), 1), "pause immutable source producer before capture");
        }
        check(interop.begin(runtime.producerContext.Get(), sequence, 1), "begin immutable stereo source");
        std::array<std::array<ComPtr<ID3D11Texture2D>, 5>, 2> sources;
        for (unsigned e = 0; e < 2; ++e) {
            const bool supersampled = supersampledHud(route, e), empty = route == 4 && e == 1;
            const unsigned hudWidth = supersampled ? supersampledWidth : width;
            const unsigned hudHeight = supersampled ? supersampledHeight : height;
            const unsigned multWidth = e == 0 ? hudWidth : width;
            const unsigned multHeight = e == 0 ? hudHeight : height;
            std::vector<Pixel> colors(width * height), layers(hudWidth * hudHeight);
            std::vector<Pixel> mults(multWidth * multHeight, emptyMultiply);
            for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
                colors[y * width + x] = world(0, e, x, y);
            for (unsigned y = 0; y < hudHeight; ++y) for (unsigned x = 0; x < hudWidth; ++x)
                layers[y * hudWidth + x] = empty ? emptyLayer : layer(e, x, y, supersampled);
            if (e == 0) for (unsigned y = 0; y < multHeight; ++y) for (unsigned x = 0; x < multWidth; ++x)
                mults[y * multWidth + x] = multiplyAt(e, x, supersampled);
            std::vector<float> depth(width * height, .5f); std::vector<uint32_t> motion(width * height, 0);
            sources[e][0] = image(runtime.producer.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, colors.data(), width * sizeof(Pixel));
            sources[e][1] = image(runtime.producer.Get(), DXGI_FORMAT_R32_FLOAT, depth.data(), width * sizeof(float));
            sources[e][2] = image(runtime.producer.Get(), DXGI_FORMAT_R16G16_FLOAT, motion.data(), width * sizeof(uint32_t));
            sources[e][3] = image(runtime.producer.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, layers.data(),
                hudWidth * sizeof(Pixel), false, hudWidth, hudHeight);
            sources[e][4] = image(runtime.producer.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, mults.data(),
                multWidth * sizeof(Pixel), false, multWidth, multHeight);
            for (unsigned p = 0; p < 5; ++p) check(interop.capture(runtime.producerContext.Get(), sequence, 1, e,
                FrameGenerationPlane(p), sources[e][p].Get(), sequence * 100 + e * 10 + p + 1), "capture actual immutable plane");
        }
        auto captured = interop.publish(runtime.producerContext.Get(), sequence, 1, 31);
        check(bool(captured), "publish exact paired source");
        FrameGenerationOutputSource source; source.previous = metadata(sequence - 1); source.current = metadata(sequence);
        source.historyGeneration = 1; source.multiplier = multiplier;
        if (recomposed)
            for (auto& eye : source.previous.views) eye.position[0] -= .03f;
        FrameGenerationOutputUi ui[2]; ui[0].useMult = true;
        ui[1].empty=route==4;
        if(route==0)source.previous={}; // First native seed has no publishable generated history.
        if (route == 4) checkRecomposition(*output, device12, queue.Get(), producerGate.fence.Get(), captured, ui, source, *keepAlive);
        hr(queue->Wait(captured->producerFence(), captured->producerValue()), "synthetic backend completion covers current immutable producer");
        hr(queue->Signal(syntheticReady.Get(), sequence), "synthetic backend source completion signal");
        const auto sourceStart = GetTickCount64();
        while (syntheticReady->GetCompletedValue() < sequence && GetTickCount64() - sourceStart < 3000) Sleep(1);
        check(syntheticReady->GetCompletedValue() == sequence, "synthetic source completion is actually fenced");
        FrameGenerationNgxTicket synthetic;
        synthetic.sequence = sequence; synthetic.generatedCount = multiplier - 1; synthetic.evaluationSucceeded = route != 2;
        synthetic.uiRecomposed = recomposed;
        synthetic.completionFence = syntheticReady.Get(); synthetic.completionValue = sequence;
        auto syntheticOwner = std::make_shared<SyntheticOwner>(); syntheticOwner->fence = syntheticReady;
        for (unsigned e = 0; e < 2; ++e) for (unsigned p = 0; p < 3; ++p) syntheticOwner->resources[e][p] = generated[e][p].privateResource;
        synthetic.lease = std::shared_ptr<FrameGenerationNgxLease>(syntheticOwner,
            reinterpret_cast<FrameGenerationNgxLease*>(syntheticOwner.get()));
        for (unsigned e = 0; e < 2; ++e) for (unsigned p = 0; p < multiplier - 1; ++p) synthetic.generated[e][p] = generated[e][p].privateResource.Get();
        if (route == 4 || route == 5) {
            FrameGenerationOutputUi mismatched[] = {ui[0], ui[1]};
            const unsigned eye = route == 4 ? 0 : 1;
            mismatched[eye].useMult = !mismatched[eye].useMult;
            const auto rejected = output->submit(captured, synthetic, mismatched, source, nativeOnly);
            check(!rejected.accepted && output->status().failure == FrameGenerationOutputFailure::InvalidResource &&
                !output->status().failed, "multiply extent must match the active HUD or neutral scene contract");
        }
        auto ticket = output->submit(captured, synthetic, ui, source,nativeOnly);
        if(route==9) {
            check(!ticket.accepted&&!ticket.unsafe&&output->status().failure==FrameGenerationOutputFailure::Stale&&
                !output->status().failed,"late fenced stale observation does not publish old pixels or terminally fault the publisher");
            check(captured->retire(synthetic.completionFence,synthetic.completionValue),
                "caller retires rejected stale input through its real completed backend fence");
            captured.reset();continue;
        }
        check(ticket.accepted && !ticket.unsafe && ticket.finalFence && ticket.finalValue, "actual output command submission and final fence");
        check(captured->retire(ticket.finalFence, ticket.finalValue), "source retires ONCE after final HUD fence, never NGX-only");
        EdvrFgBatch batch{};batch.version=route==7?EDVR_FG_BATCH_VERSION_3:route==8?EDVR_FG_BATCH_VERSION_2:EDVR_FG_BATCH_VERSION_4;
        batch.size=edvrFgBatchSize(batch.version);
        constexpr uint64_t suffixSentinel=0x5a5a5a5a5a5a5a5aull;
        if(batch.version!=EDVR_FG_BATCH_VERSION_4)batch.allocationId=suffixSentinel;
        if(batch.version==EDVR_FG_BATCH_VERSION_2){batch.nativeWorldTextures[0]=HANDLE(uintptr_t(13));batch.nativeWorldTextures[1]=HANDLE(uintptr_t(14));}
        auto pending = ticket;
        ++pending.finalValue; // The next output submission has not been queued.
        check(pending.exportStatus() == S_FALSE && pending.acquireBatch(&batch) == S_FALSE && !batch.token,
            "an unsignaled final fence keeps both readiness and token transfer pending");
        pending = {};
        HRESULT exportReady = S_FALSE; const auto started = GetTickCount64();
        while (exportReady == S_FALSE && GetTickCount64() - started < 3000) { exportReady = ticket.exportStatus(); if (exportReady == S_FALSE) Sleep(1); }
        check(exportReady == S_OK && ticket.exportStatus() == S_OK && !batch.token, "repeated readiness polls leave the batch available");
        const HRESULT acquired = ticket.acquireBatch(&batch);
        hr(acquired, "acquire actually fenced output"); check(acquired == S_OK && batch.token, "one retained ABI batch transferred");
        check(ticket.exportStatus() == S_FALSE && ticket.acquireBatch(&batch) == S_FALSE, "transferred batch cannot advertise readiness or a second initial token");
        const bool hasGenerated = !nativeOnly&&(route == 0 || route >= 3);
        const bool hasSeparateUi = hasGenerated && !recomposed;
        check(ticket.generatedCount==(hasGenerated?multiplier-1:0),"internal ticket counts exactly the phases actually submitted");
        if(batch.version==EDVR_FG_BATCH_VERSION_4) {
            check(batch.allocationId!=0,"v4 exports a nonzero allocation-set incarnation");
            if(route==0||route==3||route==5||route==6)
                check(batch.allocationId>previousAllocation,"later generated/UI texture allocation refreshes the whole set identity");
            else check(batch.allocationId==previousAllocation,"plain output slot reuse preserves allocation identity");
            previousAllocation=batch.allocationId;
        } else {
            check(batch.allocationId==suffixSentinel,"legacy export does not write the appended v4 field");
            if(batch.version==EDVR_FG_BATCH_VERSION_2) {
                check(batch.nativeWorldTextures[0]==HANDLE(uintptr_t(13))&&batch.nativeWorldTextures[1]==HANDLE(uintptr_t(14)),
                    "v2 export does not write the v3 suffix");
                batch.nativeWorldTextures[0]=batch.nativeWorldTextures[1]=nullptr;
            }
            batch.allocationId=0;
        }
        check(batch.uiFlags == (hasSeparateUi ? EDVR_FG_UI_NATIVE_COVERAGE : 0u),
            "recomposed generated eyes must not be advertised with a second native-pose HUD");
        if (recomposed) for (unsigned eye = 0; eye < 2; ++eye)
            check(!batch.uiTextures[eye] && !batch.nativeWorldTextures[eye],
                "recomposed submission exports only its final stereo phases");
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
            if(p>=phases&&!(hasSeparateUi&&p>=4))continue;
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
            if(p>=phases&&!(hasSeparateUi&&p>=4))continue;
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
                const bool supersampled = supersampledHud(route, e), empty = route == 4 && e == 1;
                const auto expectedLayer = filterHud(x, y, supersampled, [&](unsigned sx, unsigned sy) {
                    return empty ? emptyLayer : layer(e, sx, sy, supersampled);
                });
                const auto mult = filterHud(x, y, supersampled, [&](unsigned sx, unsigned) {
                    return e == 0 ? multiplyAt(e, sx, supersampled) : emptyMultiply;
                });
                const bool covered = expectedLayer != FilteredPixel{0, 0, 0, 255} ||
                    mult != FilteredPixel{255, 255, 255, 255};
                if (p == 4 && supersampled && x == hudEdge)
                    check(expectedLayer[3] > layer(e, supersampledHudEdge, 0, true)[3] && expectedLayer[3] < 255.f,
                        "native coverage edge crosses only one third of a scene texel");
                const auto* actual = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                for (unsigned c = 0; c < 3; ++c) {
                    float result = expectedLayer[c] + expectedWorld[c] * (expectedLayer[3] / 255.f) * (mult[c] / 255.f);
                    if((p>0&&p<4)||p==5)result=expectedWorld[c];
                    if (p == 4 && !covered) result = 0;
                    check(std::abs(int(actual[c]) - int(std::lround(result))) <= 1,
                        p==5?"raw native world supports matching HUD composition":p==4?"native-pose UI coverage retains exact production color":
                        p ? (recomposed ? "final recomposed phase retains the backend pixels" : "generated world contains no native HUD")
                          : "native endpoint preserves original UI blend");
                }
                check(actual[3] == (p == 4 ? (covered ? 255 : 0) : expectedWorld[3]),
                    "native UI footprint alpha is opaque or transparent; world alpha is preserved");
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

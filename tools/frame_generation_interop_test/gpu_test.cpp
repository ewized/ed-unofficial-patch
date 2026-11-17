// No D3D imports or runtime calls on the default CPU path. --gpu explicitly
// opts into a standalone NVIDIA same-adapter D3D11/D3D12 correctness fixture.
#include "../../src/d3d11/frame_generation_interop.h"
#include "../../src/d3d11/frame_generation_color.h"
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace edvr;
using Lease = std::shared_ptr<FrameGenerationInteropLease>;
namespace {
unsigned checks = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) throw std::runtime_error(label);
}
void hr(HRESULT value, const char* label) {
    if (FAILED(value)) std::printf("HRESULT %s=0x%08lx\n", label, static_cast<unsigned long>(value));
    check(SUCCEEDED(value), label);
}
void cpu() {
    check(!GetModuleHandleW(L"d3d11.dll") && !GetModuleHandleW(L"d3d12.dll") && !GetModuleHandleW(L"dxgi.dll"),
          "CPU entry has no D3D11/D3D12/DXGI modules loaded");
    FrameGenerationInterop adapter;
    FrameGenerationInteropInit config;
    config.device11 = reinterpret_cast<ID3D11Device*>(std::uintptr_t{1});
    config.context = reinterpret_cast<ID3D11DeviceContext*>(std::uintptr_t{2});
    check(!adapter.initialize(config) && adapter.status().failure == FrameGenerationInteropFailure::Off,
          "actual adapter OFF does not dereference poison COM pointers");
    check(!adapter.begin(config.context, 1, 1) && !adapter.device12(), "OFF cannot start an interop transaction");
    FrameGenerationColor color;
    ID3D11Texture2D* normalized = reinterpret_cast<ID3D11Texture2D*>(std::uintptr_t{3});
    check(!color.initialize(false, config.device11, config.context, 1) &&
          !color.normalize(config.context, 0, normalized, &normalized) && !normalized,
          "production color OFF and uninitialized normalization do not query poison COM pointers");
    check(!GetModuleHandleW(L"d3dcompiler_47.dll"), "CPU color path does not load shader compiler");
    check(!GetModuleHandleW(L"d3d11.dll") && !GetModuleHandleW(L"d3d12.dll") && !GetModuleHandleW(L"dxgi.dll"),
          "CPU actual-adapter path loaded no graphics runtime or driver");
}

HMODULE systemModule(const wchar_t* leaf) {
    wchar_t path[32768]{};
    const UINT length = GetSystemDirectoryW(path, 32768);
    check(length && length < 32000, "Windows System32 directory");
    check(!wcscat_s(path, L"\\") && !wcscat_s(path, leaf), "bounded System32 runtime path");
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    wchar_t actual[32768]{};
    const DWORD actualLength = module ? GetModuleFileNameW(module, actual, 32768) : 0;
    check(actualLength && actualLength < 32768 && !_wcsicmp(path, actual), "verified System32 graphics runtime");
    // Retained until process exit, including any quarantined COM objects.
    return module;
}
struct Runtime {
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    using Create11 = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    Create11 create11 = nullptr;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Runtime() {
        auto factory = reinterpret_cast<CreateFactory>(GetProcAddress(systemModule(L"dxgi.dll"), "CreateDXGIFactory1"));
        create11 = reinterpret_cast<Create11>(GetProcAddress(systemModule(L"d3d11.dll"), "D3D11CreateDevice"));
        check(factory && create11, "required System32 graphics entry points");
        ComPtr<IDXGIFactory1> dxgi;
        hr(factory(IID_PPV_ARGS(&dxgi)), "standalone DXGI factory");
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            auto result = dxgi->EnumAdapters1(index, &candidate);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            hr(result, "enumerate adapter");
            DXGI_ADAPTER_DESC1 desc{}; hr(candidate->GetDesc1(&desc), "adapter descriptor");
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                adapter = candidate;
                std::printf("NVIDIA adapter LUID=%08lx:%08lx vendor=0x%04x device=0x%04x\n",
                    static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                    static_cast<unsigned long>(desc.AdapterLuid.LowPart), desc.VendorId, desc.DeviceId);
                break;
            }
        }
        check(bool(adapter), "NVIDIA hardware adapter required; no WARP or alternate-device fallback");
        makeDevice(device, context);
    }
    void makeDevice(ComPtr<ID3D11Device>& made, ComPtr<ID3D11DeviceContext>& ctx) {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        hr(create11(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            &level, 1, D3D11_SDK_VERSION, &made, nullptr, &ctx), "NVIDIA standalone D3D11 device");
    }
};

unsigned bytesPerPixel(DXGI_FORMAT format) { return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4; }
std::vector<unsigned char> pattern(unsigned width, unsigned height, DXGI_FORMAT format, std::uint64_t tag) {
    std::vector<unsigned char> out(static_cast<size_t>(width) * height * bytesPerPixel(format));
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<unsigned char>((i * 73 + tag * 19 + (i / 4) * 11) & 255);
    return out;
}
ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, DXGI_FORMAT format, unsigned width, unsigned height,
    const std::vector<unsigned char>& bytes) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format; desc.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial{bytes.data(), width * bytesPerPixel(format), 0};
    ComPtr<ID3D11Texture2D> out;
    hr(device->CreateTexture2D(&desc, &initial, &out), "source texture");
    return out;
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}
bool waitFence(ID3D12Fence* fence, std::uint64_t target, HANDLE event, DWORD timeout = 5000) noexcept {
    if (!fence || !target || !event) return false;
    const ULONGLONG deadline = GetTickCount64() + timeout;
    for (;;) {
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) return false;
        if (completed >= target) return true;
        const auto now = GetTickCount64();
        if (now >= deadline || FAILED(fence->SetEventOnCompletion(target, event))) return false;
        const auto remaining = static_cast<DWORD>(deadline - now);
        if (WaitForSingleObject(event, remaining) != WAIT_OBJECT_0) return false;
    }
}
struct Captured {
    Lease lease;
    std::array<std::vector<unsigned char>, 10> expected;
    std::array<std::uint64_t, 10> sourceIdentities{};
};
struct Copy {
    ComPtr<ID3D12Resource> source, readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    unsigned rows = 0;
    UINT64 rowBytes = 0;
    std::vector<unsigned char> expected;
};
struct Job {
    Lease lease;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::vector<Copy> copies;
    std::uint64_t completion = 0;
};
// Interop uses Context4 while normalization uses the base interface. Hook
// both per-object tables when QI returns distinct interface pointers.
struct CaptureCopySpy {
    using Copy = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
    struct Table { ID3D11DeviceContext* context = nullptr; void** original = nullptr; std::vector<void*> slots; };
    inline static CaptureCopySpy* active = nullptr;
    ComPtr<ID3D11DeviceContext4> context4;
    std::array<Table, 2> tables;
    unsigned copies = 0;
    static void STDMETHODCALLTYPE copy(ID3D11DeviceContext* c, ID3D11Resource* target, ID3D11Resource* source) {
        ++active->copies;
        for (const auto& table : active->tables) if (table.context == c) {
            reinterpret_cast<Copy>(table.original[47])(c, target, source); return;
        }
        std::terminate();
    }
    static void install(Table& table, void** slots) {
        DWORD previous = 0, ignored = 0;
        check(VirtualProtect(table.context, sizeof(void*), PAGE_READWRITE, &previous) != 0, "capture spy context writable");
        *reinterpret_cast<void***>(table.context) = slots;
        check(VirtualProtect(table.context, sizeof(void*), previous, &ignored) != 0, "capture spy protection restored");
    }
    void attach(unsigned index, ID3D11DeviceContext* context, size_t count) {
        auto& table = tables[index]; table.context = context; table.original = *reinterpret_cast<void***>(context);
        table.slots.assign(table.original, table.original + count); table.slots[47] = reinterpret_cast<void*>(&copy);
        install(table, table.slots.data());
    }
    explicit CaptureCopySpy(ID3D11DeviceContext* context) {
        check(!active, "one capture copy spy owner");
        hr(context->QueryInterface(IID_PPV_ARGS(&context4)), "capture spy queries actual Context4");
        active = this;
        // Context4 has 149 methods, including Signal and Wait. A distinct base
        // interface needs only its own 115 methods; never truncate Context4.
        if (context == static_cast<ID3D11DeviceContext*>(context4.Get())) attach(0, context, 149);
        else { attach(0, context, 115); attach(1, context4.Get(), 149); }
    }
    ~CaptureCopySpy() {
        for (auto& table : tables) if (table.context) install(table, table.original);
        active = nullptr;
    }
};

struct Gpu {
    Runtime runtime;
    std::unique_ptr<FrameGenerationInterop> adapter;
    FrameGenerationColor normalizer;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> completion, gate, producerGate;
    ComPtr<ID3D11Fence> producerGate11;
    HANDLE event = nullptr;
    std::uint64_t serial = 0;
    // Every submission retains its command storage and resources until a
    // bounded completion proof, or this entire owner is quarantined on failure.
    std::vector<std::unique_ptr<Job>> jobs;
    std::vector<ComPtr<ID3D11Texture2D>> sources;
    std::uint64_t nextSequence = 1;
    ~Gpu() { if (event) CloseHandle(event); }
    FrameGenerationInteropInit config() {
        FrameGenerationInteropInit out;
        out.enabled = true; out.device11 = runtime.device.Get(); out.context = runtime.context.Get();
        out.renderThreadId = GetCurrentThreadId(); out.colorWidth = 13; out.colorHeight = 9;
        out.guideWidth = 7; out.guideHeight = 5;
        return out;
    }
    void initialize() {
        adapter = std::make_unique<FrameGenerationInterop>();
        check(adapter->initialize(config()), "actual adapter initializes NVIDIA same-adapter shared fences/device");
        check(normalizer.initialize(true, runtime.device.Get(), runtime.context.Get(), GetCurrentThreadId()),
              "production color normalizer shares the authoritative render owner");
        device = adapter->device12();
        D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "standalone consumer queue");
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&completion)), "consumer completion fence");
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "CPU-releasable private-queue gate");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr); check(event != nullptr, "bounded completion event");
    }
    bool drain() noexcept {
        // Always release the private queue first, including every exception,
        // allocation failure and failed assertion after Queue::Wait.
        if (producerGate && FAILED(producerGate->Signal(1))) return false;
        if (gate && FAILED(gate->Signal(1))) return false;
        if (runtime.context) runtime.context->Flush();
        if (!queue) return true;
        if (!completion || !event) return false;
        const auto target = ++serial;
        return SUCCEEDED(queue->Signal(completion.Get(), target)) && waitFence(completion.Get(), target, event);
    }
    // Private shared fence blocks producer GPU execution without a CPU wait.
    // drain() always releases it before attempting any bounded cleanup.
    void blockProducer() {
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&producerGate)), "private producer gate fence");
        ComPtr<ID3D11Device5> device5;
        hr(runtime.device.As(&device5), "private producer gate Device5");
        HANDLE shared = nullptr;
        hr(device->CreateSharedHandle(producerGate.Get(), nullptr, GENERIC_ALL, nullptr, &shared), "share private producer gate");
        const HRESULT opened = device5->OpenSharedFence(shared, IID_PPV_ARGS(&producerGate11));
        CloseHandle(shared);
        hr(opened, "import private producer gate into D3D11");
        ComPtr<ID3D11DeviceContext4> context4;
        hr(runtime.context.As(&context4), "private producer gate Context4");
        hr(context4->Wait(producerGate11.Get(), 1), "enqueue private producer GPU wait");
    }
    void begin(std::uint64_t sequence) {
        const auto deadline = GetTickCount64() + 5000;
        while (!adapter->begin(runtime.context.Get(), sequence, 1)) {
            check(adapter->status().failure == FrameGenerationInteropFailure::Busy && GetTickCount64() < deadline,
                  "ring becomes reusable within bounded producer/consumer completion");
            Sleep(1);
        }
    }
    Captured capture(DXGI_FORMAT color, DXGI_FORMAT ui, bool normalizeColor = false,
        std::shared_ptr<const void> sourceOwner = {}, DXGI_FORMAT rightColor = DXGI_FORMAT_UNKNOWN) {
        Captured captured;
        const auto sequence = nextSequence++;
        begin(sequence);
        for (unsigned eye = 0; eye < 2; ++eye) for (unsigned role = 0; role < 5; ++role) {
            const auto plane = static_cast<FrameGenerationPlane>(role);
            const bool guide = role == 1 || role == 2;
            const unsigned width = guide ? 7 : 13, height = guide ? 5 : 9;
            const DXGI_FORMAT format = role == 1 ? DXGI_FORMAT_R32_FLOAT : role == 2 ? DXGI_FORMAT_R16G16_FLOAT : role ? ui : eye && rightColor != DXGI_FORMAT_UNKNOWN ? rightColor : color;
            const auto tag = sequence * 32 + eye * 8 + role + 1;
            const auto original = pattern(width, height, format, tag);
            auto source = texture(runtime.device.Get(), format, width, height, original);
            ID3D11Texture2D* prepared = source.Get();
            if (normalizeColor && !role) {
                CaptureCopySpy calls(runtime.context.Get());
                ID3D11Texture2D* normalized = nullptr;
                check(normalizer.normalize(runtime.context.Get(), eye, source.Get(), &normalized),
                      "production normalization prepares authoritative color");
                check(adapter->capture(runtime.context.Get(), sequence, 1, eye, plane, normalized, tag),
                      "production immutable capture immediately follows normalization");
                const bool borrowed = format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
                check(calls.copies == (borrowed ? 1u : 2u),
                      "UNORM/typeless normalize plus interop enqueues one copy; fallback enqueues two");
                check((normalized == source.Get()) == borrowed, "compatible RGBA8 retains original source identity through preparation");
                prepared = normalized;
                std::printf("color_capture format=%u eye=%u normalization_and_interop_copies=%u\n",
                    unsigned(format), eye, calls.copies);
            } else {
                check(adapter->capture(runtime.context.Get(), sequence, 1, eye, plane, source.Get(), tag,
                      role >= 3 ? sourceOwner : std::shared_ptr<const void>{}),
                      "actual capture copies every authoritative plane immediately");
            }
            ComPtr<IUnknown> preparedIdentity;
            hr(prepared->QueryInterface(IID_PPV_ARGS(&preparedIdentity)), "prepared Color/guide/UI COM identity");
            captured.sourceIdentities[eye * 5 + role] = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(preparedIdentity.Get()));
            // Same immediate context: the source update follows its snapshot
            // copy and precedes the one producer Signal/Flush in publish().
            const auto changed = pattern(width, height, format, tag + 77);
            check(changed != original, "source mutation is discriminating");
            runtime.context->UpdateSubresource(source.Get(), 0, nullptr, changed.data(), width * bytesPerPixel(format), 0);
            auto expected = original;
            if (normalizeColor && !role && fg_color::bgra(format))
                for (size_t i = 0; i < expected.size(); i += 4) std::swap(expected[i], expected[i + 2]);
            captured.expected[eye * 5 + role] = std::move(expected);
            sources.push_back(source);
        }
        check(!adapter->publish(runtime.context.Get(), sequence, 1, frameGenerationRequiredPlanes),
              "extra captured UI cannot be silently omitted by a smaller required mask");
        captured.lease = adapter->publish(runtime.context.Get(), sequence, 1, fg_interop::allPlanes);
        check(captured.lease && captured.lease->sequence() == sequence && captured.lease->historyGeneration() == 1,
              "actual publication has paired eyes and exact full plane mask");
        check(captured.lease->producerFence() && captured.lease->producerValue(), "publication exposes ordered producer fence");
        for (unsigned eye = 0; eye < 2; ++eye) for (unsigned role = 0; role < 5; ++role) {
            const auto* item = captured.lease->plane(eye, static_cast<FrameGenerationPlane>(role));
            const auto* other = captured.lease->plane(1 - eye, static_cast<FrameGenerationPlane>(role));
            check(item && other && item->resource12 != other->resource12 && item->alias11 != other->alias11 &&
                item->state == D3D12_RESOURCE_STATE_COMMON && item->sourceTextureIdentity &&
                item->sourceIdentity == sequence * 32 + eye * 8 + role + 1,
                "stereo planes retain distinct immutable identities and COMMON import state");
            check(item->sourceTextureIdentity == captured.sourceIdentities[eye * 5 + role],
                  "immutable plane preserves actual prepared source COM identity");
            if (!role) {
                const auto sourceFormat = eye && rightColor != DXGI_FORMAT_UNKNOWN ? rightColor : color;
                const auto preparedFormat = normalizeColor ? fg_color::canonical(sourceFormat) : sourceFormat;
                const auto capturedFormat = fg_interop::captureFormat(FrameGenerationPlane::Color, preparedFormat);
                D3D11_TEXTURE2D_DESC aliasDesc{}; item->alias11->GetDesc(&aliasDesc);
                check(item->sourceDesc.Format == capturedFormat && aliasDesc.Format == capturedFormat &&
                      item->resource12->GetDesc().Format == capturedFormat,
                      "Color metadata, actual D3D11 alias and D3D12 import share exact typed capture format");
            }
        }
        check(!captured.lease->plane(2, FrameGenerationPlane::Color) &&
              !captured.lease->plane(0, FrameGenerationPlane::Count), "invalid eye/role cannot expose a plane");
        return captured;
    }
    Job& copy(const Captured& captured, bool blocked = false) {
        auto created = std::make_unique<Job>(); created->lease = captured.lease;
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&created->allocator)), "readback allocator");
        hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, created->allocator.Get(), nullptr,
            IID_PPV_ARGS(&created->list)), "readback command list");
        for (unsigned eye = 0; eye < 2; ++eye) for (unsigned role = 0; role < 5; ++role) {
            Copy item;
            item.source = captured.lease->plane(eye, static_cast<FrameGenerationPlane>(role))->resource12;
            item.expected = captured.expected[eye * 5 + role];
            const auto desc = item.source->GetDesc(); UINT64 total = 0;
            device->GetCopyableFootprints(&desc, 0, 1, 0, &item.footprint, &item.rows, &item.rowBytes, &total);
            check(total && total != UINT64_MAX && item.rows && item.rowBytes, "valid full-plane copy footprints");
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = total; buffer.Height = 1; buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&item.readback)), "full-plane readback buffer");
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = item.source.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource = item.readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = item.footprint;
            transition(created->list.Get(), item.source.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            created->list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            transition(created->list.Get(), item.source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            created->copies.push_back(std::move(item));
        }
        hr(created->list->Close(), "close full-plane readback list");
        jobs.push_back(std::move(created)); auto& job = *jobs.back();
        if (blocked) hr(queue->Wait(gate.Get(), 1), "explicit private queue gate");
        hr(queue->Wait(captured.lease->producerFence(), captured.lease->producerValue()), "actual consumer waits producer fence");
        ID3D12CommandList* lists[] = {job.list.Get()}; queue->ExecuteCommandLists(1, lists);
        job.completion = ++serial;
        hr(queue->Signal(completion.Get(), job.completion), "consumer signals after all reads and COMMON restoration");
        return job;
    }
    void verify(Job& job) {
        check(waitFence(completion.Get(), job.completion, event), "all shared-plane reads complete within timeout");
        for (auto& item : job.copies) {
            check(item.rows && item.rowBytes * item.rows == item.expected.size(), "readback footprints cover every expected byte");
            void* memory = nullptr;
            const D3D12_RANGE range{static_cast<SIZE_T>(item.footprint.Offset),
                static_cast<SIZE_T>(item.footprint.Offset +
                    static_cast<UINT64>(item.footprint.Footprint.RowPitch) * (item.rows - 1) + item.rowBytes)};
            hr(item.readback->Map(0, &range, &memory), "map shared-plane readback");
            bool equal = memory != nullptr;
            for (UINT row = 0; row < item.rows && equal; ++row)
                equal = !std::memcmp(static_cast<const unsigned char*>(memory) + item.footprint.Offset +
                    static_cast<size_t>(item.footprint.Footprint.RowPitch) * row,
                    item.expected.data() + static_cast<size_t>(item.rowBytes) * row, static_cast<size_t>(item.rowBytes));
            const D3D12_RANGE noWrites{0, 0}; item.readback->Unmap(0, &noWrites);
            check(equal, "actual shared snapshot preserves every original pixel after source mutation");
        }
        job.lease.reset();
    }
    void producerComplete(const Lease& lease) {
        check(waitFence(lease->producerFence(), lease->producerValue(), event), "producer completion proof before cancel/reuse assertion");
    }
};

void rejectionCases(Gpu& gpu) {
    auto* context = gpu.runtime.context.Get();
    bool wrongOwner = true;
    std::thread foreign([&] { wrongOwner = gpu.adapter->begin(context, 1, 1); });
    foreign.join();
    check(!wrongOwner && gpu.adapter->status().failure == FrameGenerationInteropFailure::WrongThreadOrContext,
          "actual adapter rejects foreign owner thread");
    ComPtr<ID3D11DeviceContext> deferred;
    hr(gpu.runtime.device->CreateDeferredContext(0, &deferred), "deferred context fixture");
    check(!gpu.adapter->begin(deferred.Get(), 1, 1), "actual adapter rejects deferred/foreign context");
    const auto sequence = gpu.nextSequence++;
    gpu.begin(sequence);
    auto pixels = pattern(13, 9, DXGI_FORMAT_R8G8B8A8_UNORM, 1);
    auto source = texture(gpu.runtime.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 13, 9, pixels);
    check(!gpu.adapter->capture(context, sequence + 1, 1, 0, FrameGenerationPlane::Color, source.Get(), 1), "wrong sequence refuses actual capture");
    check(!gpu.adapter->capture(context, sequence, 2, 0, FrameGenerationPlane::Color, source.Get(), 1), "wrong history refuses actual capture");
    check(!gpu.adapter->capture(context, sequence, 1, 0, FrameGenerationPlane::Color, source.Get(), 0), "zero source audit identity refused");
    ComPtr<ID3D11Device> other; ComPtr<ID3D11DeviceContext> otherContext;
    gpu.runtime.makeDevice(other, otherContext);
    auto foreignSource = texture(other.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 13, 9, pixels);
    check(!gpu.adapter->capture(context, sequence, 1, 0, FrameGenerationPlane::Color, foreignSource.Get(), 1) &&
        gpu.adapter->status().failure == FrameGenerationInteropFailure::WrongDevice, "foreign D3D11 device source refused");
    check(gpu.adapter->capture(context, sequence, 1, 0, FrameGenerationPlane::Color, source.Get(), 1), "actual first plane accepted");
    check(!gpu.adapter->capture(context, sequence, 1, 0, FrameGenerationPlane::Color, source.Get(), 1), "duplicate plane cannot overwrite captured snapshot");
    check(!gpu.adapter->publish(context, sequence, 1, fg_interop::allPlanes), "one eye/one plane cannot publish");
    check(!gpu.adapter->abort(context, sequence + 1, 1), "wrong sequence cannot abort active copies");
    check(gpu.adapter->abort(context, sequence, 1), "actual partial-copy abort seals producer work and cancels consumer use");
    gpu.sources.push_back(source);
}
void pixelCases(Gpu& gpu) {
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    for (auto format : formats) {
        auto captured = gpu.capture(format, format);
        auto& job = gpu.copy(captured);
        check(captured.lease->retire(gpu.completion.Get(), job.completion), "retirement covers consumer copies and restored COMMON states");
        check(!captured.lease->cancel() && !captured.lease->retire(gpu.completion.Get(), job.completion), "submitted retirement is immutable and cannot be canceled");
        gpu.verify(job);
    }
    auto captured = gpu.capture(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto& job = gpu.copy(captured);
    check(captured.lease->retire(gpu.completion.Get(), job.completion), "UI F16 retirement");
    gpu.verify(job);
}
// Exercise the exact coordinator order with real conversion and immutable
// interop copies. Gpu::capture mutates each original before publication.
void normalizedPixelCases(Gpu& gpu) {
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_TYPELESS,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    for (auto format : formats) {
        auto captured = gpu.capture(format, DXGI_FORMAT_R8G8B8A8_UNORM, true);
        auto& job = gpu.copy(captured);
        check(captured.lease->retire(gpu.completion.Get(), job.completion), "normalized snapshot retirement");
        gpu.verify(job);
        for (unsigned eye = 0; eye < 2; ++eye)
            check(captured.lease->plane(eye, FrameGenerationPlane::Color)->sourceDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM,
                  "all SDR normalization routes retain canonical encoded-color format");
    }
}

// TYPELESS admission belongs only to Color. Every rejection below calls the
// real adapter and must happen before any GPU copy or shared allocation use.
void typelessAdmissionCases(Gpu& gpu) {
    const auto sequence = gpu.nextSequence++;
    gpu.begin(sequence);
    auto full = texture(gpu.runtime.device.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, 13, 9,
        pattern(13, 9, DXGI_FORMAT_R8G8B8A8_TYPELESS, sequence));
    auto guide = texture(gpu.runtime.device.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, 7, 5,
        pattern(7, 5, DXGI_FORMAT_R8G8B8A8_TYPELESS, sequence));
    ComPtr<ID3D11Device> foreignDevice; ComPtr<ID3D11DeviceContext> foreignContext;
    gpu.runtime.makeDevice(foreignDevice, foreignContext);
    auto foreign = texture(foreignDevice.Get(), DXGI_FORMAT_R8G8B8A8_TYPELESS, 13, 9,
        pattern(13, 9, DXGI_FORMAT_R8G8B8A8_TYPELESS, sequence));
    D3D11_TEXTURE2D_DESC arrayDesc{}; full->GetDesc(&arrayDesc); arrayDesc.ArraySize = 2;
    ComPtr<ID3D11Texture2D> array;
    hr(gpu.runtime.device->CreateTexture2D(&arrayDesc, nullptr, &array), "typeless array descriptor falsifier");
    {
        CaptureCopySpy calls(gpu.runtime.context.Get());
        for (auto role : {FrameGenerationPlane::Depth, FrameGenerationPlane::Motion,
                FrameGenerationPlane::UiColorTransmittance, FrameGenerationPlane::UiMultiply}) {
            auto* source = role == FrameGenerationPlane::Depth || role == FrameGenerationPlane::Motion ? guide.Get() : full.Get();
            check(!gpu.adapter->capture(gpu.runtime.context.Get(), sequence, 1, 0, role, source, sequence) &&
                  gpu.adapter->status().failure == FrameGenerationInteropFailure::InvalidFormat,
                  "actual non-Color plane refuses typeless RGBA8 before copy");
        }
        check(!gpu.adapter->capture(gpu.runtime.context.Get(), sequence, 1, 0, FrameGenerationPlane::Color, guide.Get(), sequence) &&
              gpu.adapter->status().failure == FrameGenerationInteropFailure::InvalidDescriptor,
              "typeless Color still requires exact configured dimensions");
        check(!gpu.adapter->capture(gpu.runtime.context.Get(), sequence, 1, 0, FrameGenerationPlane::Color, array.Get(), sequence) &&
              gpu.adapter->status().failure == FrameGenerationInteropFailure::InvalidDescriptor,
              "typeless Color still refuses texture arrays");
        check(!gpu.adapter->capture(gpu.runtime.context.Get(), sequence, 1, 0, FrameGenerationPlane::Color, foreign.Get(), sequence) &&
              gpu.adapter->status().failure == FrameGenerationInteropFailure::WrongDevice,
              "typeless Color still refuses foreign devices");
        check(calls.copies == 0, "all typeless plane/device/descriptor refusals enqueue zero copies");
        check(gpu.adapter->capture(gpu.runtime.context.Get(), sequence, 1, 0, FrameGenerationPlane::Color, full.Get(), sequence),
              "actual Color accepts single-image typeless RGBA8");
        auto srgb = texture(gpu.runtime.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 13, 9,
            pattern(13, 9, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, sequence));
        check(!gpu.adapter->capture(gpu.runtime.context.Get(), sequence, 1, 1, FrameGenerationPlane::Color, srgb.Get(), sequence) &&
              gpu.adapter->status().failure == FrameGenerationInteropFailure::InvalidFormat && calls.copies == 1,
              "typeless Color cannot pair with incompatible captured sRGB interpretation");
    }
    check(gpu.adapter->abort(gpu.runtime.context.Get(), sequence, 1), "typeless partial Color capture abort");
    gpu.sources.push_back(full); gpu.sources.push_back(guide);
}

// Return to the same completed ring slot across source-format transitions.
// Compatible 27/28 sources share private 28 storage; sRGB retains its own
// existing interpretation. Mutation/readback verifies bits on every transition.
void typelessTransitionCases(Gpu& gpu) {
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM};
    ComPtr<ID3D11Texture2D> previous[2];
    DXGI_FORMAT previousFormat = DXGI_FORMAT_UNKNOWN;
    for (auto format : formats) {
        auto captured = gpu.capture(format, DXGI_FORMAT_R8G8B8A8_UNORM);
        const auto expected = fg_interop::captureFormat(FrameGenerationPlane::Color, format);
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto* alias = captured.lease->plane(eye, FrameGenerationPlane::Color)->alias11;
            if (previous[eye]) check((alias == previous[eye].Get()) == (expected == previousFormat),
                "completed Color storage reuses compatible format and rebuilds on interpretation change");
            previous[eye] = alias;
        }
        previousFormat = expected;
        auto& job = gpu.copy(captured);
        check(captured.lease->retire(gpu.completion.Get(), job.completion), "format-transition snapshot retirement");
        gpu.verify(job);
    }
    auto mixed = gpu.capture(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM,
        false, {}, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto& job = gpu.copy(mixed);
    check(mixed.lease->retire(gpu.completion.Get(), job.completion), "compatible mixed-source stereo retirement");
    gpu.verify(job);
}

void ringCases(Gpu& gpu) {
    std::array<Captured, 3> held;
    for (auto& item : held) {
        item = gpu.capture(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM);
        gpu.producerComplete(item.lease);
        check(item.lease->cancel(), "actual no-consumer cancel accepted once");
        check(!item.lease->cancel(), "duplicate cancellation rejected");
    }
    const auto sequence = gpu.nextSequence++;
    check(!gpu.adapter->begin(gpu.runtime.context.Get(), sequence, 1) &&
        gpu.adapter->status().failure == FrameGenerationInteropFailure::Busy, "three completed canceled slots remain immutable while external leases exist");
    held[0].lease.reset();
    gpu.begin(sequence);
    check(gpu.adapter->abort(gpu.runtime.context.Get(), sequence, 1), "released canceled slot can be reused and empty transaction aborted");
    for (auto& item : held) item.lease.reset();

    ComPtr<ID3D12Fence> unsignaled;
    hr(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&unsignaled)), "independent unsignaled consumer fence");
    for (auto& item : held) {
        item = gpu.capture(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM);
        gpu.producerComplete(item.lease);
        check(!item.lease->retire(nullptr, 7) && !item.lease->retire(unsignaled.Get(), 0), "invalid retirement never attaches a consumer");
        check(item.lease->retire(unsignaled.Get(), 7), "actual lease attaches unsignaled same-device consumer fence");
        item.lease.reset();
    }
    const auto blockedSequence = gpu.nextSequence++;
    check(!gpu.adapter->begin(gpu.runtime.context.Get(), blockedSequence, 1) &&
        gpu.adapter->status().failure == FrameGenerationInteropFailure::Busy, "unreferenced slots cannot reuse before consumer completion");
    hr(unsignaled->Signal(7), "CPU completes no-GPU-consumer retirement fixture");
    gpu.begin(blockedSequence);
    check(gpu.adapter->abort(gpu.runtime.context.Get(), blockedSequence, 1), "completed consumer fence releases ring slot");
}
// A copied pooled source still has producer work that may read it. Its owner
// must follow the transport slot through publication, cancellation, retirement,
// and partial abort, even after the caller drops its Work-equivalent reference.
void sourceOwnerCases(Gpu& gpu) {
    gpu.blockProducer();
    auto pendingOwner = std::make_shared<unsigned>(99);
    std::weak_ptr<unsigned> pendingWeak = pendingOwner;
    auto pending = gpu.capture(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false, pendingOwner);
    pendingOwner.reset();
    const auto producerCompleted = pending.lease->producerFence()->GetCompletedValue();
    check(producerCompleted != UINT64_MAX && producerCompleted < pending.lease->producerValue(),
          "private GPU gate proves source copies still pending");
    check(!pendingWeak.expired(), "pending producer copies retain pooled source owner after caller release");
    check(pending.lease->cancel(), "pending owned source cancellation still requires producer retirement");
    check(!pendingWeak.expired(), "cancel cannot release source owner while producer GPU is gated");
    hr(gpu.producerGate->Signal(1), "release private producer gate before completion wait");
    gpu.producerComplete(pending.lease); pending.lease.reset();
    std::array<Captured, 3> held;
    std::array<std::weak_ptr<unsigned>, 3> owners;
    for (unsigned index = 0; index < held.size(); ++index) {
        auto owner = std::make_shared<unsigned>(index);
        owners[index] = owner;
        held[index] = gpu.capture(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false, owner);
        owner.reset();
        check(!owners[index].expired(), "published UI capture retains source owner before producer completion proof");
        gpu.producerComplete(held[index].lease);
        check(held[index].lease->cancel(), "owned source no-consumer cancellation");
        check(!owners[index].expired(), "canceled but externally held lease retains immutable source owner");
    }
    check(pendingWeak.expired(), "pending producer source owner releases only after completed slot reuse");
    held[0].lease.reset();
    const auto reuse = gpu.nextSequence++;
    gpu.begin(reuse);
    check(owners[0].expired() && !owners[1].expired() && !owners[2].expired(),
          "only producer-complete lease-exclusive recycled slot releases source owner");
    check(gpu.adapter->abort(gpu.runtime.context.Get(), reuse, 1), "owned source empty reuse abort");
    for (auto& captured : held) captured.lease.reset();

    ComPtr<ID3D12Fence> unsignaled;
    hr(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&unsignaled)), "owned source unsignaled consumer fence");
    for (unsigned index = 0; index < held.size(); ++index) {
        auto owner = std::make_shared<unsigned>(index + 3);
        owners[index] = owner;
        auto captured = gpu.capture(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, false, owner);
        owner.reset(); gpu.producerComplete(captured.lease);
        check(captured.lease->retire(unsignaled.Get(), 7), "owned source attaches pending consumer retirement");
        captured.lease.reset();
        check(!owners[index].expired(), "unreferenced lease retains source owner until consumer completion");
    }
    const auto blocked = gpu.nextSequence++;
    check(!gpu.adapter->begin(gpu.runtime.context.Get(), blocked, 1) &&
          gpu.adapter->status().failure == FrameGenerationInteropFailure::Busy, "source-owned pending consumer slots remain busy");
    for (const auto& owner : owners) check(!owner.expired(), "busy ring cannot release pending source owners");
    hr(unsignaled->Signal(7), "complete source-owned no-GPU-consumer retirement");
    // Retain each newly recycled slot so all three old owners must be released.
    for (auto& captured : held) {
        captured = gpu.capture(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM);
        gpu.producerComplete(captured.lease); check(captured.lease->cancel(), "hold recycled source owner slot");
    }
    for (const auto& owner : owners) check(owner.expired(), "consumer-complete recycled slots release all former source owners");
    for (auto& captured : held) captured.lease.reset();

    const auto partial = gpu.nextSequence++;
    gpu.begin(partial);
    auto owner = std::make_shared<unsigned>(9);
    std::weak_ptr<unsigned> abortedOwner = owner;
    const auto pixels = pattern(13, 9, DXGI_FORMAT_R8G8B8A8_UNORM, partial);
    auto source = texture(gpu.runtime.device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 13, 9, pixels);
    check(gpu.adapter->capture(gpu.runtime.context.Get(), partial, 1, 0, FrameGenerationPlane::UiColorTransmittance,
          source.Get(), partial, owner), "partial UI capture retains pooled source owner");
    owner.reset();
    check(gpu.adapter->abort(gpu.runtime.context.Get(), partial, 1), "owned partial UI abort seals producer copies");
    check(!abortedOwner.expired(), "partial abort retains source owner through producer retirement");
    gpu.sources.push_back(source);
    for (auto& captured : held) {
        captured = gpu.capture(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM);
        gpu.producerComplete(captured.lease); check(captured.lease->cancel(), "hold slot after partial owner abort");
    }
    check(abortedOwner.expired(), "partial aborted source owner releases after producer-complete slot reuse");
    for (auto& captured : held) captured.lease.reset();
}

void blockedQueueCase(Gpu& gpu) {
    auto captured = gpu.capture(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto& job = gpu.copy(captured, true);
    check(captured.lease->retire(gpu.completion.Get(), job.completion), "blocked real GPU consumer has an explicit retirement fence");
    check(gpu.completion->GetCompletedValue() < job.completion, "private queue cannot complete while its gate is unsignaled");
    hr(gpu.gate->Signal(1), "CPU always releases private GPU queue gate before waiting");
    gpu.verify(job);
}
void lifetimeCase(Gpu& gpu) {
    // Production deliberately retains its bounded ring owner when an external
    // lease survives destruction. This fixture verifies the public lifetime,
    // rather than claiming that this conservative path releases all objects.
    auto captured = gpu.capture(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM);
    gpu.producerComplete(captured.lease);
    gpu.adapter.reset();
    check(captured.lease->plane(0, FrameGenerationPlane::Depth) && captured.lease->producerValue(),
          "external lease remains readable after adapter destruction");
    auto& job = gpu.copy(captured);
    check(captured.lease->retire(gpu.completion.Get(), job.completion), "surviving lease retains consumer retirement metadata");
    gpu.verify(job);
    std::puts("adapter_destruction_with_external_lease=bounded_owner_retained_by_design");
}
void runGpu(Gpu& gpu) {
    gpu.initialize();
    rejectionCases(gpu);
    pixelCases(gpu);
    normalizedPixelCases(gpu);
    typelessTransitionCases(gpu);
    typelessAdmissionCases(gpu);
    ringCases(gpu);
    sourceOwnerCases(gpu);
    blockedQueueCase(gpu);
    lifetimeCase(gpu);
    check(SUCCEEDED(gpu.device->GetDeviceRemovedReason()) && SUCCEEDED(gpu.runtime.device->GetDeviceRemovedReason()),
          "standalone NVIDIA devices remain healthy");
}
}

int main(int argc, char** argv) {
    const bool enabled = argc == 2 && (!std::strcmp(argv[1], "--gpu") || !std::strcmp(argv[1], "--run"));
    if (argc != 1 && !enabled && !(argc == 2 && !std::strcmp(argv[1], "--self-test"))) {
        std::fprintf(stderr, "Usage: interop_gpu_test [--self-test | --gpu | --run]\n"); return 2;
    }
    Gpu* gpu = nullptr;
    try {
        cpu();
        if (enabled) {
            gpu = new Gpu;
            runGpu(*gpu);
            check(gpu->drain(), "bounded final queue drain after CPU gate release");
            delete gpu; gpu = nullptr;
        }
        std::printf("PASS actual interop adapter checks=%u mode=%s NGX=0 game=0 presentation=0\n",
            checks, enabled ? "NVIDIA-sharing" : "CPU-no-driver-load");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL actual interop adapter checks=%u: %s\n", checks, error.what());
        if (gpu) {
            if (gpu->drain()) delete gpu;
            else std::fputs("QUARANTINE: private queue gate release/drain not proven; retain all GPU owners until process exit\n", stderr);
        }
        return 1;
    }
}

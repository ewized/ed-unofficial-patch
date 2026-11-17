#include "frame_generation_output.h"
#include "ui_layer_shaders.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <utility>

namespace edvr {
using Microsoft::WRL::ComPtr;
namespace {
constexpr unsigned kSlots = 3, kUiImage = EDVR_FG_MAX_PHASES,
    kNativeWorldImage = EDVR_FG_MAX_PHASES + 1,
    kDescriptors = (EDVR_FG_MAX_PHASES + 2) * 2 * 4;
enum Consumer : unsigned { Discarded, Unsubmitted, Attaching, Submitted };
bool complete(uint64_t actual, uint64_t target) noexcept {
    return target && actual != UINT64_MAX && actual >= target;
}
bool reusable(unsigned owners, uint64_t ready, uint64_t target, unsigned consumer,
              uint64_t consumed, uint64_t retireValue) noexcept {
    return owners == 1 && (!target || complete(ready, target)) &&
        (consumer == Discarded || (consumer == Submitted && complete(consumed, retireValue)));
}
bool sameObject(IUnknown* left, IUnknown* right) noexcept {
    ComPtr<IUnknown> a, b;
    return left && right && SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
}
bool belongs(ID3D12Device* expected, ID3D12DeviceChild* child) noexcept {
    ComPtr<ID3D12Device> actual;
    return child && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&actual))) && sameObject(expected, actual.Get());
}
void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}
bool texture(ID3D12Device* device, ID3D12Resource* resource, unsigned width, unsigned height,
             bool world) noexcept {
    if (!belongs(device, resource)) return false;
    const auto desc = resource->GetDesc();
    const bool format = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
        (!world && (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT));
    return format && desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == width &&
        desc.Height == height && desc.DepthOrArraySize == 1 && desc.MipLevels == 1 &&
        desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
        !(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
}
bool viewValid(const EdvrFgView& view) noexcept {
    for (float value : view.orientation) if (!std::isfinite(value)) return false;
    for (float value : view.position) if (!std::isfinite(value)) return false;
    for (float value : view.fov) if (!std::isfinite(value)) return false;
    return view.fov[0] < view.fov[1] && view.fov[3] < view.fov[2];
}
bool sourceValid(const EdvrFgSource& source, unsigned width, unsigned height) noexcept {
    return source.size == sizeof(source) && source.version == EDVR_NATIVE_FRAME_GENERATION_VERSION_1 &&
        source.generation && source.sequence && source.poseDisplayTime > 0 &&
        source.width == width && source.height == height && viewValid(source.views[0]) && viewValid(source.views[1]);
}
bool pairValid(const FrameGenerationOutputSource& source, unsigned width, unsigned height,
               bool generated) noexcept {
    if ((source.multiplier != 2 && source.multiplier != 4) || !source.historyGeneration ||
        !sourceValid(source.current, width, height)) return false;
    if (!generated) return true;
    const auto& previous = source.previous;
    const auto& current = source.current;
    return sourceValid(previous, width, height) && previous.sequence < current.sequence &&
        previous.poseDisplayTime < current.poseDisplayTime && previous.generation == current.generation &&
        previous.referenceGeneration == current.referenceGeneration && previous.configGeneration == current.configGeneration;
}
bool uiValid(const FrameGenerationOutputUi& ui) noexcept {
    for (float value : ui.uv) if (!std::isfinite(value) || value < 0 || value > 1) return false;
    return ui.uv[0] < ui.uv[2] && ui.uv[1] < ui.uv[3] && !(ui.empty && ui.useMult);
}
HMODULE systemModule(const wchar_t* name) noexcept {
    wchar_t path[32768]{};
    const UINT length = GetSystemDirectoryW(path, DWORD(std::size(path)));
    if (!length || length >= std::size(path) || wcscat_s(path, L"\\") || wcscat_s(path, name)) return nullptr;
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return nullptr;
    wchar_t actual[32768]{};
    const DWORD count = GetModuleFileNameW(module, actual, DWORD(std::size(actual)));
    if (!count || count >= std::size(actual) || _wcsicmp(path, actual)) { FreeLibrary(module); return nullptr; }
    // Keep graphics code resident for quarantined GPU objects until process exit.
    return module;
}
using Compile = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
using Serialize = HRESULT (WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
    ID3DBlob**, ID3DBlob**);
} // namespace

struct FrameGenerationOutputLease {
    struct Image {
        ComPtr<ID3D12Resource> resource;
        HANDLE handle = nullptr;
        ~Image() { if (handle) CloseHandle(handle); }
    } images[EDVR_FG_MAX_PHASES+2][2];
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12Fence> ready, consumerFence;
    HANDLE readyHandle = nullptr;
    EdvrFgBatch batch{};
    std::atomic<unsigned> consumer{Discarded};
    std::atomic<bool> exported{false};
    uint64_t value = 0, consumerValue = 0, submittedMs = 0;
    uint32_t timeoutMs = 0;
    bool unsafe = false, counted = false;
    std::shared_ptr<FrameGenerationInteropLease> input;
    FrameGenerationNgxTicket backend;
    // Explicit COM ownership in addition to the producer's immutable lease.
    ComPtr<ID3D12Resource> inputs[2][3];
    ~FrameGenerationOutputLease() { if (readyHandle) CloseHandle(readyHandle); }
};
namespace {
struct Token {
    std::atomic<uint32_t> references{1};
    std::shared_ptr<FrameGenerationOutputLease> lease;
    explicit Token(std::shared_ptr<FrameGenerationOutputLease> value) : lease(std::move(value)) {}
};
} // namespace

HRESULT WINAPI frameGenerationOutputRetain(void* opaque) noexcept {
    if (!opaque) return E_INVALIDARG;
    auto& count = static_cast<Token*>(opaque)->references;
    uint32_t value = count.load(std::memory_order_relaxed);
    while (value && value != UINT32_MAX) {
        if (count.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return S_OK;
    }
    return E_UNEXPECTED;
}
void WINAPI frameGenerationOutputRelease(void* opaque) noexcept {
    if (opaque && static_cast<Token*>(opaque)->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
        delete static_cast<Token*>(opaque);
}
HRESULT WINAPI frameGenerationOutputRetire(void* opaque, HANDLE handle, uint64_t value) noexcept {
    if (!opaque || !handle || !value || value == UINT64_MAX) return E_INVALIDARG;
    auto& data = *static_cast<Token*>(opaque)->lease;
    ComPtr<ID3D12Fence> fence;
    HRESULT hr = data.device->OpenSharedHandle(handle, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) return hr;
    if (!belongs(data.device.Get(), fence.Get())) return E_INVALIDARG;
    unsigned expected = Unsubmitted;
    if (!data.consumer.compare_exchange_strong(expected, Attaching, std::memory_order_acq_rel)) return E_UNEXPECTED;
    data.consumerFence = std::move(fence);
    data.consumerValue = value;
    data.consumer.store(Submitted, std::memory_order_release);
    return S_OK;
}
HRESULT WINAPI frameGenerationOutputDiscard(void* opaque) noexcept {
    if (!opaque) return E_INVALIDARG;
    unsigned expected = Unsubmitted;
    return static_cast<Token*>(opaque)->lease->consumer.compare_exchange_strong(expected, Discarded,
        std::memory_order_acq_rel) ? S_OK : E_UNEXPECTED;
}
HRESULT FrameGenerationOutputTicket::acquireBatch(EdvrFgBatch* output) const noexcept {
    if (!output || output->size != sizeof(*output) || output->version != EDVR_FG_BATCH_VERSION_3)
        return E_INVALIDARG;
    if (!lease || unsafe || !finalFence || !finalValue) return E_FAIL;
    const auto done = finalFence->GetCompletedValue();
    if (done == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    if (!complete(done, finalValue)) return S_FALSE;
    if (GetTickCount64() - lease->submittedMs >= lease->timeoutMs) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    try {
        auto* token = new Token(lease);
        bool expected = false;
        if (!lease->exported.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            delete token; return S_FALSE;
        }
        lease->consumer.store(Unsubmitted, std::memory_order_release);
        *output = lease->batch;
        output->token = token;
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}

struct FrameGenerationOutput::Impl {
    std::mutex mutex;
    FrameGenerationOutputStatus status;
    DWORD owner = 0;
    unsigned width = 0, height = 0;
    uint32_t timeoutMs = 0;
    uint64_t serial = 0, lastFence = 0, lastSequence = 0, lastHistory = 0;
    UINT stride = 0;
    HANDLE event = nullptr;
    bool unsafe = false, drained = true;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    std::array<std::shared_ptr<FrameGenerationOutputLease>, kSlots> slots;
    ~Impl() { if (event) CloseHandle(event); }
    bool fail(FrameGenerationOutputFailure reason, HRESULT hr = E_FAIL, bool fatal = true) noexcept {
        status.failure = reason; status.lastHresult = hr;
        if (fatal) status.failed = true;
        return false;
    }
    bool owning() noexcept {
        return GetCurrentThreadId() == owner || fail(FrameGenerationOutputFailure::WrongThread, E_UNEXPECTED, false);
    }
    bool collect() noexcept {
        const auto done = fence ? fence->GetCompletedValue() : 0;
        if (done == UINT64_MAX) { status.quarantined = true; return fail(FrameGenerationOutputFailure::DeviceRemoved, DXGI_ERROR_DEVICE_REMOVED); }
        for (auto& slot : slots) if (slot && slot->value) {
            if (complete(done, slot->value)) {
                slot->input.reset(); slot->backend = {};
                for (auto& eye : slot->inputs) for (auto& input : eye) input.Reset();
                if (!slot->counted) { slot->counted = true; ++status.completed; }
            } else if (GetTickCount64() - slot->submittedMs >= timeoutMs) {
                status.quarantined = true; fail(FrameGenerationOutputFailure::Timeout, HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            }
        }
        return !status.failed;
    }
    bool canReuse(const std::shared_ptr<FrameGenerationOutputLease>& slot) noexcept {
        if (!slot) return true;
        if (slot->unsafe) return false;
        const auto consumer = slot->consumer.load(std::memory_order_acquire);
        const auto done = consumer == Submitted ? slot->consumerFence->GetCompletedValue() : 0;
        return reusable(unsigned(slot.use_count()), fence->GetCompletedValue(), slot->value,
            consumer, done, slot->consumerValue);
    }
    bool shader() noexcept {
        const HMODULE compiler = systemModule(L"d3dcompiler_47.dll");
        const HMODULE runtime = systemModule(L"d3d12.dll");
        const auto compile = compiler ? reinterpret_cast<Compile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        const auto serialize = runtime ? reinterpret_cast<Serialize>(GetProcAddress(runtime, "D3D12SerializeRootSignature")) : nullptr;
        if (!compile || !serialize) return fail(FrameGenerationOutputFailure::Shader);
        ComPtr<ID3DBlob> code, errors, signature;
        HRESULT hr = compile(kUiLayerCompositeHlsl, sizeof(kUiLayerCompositeHlsl) - 1, "EDVR_FG_production_HUD",
            nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) return fail(FrameGenerationOutputFailure::Shader, hr);
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {2, ranges};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {0, 0, sizeof(UiLayerCompositeParams) / 4};
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters = 2; desc.pParameters = params;
        hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors);
        if (FAILED(hr) || FAILED(hr = device->CreateRootSignature(0, signature->GetBufferPointer(),
            signature->GetBufferSize(), IID_PPV_ARGS(&root)))) return fail(FrameGenerationOutputFailure::Shader, hr);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root.Get();
        pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline));
        return SUCCEEDED(hr) || fail(FrameGenerationOutputFailure::Shader, hr);
    }
    bool prepare(FrameGenerationOutputLease& slot, unsigned phases,bool separateUi) noexcept {
        HRESULT hr = S_OK;
        if (!slot.allocator) {
            if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator))) ||
                FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                    IID_PPV_ARGS(&slot.list))) || FAILED(hr = slot.list->Close())) return fail(FrameGenerationOutputFailure::Allocation, hr);
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = kDescriptors;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&slot.descriptors)))) return fail(FrameGenerationOutputFailure::Allocation, hr);
            slot.device = device; slot.ready = fence;
            if (FAILED(hr = device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &slot.readyHandle)))
                return fail(FrameGenerationOutputFailure::Allocation, hr);
        }
        for (unsigned p = 0; p <= kNativeWorldImage; ++p) for (unsigned e = 0; e < 2; ++e) {
            if(p>=phases&&!(separateUi&&(p==kUiImage||p==kNativeWorldImage)))continue;
            auto& image = slot.images[p][e];
            if (image.resource) continue;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width; desc.Height = height; desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
            // D3D11 shared resources have simultaneous-access semantics. The
            // cross-API description must represent those semantics even though
            // our fences prohibit concurrent publication/consumer writes.
            // https://microsoft.github.io/DirectX-Specs/d3d/ResourceHeaps.html
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
            if (FAILED(hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&image.resource))) ||
                FAILED(hr = device->CreateSharedHandle(image.resource.Get(), nullptr, GENERIC_ALL, nullptr, &image.handle)))
                return fail(FrameGenerationOutputFailure::Allocation, hr);
        }
        return true;
    }
    void metadata(FrameGenerationOutputLease& slot, const FrameGenerationOutputSource& source, bool generated) noexcept {
        auto& batch = slot.batch; batch = {};
        batch.size = sizeof(batch); batch.version = EDVR_FG_BATCH_VERSION_3;
        const auto& previous = source.previous; const auto& current = source.current;
        batch.generation = current.generation; batch.referenceGeneration = current.referenceGeneration;
        batch.configGeneration = current.configGeneration; batch.historyGeneration = source.historyGeneration;
        batch.previousSequence = previous.sequence; batch.currentSequence = current.sequence;
        batch.previousPoseDisplayTime = previous.poseDisplayTime; batch.currentPoseDisplayTime = current.poseDisplayTime;
        std::memcpy(batch.previousViews, previous.views, sizeof(batch.previousViews));
        std::memcpy(batch.currentViews, current.views, sizeof(batch.currentViews));
        batch.multiplier = source.multiplier; batch.generatedCount = generated ? source.multiplier - 1 : 0;
        batch.phaseMask = generated ? (1u << source.multiplier) - 1 : 1;
        batch.width = width; batch.height = height; batch.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        const auto luid = device->GetAdapterLuid(); batch.adapterLuidLow = luid.LowPart; batch.adapterLuidHigh = luid.HighPart;
        const unsigned phases = generated ? source.multiplier : 1;
        for (unsigned p = 0; p < phases; ++p) for (unsigned e = 0; e < 2; ++e) batch.textures[p][e] = slot.images[p][e].handle;
        batch.readyFence = slot.readyHandle;
    }
    void composite(FrameGenerationOutputLease& slot, ID3D12Resource* world, unsigned phase, unsigned eye,
                   const FrameGenerationOutputUi& ui, D3D12_RESOURCE_STATES worldState,unsigned mode=0) noexcept {
        auto* list = slot.list.Get(); auto* output = slot.images[phase][eye].resource.Get();
        if (ui.empty) {
            transition(list, world, worldState, D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(list, output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(output, world);
            transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            transition(list, world, D3D12_RESOURCE_STATE_COPY_SOURCE, worldState);
            return;
        }
        transition(list, world, worldState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(list, output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const unsigned base = (phase * 2 + eye) * 4;
        auto cpu = slot.descriptors->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(base) * stride;
        auto gpu = slot.descriptors->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(base) * stride;
        ID3D12Resource* resources[3] = {world, slot.inputs[eye][1].Get(), slot.inputs[eye][2].Get()};
        for (unsigned i = 0; i < 3; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = resources[i]->GetDesc().Format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(resources[i], &view, cpu); cpu.ptr += stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC target{}; target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output, nullptr, &target, cpu);
        UiLayerCompositeParams constants{};
        constants.region[2] = int32_t(width); constants.region[3] = int32_t(height);
        std::memcpy(constants.uv, ui.uv, sizeof(constants.uv));
        constants.layerSize[0] = float(width); constants.layerSize[1] = float(height);
        constants.outSize[0] = width; constants.outSize[1] = height; constants.useMult = ui.useMult ? 1u : 0u;
        constants.mode=mode;
        list->SetComputeRootDescriptorTable(0, gpu);
        list->SetComputeRoot32BitConstants(1, sizeof(constants) / 4, &constants, 0);
        list->Dispatch((width + kUiLayerCompositeGroupX - 1) / kUiLayerCompositeGroupX,
            (height + kUiLayerCompositeGroupY - 1) / kUiLayerCompositeGroupY, 1);
        transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        transition(list, world, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, worldState);
    }
};

FrameGenerationOutput::~FrameGenerationOutput() {
    if (!impl_) return;
    bool provedDrained = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        provedDrained = impl_->drained && !impl_->unsafe && !impl_->status.quarantined;
    }
    // close joins the worker after its successful drain. The joining thread
    // can destroy that already-proved idle owner without making a worker call.
    if (provedDrained || drain(0)) delete impl_;
    else impl_->status.quarantined = true; // GPU objects/handles/leases live until process exit.
    impl_ = nullptr;
}
bool FrameGenerationOutput::initialize(bool enabled, ID3D12Device* device, unsigned width,
                                      unsigned height, uint32_t timeoutMs) noexcept {
    if (impl_) return false;
    if (!enabled) { initial_ = FrameGenerationOutputFailure::Off; return false; }
    if (!device || !width || !height || width > 16384 || height > 16384 || !timeoutMs || timeoutMs > 5000) {
        initial_ = FrameGenerationOutputFailure::InvalidArgument; return false;
    }
    try {
        impl_ = new Impl; auto& data = *impl_;
        data.owner = GetCurrentThreadId(); data.device = device; data.width = width; data.height = height; data.timeoutMs = timeoutMs;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT format{DXGI_FORMAT_R8G8B8A8_UNORM};
        HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format));
        if (FAILED(hr) || !(format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            return data.fail(FrameGenerationOutputFailure::InvalidResource, FAILED(hr) ? hr : DXGI_ERROR_UNSUPPORTED);
        D3D12_COMMAND_QUEUE_DESC queue{}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        if (FAILED(hr = device->CreateCommandQueue(&queue, IID_PPV_ARGS(&data.queue))) ||
            FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&data.fence))))
            return data.fail(FrameGenerationOutputFailure::Allocation, hr);
        data.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!data.event) return data.fail(FrameGenerationOutputFailure::Allocation, HRESULT_FROM_WIN32(GetLastError()));
        data.stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (!data.shader()) return false;
        data.status.initialized = true; data.status.failure = FrameGenerationOutputFailure::None;
        return true;
    } catch (...) { initial_ = FrameGenerationOutputFailure::Allocation; return false; }
}
FrameGenerationOutputTicket FrameGenerationOutput::submit(const std::shared_ptr<FrameGenerationInteropLease>& input,
    const FrameGenerationNgxTicket& backend, const FrameGenerationOutputUi (&ui)[2],
    const FrameGenerationOutputSource& source) noexcept {
    FrameGenerationOutputTicket ticket;
    if (!impl_) return ticket;
    auto& data = *impl_; std::lock_guard<std::mutex> lock(data.mutex);
    try {
        if (!data.owning() || !data.status.initialized || !data.collect()) return ticket;
        if (!input || input->sequence() != source.current.sequence || input->historyGeneration() != source.historyGeneration ||
            (source.historyGeneration == data.lastHistory && source.current.sequence <= data.lastSequence) ||
            !pairValid(source, data.width, data.height, false) || !uiValid(ui[0]) || !uiValid(ui[1])) {
            data.fail(FrameGenerationOutputFailure::InvalidMetadata, E_INVALIDARG, false); return ticket;
        }
        bool generated = false;
        if (backend.lease) {
            if (!backend.completionFence || !backend.completionValue) {
                auto retained = std::make_shared<FrameGenerationOutputLease>();
                retained->input = input; retained->backend = backend; retained->unsafe = true;
                // A failed accepted backend without a fence cannot be canceled.
                // Retain the entire owner even if every normal ring slot is busy.
                auto* quarantine = new std::shared_ptr<FrameGenerationOutputLease>(retained); (void)quarantine;
                data.unsafe = true; data.drained = false; data.status.quarantined = true;
                data.fail(FrameGenerationOutputFailure::UnsafeBackend);
                ticket.accepted = ticket.unsafe = true; ticket.lease = std::move(retained); return ticket;
            }
            if (!belongs(data.device.Get(), backend.completionFence) || backend.sequence != source.current.sequence) {
                data.fail(FrameGenerationOutputFailure::InvalidArgument, E_INVALIDARG, false); return ticket;
            }
            const auto done = backend.completionFence->GetCompletedValue();
            if (done == UINT64_MAX) { data.fail(FrameGenerationOutputFailure::DeviceRemoved, DXGI_ERROR_DEVICE_REMOVED); return ticket; }
            if (!complete(done, backend.completionValue)) { data.fail(FrameGenerationOutputFailure::Pending, S_FALSE, false); return ticket; }
            generated = backend.poll() == FrameGenerationNgxCompletion::Ready;
            if (generated && (backend.generatedCount != source.multiplier - 1 ||
                !pairValid(source, data.width, data.height, true))) {
                data.fail(FrameGenerationOutputFailure::InvalidMetadata, E_INVALIDARG, false); return ticket;
            }
        }
        if (!belongs(data.device.Get(), input->producerFence()) || !input->producerValue()) {
            data.fail(FrameGenerationOutputFailure::InvalidArgument, E_INVALIDARG, false); return ticket;
        }
        std::shared_ptr<FrameGenerationOutputLease>* selected = nullptr;
        for (auto& candidate : data.slots) if (data.canReuse(candidate)) { selected = &candidate; break; }
        if (!selected) { ++data.status.busy; data.fail(FrameGenerationOutputFailure::Busy, S_FALSE, false); return ticket; }
        if (!*selected) *selected = std::make_shared<FrameGenerationOutputLease>();
        auto& slot = **selected;
        const unsigned phases = generated ? source.multiplier : 1;
        const bool separateUi=generated&&(!ui[0].empty||!ui[1].empty);
        if (!data.prepare(slot, phases,separateUi)) return ticket;
        const FrameGenerationPlane roles[3] = {FrameGenerationPlane::Color,
            FrameGenerationPlane::UiColorTransmittance, FrameGenerationPlane::UiMultiply};
        for (unsigned e = 0; e < 2; ++e) for (unsigned i = 0; i < 3; ++i) {
            const auto* plane = input->plane(e, roles[i]);
            if (!plane || plane->state != D3D12_RESOURCE_STATE_COMMON ||
                !texture(data.device.Get(), plane->resource12, data.width, data.height, i == 0)) {
                data.fail(FrameGenerationOutputFailure::InvalidResource, E_INVALIDARG, false); return ticket;
            }
            slot.inputs[e][i] = plane->resource12;
        }
        if (generated) for (unsigned e = 0; e < 2; ++e) for (unsigned i = 0; i < backend.generatedCount; ++i)
            if (!texture(data.device.Get(), backend.generated[e][i], data.width, data.height, true)) {
                data.fail(FrameGenerationOutputFailure::InvalidResource, E_INVALIDARG, false); return ticket;
            }
        HRESULT hr = slot.allocator->Reset();
        if (FAILED(hr) || FAILED(hr = slot.list->Reset(slot.allocator.Get(), data.pipeline.Get()))) {
            data.fail(FrameGenerationOutputFailure::Queue, hr); return ticket;
        }
        slot.input = input; slot.backend = backend; slot.unsafe = false; slot.counted = false;
        slot.exported.store(false, std::memory_order_release); slot.consumer.store(Discarded, std::memory_order_release);
        slot.consumerFence.Reset(); slot.consumerValue = 0; slot.timeoutMs = data.timeoutMs;
        data.metadata(slot, source, generated);
        if(separateUi){
            slot.batch.uiFlags=EDVR_FG_UI_NATIVE_COVERAGE;
            for(unsigned eye=0;eye<2;++eye){
                slot.batch.uiTextures[eye]=slot.images[kUiImage][eye].handle;
                slot.batch.nativeWorldTextures[eye]=slot.images[kNativeWorldImage][eye].handle;
            }
        }
        auto* list = slot.list.Get(); ID3D12DescriptorHeap* heaps[] = {slot.descriptors.Get()};
        list->SetDescriptorHeaps(1, heaps); list->SetComputeRootSignature(data.root.Get());
        for (unsigned e = 0; e < 2; ++e) {
            if (!ui[e].empty||separateUi) for (unsigned i = 1; i < 3; ++i)
                transition(list, slot.inputs[e][i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            data.composite(slot, slot.inputs[e][0].Get(), 0, e, ui[e], D3D12_RESOURCE_STATE_COMMON);
            FrameGenerationOutputUi worldOnly=ui[e];worldOnly.empty=true;
            for (unsigned p = 1; p < phases; ++p)
                data.composite(slot, backend.generated[e][p - 1], p, e, worldOnly, backend.outputState);
            if(separateUi){
                data.composite(slot,slot.inputs[e][0].Get(),kNativeWorldImage,e,worldOnly,D3D12_RESOURCE_STATE_COMMON);
                auto coverage=ui[e];coverage.empty=false;
                if(ui[e].empty)coverage.useMult=false; // emptyUi is exact (0,0,0,1)
                data.composite(slot,slot.inputs[e][0].Get(),kUiImage,e,coverage,D3D12_RESOURCE_STATE_COMMON,2);
            }
            if (!ui[e].empty||separateUi) for (unsigned i = 1; i < 3; ++i)
                transition(list, slot.inputs[e][i].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        if (FAILED(hr = list->Close())) { data.fail(FrameGenerationOutputFailure::Queue, hr); return ticket; }
        if (data.serial == UINT64_MAX - 1) { data.fail(FrameGenerationOutputFailure::Queue, E_UNEXPECTED); return ticket; }
        ID3D12Fence* waitFence = backend.lease ? backend.completionFence : input->producerFence();
        const auto waitValue = backend.lease ? backend.completionValue : input->producerValue();
        data.drained = false;
        if (FAILED(hr = data.queue->Wait(waitFence, waitValue))) {
            data.fail(FrameGenerationOutputFailure::Queue, hr); return ticket;
        }
        ID3D12CommandList* lists[] = {list};
        data.queue->ExecuteCommandLists(1, lists);
        ticket.accepted = true; ticket.lease = *selected;
        slot.submittedMs = GetTickCount64(); slot.value = ++data.serial;
        if (FAILED(hr = data.queue->Signal(data.fence.Get(), slot.value))) {
            slot.unsafe = true; data.unsafe = true; data.status.quarantined = true;
            data.fail(FrameGenerationOutputFailure::Queue, hr); ticket.unsafe = true; return ticket;
        }
        data.lastFence = slot.value; slot.batch.readyValue = slot.value;
        data.lastSequence = source.current.sequence; data.lastHistory = source.historyGeneration;
        ticket.finalFence = data.fence.Get(); ticket.finalValue = slot.value;
        ++data.status.submitted; if (!generated) ++data.status.nativeOnly;
        data.status.failure = FrameGenerationOutputFailure::None; data.status.lastHresult = S_OK;
        return ticket;
    } catch (...) { data.fail(FrameGenerationOutputFailure::Allocation, E_OUTOFMEMORY); return ticket; }
}
bool FrameGenerationOutput::collect() noexcept {
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->owning() && impl_->collect();
}
bool FrameGenerationOutput::drain(uint32_t timeoutMs) noexcept {
    if (!impl_) return true;
    auto& data = *impl_; std::lock_guard<std::mutex> lock(data.mutex);
    if (!data.owning() || data.unsafe || data.status.quarantined || timeoutMs > 5000) return false;
    if (data.lastFence && !complete(data.fence->GetCompletedValue(), data.lastFence)) {
        if (!timeoutMs || FAILED(data.fence->SetEventOnCompletion(data.lastFence, data.event)) ||
            WaitForSingleObject(data.event, timeoutMs) != WAIT_OBJECT_0 ||
            !complete(data.fence->GetCompletedValue(), data.lastFence)) return false;
    }
    data.collect();
    for (const auto& slot : data.slots) if (!data.canReuse(slot)) {
        data.fail(FrameGenerationOutputFailure::OutstandingLease, S_FALSE, false); return false;
    }
    data.drained = true;
    return true;
}
FrameGenerationOutputStatus FrameGenerationOutput::status() const noexcept {
    if (!impl_) { FrameGenerationOutputStatus result; result.failure = initial_; return result; }
    std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->status;
}

#ifdef EDVR_FRAME_GENERATION_OUTPUT_TEST
unsigned frameGenerationOutputTestProtocol() noexcept {
    unsigned failures = 0;
    const auto check = [&](bool value) { if (!value) ++failures; };
    try {
        auto lease = std::make_shared<FrameGenerationOutputLease>();
        lease->consumer.store(Unsubmitted);
        auto* token = new Token(lease);
        check(frameGenerationOutputRetain(token) == S_OK && token->references.load() == 2);
        frameGenerationOutputRelease(token);
        check(token->references.load() == 1);
        check(frameGenerationOutputRetire(token, nullptr, 1) == E_INVALIDARG && lease->consumer.load() == Unsubmitted);
        check(frameGenerationOutputRetire(token, HANDLE(uintptr_t(1)), 0) == E_INVALIDARG && lease->consumer.load() == Unsubmitted);
        check(frameGenerationOutputRetire(token, HANDLE(uintptr_t(1)), UINT64_MAX) == E_INVALIDARG);
        check(!reusable(1, 4, 4, lease->consumer.load(), 4, 4));
        check(frameGenerationOutputDiscard(token) == S_OK && lease->consumer.load() == Discarded);
        check(frameGenerationOutputDiscard(token) == E_UNEXPECTED);
        check(!reusable(2, 4, 4, Discarded, 0, 0));
        check(!reusable(1, 3, 4, Discarded, 0, 0));
        check(reusable(1, 4, 4, Discarded, 0, 0));
        check(!reusable(1, UINT64_MAX, 4, Discarded, 0, 0));
        check(!reusable(1, 4, 4, Submitted, 7, 8));
        check(reusable(1, 4, 4, Submitted, 8, 8));
        check(!reusable(1, 4, 4, Submitted, UINT64_MAX, 8));
        frameGenerationOutputRelease(token);
        check(lease.use_count() == 1);
        FrameGenerationOutputUi ui;
        check(uiValid(ui)); ui.uv[0] = std::numeric_limits<float>::quiet_NaN(); check(!uiValid(ui));
        ui = {}; ui.useMult = ui.empty = true; check(!uiValid(ui));
        check(frameGenerationOutputRetain(nullptr) == E_INVALIDARG && frameGenerationOutputDiscard(nullptr) == E_INVALIDARG);
    } catch (...) { ++failures; }
    return failures;
}
#endif
} // namespace edvr

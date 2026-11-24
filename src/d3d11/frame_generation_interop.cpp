#include "frame_generation_interop.h"
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cwchar>
#include <new>
#include <utility>

namespace edvr {
using Microsoft::WRL::ComPtr;
namespace {
enum Consumer : unsigned { Unsubmitted, Attaching, Submitted, Canceled };
struct Handle {
    HANDLE value{};
    ~Handle() { if (value) CloseHandle(value); }
};
bool sameObject(IUnknown* left, IUnknown* right) noexcept {
    if (!left || !right) return false;
    ComPtr<IUnknown> a, b;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&a))) &&
        SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&b))) && a.Get() == b.Get();
}
struct Image {
    ComPtr<ID3D11Texture2D> alias11, source11;
    ComPtr<ID3D12Resource> resource12;
    std::shared_ptr<const void> sourceOwner;
    FrameGenerationInteropPlane view{};
    unsigned width{}, height{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
};
using CreateDevice12 = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
}  // namespace

struct FrameGenerationInteropLease::Impl {
    fg_interop::Transaction transaction{};
    std::array<Image, fg_interop::eyes * fg_interop::planes> images{};
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D12Fence> fence12, consumerFence;
    std::uint64_t producer{}, consumerValue{};
    std::atomic<unsigned> consumer{Canceled};
    bool published{}, poisoned{};
};
FrameGenerationInteropLease::FrameGenerationInteropLease(std::shared_ptr<Impl> value) : impl_(std::move(value)) {}
FrameGenerationInteropLease::~FrameGenerationInteropLease() = default;
std::uint64_t FrameGenerationInteropLease::sequence() const noexcept { return impl_->transaction.sequence; }
std::uint64_t FrameGenerationInteropLease::historyGeneration() const noexcept { return impl_->transaction.generation; }
const FrameGenerationInteropPlane* FrameGenerationInteropLease::plane(unsigned eye, FrameGenerationPlane role) const noexcept {
    if (!impl_->published || eye >= fg_interop::eyes || unsigned(role) >= fg_interop::planes ||
        !(impl_->transaction.captured[eye] & frameGenerationPlaneBit(role))) return nullptr;
    return &impl_->images[eye * fg_interop::planes + unsigned(role)].view;
}
ID3D12Fence* FrameGenerationInteropLease::producerFence() const noexcept { return impl_->fence12.Get(); }
std::uint64_t FrameGenerationInteropLease::producerValue() const noexcept { return impl_->producer; }
bool FrameGenerationInteropLease::retire(ID3D12Fence* fence, std::uint64_t value) noexcept {
    if (!impl_->published || !fence || !value || value == std::numeric_limits<std::uint64_t>::max()) return false;
    ComPtr<ID3D12Device> device;
    if (FAILED(fence->GetDevice(IID_PPV_ARGS(&device))) || !sameObject(device.Get(), impl_->device12.Get())) return false;
    unsigned expected = Unsubmitted;
    if (!impl_->consumer.compare_exchange_strong(expected, Attaching, std::memory_order_acq_rel)) return false;
    impl_->consumerFence = fence;
    impl_->consumerValue = value;
    impl_->consumer.store(Submitted, std::memory_order_release);
    return true;
}
bool FrameGenerationInteropLease::cancel() noexcept {
    if (!impl_->published) return false;
    unsigned expected = Unsubmitted;
    return impl_->consumer.compare_exchange_strong(expected, Canceled, std::memory_order_acq_rel);
}

struct FrameGenerationInterop::Impl {
    FrameGenerationInteropInit config{};
    FrameGenerationInteropStatus status{};
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> originalContext;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D12Fence> fence12;
    HMODULE retainedModule{};
    struct Slot {
        std::shared_ptr<FrameGenerationInteropLease> lease;
        std::shared_ptr<FrameGenerationInteropLease::Impl> storage;
    };
    std::array<Slot, fg_interop::slots> slots;
    int active{-1};
    std::uint64_t serial{}, lastSequence{};

    bool reject(FrameGenerationInteropFailure failure, HRESULT hr = S_OK) noexcept {
        status.failure = failure; status.lastHresult = hr; ++status.rejected; return false;
    }
    bool owner(ID3D11DeviceContext* supplied) noexcept {
        return fg_interop::owner(config.renderThreadId, GetCurrentThreadId(), originalContext.Get(), supplied) ||
            reject(FrameGenerationInteropFailure::WrongThreadOrContext);
    }
    bool usable(const Slot& slot) const noexcept {
        const auto& data = *slot.storage;
        if (data.poisoned || slot.lease.use_count() != 1) return false;
        const auto consumer = data.consumer.load(std::memory_order_acquire);
        const bool attached = consumer == Submitted;
        return fg_interop::reusable(1, fence12->GetCompletedValue(), data.producer,
            consumer == Canceled, attached, attached ? data.consumerFence->GetCompletedValue() : 0,
            data.consumerValue);
    }
    bool seal(FrameGenerationInteropLease::Impl& data) noexcept {
        if (serial >= std::numeric_limits<std::uint64_t>::max() - 1) {
            data.poisoned = true; status.quarantined = true;
            return reject(FrameGenerationInteropFailure::Fence);
        }
        const auto value = ++serial;
        const HRESULT hr = context4->Signal(fence11.Get(), value);
        // Flush submits copies and the producer signal; it does not CPU-wait.
        context4->Flush();
        if (FAILED(hr)) {
            data.poisoned = true; status.quarantined = true;
            return reject(FrameGenerationInteropFailure::Fence, hr);
        }
        data.producer = value;
        return true;
    }
    bool image(Image& image, const D3D11_TEXTURE2D_DESC& source) noexcept {
        if (image.alias11 && image.width == source.Width && image.height == source.Height && image.format == source.Format)
            return true;
        D3D11_TEXTURE2D_DESC desc = source;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        ComPtr<ID3D11Texture2D> alias;
        ComPtr<IDXGIResource1> resource;
        ComPtr<ID3D12Resource> imported;
        Handle handle;
        HRESULT hr = device11->CreateTexture2D(&desc, nullptr, &alias);
        if (SUCCEEDED(hr)) hr = alias.As(&resource);
        if (SUCCEEDED(hr)) hr = resource->CreateSharedHandle(nullptr,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle.value);
        if (SUCCEEDED(hr)) hr = device12->OpenSharedHandle(handle.value, IID_PPV_ARGS(&imported));
        if (FAILED(hr)) return reject(FrameGenerationInteropFailure::Allocation, hr);
        const auto actual = imported->GetDesc();
        if (actual.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || actual.Width != desc.Width ||
            actual.Height != desc.Height || actual.DepthOrArraySize != 1 || actual.MipLevels != 1 ||
            actual.Format != desc.Format || actual.SampleDesc.Count != 1 || actual.SampleDesc.Quality != 0)
            return reject(FrameGenerationInteropFailure::InvalidDescriptor);
        image.alias11 = std::move(alias); image.resource12 = std::move(imported);
        image.width = source.Width; image.height = source.Height; image.format = source.Format;
        return true;
    }
};

bool FrameGenerationInterop::retired() const noexcept {
    if (!impl_) return true;
    if (impl_->active >= 0) return false;
    for (const auto& slot : impl_->slots) if (!impl_->usable(slot)) return false;
    return true;
}
FrameGenerationInterop::~FrameGenerationInterop() {
    // Unproven GPU ownership keeps the ring alive. Orderly retirement releases
    // all source leases and imported textures without waiting on a game context.
    if (retired()) delete impl_;
}
bool FrameGenerationInterop::initialize(const FrameGenerationInteropInit& config) {
    if (!config.enabled) { initialFailure_ = FrameGenerationInteropFailure::Off; return false; }
    if (impl_) return false;
    if (!config.renderThreadId || config.renderThreadId != GetCurrentThreadId() || !config.context) {
        initialFailure_ = FrameGenerationInteropFailure::WrongThreadOrContext; return false;
    }
    if (!config.device11 || !config.colorWidth || !config.colorHeight || !config.guideWidth || !config.guideHeight) {
        initialFailure_ = FrameGenerationInteropFailure::InvalidDescriptor; return false;
    }
    std::unique_ptr<Impl> created(new Impl{});
    created->config = config; created->device11 = config.device11; created->originalContext = config.context;
    if (config.context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        initialFailure_ = FrameGenerationInteropFailure::WrongThreadOrContext; return false;
    }
    ComPtr<ID3D11Device> contextDevice;
    config.context->GetDevice(&contextDevice);
    if (!sameObject(config.device11, contextDevice.Get())) {
        initialFailure_ = FrameGenerationInteropFailure::WrongDevice; return false;
    }
    HRESULT hr = config.device11->QueryInterface(IID_PPV_ARGS(&created->device5));
    if (SUCCEEDED(hr)) hr = config.context->QueryInterface(IID_PPV_ARGS(&created->context4));
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC adapterDesc{};
    if (SUCCEEDED(hr)) hr = config.device11->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr)) hr = dxgi->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetDesc(&adapterDesc);
    if (FAILED(hr)) { initialFailure_ = FrameGenerationInteropFailure::Device; return false; }
    wchar_t path[32768]{};
    const UINT length = GetSystemDirectoryW(path, 32768);
    if (!length || length + 12 >= 32768) { initialFailure_ = FrameGenerationInteropFailure::Device; return false; }
    if (wcscat_s(path, L"\\d3d12.dll")) { initialFailure_ = FrameGenerationInteropFailure::Device; return false; }
    created->retainedModule = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    wchar_t actualPath[32768]{};
    const auto actualLength = created->retainedModule ? GetModuleFileNameW(created->retainedModule, actualPath, 32768) : 0;
    if (!actualLength || actualLength >= 32768 || _wcsicmp(path, actualPath)) {
        initialFailure_ = FrameGenerationInteropFailure::Device; return false;
    }
    const auto create = reinterpret_cast<CreateDevice12>(GetProcAddress(created->retainedModule, "D3D12CreateDevice"));
    if (!create || FAILED(create(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&created->device12)))) {
        initialFailure_ = FrameGenerationInteropFailure::Device; return false;
    }
    const auto luid = created->device12->GetAdapterLuid();
    if (luid.LowPart != adapterDesc.AdapterLuid.LowPart || luid.HighPart != adapterDesc.AdapterLuid.HighPart) {
        initialFailure_ = FrameGenerationInteropFailure::WrongAdapter; return false;
    }
    hr = created->device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&created->fence11));
    Handle fenceHandle;
    if (SUCCEEDED(hr)) hr = created->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle.value);
    if (SUCCEEDED(hr)) hr = created->device12->OpenSharedHandle(fenceHandle.value, IID_PPV_ARGS(&created->fence12));
    if (FAILED(hr)) { initialFailure_ = FrameGenerationInteropFailure::Fence; return false; }
    for (auto& slot : created->slots) {
        slot.storage = std::make_shared<FrameGenerationInteropLease::Impl>();
        slot.storage->device11 = created->device11; slot.storage->device12 = created->device12;
        slot.storage->fence11 = created->fence11; slot.storage->fence12 = created->fence12;
        slot.lease = std::shared_ptr<FrameGenerationInteropLease>(new FrameGenerationInteropLease(slot.storage));
    }
    created->status.initialized = true; created->status.failure = FrameGenerationInteropFailure::None;
    impl_ = created.release(); initialFailure_ = FrameGenerationInteropFailure::None;
    return true;
}
ID3D12Device* FrameGenerationInterop::device12() const noexcept { return impl_ ? impl_->device12.Get() : nullptr; }
bool FrameGenerationInterop::begin(ID3D11DeviceContext* context, std::uint64_t sequence, std::uint64_t generation) noexcept {
    if (!impl_ || !impl_->owner(context)) return false;
    if (impl_->active >= 0) return impl_->reject(FrameGenerationInteropFailure::Busy);
    if (!sequence || !generation || sequence <= impl_->lastSequence) return impl_->reject(FrameGenerationInteropFailure::InvalidSequence);
    const HRESULT removed = impl_->device12->GetDeviceRemovedReason();
    if (FAILED(removed)) return impl_->reject(FrameGenerationInteropFailure::DeviceRemoved, removed);
    for (unsigned index = 0; index < fg_interop::slots; ++index) {
        auto& slot = impl_->slots[index];
        if (!impl_->usable(slot)) continue;
        auto& data = *slot.storage;
        data.transaction = {sequence, generation, {0, 0}};
        data.producer = data.consumerValue = 0; data.consumerFence.Reset();
        data.published = false; data.consumer.store(Unsubmitted, std::memory_order_release);
        // usable() proved GPU retirement before any pooled source can return
        // to its producer. A COM reference alone cannot prevent pixel reuse.
        for (auto& image : data.images) { image.view = {}; image.source11.Reset(); image.sourceOwner.reset(); }
        impl_->active = static_cast<int>(index); impl_->lastSequence = sequence;
        impl_->status.failure = FrameGenerationInteropFailure::None;
        return true;
    }
    ++impl_->status.busy;
    return impl_->reject(FrameGenerationInteropFailure::Busy);
}
bool FrameGenerationInterop::capture(ID3D11DeviceContext* context, std::uint64_t sequence, std::uint64_t generation,
    unsigned eye, FrameGenerationPlane role, ID3D11Texture2D* source, std::uint64_t identity,
    std::shared_ptr<const void> sourceOwner) noexcept {
    if (!impl_ || !impl_->owner(context)) return false;
    if (impl_->active < 0) return impl_->reject(FrameGenerationInteropFailure::InvalidSequence);
    auto& data = *impl_->slots[impl_->active].storage;
    if (!data.transaction.accepts(sequence, generation, eye, role) || !source || !identity)
        return impl_->reject(FrameGenerationInteropFailure::InvalidSequence);
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    const bool guide = role == FrameGenerationPlane::Depth || role == FrameGenerationPlane::Motion;
    if (!fg_interop::descriptor(desc, guide ? impl_->config.guideWidth : impl_->config.colorWidth,
            guide ? impl_->config.guideHeight : impl_->config.colorHeight))
        return impl_->reject(FrameGenerationInteropFailure::InvalidDescriptor);
    if (!fg_interop::format(role, desc.Format)) return impl_->reject(FrameGenerationInteropFailure::InvalidFormat);
    ComPtr<ID3D11Device> sourceDevice; source->GetDevice(&sourceDevice);
    if (!sameObject(sourceDevice.Get(), impl_->device11.Get())) return impl_->reject(FrameGenerationInteropFailure::WrongDevice);
    auto capturedDesc = desc;
    capturedDesc.Format = fg_interop::captureFormat(role, desc.Format);
    const auto& other = data.images[(1 - eye) * fg_interop::planes + unsigned(role)];
    // Both eyes must expose the same immutable format. Compatible Color
    // TYPELESS/UNORM inputs therefore form the same canonical UNORM pair.
    if ((data.transaction.captured[1 - eye] & frameGenerationPlaneBit(role)) && other.view.sourceDesc.Format != capturedDesc.Format)
        return impl_->reject(FrameGenerationInteropFailure::InvalidFormat);
    auto& image = data.images[eye * fg_interop::planes + unsigned(role)];
    if (!impl_->image(image, capturedDesc)) return false;
    ComPtr<IUnknown> sourceIdentity;
    const HRESULT hr = source->QueryInterface(IID_PPV_ARGS(&sourceIdentity));
    if (FAILED(hr)) return impl_->reject(FrameGenerationInteropFailure::WrongDevice, hr);
    image.source11 = source;
    image.sourceOwner = std::move(sourceOwner);
    image.view = {image.alias11.Get(), image.resource12.Get(), capturedDesc, identity,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(sourceIdentity.Get())), D3D12_RESOURCE_STATE_COMMON};
    impl_->context4->CopyResource(image.alias11.Get(), source);
    data.transaction.captured[eye] |= frameGenerationPlaneBit(role);
    return true;
}
std::shared_ptr<FrameGenerationInteropLease> FrameGenerationInterop::publish(ID3D11DeviceContext* context,
    std::uint64_t sequence, std::uint64_t generation, unsigned mask) noexcept {
    if (!impl_ || !impl_->owner(context)) return {};
    if (impl_->active < 0) { impl_->reject(FrameGenerationInteropFailure::InvalidSequence); return {}; }
    auto& slot = impl_->slots[impl_->active];
    if (!slot.storage->transaction.paired(sequence, generation, mask)) {
        impl_->reject(FrameGenerationInteropFailure::Incomplete); return {};
    }
    const bool sealed = impl_->seal(*slot.storage);
    impl_->active = -1;
    if (!sealed) return {};
    slot.storage->published = true;
    ++impl_->status.published; impl_->status.failure = FrameGenerationInteropFailure::None;
    return slot.lease;
}
bool FrameGenerationInterop::abort(ID3D11DeviceContext* context, std::uint64_t sequence, std::uint64_t generation) noexcept {
    if (!impl_ || !impl_->owner(context)) return false;
    if (impl_->active < 0) return impl_->reject(FrameGenerationInteropFailure::InvalidSequence);
    auto& data = *impl_->slots[impl_->active].storage;
    if (!data.transaction.matches(sequence, generation)) return impl_->reject(FrameGenerationInteropFailure::InvalidSequence);
    const bool copied = data.transaction.captured[0] || data.transaction.captured[1];
    const bool sealed = !copied || impl_->seal(data);
    data.consumer.store(Canceled, std::memory_order_release); impl_->active = -1;
    return sealed;
}
FrameGenerationInteropStatus FrameGenerationInterop::status() const noexcept {
    if (impl_) return impl_->status;
    FrameGenerationInteropStatus status; status.failure = initialFailure_; return status;
}
}  // namespace edvr

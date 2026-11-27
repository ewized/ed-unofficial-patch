// CPU admission checks by default. --gpu exercises immutable captures with the
// production SMAA and FXAA filters on WARP.
// No game/runtime/NVAPI calls on either path.
#include "../../src/d3d11/ui_layer.h"
#include "../../src/d3d11/ui_layer_fg_adapter.h"
#include "../../src/d3d11/ui_layer_math.h"
#include "../../src/d3d11/ui_layer_shaders.h"
#include "temporal_shader_bytecode.h" // build-generated production bytecode include directory
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
static unsigned checks = 0;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) throw std::runtime_error(what);
}
static void hr(HRESULT result, const char* what) { check(SUCCEEDED(result), what); }

static void contracts() {
    using edvr::UiLayerFgRefusal;
    edvr::UiLayerFgFacts good;
    good.arguments = good.owner = good.available = good.sequence = true;
    good.eye = good.size = good.complete = true;
    check(edvr::uiLayerFgAdmission(good) == UiLayerFgRefusal::None, "authoritative empty admitted");
    good.hasUi = true;
    check(edvr::uiLayerFgAdmission(good) == UiLayerFgRefusal::None, "published UI admitted");
    struct Falsifier { bool edvr::UiLayerFgFacts::*member; bool value; UiLayerFgRefusal expected; };
    const Falsifier cases[] = {
        {&edvr::UiLayerFgFacts::arguments, false, UiLayerFgRefusal::InvalidArgument},
        {&edvr::UiLayerFgFacts::owner, false, UiLayerFgRefusal::ForeignOwner},
        {&edvr::UiLayerFgFacts::available, false, UiLayerFgRefusal::Unavailable},
        {&edvr::UiLayerFgFacts::sequence, false, UiLayerFgRefusal::WrongSequence},
        {&edvr::UiLayerFgFacts::eye, false, UiLayerFgRefusal::EyeUnproven},
        {&edvr::UiLayerFgFacts::size, false, UiLayerFgRefusal::SizeMismatch},
        {&edvr::UiLayerFgFacts::scope, true, UiLayerFgRefusal::ScopeActive},
        {&edvr::UiLayerFgFacts::ripple, true, UiLayerFgRefusal::RippleActive},
        {&edvr::UiLayerFgFacts::world, true, UiLayerFgRefusal::WorldRoute},
        {&edvr::UiLayerFgFacts::complete, false, UiLayerFgRefusal::IncompletePublication},
    };
    for (const auto& c : cases) for (bool content : {false, true}) {
        auto f = good; f.hasUi = content; f.*(c.member) = c.value;
        check(edvr::uiLayerFgAdmission(f) == c.expected, "unsafe empty/content refused");
    }
    // Pin the live wrapper's links to the adapter with deletion controls. The
    // GPU fixture below calls that shared adapter directly, without live g_eye.
    std::ifstream source("src/d3d11/ui_layer.cpp", std::ios::binary);
    check(bool(source), "production source available from tree root");
    std::string text((std::istreambuf_iterator<char>(source)), {});
    std::ifstream adapter("src/d3d11/ui_layer_fg_adapter.h", std::ios::binary);
    check(bool(adapter), "shared production adapter available");
    text.append(std::istreambuf_iterator<char>(adapter), std::istreambuf_iterator<char>());
    const char* pins[] = {
        "context->CopyResource(texture.Get(), source);",
        "renderBoundaryValidateOwner(device, context, true)",
        "e.submittedSeq == sequence && e.temporalOutSeq == sequence",
        "e.door.doorSeq == sequence && e.doorFromPass && e.compositedSeq != sequence",
        "facts.hasUi = e.seq == sequence && e.draws != 0",
        "e.hdrToneSeq == sequence", "e.wPubSeq == sequence",
        "frame == source.expectedFrame",
        "runSmooth(cache.smaa, context, source.layerView)",
        "uiLayerFxaaRun(cache.fxaa, context, source.layerView)",
        "facts.complete = !e.coveragePending",
        "facts.world = g_screenHeld == 1",
        "return ui_layer_fg::capture(context, frame, source, g_fgCaptureCache[eye], info)",
        "return ui_layer_fg::captureTo(context, frame, source, g_fgCaptureCache[eye], sink, cookie, info)",
        "return ui_layer_fg::retireStorage(g_fgCaptureCache, context, device.Get())",
    };
    for (const char* pin : pins) {
        const auto at = text.find(pin);
        check(at != std::string::npos, "snapshot production wiring");
        auto deleted = text;
        if (at != std::string::npos) deleted.erase(at, std::strlen(pin));
        check(deleted.find(pin) == std::string::npos, "wiring deletion control detects removal");
    }
    const auto observeStart = text.find("bool uiLayerFgObserveCoverage(");
    const auto observeEnd = text.find("\n}", observeStart);
    check(observeStart != std::string::npos && observeEnd != std::string::npos,
        "production HUD door exposes current coverage without a producer");
    const auto observe = text.substr(observeStart, observeEnd - observeStart);
    const auto admission = observe.find("uiLayerFgSource(nullptr, sequence, eye, frame, region, layerUv, facts, nullptr, nullptr)");
    check(observe.find("hasUi = false;") < admission && admission < observe.find("hasUi = facts.hasUi;") &&
        observe.find("hasUi = facts.hasUi;") != std::string::npos && observe.find("g_fgCaptureCache") == std::string::npos,
        "coverage reads current admitted draw facts without obtaining capture planes or storage");
    std::ifstream sharpen("src/d3d11/native_sharpen.cpp", std::ios::binary);
    check(bool(sharpen), "ordinary sharpen source available for door wiring");
    const std::string sharpenText((std::istreambuf_iterator<char>(sharpen)), {});
    const auto seen = sharpenText.find("uiLayerDoorSeen(seq,eye,source)");
    const auto submitted = sharpenText.find("if(fgBoundsSupported)edvr::nativeFgCaptureDoor(seq,eye,source,region,layerUv)", seen);
    const auto composited = sharpenText.find("uiLayerComposite(seq,eye,", submitted);
    check(seen < submitted && submitted < composited && composited != std::string::npos,
        "ordinary supported door reaches the coordinator before UI composition without an FG transaction check");
    std::ifstream coordinator("src/d3d11/frame_generation_coordinator.cpp", std::ios::binary);
    check(bool(coordinator), "coordinator source available for producer handoff wiring");
    const std::string body((std::istreambuf_iterator<char>(coordinator)), {});
    const auto function = [&](const char* signature) {
        const auto start = body.find(signature);
        const auto end = body.find("\n}", start);
        check(start != std::string::npos && end != std::string::npos, "coordinator function available for wiring check");
        return body.substr(start, end - start);
    };
    const auto captureDoor = function("void nativeFgCaptureDoor(");
    const auto observeCoverage = captureDoor.find("uiLayerFgObserveCoverage(");
    const auto requireTransaction = captureDoor.find("!c->transaction");
    const auto copyToInterop = captureDoor.find("uiLayerFgCaptureTo(");
    check(observeCoverage < requireTransaction && requireTransaction < copyToInterop && copyToInterop != std::string::npos,
        "coordinator observes coverage before demanding an expensive capture transaction");
    check(copyToInterop != std::string::npos &&
        captureDoor.find("uiLayerFgCapture(") == std::string::npos,
        "production HUD door delivers directly to its interop sink without a retained snapshot copy");
    const char* retireCall = "uiLayerFgRetireStorage(context)";
    auto creation = function("void nativeFgCaptureGuides(");
    const auto handoff = [&](const std::string& code) {
        const auto init = code.find("if (!made->interop.initialize(init)");
        const auto retire = code.find(retireCall);
        const auto assign = code.find("c->retiringProducer.replace(");
        const auto begin = code.find("c->producer->interop.begin(context");
        return init < retire && retire < assign && assign < begin && begin != std::string::npos &&
            code.find(retireCall, retire + 1) == std::string::npos;
    };
    check(handoff(creation), "storage retirement occurs once in new-producer creation before assignment and first begin");
    creation.erase(creation.find(retireCall), std::strlen(retireCall));
    check(!handoff(creation), "handoff wiring regression detects omitted generation retirement");

    const auto entry = function("void nativeFgServiceProducer(");
    const auto serviceCall = entry.find("serviceProducer(*c)");
    check(entry.find("generation == c->generation && owner(*c)") < serviceCall && serviceCall != std::string::npos,
        "producer service requires the current generation and render owner");
    const auto service = function("bool serviceProducer(");
    const auto retired = service.find("c.producer.use_count() != 1 || !c.producer->interop.retired()");
    const auto retireUi = service.find(retireCall);
    const auto reset = service.find("c.producer.reset()");
    check(retired < retireUi && retireUi < reset && reset != std::string::npos,
        "producer service waits for outstanding leases before retiring UI storage and releasing the producer");
}

using Pixel = std::array<unsigned char, 4>;
struct Image { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11UnorderedAccessView> uav; };
static Image image(ID3D11Device* device, Pixel pixel, bool writable = false,
    unsigned width = 8, unsigned height = 8, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    std::vector<Pixel> pixels(static_cast<size_t>(width) * height, pixel);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width; d.Height = height; d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = format; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (writable) d.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SUBRESOURCE_DATA data{pixels.data(), width * static_cast<UINT>(sizeof(Pixel)), 0};
    Image out;
    hr(device->CreateTexture2D(&d, &data, &out.tex), "pixel texture");
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels = 1;
    hr(device->CreateShaderResourceView(out.tex.Get(), format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? &view : nullptr,
        &out.srv), "pixel view");
    if (writable) hr(device->CreateUnorderedAccessView(out.tex.Get(), nullptr, &out.uav), "pixel UAV");
    return out;
}
static Pixel read(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags = desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    hr(device->CreateTexture2D(&desc, nullptr, &staging), "readback texture");
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map{};
    hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "pixel readback");
    Pixel value{}; std::memcpy(value.data(), map.pData, sizeof(value));
    context->Unmap(staging.Get(), 0); return value;
}
// Read every pixel, accounting for driver row padding. Map also supplies the
// completion proof before a fixture releases its snapshot for slot reuse.
static std::vector<Pixel> readAll(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags = desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    hr(device->CreateTexture2D(&desc, nullptr, &staging), "full readback texture");
    context->CopyResource(staging.Get(), texture);
    std::vector<Pixel> pixels(static_cast<size_t>(desc.Width) * desc.Height);
    D3D11_MAPPED_SUBRESOURCE map{};
    hr(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "full pixel readback");
    for (unsigned y = 0; y < desc.Height; ++y)
        std::memcpy(pixels.data() + static_cast<size_t>(y) * desc.Width,
            static_cast<const unsigned char*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
            static_cast<size_t>(desc.Width) * sizeof(Pixel));
    context->Unmap(staging.Get(), 0); return pixels;
}
static std::vector<Pixel> pixels(unsigned width, unsigned height, unsigned seed) {
    std::vector<Pixel> out(static_cast<size_t>(width) * height);
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x)
        for (unsigned c = 0; c < 4; ++c)
            out[static_cast<size_t>(y) * width + x][c] = static_cast<unsigned char>(seed + x * 13 + y * 29 + c * 47);
    return out;
}
static void write(ID3D11DeviceContext* context, ID3D11Texture2D* texture, const std::vector<Pixel>& value) {
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    check(value.size() == static_cast<size_t>(desc.Width) * desc.Height, "full source update dimensions");
    context->UpdateSubresource(texture, 0, nullptr, value.data(), desc.Width * static_cast<UINT>(sizeof(Pixel)), 0);
}
// The preceding pool has no handoff operation. A no-op fallback lets this same
// pixel regression demonstrate its runtime failure against that source.
template<class Cache>
static auto retireUiStorage(Cache (&caches)[2], ID3D11DeviceContext* context, ID3D11Device* device, int)
    -> decltype(retireStorage(caches, context, device)) { return retireStorage(caches, context, device); }
template<class Cache>
static bool retireUiStorage(Cache (&)[2], ID3D11DeviceContext*, ID3D11Device*, long) { return true; }
static void retirementCases(ID3D11Device* device, ID3D11DeviceContext* context) {
    namespace adapter = edvr::ui_layer_fg;
    adapter::CaptureCache caches[2];
    auto scene = image(device, {}), layer = image(device, {}), multiply = image(device, {});
    adapter::Source source;
    source.facts.arguments = source.facts.owner = source.facts.available = source.facts.sequence = true;
    source.facts.eye = source.facts.size = source.facts.complete = source.facts.hasUi = true;
    source.info.sequence = 1; source.expectedFrame = scene.tex.Get(); source.useMult = true;
    source.layer = layer.tex.Get(); source.layerView = layer.srv.Get();
    source.multiply = multiply.tex.Get(); source.multiplyView = multiply.srv.Get();
    std::shared_ptr<const edvr::UiLayerFgSnapshot> old[2][3], current[2][3];
    std::vector<Pixel> expected[2][3];
    check(retireUiStorage(caches, context, device, 0), "initial producer retirement accepts empty unbound caches");
    for (unsigned eye = 0; eye < 2; ++eye) for (unsigned slot = 0; slot < 3; ++slot) {
        source.info.eye = eye; ++source.info.sequence; expected[eye][slot] = pixels(8, 8, eye * 51 + slot * 17);
        write(context, layer.tex.Get(), expected[eye][slot]); write(context, multiply.tex.Get(), expected[eye][slot]);
        old[eye][slot] = adapter::capture(context, scene.tex.Get(), source, caches[eye], nullptr);
        check(bool(old[eye][slot]), "old generation fills three UI snapshots per eye");
    }
    edvr::UiLayerSmaaFail failure;
    check(caches[0].ensure(8, 8, &failure), "generation fixture warms actual SMAA cache");
    const auto smaaOutput = caches[0].smaa.out, smaaLookup = caches[0].smaa.area;
    const auto smaaShader = caches[0].smaa.edgesCs;
    check(retireUiStorage(caches, context, device, 0), "producer handoff retires both old shared cache references");
    check(caches[0].smaa.out.Get() == smaaOutput.Get() && caches[0].smaa.area.Get() == smaaLookup.Get() &&
        caches[0].smaa.edgesCs.Get() == smaaShader.Get() && caches[0].builderCalls == 1,
        "producer handoff preserves actual SMAA targets lookup tables shaders and builder state");
    for (unsigned eye = 0; eye < 2; ++eye) for (unsigned slot = 0; slot < 3; ++slot) {
        source.info.eye = eye; ++source.info.sequence;
        current[eye][slot] = adapter::capture(context, scene.tex.Get(), source, caches[eye], nullptr);
        check(bool(current[eye][slot]), "new generation captures while every old snapshot remains held");
        check(readAll(device, context, old[eye][slot]->layer.Get()) == expected[eye][slot] &&
            readAll(device, context, old[eye][slot]->multiply.Get()) == expected[eye][slot], "old generation exact pixels survive shared cache retirement");
    }
    bool foreign = true;
    std::thread otherThread([&] { foreign = retireUiStorage(caches, context, device, 0); }); otherThread.join();
    check(!foreign, "foreign-thread handoff cannot retire either eye");
    ComPtr<ID3D11Device> otherDevice; ComPtr<ID3D11DeviceContext> otherContext;
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &otherDevice, nullptr, &otherContext), "retirement foreign device");
    check(!retireUiStorage(caches, otherContext.Get(), otherDevice.Get(), 0) &&
        !retireUiStorage(caches, context, otherDevice.Get(), 0), "foreign context and device cannot retire either eye");
    for (auto& cache : caches) check(!cache.acquire(), "refused handoffs preserve both full three-slot bounds");
    adapter::CaptureCache mismatch[2];
    check(mismatch[0].bind(context, device, 0) && mismatch[1].bind(context, device, 0), "second-eye ownership mismatch fixture");
    auto firstHeld = mismatch[0].acquire();
    check(!retireUiStorage(mismatch, context, device, 0) && firstHeld.use_count() == 2,
        "second-eye preflight refusal leaves first-eye leased cache reference intact");
    check(mismatch[0].resetOwned(), "clear first preflight fixture binding");
    check(!retireUiStorage(mismatch, context, device, 0) && mismatch[0].bind(otherContext.Get(), otherDevice.Get(), 0),
        "failed pair preflight cannot claim an initially unbound eye");
    for (auto& cache : mismatch) check(cache.resetOwned(), "owned preflight fixture cleanup");
    ComPtr<ID3D11Texture2D> remembered[2]; uint64_t allocations[2]{};
    for (unsigned eye = 0; eye < 2; ++eye) {
        remembered[eye] = current[eye][0]->layer; allocations[eye] = caches[eye].textureAllocations;
        for (auto& snapshot : current[eye]) snapshot.reset();
    }
    check(retireUiStorage(caches, context, device, 0), "handoff keeps cache-exclusive complete storage");
    for (unsigned eye = 0; eye < 2; ++eye) {
        source.info.eye = eye;
        auto reused = adapter::capture(context, scene.tex.Get(), source, caches[eye], nullptr);
        check(reused && reused->layer.Get() == remembered[eye].Get() && caches[eye].textureAllocations == allocations[eye],
            "exclusive matching planes survive handoff and ordinary capture without new allocation");
    }
}
static void poolCases(ID3D11Device* device, ID3D11DeviceContext* context) {
    namespace adapter = edvr::ui_layer_fg;
    ComPtr<ID3D11Device> otherDevice; ComPtr<ID3D11DeviceContext> otherContext;
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &otherDevice, nullptr, &otherContext), "pool lifecycle device");
    for (unsigned eye = 0; eye < 2; ++eye) {
        adapter::CaptureCache cache;
        auto scene = image(device, {}), layer = image(device, {}), multiply = image(device, {});
        adapter::Source source;
        source.facts.arguments = source.facts.owner = source.facts.available = source.facts.sequence = true;
        source.facts.eye = source.facts.size = source.facts.complete = source.facts.hasUi = true;
        source.info.sequence = 1; source.info.eye = eye; source.expectedFrame = scene.tex.Get();
        source.layer = layer.tex.Get(); source.layerView = layer.srv.Get();
        source.multiply = multiply.tex.Get(); source.multiplyView = multiply.srv.Get(); source.useMult = true;
        edvr::UiLayerFgInfo info;
        const auto initialLayer = pixels(8, 8, 3), initialMultiply = pixels(8, 8, 7);
        write(context, layer.tex.Get(), initialLayer); write(context, multiply.tex.Get(), initialMultiply);
        auto initial = adapter::capture(context, scene.tex.Get(), source, cache, &info);
        check(initial && readAll(device, context, initial->layer.Get()) == initialLayer &&
            readAll(device, context, initial->multiply.Get()) == initialMultiply, "pool initial exact planes");
        // Keep COM objects alive to prevent allocator address recycling. This
        // deterministically fails on the original always-allocate adapter.
        const auto rememberedLayer = initial->layer, rememberedMultiply = initial->multiply;
        const auto rememberedLayerView = initial->layerSrv, rememberedMultiplyView = initial->multiplySrv;
        std::weak_ptr<const edvr::UiLayerFgSnapshot> oldPublication = initial;
        initial.reset();
        check(oldPublication.expired(), "pool retains storage rather than old publication metadata");
        std::array<std::shared_ptr<const edvr::UiLayerFgSnapshot>, 3> held;
        std::array<std::vector<Pixel>, 3> expectedLayer, expectedMultiply;
        for (unsigned slot = 0; slot < held.size(); ++slot) {
            expectedLayer[slot] = pixels(8, 8, 19 + slot * 31);
            expectedMultiply[slot] = pixels(8, 8, 23 + slot * 37);
            write(context, layer.tex.Get(), expectedLayer[slot]); write(context, multiply.tex.Get(), expectedMultiply[slot]);
            ++source.info.sequence;
            held[slot] = adapter::capture(context, scene.tex.Get(), source, cache, &info);
            check(held[slot] && info.fullSupported, "pool captures distinct held publication");
            if (!slot) check(held[0]->layer.Get() == rememberedLayer.Get() &&
                held[0]->multiply.Get() == rememberedMultiply.Get() && held[0]->layerSrv.Get() == rememberedLayerView.Get() &&
                held[0]->multiplySrv.Get() == rememberedMultiplyView.Get(), "retired same-size planes reuse texture and SRV allocations");
            for (unsigned prior = 0; prior <= slot; ++prior)
                check(readAll(device, context, held[prior]->layer.Get()) == expectedLayer[prior] &&
                    readAll(device, context, held[prior]->multiply.Get()) == expectedMultiply[prior],
                    "all held layer and multiply pixels remain exact across captures");
        }
        write(context, layer.tex.Get(), pixels(8, 8, 211)); write(context, multiply.tex.Get(), pixels(8, 8, 223));
        check(!adapter::capture(context, scene.tex.Get(), source, cache, &info) &&
            info.refusal == edvr::UiLayerFgRefusal::CopyFailed && !info.fullSupported, "three leased slots refuse overwrite for native fallback");
        auto emptySource = source; emptySource.facts.hasUi = false; emptySource.useMult = false;
        auto empty = adapter::capture(context, scene.tex.Get(), emptySource, cache, &info);
        check(empty && info.empty && !empty->layer && !empty->multiply, "authoritative empty capture needs no pooled storage");
        const auto releasedLayer = held[1]->layer, releasedMultiply = held[1]->multiply;
        std::weak_ptr<const edvr::UiLayerFgSnapshot> releasedPublication = held[1];
        // Worker-side destruction returns ownership only; it makes no calls on
        // the producer context. Every fixture read above completed its GPU use.
        std::thread release([value = std::move(held[1])]() mutable { value.reset(); }); release.join();
        check(releasedPublication.expired(), "foreign-thread final publication release completes");
        expectedLayer[1] = pixels(8, 8, 67); expectedMultiply[1] = pixels(8, 8, 71);
        write(context, layer.tex.Get(), expectedLayer[1]); write(context, multiply.tex.Get(), expectedMultiply[1]);
        held[1] = adapter::capture(context, scene.tex.Get(), source, cache, &info);
        check(held[1] && held[1]->layer.Get() == releasedLayer.Get() && held[1]->multiply.Get() == releasedMultiply.Get(),
            "only released slot returns to capture");
        for (unsigned slot = 0; slot < held.size(); ++slot)
            check(readAll(device, context, held[slot]->layer.Get()) == expectedLayer[slot] &&
                readAll(device, context, held[slot]->multiply.Get()) == expectedMultiply[slot], "exhaustion and reuse preserve every held pixel");
        held[1].reset();
        auto singleSource = source; singleSource.useMult = false;
        auto single = adapter::capture(context, scene.tex.Get(), singleSource, cache, &info);
        check(single && !single->useMult && !single->multiply && !single->multiplySrv &&
            readAll(device, context, single->layer.Get()) == expectedLayer[1], "unused retained multiply is absent from fresh publication");
        single.reset();
        expectedMultiply[1] = pixels(8, 8, 79); write(context, multiply.tex.Get(), expectedMultiply[1]);
        single = adapter::capture(context, scene.tex.Get(), source, cache, &info);
        check(single && single->multiply.Get() == releasedMultiply.Get() &&
            readAll(device, context, single->multiply.Get()) == expectedMultiply[1], "returning multiply reuses storage with current pixels");
        single.reset();
        auto resizedScene = image(device, {}, false, 13, 9), resizedLayer = image(device, {}, false, 13, 9);
        auto resizedMultiply = image(device, {}, false, 13, 9);
        const auto resizedPixels = pixels(13, 9, 83), resizedTint = pixels(13, 9, 89);
        write(context, resizedLayer.tex.Get(), resizedPixels); write(context, resizedMultiply.tex.Get(), resizedTint);
        auto resizedSource = source; resizedSource.expectedFrame = resizedScene.tex.Get();
        resizedSource.layer = resizedLayer.tex.Get(); resizedSource.layerView = resizedLayer.srv.Get();
        resizedSource.multiply = resizedMultiply.tex.Get(); resizedSource.multiplyView = resizedMultiply.srv.Get();
        auto resized = adapter::capture(context, resizedScene.tex.Get(), resizedSource, cache, &info);
        check(resized && info.width == 13 && info.height == 9 && resized->layer.Get() != releasedLayer.Get() &&
            resized->multiply.Get() != releasedMultiply.Get() && readAll(device, context, resized->layer.Get()) == resizedPixels &&
            readAll(device, context, resized->multiply.Get()) == resizedTint, "resize rebuilds only available storage with exact pixels");
        check(!adapter::capture(context, resizedScene.tex.Get(), resizedSource, cache, &info), "resized leased slot also participates in bound");
        resized.reset();
        auto typedLayer = image(device, {}, false, 8, 8, DXGI_FORMAT_R8G8B8A8_TYPELESS);
        const auto typedPixels = pixels(8, 8, 97); write(context, typedLayer.tex.Get(), typedPixels);
        auto typedSource = source; typedSource.layer = typedLayer.tex.Get(); typedSource.layerView = typedLayer.srv.Get();
        auto typed = adapter::capture(context, scene.tex.Get(), typedSource, cache, &info);
        check(typed && readAll(device, context, typed->layer.Get()) == typedPixels, "texture format change rebuilds exact storage");
        const auto unormTexture = typed->layer;
        const auto unormView = typed->layerSrv;
        typed.reset();
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; typedLayer.srv->GetDesc(&sd); sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        ComPtr<ID3D11ShaderResourceView> srgb;
        hr(device->CreateShaderResourceView(typedLayer.tex.Get(), &sd, &srgb), "alternate typed source view");
        typedSource.layerView = srgb.Get();
        typed = adapter::capture(context, scene.tex.Get(), typedSource, cache, &info);
        check(typed && typed->layer.Get() != unormTexture.Get() && typed->layerSrv.Get() != unormView.Get() &&
            readAll(device, context, typed->layer.Get()) == typedPixels, "changed SRV interpretation cannot reuse stale view");
        typed->layerSrv->GetDesc(&sd);
        check(sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "snapshot retains exact source view format");
        check(cache.resetOwned(), "pool reset drops cache ownership while old snapshots remain leased");
        auto newScene = image(otherDevice.Get(), {}, false, 17, 3), newLayer = image(otherDevice.Get(), {}, false, 17, 3);
        const auto newPixels = pixels(17, 3, 109); write(otherContext.Get(), newLayer.tex.Get(), newPixels);
        auto newSource = source; newSource.expectedFrame = newScene.tex.Get(); newSource.useMult = false;
        newSource.layer = newLayer.tex.Get(); newSource.layerView = newLayer.srv.Get();
        auto newSnapshot = adapter::capture(otherContext.Get(), newScene.tex.Get(), newSource, cache, &info);
        check(newSnapshot && newSnapshot->device.Get() == otherDevice.Get() && !newSnapshot->multiply &&
            readAll(otherDevice.Get(), otherContext.Get(), newSnapshot->layer.Get()) == newPixels, "new device captures independently after reset");
        check(!adapter::capture(context, scene.tex.Get(), source, cache, &info) &&
            info.refusal == edvr::UiLayerFgRefusal::ForeignOwner, "old device rejected after pool rebind");
        for (unsigned slot : {0u, 2u})
            check(readAll(device, context, held[slot]->layer.Get()) == expectedLayer[slot] &&
                readAll(device, context, held[slot]->multiply.Get()) == expectedMultiply[slot],
                "held old-device planes survive resize reset and device replacement");
        check(readAll(device, context, typed->layer.Get()) == typedPixels, "held typed plane survives device replacement");
        check(cache.resetOwned() && readAll(otherDevice.Get(), otherContext.Get(), newSnapshot->layer.Get()) == newPixels,
            "new-device held snapshot survives its own cache retirement");
    }
}
static void directCaptureCases(ID3D11Device* device, ID3D11DeviceContext* context) {
    namespace adapter = edvr::ui_layer_fg;
    const Pixel hud{32, 48, 64, 128}, tint{64, 128, 192, 255}, changed{96, 80, 72, 200};
    const auto paint = [&](ID3D11Texture2D* texture, Pixel pixel) {
        std::array<Pixel, 64> pixels;
        pixels.fill(pixel);
        context->UpdateSubresource(texture, 0, nullptr, pixels.data(), 8 * sizeof(Pixel), 0);
    };
    for (const auto mode : {edvr::HudAaMode::kOff, edvr::HudAaMode::kSmaa, edvr::HudAaMode::kFxaa}) {
        auto live = image(device, hud), multiply = image(device, tint), scene = image(device, {});
        auto otherEye = image(device, {});
        auto firstLayer = image(device, {}), firstMult = image(device, {});
        auto nextLayer = image(device, {}), nextMult = image(device, {});
        auto partialLayer = image(device, {}), unusedMult = image(device, {});
        adapter::Source source;
        source.facts.arguments = source.facts.owner = source.facts.available = source.facts.sequence = true;
        source.facts.eye = source.facts.size = source.facts.complete = source.facts.hasUi = true;
        source.info.eye = 1; source.info.sequence = 61; source.expectedFrame = scene.tex.Get();
        source.layer = live.tex.Get(); source.layerView = live.srv.Get();
        source.multiply = multiply.tex.Get(); source.multiplyView = multiply.srv.Get(); source.useMult = true;
        source.smooth = mode == edvr::HudAaMode::kSmaa;
        source.fxaa = mode == edvr::HudAaMode::kFxaa;

        adapter::CaptureCache cache;
        std::array<std::shared_ptr<const edvr::UiLayerFgSnapshot>, 3> references;
        for (auto& reference : references) {
            reference = adapter::capture(context, scene.tex.Get(), source, cache, nullptr);
            check(bool(reference), "retained slots have independent reference pixels");
        }
        const uint64_t allocations = cache.textureAllocations;
        const uint64_t reuses = cache.planeReuses, misses = cache.poolMisses;
        auto* preparedLayer = source.smooth ? cache.smaa.out.Get() :
            source.fxaa ? cache.fxaa.out.Get() : live.tex.Get();

        // This sink records the actual source identities and issues one
        // destination copy per accepted plane on the producer context.
        struct Sink {
            ID3D11DeviceContext* context;
            ID3D11Texture2D* layer;
            ID3D11Texture2D* multiply;
            ID3D11Texture2D* expectedLayer;
            ID3D11Texture2D* expectedMultiply;
            unsigned calls = 0, copies = 0;
            bool acceptMultiply = true;
            bool sourceMatches = false;
        };
        const auto submit = +[](void* cookie, const edvr::UiLayerFgPlanes& planes) noexcept {
            auto& sink = *static_cast<Sink*>(cookie);
            ++sink.calls;
            if (!planes.layer || !planes.multiply || !planes.useMult) return false;
            sink.sourceMatches = planes.layer == sink.expectedLayer && planes.multiply == sink.expectedMultiply;
            sink.context->CopyResource(sink.layer, planes.layer);
            ++sink.copies;
            if (!sink.acceptMultiply) return false;
            sink.context->CopyResource(sink.multiply, planes.multiply);
            ++sink.copies;
            return true;
        };
        Sink first{context, firstLayer.tex.Get(), firstMult.tex.Get(), preparedLayer, multiply.tex.Get()};
        edvr::UiLayerFgInfo info;
        check(!adapter::captureTo(context, otherEye.tex.Get(), source, cache, submit, &first, &info) &&
            info.refusal == edvr::UiLayerFgRefusal::EyeUnproven && !first.calls,
            "wrong-eye source never reaches an interop sink");
        auto wrongView = source;
        wrongView.multiplyView = live.srv.Get();
        check(!adapter::captureTo(context, scene.tex.Get(), wrongView, cache, submit, &first, &info) &&
            info.refusal == edvr::UiLayerFgRefusal::SizeMismatch && !first.calls,
            "wrong multiply view never reaches an interop sink");

        Sink partial{context, partialLayer.tex.Get(), unusedMult.tex.Get(), preparedLayer, multiply.tex.Get()};
        partial.acceptMultiply = false;
        check(!adapter::captureTo(context, scene.tex.Get(), source, cache, submit, &partial, &info) &&
            partial.calls == 1 && partial.copies == 1 && !info.fullSupported &&
            info.refusal == edvr::UiLayerFgRefusal::CopyFailed,
            "refusal after one plane copy reports an incomplete publication to the caller");
        check(adapter::captureTo(context, scene.tex.Get(), source, cache, submit, &first, &info) &&
            first.calls == 1 && first.copies == 2 && info.fullSupported && !info.empty &&
            info.smoothingApplied == (source.smooth || source.fxaa) &&
            info.sequence == 61 && info.eye == 1 && info.width == 8 && info.height == 8 &&
            info.ownerThread == GetCurrentThreadId() && info.sourceDevice == reinterpret_cast<uintptr_t>(device) &&
            first.sourceMatches,
            "valid publication queues just one copy per actual final HUD or multiply source");

        // Reuse both live objects and the AA cache before reading back either
        // capture. Both destinations must retain the pixels at their door.
        paint(live.tex.Get(), changed);
        paint(multiply.tex.Get(), changed);
        ++source.info.sequence;
        Sink next{context, nextLayer.tex.Get(), nextMult.tex.Get(), preparedLayer, multiply.tex.Get()};
        check(adapter::captureTo(context, scene.tex.Get(), source, cache, submit, &next, &info) &&
            next.copies == 2 && info.sequence == 62 && next.sourceMatches,
            "next frame can reuse the exact producer texture after the copy is enqueued");
        check(cache.textureAllocations == allocations && cache.planeReuses == reuses && cache.poolMisses == misses,
            "direct capture never allocates, rewrites or acquires occupied retained slots");
        const auto referencePixels = readAll(device, context, references[0]->layer.Get());
        check(readAll(device, context, firstLayer.tex.Get()) == referencePixels &&
            readAll(device, context, firstMult.tex.Get()) == readAll(device, context, references[0]->multiply.Get()) &&
            readAll(device, context, partialLayer.tex.Get()) == referencePixels &&
            read(device, context, unusedMult.tex.Get()) == Pixel{},
            "accepted copies precede producer reuse and a declined second copy leaves its destination untouched");
        check(readAll(device, context, nextLayer.tex.Get()) == readAll(device, context, preparedLayer) &&
            read(device, context, nextMult.tex.Get()) == changed,
            "next frame contains the replacement source pixels");
    }
}

static void gpu() {
    namespace adapter = edvr::ui_layer_fg;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context), "WARP device");
    poolCases(device.Get(), context.Get());
    retirementCases(device.Get(), context.Get());
    directCaptureCases(device.Get(), context.Get());
    ComPtr<ID3D11ComputeShader> sentinelShader;
    hr(device->CreateComputeShader(edvr::kUiLayerCompositeInPlaceBytecode, sizeof(edvr::kUiLayerCompositeInPlaceBytecode),
        nullptr, &sentinelShader), "distinct sentinel CS");
    edvr::UiLayerCompositeParams p{};
    p.region[2] = p.region[3] = 8; p.uv[2] = p.uv[3] = 1;
    p.layerSize[0] = p.layerSize[1] = 8; p.outSize[0] = p.outSize[1] = 8; p.useMult = 1;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(p); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{&p, 0, 0}; ComPtr<ID3D11Buffer> sentinelConstants;
    hr(device->CreateBuffer(&bd, &initial, &sentinelConstants), "sentinel constants");
    const Pixel hud{32, 48, 64, 128}, tint{64, 128, 192, 255}, world{80, 100, 120, 255};
    auto live = image(device.Get(), hud), liveMult = image(device.Get(), tint);
    auto scene = image(device.Get(), world), wrongEye = image(device.Get(), world);
    auto sent0 = image(device.Get(), {}), sent1 = image(device.Get(), {}), sent2 = image(device.Get(), {});
    auto sentOut = image(device.Get(), {}, true);
    ComPtr<ID3D11SamplerState> sampler;
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr(device->CreateSamplerState(&sd, &sampler), "sentinel sampler");
    const auto bindSentinels = [&] {
        ID3D11Buffer* cb = sentinelConstants.Get();
        ID3D11ShaderResourceView* inputs[3] = {sent0.srv.Get(), sent1.srv.Get(), sent2.srv.Get()};
        ID3D11UnorderedAccessView* target = sentOut.uav.Get();
        ID3D11SamplerState* samplers[2] = {sampler.Get(), sampler.Get()};
        context->CSSetShader(sentinelShader.Get(), nullptr, 0); context->CSSetConstantBuffers(0, 1, &cb);
        context->CSSetShaderResources(0, 3, inputs); context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
        context->CSSetSamplers(0, 2, samplers);
    };
    const auto sentinels = [&] {
        ComPtr<ID3D11ComputeShader> cs; ComPtr<ID3D11Buffer> cb; ComPtr<ID3D11UnorderedAccessView> uav;
        context->CSGetShader(&cs, nullptr, nullptr); context->CSGetConstantBuffers(0, 1, &cb);
        context->CSGetUnorderedAccessViews(0, 1, &uav);
        check(cs.Get() == sentinelShader.Get() && cb.Get() == sentinelConstants.Get() && uav.Get() == sentOut.uav.Get(),
            "actual adapter restores sentinel CS/CB0/UAV0");
        ID3D11ShaderResourceView* srvs[3]{}; context->CSGetShaderResources(0, 3, srvs);
        const bool same = srvs[0] == sent0.srv.Get() && srvs[1] == sent1.srv.Get() && srvs[2] == sent2.srv.Get();
        for (auto* view : srvs) if (view) view->Release();
        check(same, "actual adapter restores sentinel SRV0..2");
        ID3D11SamplerState* ss[2]{}; context->CSGetSamplers(0, 2, ss);
        const bool sameSamplers = ss[0] == sampler.Get() && ss[1] == sampler.Get();
        for (auto* state : ss) if (state) state->Release();
        check(sameSamplers, "actual adapter restores samplers0..1");
    };
    adapter::CaptureCache cache;
    adapter::Source source;
    source.facts.arguments = source.facts.owner = source.facts.available = source.facts.sequence = true;
    source.facts.eye = source.facts.size = source.facts.complete = source.facts.hasUi = true;
    source.info.sequence = 42; source.info.eye = 0; source.expectedFrame = scene.tex.Get();
    source.layer = live.tex.Get(); source.layerView = live.srv.Get();
    source.multiply = liveMult.tex.Get(); source.multiplyView = liveMult.srv.Get(); source.useMult = true;
    edvr::UiLayerFgInfo info;
    bindSentinels();
    auto snapshot = adapter::capture(context.Get(), scene.tex.Get(), source, cache, &info);
    check(snapshot && info.fullSupported && !info.empty, "actual production capture"); sentinels();
    check(!adapter::capture(context.Get(), wrongEye.tex.Get(), source, cache, &info) &&
        info.refusal == edvr::UiLayerFgRefusal::EyeUnproven, "same-size same-device wrong-eye frame rejected"); sentinels();
    ComPtr<ID3D11DeviceContext> deferred; hr(device->CreateDeferredContext(0, &deferred), "deferred fixture");
    check(!adapter::capture(deferred.Get(), scene.tex.Get(), source, cache, &info) &&
        info.refusal == edvr::UiLayerFgRefusal::ForeignOwner, "deferred capture rejected"); sentinels();
    source.smooth = true;
    auto smooth = adapter::capture(context.Get(), scene.tex.Get(), source, cache, &info);
    check(smooth && info.smoothingApplied, "actual production SMAA capture"); sentinels();
    check(cache.builderCalls == 1 && cache.reused == 0, "first smoothed capture builds once");
    const auto cachedShader = cache.smaa.edgesCs;
    const auto cachedLookup = cache.smaa.area;
    const auto cachedOutput = cache.smaa.out;
    const Pixel smoothFirst = read(device.Get(), context.Get(), smooth->layer.Get());
    const auto smoothPixels = readAll(device.Get(), context.Get(), smooth->layer.Get());
    std::array<Pixel, 64> newUi; newUi.fill(Pixel{96, 80, 72, 200});
    context->UpdateSubresource(live.tex.Get(), 0, nullptr, newUi.data(), 8 * sizeof(Pixel), 0);
    auto smoothNext = adapter::capture(context.Get(), scene.tex.Get(), source, cache, &info);
    check(smoothNext && cache.builderCalls == 1 && cache.reused == 1, "same size reuses builder and resources"); sentinels();
    check(cache.smaa.edgesCs.Get() == cachedShader.Get() && cache.smaa.area.Get() == cachedLookup.Get() &&
        cache.smaa.out.Get() == cachedOutput.Get(), "SMAA shader LUT and target identities retained");
    check(smoothNext->layer.Get() != smooth->layer.Get() && smoothNext->layer.Get() != cache.smaa.out.Get() &&
        read(device.Get(), context.Get(), smoothNext->layer.Get()) != smoothFirst &&
        read(device.Get(), context.Get(), smooth->layer.Get()) == smoothFirst,
        "changed UI gets independent immutable smoothed pixels");
    smoothNext.reset(); // Leave a slot for the actual failure/exception calls.
    newUi.fill(hud); context->UpdateSubresource(live.tex.Get(), 0, nullptr, newUi.data(), 8 * sizeof(Pixel), 0);
    auto mismatched = source; mismatched.layerView = wrongEye.srv.Get();
    check(!adapter::capture(context.Get(), scene.tex.Get(), mismatched, cache, &info),
        "smoothed capture refuses SRV naming a different texture"); sentinels();
    check(!cache.bind(deferred.Get(), device.Get(), 0) && !cache.bind(context.Get(), device.Get(), 1),
        "cache refuses foreign context or eye");
    bool foreignBind = true, foreignReset = true;
    std::thread foreign([&] { foreignBind = cache.bind(context.Get(), device.Get(), 0); foreignReset = cache.resetOwned(); });
    foreign.join();
    check(!foreignBind && !foreignReset && cache.smaa.out.Get() == cachedOutput.Get(),
        "cache refuses foreign thread without clearing producer resources");
    static unsigned failedSmaaCalls = 0, threwSmaaCalls = 0;
    const auto failAfterSmaa = +[](edvr::UiLayerSmaa& smaa, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* layer) -> ID3D11ShaderResourceView* {
        ++failedSmaaCalls;
        edvr::uiLayerSmaaRun(smaa, ctx, layer); return nullptr;
    };
    check(!adapter::capture(context.Get(), scene.tex.Get(), source, cache, &info, failAfterSmaa), "capture failure after actual SMAA"); sentinels();
    check(failedSmaaCalls == 1, "failure injection reached SMAA rather than pool exhaustion");
    const auto throwAfterSmaa = +[](edvr::UiLayerSmaa& smaa, ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* layer) -> ID3D11ShaderResourceView* {
        ++threwSmaaCalls;
        edvr::uiLayerSmaaRun(smaa, ctx, layer); throw std::runtime_error("injected failure after SMAA");
    };
    check(!adapter::capture(context.Get(), scene.tex.Get(), source, cache, &info, throwAfterSmaa), "capture exception after actual SMAA"); sentinels();
    check(threwSmaaCalls == 1, "exception injection reached SMAA rather than pool exhaustion");
    auto recoveredSmooth = adapter::capture(context.Get(), scene.tex.Get(), source, cache, &info);
    check(recoveredSmooth && readAll(device.Get(), context.Get(), smooth->layer.Get()) == smoothPixels &&
        readAll(device.Get(), context.Get(), recoveredSmooth->layer.Get()) == smoothPixels,
        "SMAA failure storage recovers without changing held smoothed pixels"); sentinels();
    recoveredSmooth.reset();
    source.smooth = false;
    source.fxaa = true;
    auto fxaa = adapter::capture(context.Get(), scene.tex.Get(), source, cache, &info);
    check(fxaa && info.smoothingApplied && fxaa->layer.Get() != cache.fxaa.out.Get(),
        "FXAA publishes an immutable copy"); sentinels();
    check(readAll(device.Get(), context.Get(), fxaa->layer.Get()) ==
          readAll(device.Get(), context.Get(), cache.fxaa.out.Get()), "published FXAA pixels match the filter output");
    fxaa.reset();
    source.fxaa = false;
    std::array<Pixel, 64> changed; changed.fill(Pixel{200, 0, 0, 0});
    context->UpdateSubresource(live.tex.Get(), 0, nullptr, changed.data(), 8 * sizeof(Pixel), 0);
    context->UpdateSubresource(liveMult.tex.Get(), 0, nullptr, changed.data(), 8 * sizeof(Pixel), 0);
    check(read(device.Get(), context.Get(), snapshot->layer.Get()) == hud &&
          read(device.Get(), context.Get(), snapshot->multiply.Get()) == tint,
          "native world and HUD can advance while the worker uses the saved UI planes");
    source.facts.hasUi = false; source.useMult = false;
    source.layer = nullptr; source.layerView = nullptr; source.multiply = nullptr; source.multiplyView = nullptr;
    auto empty = adapter::capture(context.Get(), scene.tex.Get(), source, cache, &info);
    check(empty && info.empty && info.fullSupported, "actual authoritative empty capture"); sentinels();
    source.facts.complete = false;
    check(!adapter::capture(context.Get(), scene.tex.Get(), source, cache, &info), "missing publication is not empty"); sentinels();
    std::weak_ptr<const edvr::UiLayerFgSnapshot> lease = snapshot;
    auto retained = snapshot; snapshot.reset(); check(!lease.expired(), "consumer lease retains snapshot");
    retained.reset(); check(lease.expired(), "snapshot retires after final lease");
    check(cache.resetOwned() && !cache.smaa.ready() && !cache.fxaa.ready(), "owned lifecycle reset releases HUD antialiasing resources");
    check(readAll(device.Get(), context.Get(), smooth->layer.Get()) == smoothPixels, "snapshot survives cache reset");
    ComPtr<ID3D11Device> otherDevice; ComPtr<ID3D11DeviceContext> otherContext;
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &otherDevice, nullptr, &otherContext), "other lifecycle device");
    check(cache.bind(otherContext.Get(), otherDevice.Get(), 0), "owned reset permits new context/device binding");
    check(!cache.bind(context.Get(), device.Get(), 0), "wrong device/context rejected after rebind");
    check(cache.resetOwned(), "final owned cache retirement");
    check(read(device.Get(), context.Get(), scene.tex.Get()) == world, "source world unchanged");
}

int main(int argc, char** argv) {
    try {
        check(argc == 1 || (argc == 2 && !std::strcmp(argv[1], "--gpu")), "optional --gpu only");
        contracts();
        if (argc == 2) gpu();
        std::printf("PASS ui_layer_fg_test: %u checks; WARP=%s; live calls=0\n", checks, argc == 2 ? "yes" : "not run");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL ui_layer_fg_test: %s\n", e.what()); return 1; }
}

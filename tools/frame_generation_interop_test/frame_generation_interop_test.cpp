#include "../../src/d3d11/frame_generation_interop.h"
#include <cstdio>
#include <limits>
#include <type_traits>

namespace {
unsigned checks{}, failures{};
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", label); }
}
}
int main() {
    using namespace edvr;
    using namespace edvr::fg_interop;
    FrameGenerationInterop off;
    FrameGenerationInteropInit disabled;
    // Poison values prove OFF returns before querying any COM interface.
    disabled.device11 = reinterpret_cast<ID3D11Device*>(std::uintptr_t{1});
    disabled.context = reinterpret_cast<ID3D11DeviceContext*>(std::uintptr_t{2});
    check(!off.initialize(disabled) && !off.device12() && !off.status().initialized &&
        off.status().failure == FrameGenerationInteropFailure::Off,
        "default OFF avoids loader, COM access and allocation");
    check(!off.begin(disabled.context, 1, 1) &&
        !off.capture(disabled.context, 1, 1, 0, FrameGenerationPlane::Color, nullptr, 1) &&
        !off.publish(disabled.context, 1, 1, frameGenerationRequiredPlanes),
        "uninitialized publish/capture cannot enter device or context");
    check(!std::is_copy_constructible<FrameGenerationInteropLease>::value,
        "external lease copies cannot bypass ring shared ownership count");
    const void* context = reinterpret_cast<void*>(std::uintptr_t{1});
    check(owner(7, 7, context, context), "accept authoritative render thread and exact context");
    check(!owner(7, 8, context, context) && !owner(0, 0, context, context) &&
        !owner(7, 7, context, nullptr) && !owner(7, 7, context, reinterpret_cast<void*>(std::uintptr_t{2})),
        "reject foreign thread, absent owner and foreign context");
    const auto removed = std::numeric_limits<std::uint64_t>::max();
    check(complete(9, 9) && complete(10, 9) && !complete(8, 9) && !complete(removed, 9) && !complete(0, 0),
        "UINT64_MAX removal sentinel is never successful completion");
    check(reusable(1, 10, 9, false, true, 20, 19), "reuse after both GPU completion points and release");
    check(!reusable(2, 10, 9, false, true, 20, 19), "external consumer lease prevents overwrite");
    check(!reusable(1, 8, 9, false, true, 20, 19), "producer GPU must complete before reuse");
    check(!reusable(1, 10, 9, false, true, 18, 19), "consumer GPU must complete before reuse");
    check(!reusable(1, 10, 9, false, false, 20, 19), "released lease without retirement proof stays busy");
    check(reusable(1, 10, 9, true, false, 0, 0) && !reusable(1, 8, 9, true, false, 0, 0),
        "explicit no-consumer cancel still waits nonblockingly for producer completion");
    check(!reusable(1, removed, 0, true, false, 0, 0) &&
        !reusable(1, 10, 9, false, true, removed, 19), "removed devices never free a slot");
    for (auto color : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
        check(format(FrameGenerationPlane::Color, color) &&
            format(FrameGenerationPlane::UiColorTransmittance, color) &&
            format(FrameGenerationPlane::UiMultiply, color), "preserve supported color/UI formats");
    }
    check(format(FrameGenerationPlane::Depth, DXGI_FORMAT_R32_FLOAT) &&
        format(FrameGenerationPlane::Motion, DXGI_FORMAT_R16G16_FLOAT), "support actual depth and motion guide formats");
    check(format(FrameGenerationPlane::UiMultiply, DXGI_FORMAT_R16G16B16A16_FLOAT) &&
        !format(FrameGenerationPlane::Color, DXGI_FORMAT_R16G16B16A16_FLOAT), "UI float plane allowance does not widen color contract");
    check(!format(FrameGenerationPlane::Depth, DXGI_FORMAT_D32_FLOAT) &&
        !format(FrameGenerationPlane::Depth, DXGI_FORMAT_R32_TYPELESS) &&
        !format(FrameGenerationPlane::Motion, DXGI_FORMAT_R32_FLOAT) &&
        !format(FrameGenerationPlane::Count, DXGI_FORMAT_R8G8B8A8_UNORM), "no silent format reinterpretation or invalid roles");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 3200; desc.Height = 3200; desc.MipLevels = desc.ArraySize = 1;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    check(descriptor(desc, 3200, 3200), "accept full-eye default single-image descriptor");
    for (unsigned kind = 0; kind < 9; ++kind) {
        auto invalid = desc;
        switch (kind) {
        case 0: invalid.Width--; break;
        case 1: invalid.Height--; break;
        case 2: invalid.MipLevels = 2; break;
        case 3: invalid.ArraySize = 2; break;
        case 4: invalid.SampleDesc.Count = 2; break;
        case 5: invalid.SampleDesc.Quality = 1; break;
        case 6: invalid.Usage = D3D11_USAGE_STAGING; break;
        case 7: invalid.CPUAccessFlags = D3D11_CPU_ACCESS_READ; break;
        case 8: invalid.Usage = D3D11_USAGE_DYNAMIC; break;
        }
        check(!descriptor(invalid, 3200, 3200), "reject size/array/mip/MSAA/CPU descriptor mismatch");
    }
    check(!descriptor(desc, 0, 3200), "zero configuration dimensions cannot pass");
    Transaction tx{100, 5, {0, 0}};
    check(tx.accepts(100, 5, 0, FrameGenerationPlane::Depth), "accept first authoritative plane");
    check(!tx.accepts(101, 5, 0, FrameGenerationPlane::Depth) &&
        !tx.accepts(100, 6, 0, FrameGenerationPlane::Depth) &&
        !tx.accepts(100, 5, 2, FrameGenerationPlane::Depth), "sequence/history/eye mismatch cannot capture");
    tx.captured[0] = frameGenerationPlaneBit(FrameGenerationPlane::Depth);
    check(!tx.accepts(100, 5, 0, FrameGenerationPlane::Depth), "duplicate plane cannot overwrite snapshot before publication");
    check(!tx.paired(100, 5, frameGenerationRequiredPlanes), "one plane or one eye cannot publish");
    tx.captured[0] = tx.captured[1] = frameGenerationRequiredPlanes;
    check(tx.paired(100, 5, frameGenerationRequiredPlanes), "paired guides and full-eye color publish together");
    check(!tx.paired(100, 6, frameGenerationRequiredPlanes) && !tx.paired(100, 5, 3) &&
        !tx.paired(100, 5, frameGenerationRequiredPlanes | 0x8000), "invalid generation/required mask cannot publish");
    const auto ui = frameGenerationPlaneBit(FrameGenerationPlane::UiColorTransmittance);
    tx.captured[0] |= ui;
    check(!tx.paired(100, 5, frameGenerationRequiredPlanes | ui) &&
        !tx.paired(100, 5, frameGenerationRequiredPlanes), "one-eye UI is neither complete nor implicitly empty");
    tx.captured[1] |= ui;
    check(tx.paired(100, 5, frameGenerationRequiredPlanes | ui), "explicit paired final UI plane is immutable with same batch");
    std::printf("%s FG interop CPU policy checks=%u failures=%u live_gpu_calls=0 driver_loads=0\n",
        failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

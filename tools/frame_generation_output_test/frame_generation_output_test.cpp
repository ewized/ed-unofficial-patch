#include "../../src/d3d11/frame_generation_output.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Default fixtures must never reach NGX or instantiate a graphics device.
namespace edvr {
FrameGenerationNgxCompletion FrameGenerationNgxTicket::poll() const noexcept { std::abort(); }
}
int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test")) return 2;
    const auto graphicsLoaded = [] {
        return GetModuleHandleW(L"d3d11.dll") || GetModuleHandleW(L"d3d12.dll") ||
            GetModuleHandleW(L"dxgi.dll") || GetModuleHandleW(L"d3dcompiler_47.dll");
    };
    if (graphicsLoaded()) return 3;
    unsigned failures = edvr::frameGenerationOutputTestProtocol();
    {
        edvr::FrameGenerationOutput output;
        auto* invalidDevice = reinterpret_cast<ID3D12Device*>(uintptr_t(1));
        if (output.initialize(false, invalidDevice, 3200, 3200) ||
            output.status().failure != edvr::FrameGenerationOutputFailure::Off || !output.drain()) ++failures;
        if (output.initialize(true, nullptr, 3200, 3200) ||
            output.status().failure != edvr::FrameGenerationOutputFailure::InvalidArgument) ++failures;
        edvr::FrameGenerationOutputUi ui[2];
        const auto ticket = output.submit({}, {}, ui, {});
        if (ticket.accepted || ticket.unsafe || ticket.finalFence || ticket.finalValue || ticket.lease) ++failures;
        EdvrFgBatch batch{}; batch.size = sizeof(batch); batch.version = EDVR_FG_BATCH_VERSION_4;
        if (ticket.exportStatus() != E_FAIL || ticket.acquireBatch(&batch) != E_FAIL || batch.token || batch.phaseMask) ++failures;
        if (ticket.acquireBatch(nullptr) != E_INVALIDARG) ++failures;
        if (output.collect()) ++failures;
    }
    if (graphicsLoaded()) ++failures;
    std::printf("FG output production protocol + OFF fixtures: failures=%u GPU=0 NGX=0 D3Dmodules=0\n", failures);
    return failures ? 1 : 0;
}

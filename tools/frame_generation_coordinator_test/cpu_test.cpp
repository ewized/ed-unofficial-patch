#include "../../src/d3d11/frame_generation_coordinator.h"
#include "../../src/d3d11/frame_generation_ngx.h"
#include "../../src/d3d11/ui_layer.h"
#include "../../src/d3d11/scene_projection_capture.h"
#include "../../src/common/log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3dcompiler.h>
#include <array>
#include <limits>

extern "C" HRESULT WINAPI D3DCompile(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**) {std::abort();}

// Production coordinator admission/matrix functions are exercised unchanged.
// These link-time tripwires guarantee that CPU fixtures cannot start a worker,
// query NGX, capture HUD, or replace the real color/interop/output adapters.
namespace edvr {
Log& Log::get() {static Log* instance = new Log; return *instance;}
void Log::note(const char*, ...) {}
FrameGenerationNgx::~FrameGenerationNgx() {if (impl_) std::abort();}
bool FrameGenerationNgx::initialize(const FrameGenerationNgxInit&) {std::abort();}
FrameGenerationNgxTicket FrameGenerationNgx::submit(const FrameGenerationNgxInput&) {std::abort();}
bool FrameGenerationNgx::collect() noexcept {std::abort();}
bool FrameGenerationNgx::drain(uint32_t) noexcept {std::abort();}
FrameGenerationNgxStatus FrameGenerationNgx::status() const noexcept {std::abort();}
FrameGenerationNgxCompletion FrameGenerationNgxTicket::poll() const noexcept {std::abort();}
std::shared_ptr<const UiLayerFgSnapshot> uiLayerFgCapture(ID3D11DeviceContext*, uint64_t, uint32_t,
    ID3D11Texture2D*, const uint32_t[4], const float[4], UiLayerFgInfo*) noexcept {std::abort();}
bool uiLayerFgGetPlanes(const std::shared_ptr<const UiLayerFgSnapshot>&, UiLayerFgPlanes*) noexcept {std::abort();}
bool uiLayerFgRetireStorage(ID3D11DeviceContext*) noexcept {std::abort();}
}
unsigned testProjectionCapture(unsigned& checks) {
    unsigned failures=0;
    const auto check=[&](bool pass,const char* name){++checks;if(!pass){++failures;std::printf("FAIL projection: %s\n",name);}};
    std::array<float,1104> cb{};
    auto* clip=cb.data()+270*4;
    // Actual forward-columns convention: x*r270+y*r271+z*r272+r273.
    // Rotated, asymmetric and translated perspective; clip z is measured
    // from data at .03125, deliberately different from the .025 fallback.
    clip[0]=1.04f;clip[3]=.6f;clip[5]=1.1f;clip[8]=-.78f;clip[11]=.8f;
    clip[12]=.2f;clip[13]=-.1f;clip[14]=.03125f;clip[15]=2;
    const auto capture=[&](float nearZ=0){return edvr::captureSceneProjection(cb.data(),uint32_t(sizeof(cb)),nearZ);};
    auto result=capture(.03125f);
    check(result.source==edvr::SceneProjectionSource::Clip270,"zero legacy uses actual clip-row source");
    check(result.a==0 && result.b==.03125f,"actual B is read, not a fallback constant");
    auto changed=cb;changed[273*4+2]=.0625f;
    result=edvr::captureSceneProjection(changed.data(),uint32_t(sizeof(changed)),.0625f);
    check(result.b==.0625f,"changing actual source changes measured B");
    check(capture(.05f).source==edvr::SceneProjectionSource::None,"runtime near mismatch refused");
    check(edvr::captureSceneProjection(cb.data(),274*16-1).source==edvr::SceneProjectionSource::None,"truncated clip rows refused");
    changed=cb;changed[270*4+2]=.001f;
    check(edvr::captureSceneProjection(changed.data(),uint32_t(sizeof(changed))).source==edvr::SceneProjectionSource::None,"nonparallel clip-z/clip-w refused");
    changed=cb;changed[270*4+3]=.3f;changed[272*4+3]=.4f;
    check(edvr::captureSceneProjection(changed.data(),uint32_t(sizeof(changed))).source==edvr::SceneProjectionSource::None,"nonunit depth direction refused");
    changed=cb;changed[271*4]=changed[270*4];changed[271*4+1]=changed[270*4+1];changed[271*4+2]=changed[270*4+2];changed[271*4+3]=changed[270*4+3];
    check(edvr::captureSceneProjection(changed.data(),uint32_t(sizeof(changed))).source==edvr::SceneProjectionSource::None,"singular clip basis refused");
    changed=cb;changed[273*4+2]=-.03125f;
    check(edvr::captureSceneProjection(changed.data(),uint32_t(sizeof(changed))).source==edvr::SceneProjectionSource::None,"negative B refused");
    changed=cb;changed[270*4]=(std::numeric_limits<float>::quiet_NaN)();
    check(edvr::captureSceneProjection(changed.data(),uint32_t(sizeof(changed))).source==edvr::SceneProjectionSource::None,"nonfinite actual rows refused");
    changed=cb;changed[270*4+2]=-.00006f;changed[272*4+2]=-.00008f;changed[273*4+2]=.03105f;
    result=edvr::captureSceneProjection(changed.data(),uint32_t(sizeof(changed)));
    check(result.source==edvr::SceneProjectionSource::Clip270 && std::fabs(result.a+.0001f)<1e-9f && std::fabs(result.b-.03125f)<1e-8f,"finite reversed depth derives A and translation-corrected B");
    cb[794]=-.0001f;cb[795]=.04f;result=capture();
    check(result.source==edvr::SceneProjectionSource::Legacy198 && result.a==cb[794] && result.b==cb[795],"valid legacy projection preserved exactly");
    cb.fill(0);check(capture().source==edvr::SceneProjectionSource::None,"missing both sources remains unmeasured");
    return failures;
}
int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test")) return 2;
    const auto graphicsLoaded = [] {
        return GetModuleHandleW(L"d3d11.dll") || GetModuleHandleW(L"d3d12.dll") ||
            GetModuleHandleW(L"dxgi.dll") || GetModuleHandleW(L"d3dcompiler_47.dll");
    };
    if (graphicsLoaded()) return 3;
    unsigned checks = 0;
    unsigned failures = edvr::nativeFgTestProtocol(&checks);
    failures += testProjectionCapture(checks);
    if (graphicsLoaded()) ++failures;
    std::printf("FG actual coordinator CPU admission/camera: checks=%u failures=%u GPU=0 NGX=0 D3Dmodules=0\n", checks, failures);
    return failures ? 1 : 0;
}

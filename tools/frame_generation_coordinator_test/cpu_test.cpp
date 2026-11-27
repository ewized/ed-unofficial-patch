#include "../../src/d3d11/frame_generation_coordinator.h"
#include "../../src/d3d11/frame_generation_ngx.h"
#include "../../src/d3d11/frame_generation_qualification_runner.h"
#include "../../src/d3d11/native_reflex.h"
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
namespace {
uint64_t fixtureReflexProof = 0;
unsigned admittedDoors = 0, coveredDoors = 0;
}
void nativeFgTestDoors(unsigned admitted,unsigned covered) noexcept {admittedDoors=admitted;coveredDoors=covered;}
bool uiLayerFgObserveCoverage(uint64_t,unsigned eye,ID3D11Texture2D*,
    const uint32_t[4],const float[4],bool& hasUi) noexcept {
    hasUi=eye<2 && (coveredDoors&(1u<<eye))!=0;
    return eye<2 && (admittedDoors&(1u<<eye))!=0;
}
void nativeFgTestSetReflexProof(uint64_t proof) noexcept {fixtureReflexProof = proof;}
uint64_t native_reflex::confirmedFrameGenerationProof(ID3D11Device* device, uint64_t generation) noexcept {
    return device == reinterpret_cast<ID3D11Device*>(uintptr_t(1)) && generation == 7 ? fixtureReflexProof : 0;
}
Log& Log::get() {static Log* instance = new Log; return *instance;}
void Log::note(const char*, ...) {}
FrameGenerationNgx::~FrameGenerationNgx() {if (impl_) std::abort();}
bool FrameGenerationNgx::initialize(const FrameGenerationNgxInit&) {std::abort();}
FrameGenerationNgxTicket FrameGenerationNgx::submit(const FrameGenerationNgxInput&) {std::abort();}
bool FrameGenerationNgx::collect() noexcept {std::abort();}
bool FrameGenerationNgx::drain(uint32_t) noexcept {std::abort();}
bool FrameGenerationNgx::resetAfterQualification(
    const std::shared_ptr<const FrameGenerationNgxQualification>&) noexcept {std::abort();}
void FrameGenerationNgx::quarantineQualification() noexcept {std::abort();}
FrameGenerationNgxStatus FrameGenerationNgx::status() const noexcept {std::abort();}
FrameGenerationNgxCompletion FrameGenerationNgxTicket::poll() const noexcept {std::abort();}
FrameGenerationQualificationResult qualifyFrameGeneration(FrameGenerationNgx&, const FrameGenerationNgxInit&,
    const FrameGenerationQualificationLimits&) noexcept {std::abort();}
std::shared_ptr<const UiLayerFgSnapshot> uiLayerFgCapture(ID3D11DeviceContext*, uint64_t, uint32_t,
    ID3D11Texture2D*, const uint32_t[4], const float[4], UiLayerFgInfo*) noexcept {std::abort();}
bool uiLayerFgCaptureTo(ID3D11DeviceContext*, uint64_t, uint32_t, ID3D11Texture2D*,
    const uint32_t[4], const float[4], UiLayerFgCaptureSink, void*, UiLayerFgInfo*) noexcept {std::abort();}
bool uiLayerFgGetPlanes(const std::shared_ptr<const UiLayerFgSnapshot>&, UiLayerFgPlanes*) noexcept {std::abort();}
bool uiLayerFgRetireStorage(ID3D11DeviceContext*) noexcept {std::abort();}
}
unsigned testProjectionCapture(unsigned& checks) {
    unsigned failures=0;
    const auto check=[&](bool pass,const char* name){++checks;if(!pass){++failures;std::printf("FAIL projection: %s\n",name);}};
    using SceneBuffer = std::array<float,1104>;
    SceneBuffer cb{};
    auto* clip=cb.data()+270*4;
    // Actual forward-columns convention: x*r270+y*r271+z*r272+r273.
    // Rotated, asymmetric and translated perspective; clip z is measured
    // from data at .03125, deliberately different from the .025 fallback.
    clip[0]=1.04f;clip[3]=.6f;clip[5]=1.1f;clip[8]=-.78f;clip[11]=.8f;
    clip[12]=.2f;clip[13]=-.1f;clip[14]=.03125f;clip[15]=2;
    using edvr::SceneProjectionSource;
    float a = 0, b = 0;
    const auto resolve = [&](const SceneBuffer& data, float nearZ, unsigned bytes = sizeof(SceneBuffer)) {
        const auto write = edvr::captureSceneProjection(data.data(), bytes);
        return edvr::resolveSceneProjection(write, nearZ, &a, &b);
    };
    check(resolve(cb,.03125f)==SceneProjectionSource::Clip270,"zero legacy uses actual clip-row source");
    check(a==0 && b==.03125f,"actual B is read, not a fallback constant");
    auto changed=cb;changed[273*4+2]=.0625f;
    check(resolve(changed,.0625f)==SceneProjectionSource::Clip270 && b==.0625f,"changing actual source changes measured B");
    check(resolve(cb,.05f)==SceneProjectionSource::None,"runtime near mismatch refused");
    check(resolve(cb,.03125f,274*16-1)==SceneProjectionSource::None,"truncated clip rows refused");
    changed=cb;changed[270*4+2]=.001f;
    check(resolve(changed,.03125f)==SceneProjectionSource::None,"nonparallel clip-z/clip-w refused");
    changed=cb;changed[270*4+3]=.3f;changed[272*4+3]=.4f;
    check(resolve(changed,.03125f)==SceneProjectionSource::None,"nonunit depth direction refused");
    changed=cb;changed[271*4]=changed[270*4];changed[271*4+1]=changed[270*4+1];changed[271*4+2]=changed[270*4+2];changed[271*4+3]=changed[270*4+3];
    check(resolve(changed,.03125f)==SceneProjectionSource::None,"singular clip basis refused");
    changed=cb;changed[273*4+2]=-.03125f;
    check(resolve(changed,.03125f)==SceneProjectionSource::None,"negative B refused");
    changed=cb;changed[270*4]=(std::numeric_limits<float>::quiet_NaN)();
    check(resolve(changed,.03125f)==SceneProjectionSource::None,"nonfinite actual rows refused");
    changed=cb;changed[270*4+2]=-.00006f;changed[272*4+2]=-.00008f;changed[273*4+2]=.03105f;
    check(resolve(changed,.03125f)==SceneProjectionSource::Clip270 && std::fabs(a+.0001f)<1e-9f && std::fabs(b-.03125f)<1e-8f,"finite reversed depth derives A and translation-corrected B");

    // The mapping expires before either eye is treated. The same captured
    // write must resolve independently against both runtime near planes.
    cb[794]=-.0001f; cb[795]=.0675f; clip[14]=.025f;
    const auto write=edvr::captureSceneProjection(cb.data(),sizeof(cb));
    cb.fill(0);
    check(edvr::resolveSceneProjection(write,.025f,&a,&b)==SceneProjectionSource::Clip270 && a==0 && b==.025f,
          "wrong legacy near cannot discard the matching clip projection");
    check(edvr::resolveSceneProjection(write,.0675f,&a,&b)==SceneProjectionSource::Legacy198 && a==-.0001f && b==.0675f,
          "other eye can use the exact legacy row from the same write");
    check(edvr::resolveSceneProjection(write,.1f,&a,&b)==SceneProjectionSource::None && a==0 && b==.1f,
          "mismatching both candidates preserves SR fallback and refuses measured FG depth");
    cb[795]=.025f; clip[0]=clip[5]=clip[11]=1; clip[14]=.0255f;
    check(resolve(cb,.025f)==SceneProjectionSource::Legacy198 && b==.025f,"matching legacy keeps precedence over matching clip");
    cb.fill(0);check(resolve(cb,.025f)==SceneProjectionSource::None,"missing both sources remains unmeasured");
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

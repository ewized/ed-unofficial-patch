#include "native_gaze.h"
#include "native_gaze_policy.h"

#include <windows.h>
#include <cmath>

// Isolate SDK types as well as translation units from ED-UP's legacy 012 ABI.
// The pinned official header is unmodified; namespace spelling changes no ABI.
#define vr edvr_openvr026
#include "openvr026_official/openvr.h"
#undef vr
namespace vr026=edvr_openvr026;

namespace edvr {
namespace {

using GetInterface = void* (__cdecl*)(const char*, vr026::EVRInitError*);
using Initialize = std::uint32_t (__cdecl*)(vr026::EVRInitError*, vr026::EVRApplicationType);
using InitToken = std::uint32_t (__cdecl*)();
native_gaze_detail::State& providerState(){static native_gaze_detail::State state{};return state;}

bool initToken(void*,void* module,std::uint32_t* value) noexcept {
    // The module was already verified. Valve's interface cache uses this same
    // official export to drop handles after SDK Init/Cleanup (026 CheckClear).
    static void* owner=nullptr;static InitToken get=nullptr;
    if(owner!=module){owner=module;get=reinterpret_cast<InitToken>(
        GetProcAddress(static_cast<HMODULE>(module),"VR_GetInitToken"));}
    if(!get)return false;
    *value=get();return true; // zero is a valid pre-initialization token
}

void* loadModule(void*) noexcept {
    return LoadLibraryExW(native_gaze_detail::clientPath, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

bool verifyModule(void*, void* module) noexcept {
    wchar_t actual[32768]{};
    const DWORD count = GetModuleFileNameW(static_cast<HMODULE>(module), actual, 32768);
    return count != 0 && count < 32768 && native_gaze_detail::pathMatches(actual);
}

void* getInterface(void*, void* module, int* error) noexcept {
    const auto get = reinterpret_cast<GetInterface>(
        GetProcAddress(static_cast<HMODULE>(module), "VR_GetGenericInterface"));
    vr026::EVRInitError result = vr026::VRInitError_Init_InterfaceNotFound;
    void* system = get ? get(vr026::IVRSystem_Version, &result) : nullptr;
    *error = static_cast<int>(result);
    return system;
}

bool initializeBackground(void*, void* module, int* error) noexcept {
    // The ownership policy never initializes a module missing its getter.
    if (!GetProcAddress(static_cast<HMODULE>(module), "VR_GetGenericInterface")) {
        *error = static_cast<int>(vr026::VRInitError_Init_InterfaceNotFound);
        return false;
    }
    const auto init = reinterpret_cast<Initialize>(
        GetProcAddress(static_cast<HMODULE>(module), "VR_InitInternal"));
    vr026::EVRInitError result = vr026::VRInitError_Init_InterfaceNotFound;
    if (init) init(&result, vr026::VRApplication_Background);
    *error = static_cast<int>(result);
    return init && result == vr026::VRInitError_None;
}

bool query(void*, void* system, float ndc[2][2]) noexcept {
    vr026::HmdVector2_t left{}, right{};
    const bool valid = static_cast<vr026::IVRSystem*>(system)->GetEyeTrackedFoveationCenter(
        &left, &right);
    ndc[0][0] = left.v[0];
    ndc[0][1] = left.v[1];
    ndc[1][0] = right.v[0];
    ndc[1][1] = right.v[1];
    return valid;
}

bool receiptClock(void*, std::uint64_t* tick, std::uint64_t* frequency) noexcept {
    LARGE_INTEGER now{}, rate{};
    const bool valid = QueryPerformanceCounter(&now) && QueryPerformanceFrequency(&rate) &&
        now.QuadPart > 0 && rate.QuadPart > 0;
    if (!valid) return false;
    *tick = static_cast<std::uint64_t>(now.QuadPart);
    *frequency = static_cast<std::uint64_t>(rate.QuadPart);
    return true;
}

}  // namespace

NativeGazeSample nativeGazePoll(bool enabled) noexcept {
    if (!enabled) return {};
    auto& state=providerState();
    static const native_gaze_detail::Ops ops{
        nullptr, loadModule, verifyModule, getInterface, initializeBackground, query, receiptClock,initToken};
    // This module and borrowed/Background context stay alive for process lifetime.
    // Loading occurs only here on the renderer thread, never from DllMain.
    auto sample=native_gaze_detail::poll(true, state, ops);
    bool stable=true;
    if(sample.usable&&state.system)for(unsigned eye=0;eye<2;++eye){
        if(!native_gaze_detail::currentToken(state,ops)){stable=false;break;}
        const auto matrix=static_cast<vr026::IVRSystem*>(state.system)->GetProjectionMatrix(
            static_cast<vr026::EVREye>(eye),.1f,100.f);
        auto& value=sample.eyes[eye];
        value.projection[0]=matrix.m[0][0];value.projection[1]=matrix.m[1][1];
        value.projection[2]=matrix.m[0][2];value.projection[3]=matrix.m[1][2];
        value.projectionValid=std::isfinite(value.projection[0])&&value.projection[0]>0&&
            std::isfinite(value.projection[1])&&value.projection[1]>0&&
            std::isfinite(value.projection[2])&&std::isfinite(value.projection[3])&&
            matrix.m[0][1]==0&&matrix.m[1][0]==0&&matrix.m[0][3]==0&&matrix.m[1][3]==0&&
            matrix.m[3][0]==0&&matrix.m[3][1]==0&&matrix.m[3][2]==-1&&matrix.m[3][3]==0;
        if(!native_gaze_detail::currentToken(state,ops)){stable=false;break;}
        const auto eyeToHead=static_cast<vr026::IVRSystem*>(state.system)->GetEyeToHeadTransform(static_cast<vr026::EVREye>(eye));
        for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)
            value.projectionValid=value.projectionValid&&std::isfinite(eyeToHead.m[row][col])&&
                std::fabs(eyeToHead.m[row][col]-(row==col?1.f:0.f))<.0001f;
    }
    if(sample.usable&&(!stable||!native_gaze_detail::currentToken(state,ops))){
        sample.usable=false;sample.eyes[0].valid=sample.eyes[1].valid=false;
        sample.eyes[0].projectionValid=sample.eyes[1].projectionValid=false;sample.failure=state.failure;
    }
    return sample;
}
void nativeGazeSetLifetime(std::uint64_t epoch) noexcept {native_gaze_detail::setLifetime(providerState(),epoch);}
void nativeGazeReset() noexcept {nativeGazeSetLifetime(0);}

}  // namespace edvr

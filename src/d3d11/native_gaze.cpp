#include "native_gaze.h"
#include "native_gaze_policy.h"

#include <windows.h>
#include <tlhelp32.h>
#include <cmath>
#include <cwchar>
#include <iterator>

// Isolate Valve's 026 ABI from the legacy 012 interface served to Elite.
#define vr edvr_openvr026
#include "openvr026_official/openvr.h"
#include "openvr026_official/ivrclientcore.h"
#undef vr
namespace vr026 = edvr_openvr026;

namespace edvr {
namespace {

using ClientFactory = void* (__cdecl*)(const char*, int*);

NativeGazeSample refusal(NativeGazeFailure failure) noexcept {
    NativeGazeSample sample{};
    sample.failure = failure;
    return sample;
}

struct ClientSelection {
    std::uint64_t epoch{};
    HMODULE image{};
    wchar_t path[MAX_PATH]{};
    NativeGazeFailure failure{NativeGazeFailure::ModuleCensusIncomplete};
};

bool samePath(const wchar_t* a, const wchar_t* b) noexcept {
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

NativeGazeFailure scanClientImages(HANDLE snapshot, ClientSelection& selected) noexcept {
    // Complete the census before checking exports. A second client with no
    // compatible factory is still a second possible owner of SteamVR state.
    MODULEENTRY32W entry{};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    SetLastError(ERROR_SUCCESS);
    BOOL more = Module32FirstW(snapshot, &entry);
    bool ambiguous = false;
    unsigned modules = 0;
    while (more) {
        constexpr unsigned kModuleLimit = 4096;
        if (++modules > kModuleLimit || !std::wmemchr(entry.szModule, L'\0', std::size(entry.szModule)))
            return NativeGazeFailure::ModuleCensusIncomplete;
        if (native_gaze_detail::isClientImageName(entry.szModule)) {
            const auto* end = std::wmemchr(entry.szExePath, L'\0', std::size(entry.szExePath));
            if (!entry.hModule || !end || end == entry.szExePath ||
                end == entry.szExePath + std::size(entry.szExePath) - 1)
                return NativeGazeFailure::ModuleCensusIncomplete;
            if (!selected.image) {
                selected.image = entry.hModule;
                std::wmemcpy(selected.path, entry.szExePath, static_cast<std::size_t>(end - entry.szExePath) + 1);
            } else if (entry.hModule != selected.image) {
                ambiguous = true;
            } else if (!samePath(entry.szExePath, selected.path)) {
                return NativeGazeFailure::ModuleCensusIncomplete;
            }
        }
        SetLastError(ERROR_SUCCESS);
        more = Module32NextW(snapshot, &entry);
    }
    if (GetLastError() != ERROR_NO_MORE_FILES) return NativeGazeFailure::ModuleCensusIncomplete;
    if (ambiguous) return NativeGazeFailure::ModuleAmbiguous;
    return selected.image ? NativeGazeFailure::None : NativeGazeFailure::ModuleNotLoaded;
}

void selectClientImage(ClientSelection& selected, std::uint64_t epoch) noexcept {
    selected = {};
    selected.epoch = epoch;
    // The loader can change during snapshot creation. Retry that documented
    // transient error, within the one first-poll census budget.
    constexpr unsigned kSnapshotAttempts = 3;
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < kSnapshotAttempts; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snapshot == INVALID_HANDLE_VALUE) return;
    selected.failure = scanClientImages(snapshot, selected);
    CloseHandle(snapshot);
}

bool ownsAddress(HMODULE module, LPCWSTR address) noexcept {
    HMODULE owner = nullptr;
    return address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, address, &owner) && owner == module;
}

NativeGazeSample pollClient(HMODULE module, const ClientSelection& selected) noexcept {
    wchar_t path[MAX_PATH]{};
    const auto capacity = static_cast<DWORD>(std::size(path));
    const DWORD length = GetModuleFileNameW(module, path, capacity);
    if (!length || length >= capacity || !samePath(path, selected.path))
        return refusal(NativeGazeFailure::ModulePathMismatch);

    const auto entry = GetProcAddress(module, "VRClientCoreFactory");
    if (!ownsAddress(module, reinterpret_cast<LPCWSTR>(entry)))
        return refusal(NativeGazeFailure::ContextUnavailable);

    // SteamVR's OpenXR instance already owns its client core. This is a
    // compatibility lookup using Valve's SDK source ABI. The core and system
    // pointers are borrowed only for this call, under the instance owner's lease.
    const auto factory = reinterpret_cast<ClientFactory>(entry);
    int coreError = vr026::VRInitError_None; // Valve SDK seeds the factory result with zero.
    auto* core = static_cast<vr026::IVRClientCore*>(factory(vr026::IVRClientCore_Version, &coreError));
    if (!core || coreError != vr026::VRInitError_None)
        return refusal(NativeGazeFailure::ContextUnavailable);
    vr026::EVRInitError systemError = vr026::VRInitError_None;
    auto* system = static_cast<vr026::IVRSystem*>(core->GetGenericInterface(vr026::IVRSystem_Version, &systemError));
    if (!system || systemError != vr026::VRInitError_None)
        return refusal(NativeGazeFailure::ContextUnavailable);

    vr026::HmdVector2_t left{}, right{};
    const bool valid = system->GetEyeTrackedFoveationCenter(&left, &right);
    LARGE_INTEGER now{}, rate{};
    const bool clockValid = QueryPerformanceCounter(&now) && QueryPerformanceFrequency(&rate) &&
        now.QuadPart > 0 && rate.QuadPart > 0;
    const float ndc[2][2]{{left.v[0], left.v[1]}, {right.v[0], right.v[1]}};
    static std::uint64_t sequence = 0; // Serialized by the instance owner.
    auto sample = native_gaze_detail::sampleGaze(valid, ndc, ++sequence,
        clockValid ? static_cast<std::uint64_t>(now.QuadPart) : 0,
        clockValid ? static_cast<std::uint64_t>(rate.QuadPart) : 0);
    if (!sample.usable) return sample;

    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto vrEye = static_cast<vr026::EVREye>(eye);
        const auto matrix = system->GetProjectionMatrix(vrEye, .1f, 100.f);
        auto& value = sample.eyes[eye];
        value.projection[0] = matrix.m[0][0]; value.projection[1] = matrix.m[1][1];
        value.projection[2] = matrix.m[0][2]; value.projection[3] = matrix.m[1][2];
        value.projectionValid = std::isfinite(value.projection[0]) && value.projection[0] > 0 &&
            std::isfinite(value.projection[1]) && value.projection[1] > 0 &&
            std::isfinite(value.projection[2]) && std::isfinite(value.projection[3]) &&
            matrix.m[0][1] == 0 && matrix.m[1][0] == 0 && matrix.m[0][3] == 0 &&
            matrix.m[1][3] == 0 && matrix.m[3][0] == 0 && matrix.m[3][1] == 0 &&
            matrix.m[3][2] == -1 && matrix.m[3][3] == 0;
        const auto eyeToHead = system->GetEyeToHeadTransform(static_cast<vr026::EVREye>(eye));
        for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)
            value.projectionValid = value.projectionValid && std::isfinite(eyeToHead.m[row][col]) &&
                std::fabs(eyeToHead.m[row][col] - (row == col ? 1.f : 0.f)) < .0001f;
    }
    return sample;
}

}  // namespace

NativeGazeSample nativeGazePoll(std::uint64_t admittedEpoch, const void* instanceFunction) noexcept {
    if (!admittedEpoch) return {};
    // The native temporal mutex serializes the live-instance lease. The
    // cached identity neither retains a reference nor survives its epoch.
    static ClientSelection selected;
    if (selected.epoch != admittedEpoch) selectClientImage(selected, admittedEpoch);
    if (selected.failure != NativeGazeFailure::None) return refusal(selected.failure);

    HMODULE module = nullptr;
    // Ref by the selected base, never by a basename that can switch to a
    // later duplicate. Balance the reference even on a contained SEH fault.
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(selected.image), &module) || !module) {
        selected.failure = NativeGazeFailure::ModuleNotLoaded;
        return refusal(selected.failure);
    }
    NativeGazeSample sample{};
#ifdef _MSC_VER
    __try {
#endif
        sample = module == selected.image ? pollClient(module, selected) : refusal(NativeGazeFailure::ModulePathMismatch);
        if (sample.failure == NativeGazeFailure::ModulePathMismatch) selected.failure = sample.failure;
        else if (instanceFunction)
            sample.instanceFunctionInClient = ownsAddress(module, static_cast<LPCWSTR>(instanceFunction));
#ifdef _MSC_VER
    } __finally {
        FreeLibrary(module);
    }
#else
    FreeLibrary(module);
#endif
    return sample;
}

}  // namespace edvr

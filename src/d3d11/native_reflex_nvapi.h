#pragma once

#include "native_reflex.h"

#ifndef EDVR_NATIVE_FRAME_TEST
#include "../common/guard.h"
#include "vrs_coverage_policy.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <string_view>
#endif

#include "../common/log.h"

#include <cstddef>
#include <string>

namespace edvr::native_reflex {

namespace detail {

// NVAPI's public Dx V1 ABI, from NVIDIA/nvapi's nvapi.h and nvapi_interface.h.
// NvBool is an NvU8. Zero-initialize the reserved bytes and leave the optional
// limiter, marker optimization and queue-time optimization disabled.
struct SetSleepMode {
    uint32_t version;
    uint8_t lowLatencyMode;
    uint8_t lowLatencyBoost;
    uint32_t minimumIntervalUs;
    uint8_t useMarkersToOptimize;
    uint8_t useMinQueueTime;
    uint8_t reserved[30];
};

struct GetSleepStatus {
    uint32_t version;
    uint8_t lowLatencyMode;
    uint8_t fullscreenVrr;
    uint8_t controlPanelVsyncOn;
    uint32_t sleepIntervalUs;
    uint8_t useGameSleep;
    uint8_t fullscreenIFlip;
    uint8_t frameGenerationMultiplier;
    uint8_t dynamicFrameGeneration;
    uint32_t dynamicFrameGenerationTargetUs;
    uint8_t reserved[114];
};

static_assert(sizeof(SetSleepMode) == 44 && offsetof(SetSleepMode, minimumIntervalUs) == 8 &&
              offsetof(SetSleepMode, reserved) == 14, "NV_SET_SLEEP_MODE_PARAMS_V1 layout");
static_assert(sizeof(GetSleepStatus) == 136 && offsetof(GetSleepStatus, sleepIntervalUs) == 8 &&
              offsetof(GetSleepStatus, useGameSleep) == 12 && offsetof(GetSleepStatus, reserved) == 20,
              "NV_GET_SLEEP_STATUS_PARAMS_V1 layout");

constexpr uint32_t version1(size_t size) noexcept { return static_cast<uint32_t>(size) | (1u << 16); }

constexpr SetSleepMode modeParameters(Mode mode) noexcept {
    SetSleepMode params{};
    params.version = version1(sizeof(params));
    params.lowLatencyMode = mode != Mode::Off;
    params.lowLatencyBoost = mode == Mode::Boost;
    return params;
}

constexpr int kUnavailable = 1;
constexpr int kDriverFault = 2;

struct Result {
    const char* reason;
    bool supported = false;
    int setStatus = kUnavailable;
    int getStatus = kUnavailable;
    GetSleepStatus readback{};
};

#ifndef EDVR_NATIVE_FRAME_TEST
using Query = void* (__cdecl*)(uint32_t);
using Initialize = int (__cdecl*)();
using SetMode = int (__cdecl*)(IUnknown*, SetSleepMode*);
using GetStatus = int (__cdecl*)(IUnknown*, GetSleepStatus*);

struct Driver {
    SetMode set = nullptr;
    GetStatus get = nullptr;
    const char* reason = "driver_unavailable";
};

inline HMODULE moduleOf(void* address, DWORD flags) noexcept {
    HMODULE module = nullptr;
    if (address) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | flags,
                                   reinterpret_cast<LPCWSTR>(address), &module);
    return module;
}

inline std::wstring_view modulePath(HMODULE module, wchar_t (&buffer)[MAX_PATH]) noexcept {
    const DWORD size = module ? GetModuleFileNameW(module, buffer, MAX_PATH) : 0;
    return size && size < MAX_PATH ? std::wstring_view(buffer, size) : std::wstring_view{};
}

// Use the system NVAPI DLL and the same driver-module path checks as native
// VRS. Keep successful entry points resident for the life of the process.
inline Driver loadDriver() {
    wchar_t system[MAX_PATH]{}, stubPath[MAX_PATH]{};
    const UINT size = GetSystemDirectoryW(system, MAX_PATH);
    if (!size || size >= MAX_PATH) return {};
    const std::wstring path = std::wstring(system, size) + L"\\nvapi64.dll";
    HMODULE stub = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!stub) return {};
    const auto driver = [&]() -> Driver {
        const auto publicPath = modulePath(stub, stubPath);
        if (!vrs_coverage::nvapiSystemStubPath(system, publicPath))
            return {nullptr, nullptr, "driver_path_refused"};
        const auto query = reinterpret_cast<Query>(GetProcAddress(stub, "nvapi_QueryInterface"));
        if (moduleOf(reinterpret_cast<void*>(query), GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) != stub)
            return {nullptr, nullptr, "driver_query_unavailable"};
        void* initialize = nullptr;
        void* set = nullptr;
        void* get = nullptr;
        if (!guarded("nativeReflex/query", [&] {
                initialize = query(0x0150e828); // NvAPI_Initialize
                set = query(0xac1ca9e0);        // NvAPI_D3D_SetSleepMode
                get = query(0xaef96ca1);        // NvAPI_D3D_GetSleepStatus
            }) || !initialize || !set || !get)
            return {nullptr, nullptr, "driver_api_unavailable"};
        const auto trusted = [&](void* address) {
            wchar_t buffer[MAX_PATH]{};
            const auto owner = moduleOf(address, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT);
            const auto ownerPath = modulePath(owner, buffer);
            return vrs_coverage::nvapiOwnerPathKind(system, publicPath, ownerPath, ownerPath) &&
                   moduleOf(address, GET_MODULE_HANDLE_EX_FLAG_PIN) == owner;
        };
        if (!trusted(initialize) || !trusted(set) || !trusted(get))
            return {nullptr, nullptr, "driver_owner_refused"};
        int initialized = kDriverFault;
        guarded("nativeReflex/initialize", [&] { initialized = reinterpret_cast<Initialize>(initialize)(); });
        if (initialized != 0) return {nullptr, nullptr, "driver_initialize_failed"};
        return {reinterpret_cast<SetMode>(set), reinterpret_cast<GetStatus>(get), nullptr};
    }();
    if (!driver.set) FreeLibrary(stub);
    return driver;
}

inline const char* deviceRefusal(ID3D11Device* device) {
    using Microsoft::WRL::ComPtr;
    if (FAILED(device->GetDeviceRemovedReason())) return "device_removed";
    if (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
        return "singlethreaded_device";
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))
        return "adapter_unavailable";
    return desc.VendorId != 0x10de ? "non_nvidia_adapter" : nullptr;
}

inline const Driver& driverFunctions() {
    static const Driver driver = loadDriver();
    return driver;
}

inline void readback(ID3D11Device* device, Mode mode, const Driver& driver, Result& result) {
    result.readback.version = version1(sizeof(result.readback));
    result.getStatus = kDriverFault;
    guarded("nativeReflex/getStatus", [&] { result.getStatus = driver.get(device, &result.readback); });
    result.reason = result.getStatus != 0 ? "readback_failed"
        : bool(result.readback.lowLatencyMode) != (mode != Mode::Off) ? "driver_mode_differs"
        : result.setStatus == 0 ? "applied" : "readback_matches";
}

// Device-only calls on the enrolled D3D11 device, which remains alive through
// normal XR close. Validate vendor/threading before issuing a mode request.
inline Result apply(ID3D11Device* device, Mode mode) {
    if (const char* refused = deviceRefusal(device)) return {refused};
    const auto& driver = driverFunctions();
    if (!driver.set) return {driver.reason};
    Result result{"set_failed", true, kDriverFault};
    auto params = modeParameters(mode);
    guarded("nativeReflex/setMode", [&] { result.setStatus = driver.set(device, &params); });
    if (result.setStatus == 0) readback(device, mode, driver, result);
    return result;
}

// Only called for a previously accepted request on the same enrolled device.
// The prior request already resolved and pinned the driver entry points.
inline Result observe(ID3D11Device* device, Mode mode) {
    Result result{"device_removed", true};
    if (SUCCEEDED(device->GetDeviceRemovedReason()))
        readback(device, mode, driverFunctions(), result);
    return result;
}
#else
// The Windows native-frame rig must never touch the installed GPU driver.
inline Result apply(ID3D11Device*, Mode) { return {"test_driver_unavailable"}; }
inline Result observe(ID3D11Device*, Mode) { return {"test_driver_unavailable"}; }
#endif

inline std::string statusWord(int status) {
    if (status == kUnavailable) return "na";
    if (status == kDriverFault) return "fault";
    return std::to_string(status);
}

inline void logMode(Mode mode, const char* event, uint64_t session, const Result& result,
                    unsigned presents = 0) {
    const char* requested = mode == Mode::Boost ? "boost" : mode == Mode::On ? "on" : "off";
    const auto set = statusWord(result.setStatus), get = statusWord(result.getStatus);
    const auto readbackBit = [&](uint8_t value) {
        return result.getStatus != 0 ? "unknown" : value != 0 ? "1" : "0";
    };
    const std::string observation = presents
        ? ",after_presents=" + std::to_string(presents) +
            ",final=" + (presents == ModeHistory::kLastObservation ? "1" : "0")
        : std::string{};
    Log::get().note(
        "%s,requested=%s,event=%s,supported=%u,set_status=%s,get_status=%s,"
        "driver_enabled=%s,boost_requested=%u,game_sleep=%s,reason=%s,session=%llu%s",
        presents ? "native_reflex_readback" : "native_reflex", requested, event,
        unsigned(result.supported), set.c_str(), get.c_str(),
        readbackBit(result.readback.lowLatencyMode), unsigned(mode == Mode::Boost),
        readbackBit(result.readback.useGameSleep), result.reason,
        static_cast<unsigned long long>(session), observation.c_str());
}

} // namespace detail

inline bool updateMode(ModeHistory& history, ID3D11Device* device, Mode mode, uint64_t session) {
    if (!history.change(mode)) return false;
    const auto result = history.requiresDriver()
        ? detail::apply(device, mode) : detail::Result{"off_unmodified"};
    if (result.setStatus == 0) history.acceptRequest();
    detail::logMode(mode, "apply", session, result);
    return true;
}

inline void observeMode(ModeHistory& history, ID3D11Device* device, uint64_t session,
                        unsigned presents) {
    const auto result = detail::observe(device, history.requested());
    detail::logMode(history.requested(), "observe", session, result, presents);
    if (presents == ModeHistory::kLastObservation)
        history.finishObservation(result.getStatus == 0 && !result.readback.lowLatencyMode);
}

inline void closeMode(const ModeHistory& history, ID3D11Device* device, uint64_t session) {
    if (!history.needsReset()) return;
    detail::logMode(Mode::Off, "close", session, detail::apply(device, Mode::Off));
}

} // namespace edvr::native_reflex

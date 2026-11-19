#pragma once

#include "native_reflex.h"
#include "native_reflex_frame.h"

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

struct MarkerParams {
    uint32_t version;
    uint64_t frameId;
    Marker marker;
    uint64_t reserved0;
    uint8_t reserved[56];
};
static_assert(sizeof(MarkerParams) == 88 && offsetof(MarkerParams, frameId) == 8 &&
              offsetof(MarkerParams, marker) == 16 && offsetof(MarkerParams, reserved0) == 24,
              "NV_LATENCY_MARKER_PARAMS_V1 layout");
static_assert(sizeof(SetSleepMode) == 44 && offsetof(SetSleepMode, minimumIntervalUs) == 8 &&
              offsetof(SetSleepMode, reserved) == 14, "NV_SET_SLEEP_MODE_PARAMS_V1 layout");
static_assert(sizeof(GetSleepStatus) == 136 && offsetof(GetSleepStatus, sleepIntervalUs) == 8 &&
              offsetof(GetSleepStatus, useGameSleep) == 12 && offsetof(GetSleepStatus, reserved) == 20,
              "NV_GET_SLEEP_STATUS_PARAMS_V1 layout");

constexpr uint32_t version1(size_t size) noexcept { return static_cast<uint32_t>(size) | (1u << 16); }

constexpr MarkerParams markerParameters(uint64_t sourceId, Marker marker) noexcept {
    MarkerParams params{};
    params.version = version1(sizeof(params));
    params.frameId = sourceId;
    params.marker = marker;
    return params;
}

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
    bool apiResolved = false;
    int setStatus = kUnavailable;
    int getStatus = kUnavailable;
    GetSleepStatus readback{};
    bool externalPolicy = false;
    bool transportReady = false;
};

inline DriverMode observed(const Result& r) {
    return r.getStatus != 0 ? DriverMode::Unknown : r.readback.lowLatencyMode ? DriverMode::On : DriverMode::Off;
}

inline Snapshot snapshot(Mode mode, RequestState request, const Result& r) {
    return {mode, request, observed(r), r.apiResolved, r.getStatus == 0 && r.readback.useGameSleep, r.transportReady, false};
}

// GetSleepStatus exposes the effective mode and last sleep interval, not
// the previous setter's Boost, limiter, or optimization parameters. Leave
// an observed external policy alone rather than pretending to restore it.
template<class Read, class Set>
inline Result requestMode(Mode mode, bool alreadyOwned, Read read, Set set) {
    Result result{"set_failed", true};
    if (!alreadyOwned) {
        result.readback.version = version1(sizeof(result.readback));
        result.getStatus = read(result.readback);
        if (result.getStatus != 0) { result.reason = "baseline_unknown"; return result; }
        if (result.readback.lowLatencyMode || result.readback.useGameSleep || result.readback.sleepIntervalUs) {
            result.reason = "existing_driver_policy";
            result.externalPolicy = true;
            return result;
        }
    }
    auto params = modeParameters(mode);
    result.setStatus = set(params);
    result.readback = {};
    result.getStatus = kUnavailable;
    if (result.setStatus == 0) {
        result.readback.version = version1(sizeof(result.readback));
        result.getStatus = read(result.readback);
        result.reason = result.getStatus != 0 ? "readback_failed" :
            bool(result.readback.lowLatencyMode) == (mode != Mode::Off) ? "driver_mode_matches" : "driver_mode_differs";
    }
    return result;
}

#ifndef EDVR_NATIVE_FRAME_TEST
using Query = void* (__cdecl*)(uint32_t);
using Initialize = int (__cdecl*)();
using SetMode = int (__cdecl*)(IUnknown*, SetSleepMode*);
using GetStatus = int (__cdecl*)(IUnknown*, GetSleepStatus*);
using Sleep = int (__cdecl*)(IUnknown*);
using SetMarker = int (__cdecl*)(IUnknown*, MarkerParams*);

struct Driver {
    SetMode set = nullptr;
    GetStatus get = nullptr;
    const char* reason = "driver_unavailable";
    Sleep sleep = nullptr;
    SetMarker marker = nullptr;
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
        void* sleep = nullptr;
        void* marker = nullptr;
        if (!guarded("nativeReflex/query", [&] {
                initialize = query(0x0150e828); // NvAPI_Initialize
                set = query(0xac1ca9e0);        // NvAPI_D3D_SetSleepMode
                get = query(0xaef96ca1);        // NvAPI_D3D_GetSleepStatus
                sleep = query(0x852cd1d2);      // NvAPI_D3D_Sleep
                marker = query(0xd9984c05);     // NvAPI_D3D_SetLatencyMarker
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
        const bool transport = sleep && marker && trusted(sleep) && trusted(marker);
        return {reinterpret_cast<SetMode>(set), reinterpret_cast<GetStatus>(get), nullptr,
                transport ? reinterpret_cast<Sleep>(sleep) : nullptr, transport ? reinterpret_cast<SetMarker>(marker) : nullptr};
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

// Only a proven native simulation owner may call this adapter through
// FrameEvents. Resolving the entry points never opts an XR frame into Sleep.
struct FrameTransport {
    ID3D11Device* device;
    const Driver& driver;
    bool sleep() const {
        int code = kDriverFault;
        if (driver.sleep) guarded("nativeReflex/sleep", [&] { code = driver.sleep(device); });
        return code == 0;
    }
    bool mark(uint64_t sourceId, Marker type) const {
        int code = kDriverFault;
        auto params = markerParameters(sourceId, type);
        if (driver.marker) guarded("nativeReflex/marker", [&] { code = driver.marker(device, &params); });
        return code == 0;
    }
};

inline int readStatus(ID3D11Device* device, const Driver& driver, GetSleepStatus& status) {
    int code = kDriverFault;
    guarded("nativeReflex/getStatus", [&] { code = driver.get(device, &status); });
    return code;
}

// Device-only calls on the enrolled D3D11 device, which remains alive through
// normal XR close. Validate vendor/threading before issuing a mode request.
inline Result apply(ID3D11Device* device, Mode mode, bool alreadyOwned) {
    if (const char* refused = deviceRefusal(device)) return {refused};
    const auto& driver = driverFunctions();
    if (!driver.set) return {driver.reason};
    auto result = requestMode(mode, alreadyOwned, [&](GetSleepStatus& status) { return readStatus(device, driver, status); },
        [&](SetSleepMode& params) {
            int code = kDriverFault;
            guarded("nativeReflex/setMode", [&] { code = driver.set(device, &params); });
            return code;
        });
    result.transportReady = driver.sleep && driver.marker;
    return result;
}

// Only called for a previously accepted request on the same enrolled device.
// The prior request already resolved and pinned the driver entry points.
inline Result observe(ID3D11Device* device, Mode mode) {
    Result result{"device_removed", true};
    const auto& driver = driverFunctions();
    result.transportReady = driver.sleep && driver.marker;
    if (SUCCEEDED(device->GetDeviceRemovedReason())) {
        result.readback.version = version1(sizeof(result.readback));
        result.getStatus = readStatus(device, driver, result.readback);
        result.reason = result.getStatus != 0 ? "readback_failed" :
            bool(result.readback.lowLatencyMode) == (mode != Mode::Off) ? "driver_mode_matches" : "driver_mode_differs";
    }
    return result;
}
#else
// The Windows native-frame rig must never touch the installed GPU driver.
inline Result apply(ID3D11Device*, Mode, bool) { return {"test_driver_unavailable"}; }
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
        "%s,requested=%s,event=%s,api_resolved=%u,set_status=%s,get_status=%s,"
        "driver_enabled=%s,boost_requested=%u,game_sleep=%s,marker_transport=%u,full_boundary=0,reason=%s,session=%llu%s",
        presents ? "native_reflex_readback" : "native_reflex", requested, event,
        unsigned(result.apiResolved), set.c_str(), get.c_str(),
        readbackBit(result.readback.lowLatencyMode), unsigned(mode == Mode::Boost),
        readbackBit(result.readback.useGameSleep), unsigned(result.transportReady), result.reason,
        static_cast<unsigned long long>(session), observation.c_str());
}

} // namespace detail

inline bool updateMode(ModeHistory& history, ID3D11Device* device, Mode mode, uint64_t session) {
    if (!history.change(mode)) return false;
    const auto result = history.requiresDriver()
        ? detail::apply(device, mode, history.needsReset()) : detail::Result{"off_unmodified"};
    if (result.setStatus == 0) history.acceptRequest();
    publishStatus(detail::snapshot(mode, !history.requiresDriver() ? RequestState::Untouched :
        result.setStatus == 0 ? RequestState::Accepted : result.externalPolicy ? RequestState::ExternalPolicy : RequestState::Refused,
        result));
    detail::logMode(mode, "apply", session, result);
    return true;
}

inline void observeMode(ModeHistory& history, ID3D11Device* device, uint64_t session,
                        unsigned presents) {
    const auto result = detail::observe(device, history.requested());
    publishStatus(detail::snapshot(history.requested(), RequestState::Accepted, result));
    detail::logMode(history.requested(), "observe", session, result, presents);
    if (presents == ModeHistory::kLastObservation)
        history.finishObservation(result.getStatus == 0 && !result.readback.lowLatencyMode);
}

inline void closeMode(const ModeHistory& history, ID3D11Device* device, uint64_t session) {
    publishStatus({});
    if (!history.needsReset()) return;
    detail::logMode(Mode::Off, "close", session, detail::apply(device, Mode::Off, true));
}

} // namespace edvr::native_reflex

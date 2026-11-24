// Explicit opt-in probe of the production provider under one owned XR instance.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <openxr/openxr.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "../../src/d3d11/native_gaze.h"

namespace {
DWORD WINAPI watchdog(void* event) {
    if (WaitForSingleObject(static_cast<HANDLE>(event), 20000) == WAIT_TIMEOUT) {
        std::fputs("{\"error\":\"provider probe deadline 20000ms\"}\n", stderr);
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 124);
    }
    return 0;
}

template<class T>
T load(PFN_xrGetInstanceProcAddr get, XrInstance instance, const char* name) {
    PFN_xrVoidFunction proc = nullptr;
    return get(instance, name, &proc) == XR_SUCCESS ? reinterpret_cast<T>(proc) : nullptr;
}

struct Instance {
    HMODULE loader{};
    XrInstance value{XR_NULL_HANDLE};
    PFN_xrDestroyInstance destroy{};
    ~Instance() {
        if (value && destroy) destroy(value);
        if (loader) FreeLibrary(loader);
    }
};

bool receipt(const edvr::NativeGazeSample& sample) {
    return sample.providerQueried && sample.providerReturnedValid && sample.usable &&
        sample.eyes[0].valid && sample.eyes[1].valid && sample.eyes[0].projectionValid &&
        sample.eyes[1].projectionValid && sample.receiptQpc && sample.receiptQpcFrequency &&
        sample.querySequence && sample.sensorTimestampUnavailable;
}

int run(const wchar_t* loaderPath) {
    Instance instance;
    instance.loader = LoadLibraryExW(loaderPath, nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto get = instance.loader ? reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        GetProcAddress(instance.loader, "xrGetInstanceProcAddr")) : nullptr;
    const auto create = get ? load<PFN_xrCreateInstance>(get, XR_NULL_HANDLE, "xrCreateInstance") : nullptr;
    if (!create) {
        std::puts("{\"error\":\"OpenXR loader unavailable\"}");
        return 1;
    }
    constexpr char appName[] = "EDVR gaze borrow probe";
    XrInstanceCreateInfo info{};
    info.type = XR_TYPE_INSTANCE_CREATE_INFO;
    std::memcpy(info.applicationInfo.applicationName, appName, sizeof(appName));
    info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    const XrResult created = create(&info, &instance.value);
    if (created != XR_SUCCESS || !instance.value) {
        std::printf("{\"error\":\"xrCreateInstance\",\"result\":%d}\n", static_cast<int>(created));
        return 1;
    }
    instance.destroy = load<PFN_xrDestroyInstance>(get, instance.value, "xrDestroyInstance");
    const auto getInstance = load<PFN_xrGetInstanceProperties>(get, instance.value, "xrGetInstanceProperties");
    const auto getSystem = load<PFN_xrGetSystem>(get, instance.value, "xrGetSystem");
    const auto getSystemProperties = load<PFN_xrGetSystemProperties>(get, instance.value, "xrGetSystemProperties");
    XrInstanceProperties before{};
    before.type = XR_TYPE_INSTANCE_PROPERTIES;
    XrSystemGetInfo systemInfo{};
    systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    if (!instance.destroy || !getInstance || !getSystem || !getSystemProperties ||
        getInstance(instance.value, &before) != XR_SUCCESS ||
        std::strcmp(before.runtimeName, edvr::kNativeGazeOpenXrRuntime) != 0 ||
        getSystem(instance.value, &systemInfo, &system) != XR_SUCCESS || system == XR_NULL_SYSTEM_ID) {
        std::puts("{\"error\":\"live SteamVR OpenXR instance and HMD required\"}");
        return 1;
    }

    // This process owns the only instance. Its sequential lifetime brackets
    // both production polls and the subsequent health checks; no session exists.
    constexpr std::uint64_t instanceEpoch = 1; // This process creates one owned instance.
    const auto first = edvr::nativeGazePoll(instanceEpoch, reinterpret_cast<const void*>(getInstance));
    const auto second = edvr::nativeGazePoll(instanceEpoch);
    XrInstanceProperties after{};
    after.type = XR_TYPE_INSTANCE_PROPERTIES;
    XrSystemProperties systemProperties{};
    systemProperties.type = XR_TYPE_SYSTEM_PROPERTIES;
    const bool healthy = getInstance(instance.value, &after) == XR_SUCCESS &&
        std::strcmp(after.runtimeName, edvr::kNativeGazeOpenXrRuntime) == 0 &&
        after.runtimeVersion == before.runtimeVersion &&
        getSystemProperties(instance.value, system, &systemProperties) == XR_SUCCESS &&
        systemProperties.systemId == system;
    const bool paired = receipt(first) && receipt(second) &&
        second.querySequence == first.querySequence + 1 && second.receiptQpc >= first.receiptQpc &&
        second.receiptQpcFrequency == first.receiptQpcFrequency;
    const XrResult destroyed = instance.destroy(instance.value);
    instance.value = XR_NULL_HANDLE;
    std::printf("{\"backend\":\"steamvr_client_core\",\"initialized_runtime\":0,\"xr_instances_created\":1,"
        "\"xr_sessions_created\":0,\"provider_queried\":[%s,%s],\"provider_returned_valid\":[%s,%s],"
        "\"failures\":[%u,%u],\"projection_parallel_pair_valid\":[%s,%s],"
        "\"receipt_qpc\":[%llu,%llu],\"receipt_frequency\":%llu,\"paired_receipts\":%s,"
        "\"instance_healthy\":%s,\"instance_function_in_client\":%s,"
        "\"xr_destroy_result\":%d,\"coordinates_logged\":0}\n",
        first.providerQueried ? "true" : "false", second.providerQueried ? "true" : "false",
        first.providerReturnedValid ? "true" : "false", second.providerReturnedValid ? "true" : "false",
        static_cast<unsigned>(first.failure), static_cast<unsigned>(second.failure),
        first.eyes[0].projectionValid && first.eyes[1].projectionValid ? "true" : "false",
        second.eyes[0].projectionValid && second.eyes[1].projectionValid ? "true" : "false",
        static_cast<unsigned long long>(first.receiptQpc), static_cast<unsigned long long>(second.receiptQpc),
        static_cast<unsigned long long>(second.receiptQpcFrequency), paired ? "true" : "false",
        healthy ? "true" : "false", first.instanceFunctionInClient ? "true" : "false",
        static_cast<int>(destroyed));
    return paired && healthy && destroyed == XR_SUCCESS ? 0 : 1;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 1 || (argc == 2 && std::wcscmp(argv[1], L"--dry-run") == 0)) {
        std::puts("{\"mode\":\"DRY\",\"provider_calls\":0,\"dynamic_dll_loads\":0,\"required_cli\":\"--run ABSOLUTE_OPENXR_LOADER\"}");
        return 0;
    }
    if (argc != 3 || std::wcscmp(argv[1], L"--run") != 0 || std::wcslen(argv[2]) < 3 ||
        argv[2][1] != L':' || (argv[2][2] != L'\\' && argv[2][2] != L'/')) {
        std::fputs("usage: --dry-run | --run ABSOLUTE_OPENXR_LOADER\n", stderr);
        return 2;
    }
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return 2;
    HANDLE thread = CreateThread(nullptr, 0, watchdog, event, 0, nullptr);
    if (!thread) { CloseHandle(event); return 2; }
    const int result = run(argv[2]);
    SetEvent(event);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CloseHandle(event);
    return result;
}

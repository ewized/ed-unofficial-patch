#include "../../src/d3d11/native_gaze_policy.h"
#include "win_stub/tlhelp32.h"
#include "../../src/d3d11/openvr026_official/openvr.h"
#include "../../src/d3d11/openvr026_official/ivrclientcore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <initializer_list>
#include <limits>

namespace {
using namespace edvr;
using namespace vr;
using namespace edvr::native_gaze_detail;

unsigned checks{}, failures{};
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", label); }
}

enum class Fault {None, ModuleAbsent, WrongBase, WrongPath, CasePath, MissingFactory, ForeignFactory, MissingCore,
    CoreError, MissingSystem, SystemError, NoGaze, NonFinite, OutOfRange, NoClock, Canted};
enum class Census {Single, Two, TwoSecondWithoutFactory, Incomplete, BadLength, Retry, TruncatedPath, Oversized};
Fault fault{};
Census census{};
DWORD lastError{};
unsigned snapshots{}, snapshotCloses{}, snapshotIndex{}, retries{};
unsigned moduleRefs{}, moduleReleases{}, liveRefs{}, factoryLookups{}, factoryCalls{}, systemGets{}, gazeCalls{};
unsigned projectionCalls{}, transformCalls{}, receiptCalls{};
int coreModule{}, foreignModule{}, shimModule{}, snapshotHandle{}, xrFunction{};
HMODULE originalImage = &coreModule;
HMODULE selectedFixtureImage = &coreModule;
HMODULE heldImage{};
constexpr wchar_t corePath[] = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\vrclient_x64.dll";
constexpr wchar_t alternatePath[] = L"D:\\Custom SteamLibrary\\SteamVR\\bin\\vrclient_x64.dll";
const wchar_t* selectedFixturePath = corePath;

// Only the production adapter's three methods may be used. Reject any
// accidental call to another slot, including a future eye-tracking API.
struct System final : IVRSystem {
    float x{};
    HmdMatrix44_t GetProjectionMatrix(EVREye eye, float nearZ, float farZ) override {
        ++projectionCalls;
        check(liveRefs == 1 && nearZ == .1f && farZ == 100.f, "borrow owns a module reference while reading each projection");
        HmdMatrix44_t result{};
        result.m[0][0] = eye == Eye_Left ? 1.2f : 1.3f;
        result.m[1][1] = 1.4f;
        result.m[0][2] = eye == Eye_Left ? .1f : -.1f;
        result.m[3][2] = -1;
        return result;
    }
    HmdMatrix34_t GetEyeToHeadTransform(EVREye eye) override {
        ++transformCalls;
        check(liveRefs == 1, "borrow owns a module reference while reading each transform");
        HmdMatrix34_t result{};
        result.m[0][0] = result.m[1][1] = result.m[2][2] = 1.f;
        // Per-eye position is irrelevant to parallel-panel eligibility.
        result.m[0][3] = eye == Eye_Left ? -.031f : .033f;
        if (fault == Fault::Canted && eye == Eye_Right) result.m[0][2] = .1f;
        return result;
    }
    bool GetEyeTrackedFoveationCenter(HmdVector2_t* left, HmdVector2_t* right) override {
        ++gazeCalls;
        check(liveRefs == 1, "borrow owns a module reference during gaze query");
        *left = {{x, .25f}};
        *right = {{-.5f, .75f}};
        if (fault == Fault::NonFinite) left->v[0] = (std::numeric_limits<float>::infinity)();
        if (fault == Fault::OutOfRange) right->v[1] = 1.1f;
        return fault != Fault::NoGaze;
    }
    void GetRecommendedRenderTargetSize(uint32_t *, uint32_t *) override { std::abort(); }
    void GetProjectionRaw(EVREye, float *, float *, float *, float *) override { std::abort(); }
    bool ComputeDistortion(EVREye, float, float, DistortionCoordinates_t *) override { std::abort(); }
    bool ComputeDistortionSet(EVREye, EVRDistortionChannel, bool, uint32_t, const DistortionCoordinate_t *, DistortionCoordinate_t *) override { std::abort(); }
    bool GetTimeSinceLastVsync(float *, uint64_t *) override { std::abort(); }
    int32_t GetD3D9AdapterIndex() override { std::abort(); }
    void GetDXGIOutputInfo(int32_t *) override { std::abort(); }
    void GetOutputDevice(uint64_t *, ETextureType, VkInstance_T *) override { std::abort(); }
    bool IsDisplayOnDesktop() override { std::abort(); }
    bool SetDisplayVisibility(bool) override { std::abort(); }
    void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin, float, TrackedDevicePose_t *, uint32_t) override { std::abort(); }
    HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override { std::abort(); }
    HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override { std::abort(); }
    uint32_t GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass, vr::TrackedDeviceIndex_t *, uint32_t, vr::TrackedDeviceIndex_t) override { std::abort(); }
    EDeviceActivityLevel GetTrackedDeviceActivityLevel(vr::TrackedDeviceIndex_t) override { std::abort(); }
    void ApplyTransform(TrackedDevicePose_t *, const TrackedDevicePose_t *, const HmdMatrix34_t *) override { std::abort(); }
    vr::TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(vr::ETrackedControllerRole) override { std::abort(); }
    vr::ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(vr::TrackedDeviceIndex_t) override { std::abort(); }
    ETrackedDeviceClass GetTrackedDeviceClass(vr::TrackedDeviceIndex_t) override { std::abort(); }
    bool IsTrackedDeviceConnected(vr::TrackedDeviceIndex_t) override { std::abort(); }
    bool GetBoolTrackedDeviceProperty(vr::TrackedDeviceIndex_t, ETrackedDeviceProperty, ETrackedPropertyError *) override { std::abort(); }
    float GetFloatTrackedDeviceProperty(vr::TrackedDeviceIndex_t, ETrackedDeviceProperty, ETrackedPropertyError *) override { std::abort(); }
    int32_t GetInt32TrackedDeviceProperty(vr::TrackedDeviceIndex_t, ETrackedDeviceProperty, ETrackedPropertyError *) override { std::abort(); }
    uint64_t GetUint64TrackedDeviceProperty(vr::TrackedDeviceIndex_t, ETrackedDeviceProperty, ETrackedPropertyError *) override { std::abort(); }
    HmdMatrix34_t GetMatrix34TrackedDeviceProperty(vr::TrackedDeviceIndex_t, ETrackedDeviceProperty, ETrackedPropertyError *) override { std::abort(); }
    uint32_t GetArrayTrackedDeviceProperty(vr::TrackedDeviceIndex_t, ETrackedDeviceProperty, PropertyTypeTag_t, void *, uint32_t, ETrackedPropertyError *) override { std::abort(); }
    uint32_t GetStringTrackedDeviceProperty(vr::TrackedDeviceIndex_t, ETrackedDeviceProperty, char *, uint32_t, ETrackedPropertyError *) override { std::abort(); }
    const char *GetPropErrorNameFromEnum(ETrackedPropertyError) override { std::abort(); }
    bool PollNextEvent(VREvent_t *, uint32_t) override { std::abort(); }
    bool PollNextEventWithPose(ETrackingUniverseOrigin, VREvent_t *, uint32_t, vr::TrackedDevicePose_t *) override { std::abort(); }
    bool PollNextEventWithPoseAndOverlays(vr::ETrackingUniverseOrigin, VREvent_t *, uint32_t, TrackedDevicePose_t *, VROverlayHandle_t *) override { std::abort(); }
    const char *GetEventTypeNameFromEnum(EVREventType) override { std::abort(); }
    HiddenAreaMesh_t GetHiddenAreaMesh(EVREye, EHiddenAreaMeshType) override { std::abort(); }
    bool GetEyeTrackedFoveationCenterForProjection(const HmdMatrix44_t *, HmdVector2_t *) override { std::abort(); }
    bool GetControllerState(vr::TrackedDeviceIndex_t, vr::VRControllerState_t *, uint32_t) override { std::abort(); }
    bool GetControllerStateWithPose(ETrackingUniverseOrigin, vr::TrackedDeviceIndex_t, vr::VRControllerState_t *, uint32_t, TrackedDevicePose_t *) override { std::abort(); }
    void TriggerHapticPulse(vr::TrackedDeviceIndex_t, uint32_t, unsigned short) override { std::abort(); }
    const char *GetButtonIdNameFromEnum(EVRButtonId) override { std::abort(); }
    const char *GetControllerAxisTypeNameFromEnum(EVRControllerAxisType) override { std::abort(); }
    bool IsInputAvailable() override { std::abort(); }
    bool IsSteamVRDrawingControllers() override { std::abort(); }
    bool ShouldApplicationPause() override { std::abort(); }
    bool ShouldApplicationReduceRenderingWork() override { std::abort(); }
    vr::EVRFirmwareError PerformFirmwareUpdate(vr::TrackedDeviceIndex_t) override { std::abort(); }
    void AcknowledgeQuit_Exiting() override { std::abort(); }
    uint32_t GetAppContainerFilePaths(char *, uint32_t) override { std::abort(); }
    const char *GetRuntimeVersion() override { std::abort(); }
    vr::EVRInitError SetSDKVersion(uint32_t, uint32_t, uint32_t) override { std::abort(); }
};

System systems[2];
struct Core final : IVRClientCore {
    void* GetGenericInterface(const char* version, EVRInitError* error) override {
        check(std::strcmp(version, IVRSystem_Version) == 0 && liveRefs == 1,
            "request official 026 while the core module is referenced");
        ++systemGets;
        *error = fault == Fault::SystemError ? VRInitError_Init_NotInitialized : VRInitError_None;
        return fault == Fault::MissingSystem ? nullptr : &systems[(systemGets - 1) % 2];
    }
    EVRInitError Init(EVRApplicationType, const char*) override { std::abort(); }
    void Cleanup() override { std::abort(); }
    EVRInitError IsInterfaceVersionValid(const char*) override { std::abort(); }
    bool BIsHmdPresent() override { std::abort(); }
    const char* GetEnglishStringForHmdError(EVRInitError) override { std::abort(); }
    const char* GetIDForVRInitError(EVRInitError) override { std::abort(); }
} core;

void* fakeFactory(const char* version, int* error) {
    ++factoryCalls;
    check(std::strcmp(version, IVRClientCore_Version) == 0 && liveRefs == 1,
        "request official _003 from verified borrowed module");
    if (fault == Fault::CoreError) *error = VRInitError_Init_NotInitialized;
    return fault == Fault::MissingCore ? nullptr : &core;
}
}  // namespace

DWORD GetCurrentProcessId() noexcept { return 12; }
DWORD GetLastError() noexcept { return lastError; }
void SetLastError(DWORD error) noexcept { lastError = error; }

HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid) noexcept {
    check(flags == TH32CS_SNAPMODULE && pid == GetCurrentProcessId(), "snapshot only this process's modules");
    ++snapshots;
    if (census == Census::BadLength || (census == Census::Retry && retries++ < 2)) {
        lastError = ERROR_BAD_LENGTH;
        return INVALID_HANDLE_VALUE;
    }
    snapshotIndex = 0;
    return &snapshotHandle;
}

BOOL CloseHandle(HANDLE snapshot) noexcept {
    check(snapshot == &snapshotHandle, "close the one module snapshot");
    ++snapshotCloses;
    return 1;
}

static BOOL snapshotEntry(HANDLE snapshot, MODULEENTRY32W* entry) noexcept {
    if (snapshot != &snapshotHandle || entry->dwSize != sizeof(*entry)) std::abort();
    if (snapshotIndex == 0 || (census == Census::Oversized && snapshotIndex > 1 && snapshotIndex <= 4096)) {
        entry->hModule = &shimModule;
        std::wmemcpy(entry->szModule, L"openvr_api.dll", sizeof(L"openvr_api.dll") / sizeof(wchar_t));
        const wchar_t shimPath[] = L"C:\\SteamVR\\bin\\win64\\openvr_api.dll";
        std::wmemcpy(entry->szExePath, shimPath, std::size(shimPath));
        return 1;
    }
    if (snapshotIndex == 1 && fault != Fault::ModuleAbsent) {
        entry->hModule = selectedFixtureImage;
        std::wmemcpy(entry->szModule, L"VRCLIENT_X64.dll", sizeof(L"VRCLIENT_X64.dll") / sizeof(wchar_t));
        if (census == Census::TruncatedPath) std::wmemset(entry->szExePath, L'x', MAX_PATH);
        else std::wmemcpy(entry->szExePath, selectedFixturePath, std::wcslen(selectedFixturePath) + 1);
        return 1;
    }
    if (snapshotIndex == 2 && (census == Census::Two || census == Census::TwoSecondWithoutFactory)) {
        entry->hModule = &foreignModule;
        std::wmemcpy(entry->szModule, L"vrclient_x64.dll", sizeof(L"vrclient_x64.dll") / sizeof(wchar_t));
        const wchar_t otherPath[] = L"E:\\Another SteamVR\\bin\\vrclient_x64.dll";
        std::wmemcpy(entry->szExePath, otherPath, std::size(otherPath));
        return 1;
    }
    // An incomplete walk can already have yielded a seemingly valid client.
    if (census != Census::Incomplete) lastError = ERROR_NO_MORE_FILES;
    return 0;
}

BOOL Module32FirstW(HANDLE snapshot, MODULEENTRY32W* entry) noexcept { return snapshotEntry(snapshot, entry); }
BOOL Module32NextW(HANDLE snapshot, MODULEENTRY32W* entry) noexcept {
    ++snapshotIndex;
    return snapshotEntry(snapshot, entry);
}

bool GetModuleHandleExW(DWORD flags, LPCWSTR value, HMODULE* module) noexcept {
    if (flags == GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        check(value == reinterpret_cast<LPCWSTR>(originalImage), "take a reference to the epoch's original image address");
        ++moduleRefs;
        ++liveRefs;
        heldImage = *module = fault == Fault::WrongBase ? &foreignModule : originalImage;
    } else {
        check(flags == (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) &&
            (value == reinterpret_cast<LPCWSTR>(&fakeFactory) || value == reinterpret_cast<LPCWSTR>(&xrFunction)),
            "check exact function ownership without retaining another reference");
        *module = fault == Fault::ForeignFactory ? &foreignModule : originalImage;
    }
    return true;
}

bool FreeLibrary(HMODULE module) noexcept {
    check(module == heldImage && liveRefs == 1, "release exactly the acquired core reference");
    heldImage = nullptr;
    ++moduleReleases;
    --liveRefs;
    return true;
}

int CompareStringOrdinal(LPCWSTR a, int aLength, LPCWSTR b, int bLength, BOOL ignoreCase) noexcept {
    if (aLength != -1 || bLength != -1 || !ignoreCase) std::abort();
    for (; *a && *b; ++a, ++b) if (std::towlower(*a) != std::towlower(*b)) return 1;
    return *a == *b ? CSTR_EQUAL : 1;
}

DWORD GetModuleFileNameW(HMODULE, wchar_t* path, DWORD capacity) noexcept {
    const wchar_t* source = selectedFixturePath;
    if (fault == Fault::WrongPath) source = L"C:\\Game\\vrclient_x64.dll";
    if (fault == Fault::CasePath) source = L"d:\\CUSTOM STEAMLIBRARY\\SteamVR\\bin\\vrclient_x64.dll";
    const auto size = std::wcslen(source);
    if (size + 1 > capacity) return capacity;
    std::wmemcpy(path, source, size + 1);
    return static_cast<DWORD>(size);
}

FARPROC GetProcAddress(HMODULE module, const char* name) noexcept {
    ++factoryLookups;
    check(std::strcmp(name, "VRClientCoreFactory") == 0,
        "only request the client-core factory; never SDK init, cleanup or token exports");
    if (module == &foreignModule && census == Census::TwoSecondWithoutFactory) return nullptr;
    check(module == originalImage, "only inspect the one selected client");
    return fault == Fault::MissingFactory ? nullptr : reinterpret_cast<FARPROC>(fakeFactory);
}

bool QueryPerformanceCounter(LARGE_INTEGER* output) noexcept {
    ++receiptCalls;
    check(receiptCalls == gazeCalls, "timestamp each gaze receipt after its call");
    output->QuadPart = 1000 + receiptCalls;
    return fault != Fault::NoClock;
}
bool QueryPerformanceFrequency(LARGE_INTEGER* output) noexcept {
    output->QuadPart = 1000;
    return true;
}

int main(int argc, char** argv) {
    if (argc != 2 || std::strcmp(argv[1], "--self-test") != 0) return 2;
    std::uint64_t epoch = 0;
    const auto off = nativeGazePoll(epoch);
    check(!off.providerQueried && !off.usable && off.sensorTimestampUnavailable && !snapshots && !moduleRefs && !gazeCalls,
        "OFF has no module, runtime or clock effects");
    check(isClientImageName(L"VRClient_x64.DLL") && !isClientImageName(L"openvr_api.dll") &&
        !isClientImageName(L"vrclient_x64.dll.other"), "census recognizes only the core module basename");

    for (const auto unavailable : {Fault::ModuleAbsent, Fault::WrongBase, Fault::WrongPath, Fault::MissingFactory,
             Fault::ForeignFactory, Fault::MissingCore, Fault::CoreError, Fault::MissingSystem, Fault::SystemError}) {
        fault = unavailable;
        const auto missing = nativeGazePoll(++epoch);
        check(!missing.usable && !missing.providerQueried && !missing.querySequence && !missing.receiptQpc &&
            !gazeCalls && moduleRefs == moduleReleases,
            "an unavailable existing core stays full rate without initializing or querying it");
    }
    check(factoryCalls == 4 && systemGets == 2, "missing or wrong-owner factory never runs and core errors never reach system");

    fault = Fault::None;
    systems[0].x = 1.f;
    systems[1].x = -1.f;
    selectedFixturePath = alternatePath;
    const unsigned beforeAlternate = snapshots;
    const auto first = nativeGazePoll(++epoch, &xrFunction);
    // A later duplicate must not switch this epoch's originally selected image.
    census = Census::Two;
    fault = Fault::CasePath;
    const auto second = nativeGazePoll(epoch);
    fault = Fault::None;
    check(snapshots == beforeAlternate + 1 && first.instanceFunctionInClient && !second.instanceFunctionInClient,
        "custom install and optional function witness work without repeating the census");
    census = Census::Single;
    check(first.usable && first.providerQueried && first.providerReturnedValid && first.sensorTimestampUnavailable &&
        first.querySequence == 1 && first.receiptQpc == 1001 && first.receiptQpcFrequency == 1000,
        "a borrowed system publishes a fresh real-time receipt and both eyes");
    check(first.eyes[0].xNdc == 1.f && first.eyes[0].yNdc == .25f &&
        first.eyes[1].xNdc == -.5f && first.eyes[1].yNdc == .75f &&
        second.eyes[0].xNdc == -1.f && second.querySequence == 2 &&
        second.receiptQpc > first.receiptQpc && factoryCalls == 6 && systemGets == 4 && moduleRefs == moduleReleases,
        "per-eye NDC stays distinct and the next poll reacquires and releases all borrowed objects");
    check(first.eyes[0].projectionValid && first.eyes[1].projectionValid &&
        first.eyes[0].projection[0] == 1.2f && first.eyes[1].projection[0] == 1.3f &&
        first.eyes[0].projection[2] == .1f && first.eyes[1].projection[2] == -.1f &&
        projectionCalls == 4 && transformCalls == 4,
        "per-eye projection parameters and parallel transforms reach the consumer");

    const Fault invalidCases[]{Fault::NoGaze, Fault::NonFinite, Fault::OutOfRange, Fault::NoClock};
    const NativeGazeFailure invalidReasons[]{NativeGazeFailure::ProviderInvalid, NativeGazeFailure::NonFinite,
        NativeGazeFailure::OutOfRange, NativeGazeFailure::ReceiptClockUnavailable};
    for (unsigned i = 0; i < sizeof(invalidCases) / sizeof(invalidCases[0]); ++i) {
        fault = invalidCases[i];
        const auto invalid = nativeGazePoll(epoch);
        check(invalid.providerQueried && !invalid.usable && invalid.failure == invalidReasons[i] &&
            moduleRefs == moduleReleases && projectionCalls == 4 && transformCalls == 4,
            "invalid gaze or receipt does not reach projection lookup or coarse shading");
    }
    fault = Fault::Canted;
    const auto canted = nativeGazePoll(epoch);
    check(canted.usable && canted.eyes[0].projectionValid && !canted.eyes[1].projectionValid,
        "off-axis panel transform still refuses that eye's projection mapping");

    fault = Fault::None;
    const unsigned beforeRefusals = factoryLookups;
    const Census invalidCensuses[]{Census::Two, Census::TwoSecondWithoutFactory, Census::Incomplete,
        Census::TruncatedPath, Census::BadLength, Census::Oversized};
    for (const auto invalid : invalidCensuses) {
        census = invalid;
        const unsigned beforeScan = snapshots;
        const auto refused = nativeGazePoll(++epoch);
        const auto reason = invalid == Census::Two || invalid == Census::TwoSecondWithoutFactory ?
            NativeGazeFailure::ModuleAmbiguous : NativeGazeFailure::ModuleCensusIncomplete;
        check(refused.failure == reason && !refused.providerQueried && factoryLookups == beforeRefusals,
            "refuse ambiguous or incomplete census before any factory regardless of export support");
        const unsigned afterScan = snapshots;
        census = Census::Single;
        check(nativeGazePoll(epoch).failure == reason && snapshots == afterScan &&
            afterScan == beforeScan + (invalid == Census::BadLength ? 3 : 1),
            "failed census stays retired in its epoch and transient retries are bounded");
    }

    census = Census::Retry;
    selectedFixtureImage = originalImage = &foreignModule;
    selectedFixturePath = corePath;
    const unsigned beforeRetry = snapshots;
    check(nativeGazePoll(++epoch).usable && snapshots == beforeRetry + 3 && retries == 3,
        "new epoch may select the other mapping after a bounded snapshot retry");
    const unsigned beforeChange = factoryCalls;
    fault = Fault::WrongPath;
    check(nativeGazePoll(epoch).failure == NativeGazeFailure::ModulePathMismatch && factoryCalls == beforeChange,
        "changed full path at the selected base refuses before exported code runs");
    fault = Fault::None;
    check(nativeGazePoll(epoch).failure == NativeGazeFailure::ModulePathMismatch && factoryCalls == beforeChange,
        "identity refusal prevents reselection until the next admitted epoch");
    selectedFixtureImage = originalImage = &coreModule;
    census = Census::Single;
    check(nativeGazePoll(++epoch).usable, "later epoch can borrow a newly selected image");
    fault = Fault::WrongBase;
    const unsigned beforeWrongBase = factoryLookups;
    check(nativeGazePoll(epoch).failure == NativeGazeFailure::ModulePathMismatch && factoryLookups == beforeWrongBase,
        "same-epoch base change is rejected before factory lookup even with the old path");
    fault = Fault::None;
    check(nativeGazePoll(epoch).failure == NativeGazeFailure::ModulePathMismatch && factoryLookups == beforeWrongBase,
        "same-epoch base refusal cannot silently select a later image");
    check(moduleRefs == moduleReleases && !liveRefs && snapshots - snapshotCloses == 5,
        "all successful references and snapshots are balanced");

    std::printf("%s native_gaze --self-test checks=%u failures=%u live_runtime_calls=0 gpu_devices=0\n",
        failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

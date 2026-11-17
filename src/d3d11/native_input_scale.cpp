#include "native_input_scale.h"
#include "../common/config.h"
#include "../common/log.h"
#include "../common/native_render_settings.h"
#include "../openxr/native_local_config.h"
#include "dlaa.h"
#include <mutex>
#include <cwchar>
#include <cstdio>
#include <cstring>

namespace {
std::mutex mutex;
edvr::native_input::StartupLatch startup;
HMODULE retainedBridge = nullptr;
uint32_t pairingError = 0;

std::wstring normalized(std::wstring value) {
    for (auto& c : value) if (c == L'/') c = L'\\';
    return value;
}
bool samePath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(normalized(a).c_str(), normalized(b).c_str()) == 0;
}
bool pairedBridge() {
    HMODULE graphics = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&pairedBridge), &graphics)) { pairingError = GetLastError(); return false; }
    wchar_t filename[32768]{};
    const DWORD n = GetModuleFileNameW(graphics, filename, 32768);
    if (!n || n >= 32768) { pairingError = ERROR_INVALID_NAME; return false; }
    const std::wstring graphicsPath(filename);
    const auto slash = graphicsPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) { pairingError = ERROR_INVALID_NAME; return false; }
    const std::wstring folder = graphicsPath.substr(0, slash) + L"\\Openvr\\win64";
    edvr::openxr::LocalConfig config;
    if (edvr::openxr::readLocalConfig(folder + L"\\edvr_openxr.ini", config) != edvr::openxr::LocalConfigResult::Ready ||
        !samePath(config.graphics, graphicsPath)) { pairingError = ERROR_INVALID_DATA; return false; }
    const std::wstring bridgePath = folder + L"\\openvr_api.dll";
    // This native module has no user DllMain. Its XR/device startup is lazy,
    // inside VR_InitInternal; this calls ONLY the constant CPU capability.
    // Loading here is outside DllMain, before any game sampler is created.
    retainedBridge = LoadLibraryExW(bridgePath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!retainedBridge) { pairingError = GetLastError(); return false; }
    wchar_t actual[32768]{};
    const DWORD length = GetModuleFileNameW(retainedBridge, actual, 32768);
    if (!length || length >= 32768 || !samePath(actual, bridgePath)) { pairingError = ERROR_INVALID_NAME; return false; }
    const auto address = GetProcAddress(retainedBridge, "edvrNativeInputScaleCapability");
    HMODULE owner = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &owner) || owner != retainedBridge ||
        reinterpret_cast<EdvrNativeInputScaleCapability>(address)(EDVR_NATIVE_INPUT_SCALE_CAPABILITY_EXACT_ASK) != EDVR_NATIVE_INPUT_SCALE_CAPABILITY_EXACT_ASK) {
        pairingError = ERROR_REVISION_MISMATCH; return false;
    }
    // Retain until process exit. Never VR_Init/Shutdown, unload, pose or input.
    return true;
}
bool writeAnswer(void* out, const EdvrNativeInputScaleSettings& value) noexcept {
    __try { *static_cast<EdvrNativeInputScaleSettings*>(out) = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
EdvrNativeInputScaleSettings snapshot() {
    std::lock_guard<std::mutex> lock(mutex);
    return startup.info;
}
}

namespace edvr {
float nativeInputScaleSaved(Config& cfg) { return cfg.getFloat("fix.native_input_scale", 1.0f); }
void nativeInputScaleCapture(Config& cfg, bool hmdKnown, float hmd, float ssaa) {
    std::lock_guard<std::mutex> lock(mutex);
    if (startup.captured) return;
    const float wanted = nativeInputScaleSaved(cfg);
    const bool trimEmpty = cfg.getString("experimental.fov_trim_vertical", "").empty() &&
        cfg.getString("experimental.fov_trim_outer", "").empty() &&
        cfg.getString("experimental.fov_trim_nasal", "").empty();
    const bool dlss = _stricmp(cfg.getString("fix.temporal_aa", "off").c_str(), "dlss") == 0;
    const bool needsPair = native_input::validScale(wanted) && wanted != 1 && hmdKnown &&
        std::isfinite(hmd) && std::fabs(hmd - 1.0f) <= 1.0e-5f && std::isfinite(ssaa) &&
        std::fabs(ssaa-1.0f)<=1.0e-5f && trimEmpty && dlss;
    const auto policy = native_input::choose(wanted, hmdKnown, hmd, trimEmpty, dlss,
        needsPair ? pairedBridge() : false,ssaa);
    startup.capture(wanted,policy);
    if (wanted != 1) Log::get().note(
        "native input scale: startup requested=%.9f active=%.9f policy=%s pairing_error=%u; "
        "immutable before samplers, later edits require restart; nondefault requires 3200x3200 output",
        double(wanted), double(startup.info.activeScale), native_input::reason(policy), pairingError);
}
float nativeInputScaleLatched() { return snapshot().requestedScale; }
float nativeInputScaleActive() { return snapshot().activeScale; }
std::string nativeInputScaleMenuStatus() {
    const auto s = snapshot();
    const float saved = nativeInputScaleSaved(Config::get());
    char line[200]{};
    DlaaModelSelection left{}, right{};
    const bool have = dlaaModelSelection(0, &left) && dlaaModelSelection(1, &right) &&
        left.recordedAtMs && right.recordedAtMs && GetTickCount64() - left.recordedAtMs <= 7000 &&
        GetTickCount64() - right.recordedAtMs <= 7000;
    const bool pending = std::fabs(saved-s.requestedScale)>1.0e-5f;
    const auto policy=static_cast<native_input::Policy>(s.policy);
    const char* refusal=native_input::accepted(policy)?"":native_input::reason(policy);
    if (have) std::snprintf(line, sizeof(line),
        "Saved %.4f; boot %.4f active %.4f%s %s. L/R %ux%u/%ux%u -> %ux%u/%ux%u. %s hint %s/%s; driver model unverified%s",
        double(saved), double(s.requestedScale), double(s.activeScale), pending ? " RESTART" : "",
        refusal,
        left.inputW, left.inputH, right.inputW, right.inputH, left.outputW, left.outputH, right.outputW, right.outputH,
        left.mode, left.appliedHint, right.appliedHint, left.evaluationSucceeded && right.evaluationSucceeded ? "" : "; NGX error");
    else std::snprintf(line, sizeof(line), "Saved %.4f; boot %.4f active %.4f%s. %s; observed NGX dimensions/mode unavailable or stale",
        double(saved), double(s.requestedScale), double(s.activeScale), pending ? " RESTART" : "",
        native_input::reason(static_cast<native_input::Policy>(s.policy)));
    return line;
}
}

extern "C" BOOL WINAPI edvrQueryNativeInputScale(uint32_t version, uint32_t size, void* output) {
    if (!output || version != EDVR_NATIVE_INPUT_SCALE_VERSION_1 || size != sizeof(EdvrNativeInputScaleSettings)) return FALSE;
    const auto value = snapshot();
    return writeAnswer(output, value) ? TRUE : FALSE;
}

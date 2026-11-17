#include "../../src/d3d11/native_gaze_policy.h"
#include "win_stub/windows.h"
#include "../../src/d3d11/openvr026_official/openvr.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <limits>

namespace {
using namespace edvr;
using namespace edvr::native_gaze_detail;
unsigned checks{}, failures{}, adapterLoads{}, adapterGets{}, adapterInits{}, adapterClocks{};
const char* adapterMode{};

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL %s\n", label); }
}
bool mode(const char* name) noexcept { return adapterMode && std::strcmp(adapterMode, name) == 0; }

struct Fixture {
    unsigned loads{}, verifications{}, gets{}, inits{}, queries{}, clocks{};
    bool modulePresent{true}, verified{true}, borrow{true}, afterInit{true}, initOk{true};
    bool interfaceError{}, returnedValid{true}, clockOk{true}, zeroClock{}, zeroFrequency{};
    bool clockAfterQuery{};
    bool tokenAvailable{true},changeDuringQuery{},changeDuringInit{};
    uint32_t sdkToken{0};unsigned tokenReads{};
    float ndc[2][2]{{.1f, .2f}, {-.3f, .4f}};
    static Fixture& self(void* p) noexcept { return *static_cast<Fixture*>(p); }
    static void* token() noexcept { return reinterpret_cast<void*>(std::uintptr_t{1}); }
    static void* load(void* p) noexcept {
        auto& f = self(p); ++f.loads; return f.modulePresent ? token() : nullptr;
    }
    static bool verify(void* p, void* handle) noexcept {
        auto& f = self(p); ++f.verifications; return handle == token() && f.verified;
    }
    static void* get(void* p, void*, int* error) noexcept {
        auto& f = self(p); ++f.gets;
        const bool available = f.inits ? f.afterInit : f.borrow;
        *error = f.interfaceError || !available ? 1 : 0;
        return available ? token() : nullptr;
    }
    static bool init(void* p, void*, int* error) noexcept {
        auto& f = self(p); ++f.inits;if(f.changeDuringInit)++f.sdkToken;
        *error = f.initOk ? 0 : 1; return f.initOk;
    }
    static bool query(void* p, void*, float out[2][2]) noexcept {
        auto& f = self(p); ++f.queries;
        if(f.changeDuringQuery)++f.sdkToken;
        for (unsigned eye = 0; eye < 2; ++eye)
            for (unsigned axis = 0; axis < 2; ++axis) out[eye][axis] = f.ndc[eye][axis];
        return f.returnedValid;
    }
    static bool clock(void* p, std::uint64_t* qpc, std::uint64_t* rate) noexcept {
        auto& f = self(p); ++f.clocks; f.clockAfterQuery = f.clocks == f.queries;
        *qpc = f.zeroClock ? 0 : 1000 + f.queries;
        *rate = f.zeroFrequency ? 0 : 1000;
        return f.clockOk;
    }
    static bool readToken(void* p,void*,uint32_t* value) noexcept {
        auto& f=self(p);++f.tokenReads;*value=f.sdkToken;return f.tokenAvailable;
    }
    Ops ops() noexcept { return {this, load, verify, get, init, query, clock,readToken}; }
};

void policyTests() {
    Fixture f; State state;
    auto sample = poll(false, state, f.ops());
    check(!state.attempted && !state.sequence && !f.loads && !f.queries && !f.clocks,
        "OFF does not load, initialize, query, clock or mutate state");
    check(!sample.usable && !sample.providerQueried && sample.sensorTimestampUnavailable &&
        !sample.receiptQpc && !sample.receiptQpcFrequency && !sample.querySequence,
        "OFF has no validity or invented timestamp");
    check(pathMatches(clientPath) && pathMatches(L"c:\\PROGRAM FILES (X86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openvr_api.dll"),
        "exact absolute path permits case differences");
    check(!pathMatches(nullptr) && !pathMatches(L"openvr_api.dll") &&
        !pathMatches(L"C:\\Game\\Openvr\\win64\\openvr_api.dll") &&
        !pathMatches(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openvr_api.dll.bad"),
        "reject missing, relative, proxy and prefix paths");
    sample = poll(true, state, f.ops());
    check(sample.usable && state.borrowed && !state.backgroundAttempted && f.gets == 1 && !f.inits,
        "borrow valid context without initialization");
    check(sample.eyes[0].xNdc == .1f && sample.eyes[0].yNdc == .2f &&
        sample.eyes[1].xNdc == -.3f && sample.eyes[1].yNdc == .4f,
        "retain both distinct actual NDCs without averaging or conversion");
    check(sample.sensorTimestampUnavailable && sample.receiptQpc == 1001 &&
        sample.receiptQpcFrequency == 1000 && sample.querySequence == 1 && f.clockAfterQuery,
        "QPC is callback receipt, never a sensor timestamp");
    f.ndc[0][0] = -.7f;
    sample = poll(true, state, f.ops());
    check(sample.eyes[0].xNdc == -.7f && sample.querySequence == 2 && f.queries == 2 &&
        f.loads == 1 && f.gets == 1 && state.retainedModule == Fixture::token(),
        "one query per poll with retained context and no cached geometry");
    poll(false, state, f.ops());
    check(f.queries == 2 && f.clocks == 2 && state.sequence == 2,
        "OFF after setup preserves state without side effects");
    f.returnedValid = false;
    sample = poll(true, state, f.ops());
    check(!sample.usable && !sample.providerReturnedValid && !sample.eyes[0].valid &&
        !sample.eyes[1].valid && sample.failure == NativeGazeFailure::ProviderInvalid,
        "provider false cannot promote or reuse valid geometry");
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        Fixture bad; State fresh; auto ops = bad.ops();
        if (scenario == 0) bad.modulePresent = false;
        if (scenario == 1) bad.verified = false;
        if (scenario == 2) ops.getInterface = nullptr;
        if (scenario == 3) { bad.borrow = false; ops.initializeBackground = nullptr; }
        if (scenario == 4) { bad.borrow = false; bad.initOk = false; }
        if (scenario == 5) { bad.borrow = false; bad.afterInit = false; }
        if (scenario == 6) bad.interfaceError = true;
        if (scenario == 7) bad.borrow = false;
        sample = poll(true, fresh, ops);
        if (scenario == 7) {
            check(sample.usable && fresh.backgroundSucceeded && bad.gets == 2 && bad.inits == 1,
                "missing context initializes Background once then reacquires");
        } else {
            check(!sample.usable && !sample.providerQueried && !bad.queries && !bad.clocks,
                "setup failure never queries geometry or clock");
            if (scenario <= 2) check(!bad.inits, "missing module/path/getter does not initialize");
            if (scenario == 0) check(!bad.verifications && !bad.gets,
                "failed load stops before module inspection");
            if (scenario == 1) check(!bad.gets, "unverified module stops before exports");
        }
        const auto inits = bad.inits;
        poll(true, fresh, ops);
        check(bad.loads == 1 && bad.inits == inits, "same lifetime/token never repeats module load or Background init");
    }
    {
        Fixture life;State s;
        check(poll(true,s,life.ops()).usable&&s.tokenKnown&&s.token==0,"token zero is accepted without inventing invalidity");
        const auto firstGets=life.gets;setLifetime(s,9);
        check(!s.system&&poll(true,s,life.ops()).usable&&life.gets==firstGets+1&&life.inits==0,
            "new admitted channel lifetime reacquires a borrowed interface without Init");
        ++life.sdkToken;
        check(poll(true,s,life.ops()).usable&&life.gets==firstGets+2,"SDK token change reacquires before any new virtual query");
        setLifetime(s,0);const auto q=life.queries,reads=life.tokenReads;
        check(!poll(true,s,life.ops()).usable&&life.queries==q&&life.tokenReads==reads,"closed readiness epoch performs no SDK query or token calls");
        setLifetime(s,10);life.borrow=false;life.afterInit=false;life.initOk=false;life.changeDuringInit=true;
        poll(true,s,life.ops());const auto inits=life.inits;
        setLifetime(s,0);setLifetime(s,11);poll(true,s,life.ops());poll(true,s,life.ops());
        check(inits==1&&life.inits==inits,"failed Init post-token is remembered across readiness loss and rearm");
        ++life.sdkToken;poll(true,s,life.ops());check(life.inits==2,"genuine SDK token change permits one new bounded Init attempt");
    }
    {
        Fixture changing;State s;changing.changeDuringQuery=true;
        const auto changed=poll(true,s,changing.ops());
        check(changed.providerQueried&&!changed.usable&&!changed.eyes[0].valid&&!s.system&&
            changed.failure==NativeGazeFailure::ContextChanged,"token change during query rejects the mixed-lifetime sample and clears cached interface");
        Fixture missing;State m;missing.tokenAvailable=false;
        check(!poll(true,m,missing.ops()).usable&&!missing.gets&&!missing.inits&&!missing.queries,
            "missing verified token export fails before getter, Init or virtual query");
    }
    for (unsigned eye = 0; eye < 2; ++eye) for (unsigned axis = 0; axis < 2; ++axis) {
        for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                -1.0001f, 1.0001f}) {
            Fixture bad; State fresh; bad.ndc[eye][axis] = invalid;
            sample = poll(true, fresh, bad.ops());
            check(sample.providerReturnedValid && !sample.usable && !sample.eyes[eye].valid &&
                sample.eyes[1 - eye].valid, "one invalid eye requires full rate for both");
            check(sample.failure == (std::isfinite(invalid) ? NativeGazeFailure::OutOfRange :
                NativeGazeFailure::NonFinite), "reject rather than clamp invalid NDC");
            check(!std::isfinite(invalid) ||
                (axis == 0 ? sample.eyes[eye].xNdc : sample.eyes[eye].yNdc) == invalid,
                "preserve out-of-range provider coordinates in memory");
        }
    }
    Fixture edge; State fresh;
    edge.ndc[0][0] = edge.ndc[1][1] = -1;
    edge.ndc[0][1] = edge.ndc[1][0] = 1;
    check(poll(true, fresh, edge.ops()).usable, "accept exact finite NDC boundaries");
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        Fixture bad; State clockState; auto ops = bad.ops();
        if (scenario == 0) bad.clockOk = false;
        if (scenario == 1) bad.zeroClock = true;
        if (scenario == 2) bad.zeroFrequency = true;
        if (scenario == 3) ops.receiptClock = nullptr;
        sample = poll(true, clockState, ops);
        check(sample.providerReturnedValid && sample.eyes[0].valid && sample.eyes[1].valid &&
            !sample.usable && sample.sensorTimestampUnavailable &&
            sample.failure == NativeGazeFailure::ReceiptClockUnavailable,
            "unavailable clock cannot imply freshness");
    }
}

std::uint32_t fakeInit(vr::EVRInitError* error, vr::EVRApplicationType application) noexcept {
    ++adapterInits;
    check(application == vr::VRApplication_Background, "adapter requests Background context only");
    *error = vr::VRInitError_Init_InterfaceNotFound;
    return 0;
}
void* fakeGet(const char* version, vr::EVRInitError* error) noexcept {
    ++adapterGets;
    check(std::strcmp(version, "IVRSystem_026") == 0, "adapter requests official interface026 only");
    *error = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
}  // namespace

HMODULE LoadLibraryExW(const wchar_t* path, void*, DWORD flags) noexcept {
    ++adapterLoads;
    check(edvr::native_gaze_detail::pathMatches(path), "adapter loads pinned absolute path only");
    check(flags == (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32),
        "adapter uses scoped DLL search flags");
    return mode("--adapter-load-failed") ? nullptr : Fixture::token();
}
DWORD GetModuleFileNameW(HMODULE, wchar_t* path, DWORD capacity) noexcept {
    const wchar_t* source = mode("--adapter-path-mismatch") ? L"C:\\Game\\openvr_api.dll" :
        edvr::native_gaze_detail::clientPath;
    const auto size = std::wcslen(source);
    if (size + 1 > capacity) return capacity;
    std::wmemcpy(path, source, size + 1);
    return static_cast<DWORD>(size);
}
FARPROC GetProcAddress(HMODULE, const char* name) noexcept {
    if(std::strcmp(name,"VR_GetInitToken")==0)
        return mode("--adapter-missing-token")?nullptr:reinterpret_cast<FARPROC>(+[]() noexcept -> uint32_t{return 0;});
    if (std::strcmp(name, "VR_GetGenericInterface") == 0)
        return mode("--adapter-missing-get") ? nullptr : reinterpret_cast<FARPROC>(fakeGet);
    if (std::strcmp(name, "VR_InitInternal") == 0)
        return mode("--adapter-missing-init") ? nullptr : reinterpret_cast<FARPROC>(fakeInit);
    check(false, "adapter requests no unrelated exports");
    return nullptr;
}
bool QueryPerformanceCounter(LARGE_INTEGER*) noexcept { ++adapterClocks; return false; }
bool QueryPerformanceFrequency(LARGE_INTEGER*) noexcept { ++adapterClocks; return false; }

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    adapterMode = argv[1];
    const auto off = edvr::nativeGazePoll(false);
    check(!off.providerQueried && !off.usable && off.sensorTimestampUnavailable &&
        !adapterLoads && !adapterInits && !adapterClocks, "production OFF invokes no Windows adapter");
    if (mode("--self-test")) {
        policyTests();
    } else if (mode("--adapter-missing-get") || mode("--adapter-path-mismatch") ||
        mode("--adapter-missing-init") || mode("--adapter-load-failed") ||
        mode("--adapter-init-failed") || mode("--adapter-missing-token")) {
        const auto sample = edvr::nativeGazePoll(true);
        check(!sample.providerQueried && !sample.usable && sample.sensorTimestampUnavailable,
            "failed adapter does not query provider or fabricate time");
        check(adapterInits == (mode("--adapter-init-failed") ? 1u : 0u) && !adapterClocks,
            "only verified module with required exports may initialize");
        if (mode("--adapter-missing-get") || mode("--adapter-path-mismatch") || mode("--adapter-load-failed"))
            check(!adapterGets, "missing/unverified getter is never invoked");
        if(mode("--adapter-missing-token"))check(!adapterGets&&!adapterInits,"unverified/missing SDK-token export forbids getter and Init");
        edvr::nativeGazePoll(true);
        check(adapterLoads == 1 && adapterInits <= 1, "failed production setup is attempted once");
    } else return 2;
    std::printf("%s native_gaze %s checks=%u failures=%u live_runtime_calls=0 gpu_devices=0\n",
        failures ? "FAIL" : "PASS", argv[1], checks, failures);
    return failures ? 1 : 0;
}

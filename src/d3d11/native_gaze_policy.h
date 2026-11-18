#pragma once

#include "native_gaze.h"

#include <cmath>

namespace edvr::native_gaze_detail {

inline constexpr wchar_t clientPath[] =
    L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\win64\\openvr_api.dll";

inline bool pathMatches(const wchar_t* actual) noexcept {
    if (!actual) return false;
    const wchar_t* expected = clientPath;
    for (; *actual && *expected; ++actual, ++expected) {
        const auto lower = [](wchar_t c) noexcept {
            return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c;
        };
        if (lower(*actual) != lower(*expected)) return false;
    }
    return *actual == *expected;
}

// The production adapter supplies only exports from the verified absolute DLL.
// Fixtures supply memory-only operations, without initializing a live runtime.
struct Ops {
    void* user{};
    void* (*loadModule)(void*) noexcept{};
    bool (*verifyModule)(void*, void*) noexcept{};
    void* (*getInterface)(void*, void*, int*) noexcept{};
    bool (*initializeBackground)(void*, void*, int*) noexcept{};
    bool (*query)(void*, void*, float[2][2]) noexcept{};
    bool (*receiptClock)(void*, std::uint64_t*, std::uint64_t*) noexcept{};
    bool (*initToken)(void*, void*, std::uint32_t*) noexcept{};
};

struct State {
    bool attempted{};
    bool borrowed{};
    bool backgroundAttempted{};
    bool backgroundSucceeded{};
    void* retainedModule{};
    void* system{};
    int contextError{};
    std::uint64_t sequence{};
    std::uint64_t lifetime{1}; // standalone probes are explicit opt-in
    bool moduleVerified{},resolved{},tokenKnown{},initTokenKnown{};
    std::uint32_t token{},lastInitToken{};
    NativeGazeFailure failure{NativeGazeFailure::ContextUnavailable};
};

inline void clearInterface(State& state) noexcept {
    state.system=nullptr;state.borrowed=false;state.resolved=false;
    state.failure=NativeGazeFailure::ContextUnavailable;
}
inline void setLifetime(State& state,std::uint64_t epoch) noexcept {
    if(state.lifetime==epoch)return;
    clearInterface(state);state.lifetime=epoch;
    // Keep the successful module and the SDK-token initialization budget.
    if(!state.moduleVerified)state.attempted=false;
}
inline bool currentToken(State& state,const Ops& ops) noexcept {
    std::uint32_t token=0;
    if(!ops.initToken||!ops.initToken(ops.user,state.retainedModule,&token)){
        clearInterface(state);state.failure=NativeGazeFailure::InitTokenUnavailable;return false;
    }
    if(!state.tokenKnown||state.token!=token){
        clearInterface(state);state.token=token;state.tokenKnown=true;
        state.failure=NativeGazeFailure::ContextChanged;return false;
    }
    return true;
}
inline void resolve(State& state, const Ops& ops) noexcept {
    if(!state.lifetime)return;
    if(!state.attempted){
        state.attempted=true;
        state.retainedModule=ops.loadModule?ops.loadModule(ops.user):nullptr;
        if(!state.retainedModule){state.failure=NativeGazeFailure::ModuleLoadFailed;return;}
        state.moduleVerified=ops.verifyModule&&ops.verifyModule(ops.user,state.retainedModule);
        if(!state.moduleVerified){state.failure=NativeGazeFailure::ModulePathMismatch;return;}
    }
    if(!state.moduleVerified)return;
    const bool stable=currentToken(state,ops);
    if(state.failure==NativeGazeFailure::InitTokenUnavailable)return;
    if(stable&&state.resolved)return;
    state.resolved=true;
    if(!ops.getInterface)return;
    state.system = ops.getInterface(ops.user, state.retainedModule, &state.contextError);
    if(!currentToken(state,ops))return;
    if (state.system && state.contextError == 0) {
        state.borrowed = true;
        state.failure = NativeGazeFailure::None;
        return;
    }
    state.system = nullptr;
    if (!ops.initializeBackground) return;
    // Invalidating/rearming our channel must not repeat VR_Init against the
    // same SDK context. A genuine token change permits one new attempt.
    if(state.initTokenKnown&&state.lastInitToken==state.token)return;
    state.backgroundAttempted = true;
    state.initTokenKnown=true;state.lastInitToken=state.token;
    state.contextError = 0;
    state.backgroundSucceeded = ops.initializeBackground(
        ops.user, state.retainedModule, &state.contextError);
    std::uint32_t afterInit=0;
    if(!ops.initToken||!ops.initToken(ops.user,state.retainedModule,&afterInit)){
        clearInterface(state);state.failure=NativeGazeFailure::InitTokenUnavailable;return;
    }
    // VR_Init may itself advance the token, including on a failed attempt.
    state.token=state.lastInitToken=afterInit;state.tokenKnown=true;state.resolved=true;
    if (!state.backgroundSucceeded || state.contextError != 0) return;
    state.system = ops.getInterface(ops.user, state.retainedModule, &state.contextError);
    if(!currentToken(state,ops))return;
    if (state.system && state.contextError == 0) {
        state.failure = NativeGazeFailure::None;
    } else {
        state.system = nullptr;
    }
}

inline NativeGazeSample poll(bool enabled, State& state, const Ops& ops) noexcept {
    NativeGazeSample sample{};
    if (!enabled) return sample;
    resolve(state, ops);
    if (!state.system || !ops.query) {
        sample.failure = state.failure;
        if (sample.failure == NativeGazeFailure::None)
            sample.failure = NativeGazeFailure::ContextUnavailable;
        return sample;
    }
    float ndc[2][2]{};
    sample.providerQueried = true;
    sample.querySequence = ++state.sequence;
    sample.providerReturnedValid = ops.query(ops.user, state.system, ndc);
    const bool clockValid = ops.receiptClock && ops.receiptClock(
        ops.user, &sample.receiptQpc, &sample.receiptQpcFrequency) &&
        sample.receiptQpc != 0 && sample.receiptQpcFrequency != 0;
    bool allFinite = true;
    bool allInRange = true;
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& value = sample.eyes[eye];
        value.xNdc = ndc[eye][0];
        value.yNdc = ndc[eye][1];
        const bool finite = std::isfinite(value.xNdc) && std::isfinite(value.yNdc);
        const bool inRange = finite && value.xNdc >= -1.f && value.xNdc <= 1.f &&
            value.yNdc >= -1.f && value.yNdc <= 1.f;
        allFinite = allFinite && finite;
        allInRange = allInRange && inRange;
        value.valid = sample.providerReturnedValid && inRange;
    }
    sample.failure = !sample.providerReturnedValid ? NativeGazeFailure::ProviderInvalid :
        !allFinite ? NativeGazeFailure::NonFinite :
        !allInRange ? NativeGazeFailure::OutOfRange :
        !clockValid ? NativeGazeFailure::ReceiptClockUnavailable : NativeGazeFailure::None;
    sample.usable = sample.failure == NativeGazeFailure::None;
    if(!currentToken(state,ops)){
        sample.usable=false;sample.eyes[0].valid=sample.eyes[1].valid=false;
        sample.failure=state.failure;
    }
    return sample;
}

}  // namespace edvr::native_gaze_detail

// Optional sideband for a real OpenVR game-thread Submit. Texture is borrowed
// until this call returns and may be invalid when the compositor rejected it.
#pragma once
#include <cstdint>
struct EdvrNativeReflexSubmit {
    uint32_t size, version;
    uint64_t runtimeGeneration, compositorGeneration, poseSequence;
    uintptr_t caller, textureDescriptor;
    uint32_t eye, result;
};
constexpr uint32_t EDVR_NATIVE_REFLEX_SUBMIT_VERSION_1 = 1;

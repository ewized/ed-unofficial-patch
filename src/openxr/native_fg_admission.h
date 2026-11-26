#pragma once
#include "../common/native_frame_generation.h"

namespace edvr::openxr::fgadmission {
// Independent XR cadence needs an exportable output with current provider
// proofs. Ordinary native source frames continue through the warmup.
inline bool providerReady(const EdvrFgStatus& status,uint64_t runtimeGeneration) noexcept {
  return runtimeGeneration&&status.generation==runtimeGeneration&&status.configGeneration&&status.historyGeneration&&
    status.state==EDVR_FG_READY&&nativeFgFixedMultiplier(status.requestedMultiplier)&&
    status.activeMultiplier==status.requestedMultiplier;
}
} // namespace edvr::openxr::fgadmission

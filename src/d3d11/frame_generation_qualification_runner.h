#pragma once

#include "frame_generation_ngx.h"
#include "frame_generation_recomposition_policy.h"
#include <wrl/client.h>

namespace edvr {

enum class FrameGenerationQualificationVerdict { Unverified, Passed, Unsafe };
enum class FrameGenerationQualificationReason {
    UnsupportedBackend, Allocation, Budget, Producer, Evaluation, Readback,
    Registration, Pixels, InsufficientReady, Quarantined, Verified
};

struct FrameGenerationQualificationLimits {
    uint32_t timeoutMs = 2000;
    // Bounds the summed D3D12 allocation sizes of the 10 input textures and
    // readback buffer. Dimensions must always match the actual backend.
    uint64_t fixtureBytes = 512ull * 1024 * 1024;
    bool neutralRightEye = false;
};

struct FrameGenerationQualificationResult {
    FrameGenerationQualificationVerdict verdict = FrameGenerationQualificationVerdict::Unverified;
    FrameGenerationQualificationReason reason = FrameGenerationQualificationReason::UnsupportedBackend;
    fg_recomposition::StereoReadback pixels{};
    unsigned coveredEyes = 0, passingMidpoints = 0;
    uint64_t submitted = 0, completed = 0;
    Microsoft::WRL::ComPtr<ID3D12Fence> readbackFence;
    std::shared_ptr<const FrameGenerationNgxQualification> resetReceipt;

    // A terminal negative can carry proof. An unsupported or interrupted run
    // cannot supply a receipt the owner could mistake for a current midpoint.
    bool hasTerminalProof() const noexcept {
        return readbackFence.Get() && resetReceipt && submitted &&
            completed != UINT64_MAX && completed >= submitted;
    }
};

// Generation owner only, before any real source on this newly initialized
// backend. Supply the exact original init. Passed means fenced pixels, not
// publication permission. The caller rechecks the live key and performs the
// guarded history reset and native-only seed. On Unverified retire the trial
// backend before any legacy retry. On Unsafe keep the backend quarantined;
// the runner retains its unretired D3D12 owners until process exit.
[[nodiscard]] FrameGenerationQualificationResult qualifyFrameGeneration(
    FrameGenerationNgx& backend, const FrameGenerationNgxInit& init,
    const FrameGenerationQualificationLimits& limits = {}) noexcept;

} // namespace edvr

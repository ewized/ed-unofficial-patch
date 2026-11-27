#pragma once
#include "frame_generation_ngx_policy.h"

namespace edvr::fg_recomposition {
enum class Mode { Legacy, Pending, Verified, Unsafe };

struct BackendKey {
    uint64_t sessionGeneration = 0, configGeneration = 0, resourceEpoch = 0;
    uintptr_t producer = 0, device = 0;
    unsigned width = 0, height = 0, guideWidth = 0, guideHeight = 0;
    unsigned colorFormat = 0, multiplier = 0;

    bool valid() const noexcept {
        // This witness covers one intermediate phase. A multi-phase route
        // needs evidence for every phase, including the additional indices.
        return sessionGeneration && configGeneration && producer && device &&
            width && height && guideWidth && guideHeight && colorFormat && multiplier == 2;
    }
    bool operator==(const BackendKey& other) const noexcept {
        return sessionGeneration == other.sessionGeneration && configGeneration == other.configGeneration &&
            resourceEpoch == other.resourceEpoch && producer == other.producer && device == other.device &&
            width == other.width && height == other.height && guideWidth == other.guideWidth &&
            guideHeight == other.guideHeight && colorFormat == other.colorFormat && multiplier == other.multiplier;
    }
};

// At submission, value is the signal after the output-to-readback copy.
// At observation, value is the actual completed value of that same fence.
struct FencePoint {
    uintptr_t object = 0;
    uint64_t value = 0;
};

// Independent pixel-oracle observations from the fenced phase. Bits 0 and 1
// identify the left and right eye. An absent or suppressed phase has no bits.
struct StereoReadback {
    unsigned eligibleEyes = 0, translatedEyes = 0, worldEyes = 0;
    unsigned hudEyes = 0, throughHudEyes = 0, neutralEyes = 0;
};

// The coordinator serializes this policy with queue publication. The worker
// still owns the fences, resources, readback and backend drain. A successful
// NGX call or a requested UIR option is not a visual readback.
class Activation {
public:
    class Attempt {
        friend class Activation;
        const Activation* owner_ = nullptr;
        uint64_t serial_ = 0;

        Attempt(const Activation* owner, uint64_t serial) noexcept : owner_(owner), serial_(serial) {}
    public:
        Attempt() noexcept = default;
        explicit operator bool() const noexcept { return owner_ != nullptr; }
    };

    Activation() noexcept = default;
    Activation(const Activation&) = delete;
    Activation& operator=(const Activation&) = delete;

    // Begin only after retiring the preceding backend attempt. The serial
    // still distinguishes attempts when the allocator reuses their addresses.
    Attempt start(const BackendKey& backend) noexcept {
        if (owned_ || mode_ == Mode::Unsafe || !backend.valid() || serial_ == UINT64_MAX) return {};
        owned_ = true;
        backend_ = backend;
        mode_ = Mode::Pending;
        return Attempt(this, ++serial_);
    }
    Mode mode() const noexcept { return mode_; }

    bool registerProof(Attempt attempt, FencePoint submitted, unsigned coveredEyes) noexcept {
        if (!matches(attempt) || revoked_ || mode_ != Mode::Pending || proof_.object ||
            !submitted.object || !fg_ngx::completed(submitted.value, submitted.value) ||
            !coveredEyes || coveredEyes > allEyes_) return false;
        proof_ = submitted;
        coveredEyes_ = coveredEyes;
        return true;
    }

    // Stale or premature readbacks leave this attempt pending. Consume the
    // first complete current readback, including a negative/inconclusive one.
    bool finish(Attempt attempt, const BackendKey& liveBackend, FencePoint completed,
        uint64_t nativeHistory, const StereoReadback& pixels) noexcept {
        if (!current(attempt, liveBackend) || mode_ != Mode::Pending || !proofComplete(completed) ||
            !nativeHistory || nativeHistory == UINT64_MAX) return false;
        qualificationHistory_ = nativeHistory;
        const bool passed = pixels.eligibleEyes == allEyes_ && pixels.translatedEyes == allEyes_ &&
            pixels.worldEyes == allEyes_ && pixels.hudEyes == coveredEyes_ &&
            pixels.throughHudEyes == coveredEyes_ && pixels.neutralEyes == (allEyes_ ^ coveredEyes_);
        mode_ = passed ? Mode::Verified : Mode::Legacy;
        return true;
    }

    // A clean unsupported creation or pre-submit rejection can use legacy.
    // The caller proves backend retirement before creating legacy handles.
    bool selectLegacy(Attempt attempt, const BackendKey& liveBackend) noexcept {
        if (!current(attempt, liveBackend) || mode_ != Mode::Pending || proof_.object) return false;
        mode_ = Mode::Legacy;
        return true;
    }

    // Verified can collect the first live seed. Only its subsequent phases may
    // use translated HUD coverage after the native-only seed has completed.
    bool verified(Attempt attempt, const BackendKey& liveBackend) const noexcept {
        return current(attempt, liveBackend) && mode_ == Mode::Verified;
    }
    bool noteLiveSeed(Attempt attempt, const BackendKey& liveBackend, uint64_t history,
        uint32_t actualResetReasons) noexcept {
        const uint32_t required = fg_ngx::kResetCaller |
            (liveHistory_ ? fg_ngx::kResetGenerationChanged : fg_ngx::kResetFirstSource);
        if (!verified(attempt, liveBackend) || history <= qualificationHistory_ || history <= liveHistory_ ||
            (actualResetReasons & required) != required) return false;
        liveHistory_ = history;
        return true;
    }
    bool canPublish(Attempt attempt, const BackendKey& liveBackend, uint64_t history) const noexcept {
        return verified(attempt, liveBackend) && liveHistory_ && history == liveHistory_;
    }

    // Close admission even if the proof is still in flight. Keep the owner
    // through the readback fence. The caller also drains its NGX/output work.
    bool retire(Attempt attempt, FencePoint completed = {}) noexcept {
        if (!matches(attempt) || mode_ == Mode::Unsafe) return false;
        revoked_ = true;
        if (proof_.object && !proofComplete(completed)) return false;
        proof_ = {};
        backend_ = {};
        qualificationHistory_ = liveHistory_ = 0;
        coveredEyes_ = 0;
        revoked_ = owned_ = false;
        mode_ = Mode::Legacy;
        return true;
    }

    // Accepted work without a safe completion proof makes this owner unsafe.
    // Neither a configuration change nor a stale outcome clears quarantine.
    bool quarantine(Attempt attempt) noexcept {
        if (!matches(attempt)) return false;
        mode_ = Mode::Unsafe;
        return true;
    }

private:
    bool matches(Attempt attempt) const noexcept {
        return owned_ && attempt.owner_ == this && attempt.serial_ == serial_;
    }
    bool current(Attempt attempt, const BackendKey& liveBackend) const noexcept {
        return matches(attempt) && !revoked_ && backend_ == liveBackend;
    }
    bool proofComplete(FencePoint completed) const noexcept {
        return proof_.object && completed.object == proof_.object && fg_ngx::completed(completed.value, proof_.value);
    }

    static constexpr unsigned allEyes_ = 3;
    BackendKey backend_{};
    FencePoint proof_{};
    uint64_t serial_ = 0, qualificationHistory_ = 0, liveHistory_ = 0;
    unsigned coveredEyes_ = 0;
    Mode mode_ = Mode::Legacy;
    bool owned_ = false, revoked_ = false;
};
} // namespace edvr::fg_recomposition

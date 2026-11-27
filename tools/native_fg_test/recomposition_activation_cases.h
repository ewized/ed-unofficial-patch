#pragma once
#include "../../src/d3d11/frame_generation_recomposition_policy.h"

namespace edvr::fg_recomposition::test {
template<class Check>
void activationCases(Check check) {
    const BackendKey backend{1, 2, 0, 0x10, 0x20, 3560, 3560, 2064, 2064, 28, 2};
    auto changed = backend;
    ++changed.resourceEpoch;
    const FencePoint submitted{0x30, 9};
    const StereoReadback covered{3, 3, 3, 3, 3, 0};
    const StereoReadback mixed{3, 3, 3, 1, 1, 2};
    const uint32_t firstReset = fg_ngx::kResetFirstSource | fg_ngx::kResetCaller;

    Activation policy;
    check(!policy.start(BackendKey{}), "UI recomposition refuses an unknown backend key");
    auto beyondProof = backend;
    beyondProof.multiplier = 3;
    check(!policy.start(beyondProof), "one midpoint receipt cannot authorize a 3x backend");
    auto attempt = policy.start(backend);
    check(attempt && policy.mode() == Mode::Pending &&
        !policy.verified(attempt, backend) && !policy.canPublish(attempt, backend, 2),
        "starting a requested route remains pending until pixels are proven");
    check(!policy.start(backend), "cannot replace a live owner before retiring it");
    check(!policy.finish(attempt, backend, submitted, 1, covered),
        "fenced pixels without a registered proof do not qualify");
    check(!policy.registerProof(attempt, {0x30, 0}, 3) &&
        !policy.registerProof(attempt, {0x30, UINT64_MAX}, 3) &&
        !policy.registerProof(attempt, submitted, 0), "requires a real proof fence and some covered pixels");
    check(policy.registerProof(attempt, submitted, 3) &&
        !policy.registerProof(attempt, {0x30, 10}, 3),
        "the exact attempt can register only one proof fence");
    check(!policy.selectLegacy(attempt, backend),
        "cannot select legacy while proof reader ownership is unresolved");
    check(!policy.finish(attempt, changed, submitted, 1, covered) &&
        !policy.finish(attempt, backend, {0x40, 10}, 1, covered) &&
        !policy.finish(attempt, backend, {0x30, 8}, 1, covered) &&
        !policy.finish(attempt, backend, {0x30, UINT64_MAX}, 1, covered) &&
        policy.mode() == Mode::Pending, "stale epochs, foreign or incomplete fences and removal sentinel cannot qualify");
    check(policy.finish(attempt, backend, {0x30, 10}, 1, covered) &&
        policy.verified(attempt, backend) && !policy.verified(attempt, changed),
        "completed output and both covered-eye matches qualify only the exact worker owner");
    check(!policy.finish(attempt, backend, submitted, 1, covered) &&
        !policy.canPublish(attempt, backend, 2), "visual proof is consumed once and does not seed live history");
    check(!policy.noteLiveSeed(attempt, backend, 1, firstReset) &&
        !policy.noteLiveSeed(attempt, backend, 2, fg_ngx::kResetCaller) &&
        !policy.noteLiveSeed(attempt, backend, 2, fg_ngx::kResetFirstSource),
        "the first live seed requires new history plus a genuinely reset sequence");
    check(policy.noteLiveSeed(attempt, backend, 2, firstReset) &&
        policy.canPublish(attempt, backend, 2) && !policy.canPublish(attempt, backend, 3),
        "a fresh native seed arms exactly its live history");
    check(!policy.noteLiveSeed(attempt, backend, 2, firstReset) &&
        policy.noteLiveSeed(attempt, backend, 3, fg_ngx::kResetCaller | fg_ngx::kResetGenerationChanged) &&
        !policy.canPublish(attempt, backend, 2) && policy.canPublish(attempt, backend, 3),
        "later native resets advance the live seed and never rearm old history");

    auto resized = backend;
    ++resized.guideWidth;
    auto replaced = backend;
    ++replaced.device;
    auto switchedConfig = backend;
    ++switchedConfig.configGeneration;
    auto switchedSession = backend;
    ++switchedSession.sessionGeneration;
    auto replacedProducer = backend;
    ++replacedProducer.producer;
    auto changedFormat = backend;
    ++changedFormat.colorFormat;
    check(!policy.verified(attempt, resized) && !policy.verified(attempt, replaced) &&
        !policy.verified(attempt, switchedConfig) && !policy.verified(attempt, switchedSession) &&
        !policy.verified(attempt, replacedProducer) && !policy.verified(attempt, changedFormat),
        "resolution, device, producer, color format, config and session identity are part of the proof");
    check(!policy.retire(attempt, {0x30, 8}) && !policy.canPublish(attempt, backend, 3) &&
        !policy.start(backend), "retirement closes admission immediately and retains the unfinished proof fence");
    check(policy.retire(attempt, submitted), "the qualified owner clears only after the same fence completes");
    const auto previous = attempt;
    attempt = policy.start(backend);
    check(attempt && policy.registerProof(attempt, submitted, 1) &&
        !policy.finish(previous, backend, submitted, 3, mixed) &&
        !policy.quarantine(previous), "late proof and fault cannot affect a replacement with reused object addresses");
    Activation other;
    const auto otherAttempt = other.start(backend);
    check(otherAttempt && !policy.finish(otherAttempt, backend, submitted, 3, mixed),
        "an identical key and serial from a foreign coordinator cannot complete the proof");
    check(policy.finish(attempt, backend, submitted, 3, mixed) && policy.verified(attempt, backend),
        "mixed eye proof includes the covered eye's independent backing and exact neutral eye");
    check(policy.retire(attempt, submitted), "mixed eye attempt retires with its proof fence");

    attempt = policy.start(backend);
    check(attempt && policy.registerProof(attempt, submitted, 3) &&
        !policy.retire(attempt, {0x30, 8}) && !policy.finish(attempt, backend, submitted, 3, covered),
        "a proof arriving after pending retirement cannot reopen coverage");
    check(policy.retire(attempt, submitted), "withdrawn pending proof still drains against its saved fence");
    attempt = policy.start(backend);
    auto frozenBacking = covered;
    frozenBacking.throughHudEyes = 1;
    check(attempt && policy.registerProof(attempt, submitted, 3) &&
        policy.finish(attempt, backend, submitted, 4, frozenBacking) &&
        policy.mode() == Mode::Legacy && !policy.verified(attempt, backend) &&
        !policy.finish(attempt, backend, submitted, 4, covered),
        "completed proof with frozen backing selects legacy and cannot be overwritten");
    check(policy.retire(attempt, submitted), "failed visual attempt still retires through actual fence");
    attempt = policy.start(backend);
    const StereoReadback noEligiblePixels{};
    check(attempt && policy.registerProof(attempt, submitted, 3) &&
        policy.finish(attempt, backend, submitted, 4, noEligiblePixels) &&
        policy.mode() == Mode::Legacy && !policy.canPublish(attempt, backend, 5),
        "completed but inconclusive readback leaves translated coverage closed");
    check(policy.retire(attempt, submitted), "inconclusive visual attempt still retires through actual fence");

    attempt = policy.start(backend);
    check(attempt && policy.selectLegacy(attempt, backend) && policy.mode() == Mode::Legacy &&
        !policy.registerProof(attempt, submitted, 3) && policy.retire(attempt),
        "clean unsupported creation selects legacy upstream of any proof submission");
    attempt = policy.start(backend);
    check(attempt && policy.quarantine(attempt) && policy.mode() == Mode::Unsafe &&
        !policy.selectLegacy(attempt, backend) && !policy.retire(attempt) &&
        !policy.start(changed), "unfenced accepted work keeps this coordinator quarantined");
}
} // namespace edvr::fg_recomposition::test

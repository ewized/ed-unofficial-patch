#pragma once
#include "../../src/d3d11/frame_generation_qualification_fixture.h"
#include <limits>
#include <vector>

namespace edvr::fg_fixture::test {
inline bool qualificationFixtureSelfTest() {
    static constexpr unsigned width = 1280;
    constexpr unsigned height = 720, frame = 5;
    constexpr unsigned wideWidth = 3840, wideHeight = 2160;
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (barLeft(width, frame, eye) - barLeft(width, frame - 1, eye) != 40 ||
            hudLeft(width, frame, eye) - hudLeft(width, frame - 1, eye) != -30 ||
            barLeft(wideWidth, frame, eye) - barLeft(wideWidth, frame - 1, eye) != 120 ||
            hudLeft(wideWidth, frame, eye) - hudLeft(wideWidth, frame - 1, eye) != -90) return false;
        const float reference = probeMotion(width, height, 640, frame, eye, HudFixture::Opaque).xDelta;
        const float wide = probeMotion(wideWidth, wideHeight, 2227, frame, eye, HudFixture::Opaque).xDelta;
        const float roundoff = 16.f * std::numeric_limits<float>::epsilon() * reference;
        if (!std::isfinite(reference) || reference <= 0.f || !std::isfinite(wide) ||
            std::abs(wide - reference) > roundoff) return false;
    }
    const auto midpoint = [](int a, int b) { return int(std::lround((a + b) * .5)); };
    const auto makeRow = [](unsigned eye, int scene, int hud) {
        std::vector<uint8_t> row(width * 4);
        for (int x = 0; x < int(width); ++x) {
            auto pixel = x >= scene && x < scene + int(width / 4) ? worldBar : worldBackground(eye);
            if (x >= hud && x < hud + int(width * 2 / 3)) pixel = blendHud(pixel, translucentHud(eye));
            for (unsigned channel = 0; channel < 4; ++channel)
                row[4*x + channel] = uint8_t(std::lround(255.f * pixel[channel]));
        }
        return row;
    };
    const int worldLeft = midpoint(barLeft(width, frame-1, 0), barLeft(width, frame, 0));
    const int worldRight = midpoint(barLeft(width, frame-1, 1), barLeft(width, frame, 1));
    const int hudLeftMiddle = midpoint(hudLeft(width, frame-1, 0), hudLeft(width, frame, 0));
    const int hudRightMiddle = midpoint(hudLeft(width, frame-1, 1), hudLeft(width, frame, 1));
    const auto leftWorld = makeRow(0, worldLeft, width);
    const auto rightWorld = makeRow(1, worldRight, width);
    const auto leftHud = makeRow(0, worldLeft, hudLeftMiddle);
    const auto rightHud = makeRow(1, worldRight, hudRightMiddle);
    const auto frozenBacking = makeRow(1, barLeft(width, frame, 1), hudRightMiddle);
    const auto endpointHud = makeRow(1, worldRight, hudLeft(width, frame, 1));
    const auto endpointWorld = makeRow(1, barLeft(width, frame, 1), width);
    const auto leftMotion = probeMotion(width, height, 640, frame, 0, HudFixture::Opaque);
    const auto rightMotion = probeMotion(width, height, 640, frame, 1, HudFixture::Opaque);
    const ProbeMotion stereo[2]{leftMotion, rightMotion};
    const EyeReadbackRows good[2]{{leftWorld.data(), leftHud.data()}, {rightWorld.data(), rightHud.data()}};
    auto proof = observeStereoMidpoint(good, width, frame, HudFixture::Opaque, stereo);
    if (proof.eligibleEyes != 3 || proof.translatedEyes != 3 || proof.worldEyes != 3 ||
        proof.hudEyes != 3 || proof.throughHudEyes != 3 || proof.neutralEyes != 0) return false;

    const EyeReadbackRows wrongWorld[2]{good[0], {endpointWorld.data(), rightHud.data()}};
    const EyeReadbackRows wrongHud[2]{good[0], {rightWorld.data(), endpointHud.data()}};
    const EyeReadbackRows frozen[2]{good[0], {rightWorld.data(), frozenBacking.data()}};
    const EyeReadbackRows swapped[2]{good[1], good[0]};
    const EyeReadbackRows swappedHud[2]{{leftWorld.data(), rightHud.data()}, {rightWorld.data(), leftHud.data()}};
    const EyeReadbackRows missing[2]{good[0], {rightWorld.data(), nullptr}};
    proof = observeStereoMidpoint(frozen, width, frame, HudFixture::Opaque, stereo);
    if (proof.hudEyes != 3 || proof.throughHudEyes != 1 ||
        observeStereoMidpoint(wrongWorld, width, frame, HudFixture::Opaque, stereo).worldEyes != 1 ||
        observeStereoMidpoint(wrongHud, width, frame, HudFixture::Opaque, stereo).hudEyes != 1 ||
        observeStereoMidpoint(swapped, width, frame, HudFixture::Opaque, stereo).worldEyes != 0 ||
        observeStereoMidpoint(swappedHud, width, frame, HudFixture::Opaque, stereo).hudEyes != 0 ||
        observeStereoMidpoint(missing, width, frame, HudFixture::Opaque, stereo).eligibleEyes != 1) return false;

    ProbeMotion withoutTranslation[2]{leftMotion, rightMotion};
    withoutTranslation[0].xDelta = 0.f;
    withoutTranslation[1].xDelta = std::numeric_limits<float>::quiet_NaN();
    if (observeStereoMidpoint(good, width, frame, HudFixture::Opaque, withoutTranslation).translatedEyes != 0 ||
        observeStereoMidpoint(good, width, 1, HudFixture::Opaque, stereo).eligibleEyes != 0) return false;

    const EyeReadbackRows neutral[2]{good[0], {rightWorld.data(), rightWorld.data()}};
    const EyeReadbackRows staleNeutral[2]{good[0], {rightWorld.data(), endpointWorld.data()}};
    auto leakingHud = rightWorld;
    leakingHud[0] = 200;
    leakingHud[1] = 0;
    leakingHud[2] = 200;
    const EyeReadbackRows residue[2]{good[0], {rightWorld.data(), leakingHud.data()}};
    proof = observeStereoMidpoint(neutral, width, frame, HudFixture::Neutral, stereo);
    return proof.eligibleEyes == 3 && proof.translatedEyes == 3 && proof.worldEyes == 3 &&
        proof.hudEyes == 1 && proof.throughHudEyes == 1 && proof.neutralEyes == 2 &&
        observeStereoMidpoint(staleNeutral, width, frame, HudFixture::Neutral, stereo).neutralEyes == 0 &&
        observeStereoMidpoint(residue, width, frame, HudFixture::Neutral, stereo).neutralEyes == 0 &&
        scanlineY(height, HudFixture::None, 0) == 360 &&
        scanlineY(height, HudFixture::Opaque, 0) == 225 && scanlineY(height, HudFixture::Opaque, 1) == 450;
}
} // namespace edvr::fg_fixture::test

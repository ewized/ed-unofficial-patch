#pragma once
#include "frame_generation_camera.h"
#include "frame_generation_recomposition_policy.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace edvr::fg_fixture {
inline bool inverse(const float (&m)[4][4],float (&out)[4][4]){
    double a[4][8]{};for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){a[y][x]=m[y][x];a[y][x+4]=x==y?1.:0.;}
    for(unsigned c=0;c<4;++c){unsigned pivot=c;for(unsigned y=c+1;y<4;++y)if(std::abs(a[y][c])>std::abs(a[pivot][c]))pivot=y;
        if(std::abs(a[pivot][c])<1e-12)return false;for(unsigned x=0;x<8;++x)std::swap(a[c][x],a[pivot][x]);
        const double scale=a[c][c];for(auto& value:a[c])value/=scale;
        for(unsigned y=0;y<4;++y)if(y!=c){const double factor=a[y][c];for(unsigned x=0;x<8;++x)a[y][x]-=factor*a[c][x];}}
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)out[y][x]=float(a[y][x+4]);return true;
}
// The foreground moves in both directions without wrapping at the screen edge.
// Each eye has a small disparity and a different constant background colour so
// a swapped eye cannot pass just by placing its bar at the right fraction.
inline int barLeft(unsigned width,unsigned frame,unsigned eye){
    const int step=std::clamp(int(width/32),16,48);
    const int tick=int((frame-1)%12);
    return int(width/4)+(eye==0?2:-2)+step*(tick<=6?tick:12-tick);
}

enum class HudFixture { None, Alpha, Opaque, Neutral, Control };
inline const char* hudFixtureName(HudFixture fixture) {
    switch (fixture) {
    case HudFixture::Alpha: return "alpha";
    case HudFixture::Opaque: return "opaque";
    case HudFixture::Neutral: return "neutral";
    case HudFixture::Control: return "control";
    default: return "none";
    }
}
inline bool hasHud(HudFixture fixture, unsigned eye) {
    return fixture != HudFixture::None && !(fixture == HudFixture::Neutral && eye == 1);
}
inline int hudLeft(unsigned width, unsigned frame, unsigned eye) {
    // The panel travels the opposite direction to the world edge. Its
    // translucent interior always contains at least one world edge.
    const int travel = barLeft(width, frame, eye) - barLeft(width, 1, eye);
    return int(width * 5 / 16) + (eye == 0 ? -2 : 2) - travel * 3 / 4;
}

struct EdgeMeasurement {
    int left=-1,right=-1,spread=0;
    bool eyeMatches=false;
};

template<class Score>
EdgeMeasurement measureEdges(int width, Score score, int low, int middle, int high) {
    EdgeMeasurement out;
    const auto first = [&](int threshold, bool reverse) {
        int run = 0;
        for (int i = 0; i < width; ++i) {
            const int x = reverse ? width - 1 - i : i;
            run = score(x) >= threshold ? run + 1 : 0;
            if (run == 3) return reverse ? x + 3 : x - 2; // right is exclusive
        }
        return -1;
    };
    const int lowLeft = first(low, false), highLeft = first(high, false);
    const int lowRight = first(low, true), highRight = first(high, true);
    out.left = first(middle, false);
    out.right = first(middle, true);
    if (lowLeft < 0 || highLeft < 0 || lowRight < 0 || highRight < 0) {
        out.left = out.right = -1;
        return out;
    }
    out.spread = (std::max)(highLeft - lowLeft, lowRight - highRight);
    return out;
}

// Read the green foreground silhouette on one scanline. Two contrast levels
// measure edge spread, distinguishing translation from an endpoint crossfade.
inline EdgeMeasurement measureBar(const uint8_t* row,int width,unsigned eye){
    auto out = measureEdges(width, [&](int x) { return row[4*x + 1]; }, 64, 120, 170);
    out.eyeMatches = out.left >= 0;
    for(int x:{width/16,width-width/16}){
        const auto* sample=row+x*4;
        const int tint=int(sample[eye==0?0:2])-int(sample[eye==0?2:0]);
        out.eyeMatches=out.eyeMatches&&tint>=20&&sample[1]<64;
    }
    return out;
}

inline int hudScore(const uint8_t* pixel) {
    return int(pixel[0]) + int(pixel[2]) - 2 * int(pixel[1]);
}
inline EdgeMeasurement measureHud(const uint8_t* row, int width, unsigned eye) {
    auto out = measureEdges(width, [&](int x) { return hudScore(row + 4*x); }, 90, 125, 150);
    if (out.left < 0) return out;
    const auto* center = row + 4 * ((out.left + out.right) / 2);
    out.eyeMatches = int(center[eye == 0 ? 0 : 2]) - int(center[eye == 0 ? 2 : 0]) >= 18;
    return out;
}

inline bool followsEdge(int edge, int oldEdge, int previous, int current, unsigned index, unsigned multiplier) {
    const int direction = current > previous ? 1 : -1;
    const double substep = std::abs(current - previous) / double(multiplier);
    const double expected = previous + (current - previous) * double(index) / multiplier;
    const double tolerance = (std::max)(2.0, substep * .5);
    return (edge - previous) * direction >= 2 && (current - edge) * direction >= 2 &&
           (edge - oldEdge) * direction >= 2 && std::abs(edge - expected) <= tolerance;
}

// The fixture measures image motion, independently of NGX's success code.
// Allow deviations of half an inter-phase step. Also require movement away
// from both native endpoints and forward progress from the previous phase.
inline bool followsMotion(const EdgeMeasurement& measured,const EdgeMeasurement& prior,
                   int previous,int current,int barWidth,unsigned index,unsigned multiplier){
    const double substep=std::abs(current-previous)/double(multiplier);
    return measured.eyeMatches&&measured.spread<=substep*.75+1&&
        followsEdge(measured.left,prior.left,previous,current,index,multiplier)&&
        followsEdge(measured.right-barWidth,prior.right-barWidth,previous,current,index,multiplier);
}

struct VisibleWorldEdge {
    int expected = -1, measured = -1;
    bool matches = false;
};
// The green channel still carries the world edge beneath the magenta panel.
// Look for a single transition away from the panel's own boundaries.
inline VisibleWorldEdge measureWorldThroughHud(const uint8_t* row, const EdgeMeasurement& hud,
    int previous, int current, int width, unsigned index, unsigned multiplier) {
    if (!hud.eyeMatches) return {};
    const int center = int(std::lround(previous + (current - previous) * double(index) / multiplier));
    const int radius = (std::max)(3, int(std::ceil(std::abs(current - previous) / (2.0 * multiplier))));
    VisibleWorldEdge observation{};
    for (int offset : {0, width}) {
        const int expected = center + offset;
        if (expected - radius - 3 < hud.left || expected + radius + 3 >= hud.right) continue;
        const auto bright = [&](int x) { return row[4*x + 1] >= 60; };
        int crossings = 0, measured = -1;
        for (int x = expected - radius; x <= expected + radius; ++x) {
            if (bright(x - 1) == bright(x)) continue;
            ++crossings;
            if (bright(x) == (offset == 0) && bright(x - 3) == bright(x - 1) && bright(x + 2) == bright(x))
                measured = x;
        }
        if (crossings == 1 && measured >= 0 &&
            followsEdge(measured - offset, previous, previous, current, index, multiplier))
            return {expected, measured, true};
        observation = {expected, measured, false};
    }
    return observation;
}

using ProbeColor = std::array<float, 4>;
inline ProbeColor worldBackground(unsigned eye) {
    return {eye == 0 ? 66.f/255.f : 10.f/255.f, 20.f/255.f, eye == 0 ? 10.f/255.f : 66.f/255.f, 1.f};
}
inline constexpr ProbeColor worldBar{217.f/255.f, 217.f/255.f, 166.f/255.f, 1.f};
inline ProbeColor translucentHud(unsigned eye) {
    return {eye == 0 ? 125.f/255.f : 63.f/255.f, 0.f, eye == 0 ? 63.f/255.f : 125.f/255.f, 128.f/255.f};
}
inline ProbeColor blendHud(ProbeColor world, const ProbeColor& hud) {
    for (unsigned c = 0; c < 3; ++c) world[c] = hud[c] + world[c] * (1.f - hud[3]);
    return world;
}

struct Rect { int left, top, right, bottom; };

// Composite, HUD-less, and UI all use the same eye-sized SDR grid. In opaque
// coverage mode, a covered UI texel includes the exact B background, as ED-UP
// currently does for alpha/multiply. The native color is identical either way.
template<class Clear>
void paintColorInput(unsigned plane, unsigned width, unsigned height, unsigned frame,
    unsigned eye, HudFixture fixture, Clear& clearColor) {
    const int left = barLeft(width, frame, eye);
    const Rect world{left, int(height / 4), left + int(width / 4), int(3 * height / 4)};
    const auto background = worldBackground(eye);
    constexpr ProbeColor transparent{};
    const bool ui = plane == 4;
    const auto& clear = ui ? transparent : background;
    clearColor(clear, nullptr);
    if (!ui) clearColor(worldBar, &world);
    if (plane == 3 || !hasHud(fixture, eye)) return;

    const int panelLeft = hudLeft(width, frame, eye);
    const Rect panel{panelLeft, int(3 * height / 8), panelLeft + int(width * 2 / 3), int(7 * height / 8)};
    const auto hud = translucentHud(eye);
    if (ui && fixture != HudFixture::Opaque) {
        clearColor(hud, &panel);
        return;
    }
    const auto coveredBackground = blendHud(background, hud);
    const auto coveredWorld = blendHud(worldBar, hud);
    clearColor(coveredBackground, &panel);
    const Rect overlap{
        (std::max)(panel.left, world.left), (std::max)(panel.top, world.top),
        (std::min)(panel.right, world.right), (std::min)(panel.bottom, world.bottom),
    };
    if (overlap.left < overlap.right && overlap.top < overlap.bottom)
        clearColor(coveredWorld, &overlap);
}

// Emit ordered full-texture or rectangular clears for one fixture plane.
// Each rectangle and color remains valid through its callback only.
template<class Clear>
void paintInput(unsigned plane, unsigned width, unsigned height, unsigned guideWidth, unsigned guideHeight,
    unsigned frame, unsigned eye, HudFixture fixture, float distantMotion, Clear&& clearColor) {
    if (plane == 0 || plane >= 3) {
        paintColorInput(plane, width, height, frame, eye, fixture, clearColor);
        return;
    }
    const int left=barLeft(width,frame,eye);
    const auto scaleX=[&](int x){return int(std::lround(x*double(guideWidth)/width));};
    const int previous=barLeft(width,frame>1?frame-1:frame,eye);
    const ProbeColor background[2]={{.02f,0,0,0},{distantMotion,0,0,0}};
    const ProbeColor foreground[2]={{.25f,0,0,0},{float(scaleX(previous)-scaleX(left)),0,0,0}};
    const int right=left+int(width/4);
    const int h=int(guideHeight);
    const Rect rect{scaleX(left),h/4,scaleX(right),3*h/4};
    clearColor(background[plane - 1], nullptr);
    clearColor(foreground[plane - 1], &rect);
}

inline FrameGenerationCamera camera(unsigned w,unsigned h,unsigned eye){
    FrameGenerationCamera c{};c.cameraNear=.1f;c.cameraFar=1000.f;c.cameraFOV=1.2f;c.cameraAspectRatio=float(w)/float(h);
    const float sy=1.f/std::tan(c.cameraFOV*.5f),sx=sy/c.cameraAspectRatio;
    c.cameraViewToClip[0][0]=sx;c.cameraViewToClip[1][1]=sy;
    c.cameraViewToClip[2][2]=c.cameraNear/(c.cameraFar-c.cameraNear);
    c.cameraViewToClip[2][3]=-1;c.cameraViewToClip[3][2]=c.cameraNear*c.cameraFar/(c.cameraFar-c.cameraNear);
    inverse(c.cameraViewToClip,c.clipToCameraView);
    for(unsigned i=0;i<4;++i)c.clipToLensClip[i][i]=c.clipToPrevClip[i][i]=c.prevClipToClip[i][i]=1;
    // Displace the fixed eye so its near rectangle really shifts by the two
    // pixels in barLeft(). Its 0.25 reversed depth unprojects at this distance.
    const float nearObjectDistance=c.cameraViewToClip[3][2]/(.25f+c.cameraViewToClip[2][2]);
    const float eyeOffset=4.f*nearObjectDistance/(float(h)*sy);
    c.cameraPos[0]=eye==0?-eyeOffset:eyeOffset;c.cameraUp[1]=c.cameraRight[0]=1;c.cameraFwd[2]=-1;
    // paintInput writes current-to-previous motion in guide pixels. The DLSS-G
    // Programming Guide specifies unit scale for pixel-valued inputs.
    c.mvecScale[0]=c.mvecScale[1]=1.f;c.depthInverted=true;
    c.motionVectorsInvalidValue=65504.f;c.cameraMotionIncluded=true;return c;
}

struct ProbeMotion {
    FrameGenerationCamera camera;
    float xDelta = 0.f, distantMotion = 0.f;
};

// Move two distinct eyes together. The scene guide is current-to-previous
// with the scene's depth. A HUD draw can still move independently of it.
inline ProbeMotion probeMotion(unsigned width, unsigned height, unsigned guideWidth,
    unsigned frame, unsigned eye, HudFixture fixture) {
    ProbeMotion out{camera(width, height, eye)};
    out.camera.reset = frame == 1;
    if (fixture == HudFixture::None) return out;

    auto& c = out.camera;
    const float nearDistance = c.cameraViewToClip[3][2] / (.25f + c.cameraViewToClip[2][2]);
    const float distantDistance = c.cameraViewToClip[3][2] / (.02f + c.cameraViewToClip[2][2]);
    const float pixelDistance = 2.f * nearDistance / (float(width) * c.cameraViewToClip[0][0]);
    const auto eyeShift = [&](unsigned at) {
        return float(barLeft(width, at, eye) - barLeft(width, 1, eye)) * pixelDistance * .125f;
    };
    const float shifted = eyeShift(frame);
    out.xDelta = shifted - eyeShift(frame > 1 ? frame - 1 : frame);
    c.cameraPos[0] += shifted;
    // For row vectors, clipToPrevClip = inverse(P) * shiftX(delta) * P.
    // The inverse uses the opposite translation and the same projection.
    for (unsigned row = 0; row < 4; ++row) for (unsigned col = 0; col < 4; ++col) {
        const float delta = c.clipToCameraView[row][3] * out.xDelta * c.cameraViewToClip[0][col];
        c.clipToPrevClip[row][col] += delta;
        c.prevClipToClip[row][col] -= delta;
    }
    out.distantMotion = out.xDelta * c.cameraViewToClip[0][0] * float(guideWidth) / (2.f * distantDistance);
    return out;
}


inline unsigned scanlineY(unsigned height, HudFixture fixture, unsigned row) noexcept {
    return fixture == HudFixture::None ? height / 2 : row == 0 ? 5 * height / 16 : 5 * height / 8;
}

struct EyeReadbackRows {
    const uint8_t* world = nullptr;
    const uint8_t* hud = nullptr;
};
struct PhaseInput {
    EyeReadbackRows rows;
    unsigned width, frame, eye, index, multiplier;
    HudFixture fixture;
    float xDelta;
};
struct PhaseAssessment {
    EdgeMeasurement world, hud;
    VisibleWorldEdge through;
    int previous = 0, current = 0, panelA = 0, panelB = 0;
    bool worldMatches = false, translated = false, covered = false, hudMatches = false, clearMatches = false;
};

// Each row must cover the full RGBA8 width. A standalone multi-phase probe
// carries its previous measured world edge so the next phase must advance.
inline PhaseAssessment assessPhase(const PhaseInput& input, const EdgeMeasurement* prior = nullptr) {
    PhaseAssessment out;
    if (input.width < 256 || input.width > 16384 || !input.frame || input.eye >= 2 ||
        !nativeFgFixedMultiplier(input.multiplier) || !input.index || input.index >= input.multiplier ||
        !input.rows.world) return out;
    const auto width = int(input.width);
    const auto barWidth = width / 4;
    const unsigned previousFrame = input.frame > 1 ? input.frame - 1 : input.frame;
    out.previous = barLeft(input.width, previousFrame, input.eye);
    out.current = barLeft(input.width, input.frame, input.eye);
    const EdgeMeasurement start{out.previous, out.previous + barWidth, 0, true};
    out.world = measureBar(input.rows.world, width, input.eye);
    out.worldMatches = followsMotion(out.world, prior ? *prior : start, out.previous, out.current,
        barWidth, input.index, input.multiplier);
    if (input.fixture == HudFixture::None || !input.rows.hud) return out;

    const auto* row = input.rows.hud;
    out.translated = std::isfinite(input.xDelta) && input.xDelta != 0.f;
    out.covered = hasHud(input.fixture, input.eye);
    out.hud = measureHud(row, width, input.eye);
    if (out.covered) {
        out.panelA = hudLeft(input.width, previousFrame, input.eye);
        out.panelB = hudLeft(input.width, input.frame, input.eye);
        const int panelWidth = int(input.width * 2 / 3);
        const EdgeMeasurement priorHud{out.panelA, out.panelA + panelWidth, 0, true};
        out.hudMatches = followsMotion(out.hud, priorHud, out.panelA, out.panelB, panelWidth,
            input.index, input.multiplier);
        out.through = measureWorldThroughHud(row, out.hud, out.previous, out.current, barWidth,
            input.index, input.multiplier);
    } else {
        bool clear = true;
        for (unsigned x = 0; x < input.width; ++x) clear = clear && hudScore(row + 4*x) < 90;
        out.clearMatches = clear && followsMotion(measureBar(row, width, input.eye), start,
            out.previous, out.current, barWidth, input.index, input.multiplier);
    }
    return out;
}

// Supply full rows from the same ready, non-suppressed two-eye midpoint.
// The caller waits for readback completion and applies the warmup policy.
// Motion metadata comes from painting that exact source pair.
inline fg_recomposition::StereoReadback observeStereoMidpoint(const EyeReadbackRows (&rows)[2],
    unsigned width, unsigned frame, HudFixture fixture, const ProbeMotion (&motion)[2]) {
    fg_recomposition::StereoReadback out;
    if (width < 256 || width > 16384 || frame < 2 || fixture == HudFixture::None) return out;
    for (unsigned eye = 0; eye < 2; ++eye) {
        if (!rows[eye].world || !rows[eye].hud) continue;
        const unsigned bit = 1u << eye;
        const auto phase = assessPhase({rows[eye], width, frame, eye, 1, 2, fixture, motion[eye].xDelta});
        out.eligibleEyes |= bit;
        if (phase.translated) out.translatedEyes |= bit;
        if (phase.worldMatches) out.worldEyes |= bit;
        if (phase.covered && phase.hudMatches) out.hudEyes |= bit;
        if (phase.covered && phase.through.matches) out.throughHudEyes |= bit;
        if (!phase.covered && phase.clearMatches) out.neutralEyes |= bit;
    }
    return out;
}
} // namespace edvr::fg_fixture

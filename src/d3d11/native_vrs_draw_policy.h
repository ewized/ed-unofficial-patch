#pragma once
#include "ui_layer_math.h"
#include "srv_turret_math.h"
#include "motion_vs.h"

namespace edvr::native_vrs {

inline constexpr ScreenMotionPair kLateMenuUiPair{srv_turret::kGunnerSheetVs, 0x25D5F73A4616093Bull};

inline bool protectsInterfaceShader(uint64_t vs, uint64_t ps, bool systemMap, char kind,
                                    unsigned count, unsigned instances, bool sampledUi) noexcept {
    if (sampledUi || uiVsIsHoloPanel(vs) || vs == kUiVsFlightHud || vs == kUiVsSprite ||
        vs == kUiVsPanel || vs == kUiVsLoader || vs == kUiVsGuiVector || vs == kUiVsGuiText ||
        vs == kUiVsGuiIcons || vs == kHoloTargetSphere || vs == kHoloWorldMarkerReticle ||
        uiHoloGenericHash(vs) || uiOnFootHudMatch(vs, ps) ||
        screenMotionUiPair(vs, ps) ||
        screenMotionRecognizePair(vs, ps) ||
        (vs == kLateMenuUiPair.vs && ps == kLateMenuUiPair.ps) ||
        uiLayerMapFamilyOf(vs, ps) != UiLayerFamily::kNone)
        return true;
    if (systemMap && ((vs == kUiVsMapPlane && ps == kUiPsMapPlane) ||
                      ((vs == kUiVsMapLinesA || vs == kUiVsMapLinesB) && ps == kUiPsMapLines)))
        return true;
    const bool shapeUnknown = kind == 'Z' || kind == 'Y' || kind == 'A';
    const bool scopeMarkPair = vs == kUiVsScopeMark && ps == kUiPsScopeMark;
    const bool gunnerPanelVs = vs == srv_turret::kGunnerSheetVs || vs == srv_turret::kGunnerCardsVs;
    return srv_turret::familyOf(vs, ps) >= 0 ||
           srv_turret::gunnerSignature(vs, kind, count) >= 0 ||
           (scopeMarkPair && uiScopeMarkShape(kind, count, instances)) ||
           (shapeUnknown && (gunnerPanelVs || scopeMarkPair));
}

struct SceneMaterialDraw {
    int eye = -1;
    int targetKind = 0;
    bool shadersTracked = false;
    bool interfaceDraw = false;
    bool originalIssue = false;
};

inline bool sceneMaterialCandidate(const SceneMaterialDraw& draw) noexcept {
    return draw.originalIssue && draw.eye >= 0 && draw.eye < 2 && draw.targetKind == 1 &&
           draw.shadersTracked && !draw.interfaceDraw;
}

} // namespace edvr::native_vrs

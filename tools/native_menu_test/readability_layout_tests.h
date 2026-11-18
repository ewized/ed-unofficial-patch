// GDI-only VR readability cases, called by native_menu_test --layout.
#pragma once
#include <memory>
#include <string>
#include "../../src/common/native_fg_readout.h"

inline void menuReadabilityLayoutCases(unsigned& checks, unsigned& fails) {
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) { ++fails; std::printf("FAIL: %s\n", label); }
    };
    // Keep these models off the existing fixture's already sizeable stack frame.
    auto owned = std::make_unique<edvr::MenuContent>();
    auto& page = *owned;
    page.wrapRows = true;
    page.cardPx = 1920;
    page.widthPx = static_cast<int>(1920 * edvr::kTipRatio + 0.5f);
    page.capPx = 50;
    page.popupLine = -1;
    page.tabCount = 1;
    std::strcpy(page.tabs[0], "Performance");
    std::strcpy(page.footer, "PageUp/PageDown scroll; Tab changes page");
    page.reserveHint = true;
    page.bodyMaxPx = page.capPx * 18;
    page.lineCount = edvr::kMenuMaxLines;
    std::string label;
    for (int i = 0; i < 6; ++i) label += "Native VRS setting ";
    label += "END";
    std::string value;
    for (int i = 0; i < 14; ++i) value += "1.2 ms ";
    value += "END";
    for (int i = 0; i < page.lineCount; ++i) {
        auto& row = page.lines[i];
        std::snprintf(row.left, sizeof(row.left), "%s", label.c_str());
        std::snprintf(row.right, sizeof(row.right), "%s", value.c_str());
        row.style = edvr::kMenuRow;
        row.reserveBadge = true;
        row.badge = edvr::kBadgePending;
    }
    check(sizeof(page.lines[0].left) >= 240 && sizeof(page.lines[0].right) >= 240,
          "full current status producers fit before drawing");
    check(std::strlen(page.lines[0].left) == label.size(), "the model preserves a label longer than the old 95-byte limit");
    check(edvr::menuPanelHardWrapKeepsTextForTest(L"\U0001f600\U0001f642TAIL", 1, 72),
          "a hard break narrower than one glyph preserves complete leading surrogate pairs");
    for (const int cap : {10, 14, 34, 50, 80}) {
        page.capPx = cap;
        page.bodyMaxPx = cap * 18;
        page.focusLine = page.lineCount - 1;
        const auto body = edvr::menuPanelBodyScrollForTest(page);
        const int height = edvr::menuPanelLayoutHeightForTest(page);
        const auto last = edvr::menuPanelRowRectsForTest(page, page.focusLine);
        check(height > 0 && height <= 2048, "wrapped settings fit the raster at every supported text cap");
        check(body.maxPx > 0 && body.offsetPx > 0, "the last logical row reveals itself in the bounded viewport");
        check(last.selectable && last.hitBottom > last.hitTop && last.hitBottom <= height,
              "the final row keeps a visible hit rectangle after scrolling");
        check(edvr::menuPanelRowTextFitsForTest(page, page.focusLine, false, L"END") &&
              edvr::menuPanelRowTextFitsForTest(page, page.focusLine, true, L"END"),
              "GDI receives both complete suffixes in measured non-ellipsis boxes");
        check(last.badge[1] >= last.label[3] && last.badge[3] > last.badge[1],
              "the restart badge remains below the complete wrapped primary text");
    }
    page.capPx = 50;
    page.focusLine = -1;
    page.bodyScrollPx = 0x7fffffff;
    auto body = edvr::menuPanelBodyScrollForTest(page);
    check(body.offsetPx == body.maxPx, "manual reading is bounded at the final row");
    check(!edvr::menuPanelRowRectsForTest(page, 0).selectable, "a fully scrolled-away row cannot be hit");
    page.lines[31].style = edvr::kMenuNote;
    page.lines[31].badge = edvr::kBadgeNone;
    page.lines[31].reserveBadge = false;
    page.lines[31].right[0] = 0;
    const std::string note = std::string(220, 'n') + " final native status END";
    std::snprintf(page.lines[31].left, sizeof(page.lines[31].left), "%s", note.c_str());
    check(edvr::menuPanelRowTextFitsForTest(page, 31, false, L"END"), "the trailing non-selectable native status retains its full text");
    page.tileCount = 16;
    page.graphCount = 3;
    edvr::NativeFgReadout mode;
    mode.automatic = true;
    char modeText[24]{};
    edvr::formatNativeFgTileValue(mode, modeText, sizeof(modeText));
    for (int i = 0; i < page.tileCount; ++i) {
        std::strcpy(page.tiles[i].caption, i == 0 ? "FG MODE" : "GENERATED");
        std::strcpy(page.tiles[i].value, i == 0 ? modeText : "125.0");
        std::strcpy(page.tiles[i].sub, i == 0 ? "Auto" : "fps; last 1s");
    }
    page.bodyMaxPx = 0;
    page.capPx = 80;
    const int unit = page.capPx * 9 / 10;
    const int tileW = (page.cardPx - 2 * (page.capPx * 8 / 10) - 3 * (page.capPx / 4)) / 4;
    const int tileEm = unit * 15 / 10;
    for (int state = 0; state < 7; ++state) {
        mode = edvr::NativeFgReadout{};
        mode.automatic = state == 1;
        if (state == 2 || state == 3) {
            mode.measurement = edvr::NativeFgMeasureState::Counting;
            mode.totals.fgEnabled = true;
            mode.totals.activeMultiplier = state == 2 ? 2 : 4;
        }
        if (state == 4) mode.selectedMultiplier = 2;
        if (state == 5) mode.failed = true;
        if (state == 6) mode.backendUnavailable = true;
        edvr::formatNativeFgTileValue(mode, modeText, sizeof(modeText));
        check(edvr::menuPanelTabWidth(modeText, tileEm) - tileEm / 3 <= tileW - 2 * (unit / 3),
              "every production FG mode value fits its actual bold tile rectangle");
    }
    for (const char* valueText : {"125.0", "750.0"}) {
        check(edvr::menuPanelTabWidth(valueText, tileEm) - tileEm / 3 <= tileW - 2 * (unit / 3),
              "the generated FPS number fits without reducing its font");
    }
    body = edvr::menuPanelBodyScrollForTest(page);
    check(body.maxPx > 0 && body.offsetPx == body.maxPx && edvr::menuPanelLayoutHeightForTest(page) <= 2048,
          "Monitor tiles, notes and graphs overflow into scrolling instead of a stale raster");
    check(edvr::menuPanelRowTextFitsForTest(page, 31, false, L"END"), "Monitor overflow retains the complete last note");
    // Flat keeps the old fixed row geometry, even when the backing fields grow.
    page = edvr::MenuContent{};
    page.cardPx = page.widthPx = 860;
    page.capPx = 34;
    page.lineCount = 1;
    page.lines[0].style = edvr::kMenuRow;
    std::strcpy(page.lines[0].left, "short");
    const int flatH = edvr::menuPanelLayoutHeightForTest(page);
    std::snprintf(page.lines[0].left, sizeof(page.lines[0].left), "%s", label.c_str());
    check(edvr::menuPanelLayoutHeightForTest(page) == flatH && edvr::menuPanelBodyScrollForTest(page).maxPx == 0,
          "flat layout remains fixed and does not enable VR wrapping or scrolling");
}

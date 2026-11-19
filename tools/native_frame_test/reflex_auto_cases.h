#pragma once

#include "../../src/d3d11/native_reflex.h"

namespace reflex_auto_cases {

template<class Check> void run(Check check) {
    using namespace edvr::native_reflex;
    const auto mode = [](const char* choice, const char* fg, bool vr) {
        return driverModeForSelection(choice, edvr::nativeFgSelection(fg), vr);
    };
    for (const char* fg : {"auto", "2x", "3x", "4X"})
        check(mode("AuTo", fg, true) == Mode::On,
              "Auto requests driver On for every enabled VR frame generation selection");
    check(mode("auto", "off", true) == Mode::Off &&
          mode("auto", "1x", true) == Mode::Off &&
          mode("auto", "5x", true) == Mode::Off,
          "Auto is Off when frame generation is Off or has no supported selection");
    check(mode("off", "auto", true) == Mode::Off &&
          mode("ON", "off", true) == Mode::On &&
          mode("boost", "off", true) == Mode::Boost &&
          mode("invalid", "auto", true) == Mode::Off,
          "explicit VR settings retain precedence and unknown values stay Off");
    for (const char* choice : {"auto", "off", "on", "boost"})
        check(mode(choice, "4x", false) == Mode::Off,
              "a non-VR profile leaves the driver setting Off");
    ModeHistory history;
    check(history.change(mode("auto", "2x", true)),
          "first Auto request goes to the driver");
    history.acceptRequest();
    check(!history.change(mode("auto", "auto", true)) && history.observing() &&
          !history.change(mode("auto", "4x", true)),
          "FG selection changes with the same effective mode do not restart observations");
    check(history.change(mode("auto", "off", true)) && history.requiresDriver(),
          "turning FG Off lets the owner clear its prior Auto request");
}

} // namespace reflex_auto_cases

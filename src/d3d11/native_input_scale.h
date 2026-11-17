#pragma once
#include "../common/native_input_scale.h"
#include <string>

namespace edvr {
class Config;
// Exactly once, in device setup BEFORE creating any game sampler. No later
// INI/menu reload or device recreation can recapture this process's value.
void nativeInputScaleCapture(Config& cfg, bool hmdKnown, float hmd, float ssaa);
float nativeInputScaleSaved(Config& cfg);
float nativeInputScaleLatched(); // startup REQUEST, for restart-pending UI
float nativeInputScaleActive();  // accepted effective scale,1 on refusal
std::string nativeInputScaleMenuStatus(); // cached/snapshot only; no I/O/VR
}

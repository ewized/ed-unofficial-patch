#pragma once

namespace edvr { namespace beyond_controls {

// Keep saved Magic VRS requests distinct from native driver acknowledgement.
inline const char* vrsMode(unsigned mode) {
    const char* names[] = {"Off", "Maximum", "Balanced", "Minimum", "Debug"};
    return mode < 5 ? names[mode] : "Unknown";
}

} }

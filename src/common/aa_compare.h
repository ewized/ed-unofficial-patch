// The AA comparison key (hotkey.compare_aa, default F7): one press turns EDVR's temporal anti-aliasing off for a side-by-side
// look, the next restores the running engine. Pure: no Windows, no globals. The rig is tools\aa_compare_test; the module that
// binds the key, reads the clock and shows the notice is src\d3d11\aa_compare.cpp.
//
// RUNTIME ONLY. The comparison is a Config runtime override of fix.temporal_aa (config.h: setRuntimeOverride). No ini is written,
// ever, so a crash mid-comparison leaves the player's settings exactly as they were, and the next launch has AA as configured.
// Going off is the same event as choosing "off" in the F8 menu, minus the write: every module that reads the mode re-reads it on
// the configure sweep the override triggers, which is also how fix.ui_quality follows (it is live only with AA on).
//
// THE SEMANTICS, all of them:
//   * A press with AA running turns it off and remembers the configured mode for detecting edits.
//   * A second press releases the override; a native DLSS sizing latch can keep the restored engine different from the file.
//   * A press with no AA running changes nothing: there is nothing to compare.
//   * The player changing AA while it is off -- a pick in the F8 menu, an edit of the ini, the installer's window -- ENDS the
//     comparison: the override clears, and the key's next press is a fresh "off". An F8 pick that lands on the
//     very mode that was remembered ends it too, after kMenuEditSettleMs, once the write has had time to land.
//   * A profile switch (VR <-> flat) ends it: the other profile reads another file, and Config drops every override at the switch.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "temporal_mode.h"

namespace edvr {
namespace aacmp {

inline constexpr char kDefaultKey[] = "F7";
// An F8 pick is written by a worker and reloaded within a poll; three seconds is ample for either, and the file's value having
// changed by then is the usual end.
inline constexpr uint64_t kMenuEditSettleMs = 3000;

enum class Press : uint8_t {
    WentOff,            // AA is off for the comparison
    Restored,           // the override is released
    NothingToCompare,   // AA was already off: the press changed nothing
};

enum class Seen : uint8_t {
    Nothing,
    EndedByChange,      // the saved mode changed under the comparison
    EndedBySameValue,   // an F8 pick landed on the remembered mode
    EndedByProfile,     // the override is gone (a profile switch cleared it): the comparison is over
};

inline bool modeEquals(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

struct Machine {
    bool off = false;            // the comparison is showing AA off
    std::string saved;           // the mode the settings said when it went off
    bool menuEdit = false;       // the F8 menu wrote AA while it was off
    uint64_t menuEditMs = 0;

    void reset() { off = false; saved.clear(); menuEdit = false; menuEditMs = 0; }

    // The running engine decides availability. Only the saved setting can identify edits during the comparison.
    Press press(const std::string& running, const std::string& configured) {
        if (off) {
            reset();
            return Press::Restored;
        }
        if (!temporalModeEnabled(running)) return Press::NothingToCompare;
        off = true;
        saved = configured;
        menuEdit = false;
        return Press::WentOff;
    }

    // The F8 menu queued a write of fix.temporal_aa. Its value is not known here and the file has not changed yet.
    void noteMenuEdit(uint64_t nowMs) {
        if (!off) return;
        menuEdit = true;
        menuEditMs = nowMs;
    }

    // Between presses, every frame. `overrideInForce` is Config::hasRuntimeOverride("fix.temporal_aa").
    Seen observe(const std::string& configured, bool overrideInForce, uint64_t nowMs) {
        if (!off) return Seen::Nothing;
        if (!overrideInForce) {
            reset();
            return Seen::EndedByProfile;
        }
        if (!modeEquals(configured, saved)) {
            reset();
            return Seen::EndedByChange;
        }
        if (menuEdit && nowMs - menuEditMs >= kMenuEditSettleMs) {
            reset();
            return Seen::EndedBySameValue;
        }
        return Seen::Nothing;
    }
};

// The mode as the player knows it.
inline std::string modeLabel(const std::string& mode) {
    if (_stricmp(mode.c_str(), "on") == 0) return "TAA";
    if (_stricmp(mode.c_str(), "dlaa") == 0) return "DLAA";
    if (_stricmp(mode.c_str(), "dlss") == 0) return "DLSS";
    if (_stricmp(mode.c_str(), "fsr") == 0) return "FSR";
    if (_stricmp(mode.c_str(), "xess") == 0) return "XeSS";
    if (mode.empty() || _stricmp(mode.c_str(), "off") == 0) return "off";
    return mode;
}

// The short notice for a press. On restore, read the running engine after releasing the override.
inline std::string noticeText(Press p, const std::string& running, const std::string& key) {
    switch (p) {
        case Press::WentOff: return "AA off (comparison), press " + key + " to restore";
        case Press::Restored: return "AA restored: " + modeLabel(running);
        case Press::NothingToCompare: return "AA is off, nothing to compare";
    }
    return std::string();
}

// Would a press of (vk, mods) and a press of (vk2, mods2) be the same press to the game? The same key clashes unless each side
// holds a modifier the other lacks (a bare key also fires with Ctrl held, so a bare binding and a Ctrl one overlap).
inline bool keysClash(int vk, uint32_t mods, int vk2, uint32_t mods2) {
    if (vk == 0 || vk != vk2) return false;
    return !((mods2 & ~mods) != 0 && (mods & ~mods2) != 0);
}

}  // namespace aacmp
}  // namespace edvr

#pragma once

#include "menu_schema.h"

#include <cstring>

namespace edvr {

// Keep the controls used together near the top of the bounded VR viewport.
// All remaining Performance settings keep their generated INI order.
struct VrPerformanceKey { const char* section; const char* key; };
inline constexpr VrPerformanceKey kVrPerformancePriority[] = {
    {"fix", "temporal_aa"},
    {"fix", "temporal_aa_model"},
    {"fix", "frame_generation"},
    {"fix", "nvidia_reflex"},
    {"experimental", "eye_tracked_foveation"},
    {"experimental", "foveation_focus"},
    {"experimental", "foveation_highlight"},
};

inline bool vrPerformanceMatches(const MenuRowDef& row, const VrPerformanceKey& key) {
    return std::strcmp(row.section, key.section) == 0 && std::strcmp(row.key, key.key) == 0;
}

// The VR page takes the generated Fix-tier Performance rows. Flat Performance
// has a separate source in menu_pages.h and never calls this visitor.
template<class Visit>
void forEachVrPerformanceRow(const MenuRowDef* rows, std::size_t count, Visit visit) {
    auto isPerformance = [](const MenuRowDef& row) {
        return row.tier == MenuTier::Fix && std::strcmp(row.page, "performance") == 0;
    };
    for (const auto& key : kVrPerformancePriority)
        for (std::size_t i = 0; i < count; ++i)
            if (isPerformance(rows[i]) && vrPerformanceMatches(rows[i], key))
                visit(static_cast<int>(i));
    for (std::size_t i = 0; i < count; ++i) {
        if (!isPerformance(rows[i])) continue;
        bool alreadyListed = false;
        for (const auto& key : kVrPerformancePriority)
            if (vrPerformanceMatches(rows[i], key)) { alreadyListed = true; break; }
        if (!alreadyListed) visit(static_cast<int>(i));
    }
}

} // namespace edvr

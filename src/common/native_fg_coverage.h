#pragma once
#include "native_fg_multiplier.h"
#include <cmath>

namespace edvr::fg_coverage {
// Match the optical center used to render this original SDK phase. At small
// separations a later interpolated float can already coincide with endpoint B.
inline float coordinate(float a,float b,float fraction) noexcept {return a+fraction*(b-a);}

// The generated projection locates each eye at its original SDK phase.
// Measure the larger physical displacement from the current native endpoint.
template<class View>
double maxOpticalCenterShiftMeters(const View (&previous)[2], const View (&current)[2],
    unsigned multiplier, unsigned phase) noexcept {
    if (!phase || !nativeFgFixedMultiplier(multiplier) || phase >= multiplier) return 0.;
    const float fraction = float(phase) / float(multiplier);
    double largestSquared = 0.;
    for (unsigned eye = 0; eye < 2; ++eye) {
        double squared = 0.;
        for (unsigned axis = 0; axis < 3; ++axis) {
            const float endpoint = current[eye].position[axis];
            const double shift = double(coordinate(previous[eye].position[axis], endpoint, fraction)) - endpoint;
            if (!std::isfinite(shift)) return 0.;
            squared += shift * shift;
        }
        if (squared > largestSquared) largestSquared = squared;
    }
    return std::sqrt(largestSquared);
}

template<class View>
bool fits(const View (&previous)[2],const View (&current)[2],unsigned multiplier,unsigned phase) noexcept {
    if(!phase)return true;
    if(!nativeFgFixedMultiplier(multiplier)||phase>=multiplier)return false;
    const float fraction=float(phase)/float(multiplier);
    for(unsigned eye=0;eye<2;++eye)for(unsigned axis=0;axis<3;++axis) {
        const float b=current[eye].position[axis];
        if(coordinate(previous[eye].position[axis],b,fraction)!=b)return false;
    }
    return true;
}
// A protected pair can retain a later original SDK index when earlier
// intermediate optical centers do not coincide with the HUD endpoint.
template<class View>
bool anyFits(const View (&previous)[2],const View (&current)[2],unsigned multiplier) noexcept {
    if (!nativeFgFixedMultiplier(multiplier)) return false;
    for (unsigned phase=1;phase<multiplier;++phase)
        if (fits(previous,current,multiplier,phase)) return true;
    return false;
}
} // namespace edvr::fg_coverage

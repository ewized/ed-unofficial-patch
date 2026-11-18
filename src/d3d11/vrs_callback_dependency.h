#pragma once
#include "../common/pvr_gaze_snapshot.h"
#include <array>
#include <cmath>
#include <cstdint>

namespace edvr { namespace vrs_callback_dependency {
// Callback publication recency/dependency only. Never attests sensor freshness,
// PVR returnedValid, calibrated units, physical calibration or performance.
// Identity tokens come from verified COM resource identities, never target size.
struct Sample {
    EdvrPvrGazeSnapshotV1 gaze{};
    uint64_t nowQpc=0,qpcFrequency=0;
    unsigned eye=2,rtW=0,rtH=0,mapW=0,mapH=0,tilesW=0,tilesH=0,mip=0,slice=0;
    std::array<uint32_t,16> palette{};
    uintptr_t worldIdentity=0,mapIdentity=0;
    bool worldVerified=false;
};
inline bool eligible(const Sample& s) noexcept {
    const auto& g=s.gaze;
    if(!s.worldVerified||!s.worldIdentity||!s.mapIdentity||s.eye>1||!s.rtW||!s.rtH||
       !s.tilesW||!s.tilesH||s.mapW<s.tilesW||s.mapH<s.tilesH||
       s.tilesW!=(s.rtW+15)/16||s.tilesH!=(s.rtH+15)/16)return false;
    if(g.size!=sizeof(g)||g.version!=EDVR_PVR_GAZE_SNAPSHOT_VERSION_1||!g.sequence||
       g.trackerPresent!=1||g.providerQueried!=1||g.providerFiniteValid!=1||
       g.cachedFallback!=0||g.ignored!=0||g.mode<1||g.mode>4||g.invertY>1)return false;
    if(!s.qpcFrequency||g.qpcFrequency!=s.qpcFrequency||!g.callbackQpc||
       g.callbackQpc>s.nowQpc)return false;
    const double ageUs=double(s.nowQpc-g.callbackQpc)*1000000.0/double(s.qpcFrequency);
    if(!std::isfinite(ageUs)||ageUs>50000.0)return false;
    for(unsigned eye=0;eye<2;++eye)for(unsigned axis=0;axis<2;++axis)
        if(!std::isfinite(g.returnedGazeTan[eye][axis]))return false;
    // returnedValid is intentionally untouched and is not this separate claim.
    return true;
}
inline bool pairEligible(const Sample& previous,const Sample& current) noexcept {
    if(!eligible(previous)||!eligible(current))return false;
    const auto& a=previous;const auto& b=current;
    return a.eye==b.eye&&a.worldIdentity==b.worldIdentity&&a.mapIdentity==b.mapIdentity&&
        a.rtW==b.rtW&&a.rtH==b.rtH&&a.mapW==b.mapW&&a.mapH==b.mapH&&
        a.tilesW==b.tilesW&&a.tilesH==b.tilesH&&a.mip==b.mip&&a.slice==b.slice&&a.palette==b.palette&&
        a.qpcFrequency==b.qpcFrequency&&a.gaze.mode==b.gaze.mode&&a.gaze.invertY==b.gaze.invertY&&
        b.gaze.sequence>a.gaze.sequence&&b.gaze.callbackQpc>a.gaze.callbackQpc&&b.nowQpc>a.nowQpc&&
        double(b.nowQpc-a.nowQpc)*1000000.0/double(b.qpcFrequency)<=2500000.0;
}
} }

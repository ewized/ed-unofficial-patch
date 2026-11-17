#pragma once
#include "../common/native_frame_generation.h"
#include <openxr/openxr.h>
#include <algorithm>
#include <cmath>

namespace edvr::openxr::fgschedule {
inline uint64_t nextCredit(uint64_t grantedTick,unsigned multiplier) {return grantedTick+multiplier;}
inline unsigned firstPhase(const EdvrFgBatch& batch,uint64_t displayedEndpoint) {
  return batch.generatedCount&&displayedEndpoint&&batch.previousSequence==displayedEndpoint&&
    batch.currentSequence>displayedEndpoint?1u:0u;
}
inline unsigned nextPhase(unsigned phase,unsigned generatedCount) {
  return phase&&phase<generatedCount?phase+1u:0u;
}
inline XrView view(const EdvrFgView& a,const EdvrFgView& b,float fraction) {
  XrView out{XR_TYPE_VIEW};
  double dot=0;for(unsigned i=0;i<4;++i)dot+=double(a.orientation[i])*b.orientation[i];
  const double sign=dot<0?-1.:1.;dot=(std::min)(1.,std::fabs(dot));
  double wa=1.-fraction,wb=fraction;
  if(dot<.9995) {const double theta=std::acos(dot),s=std::sin(theta);wa=std::sin((1.-fraction)*theta)/s;wb=std::sin(fraction*theta)/s;}
  double q[4]{},norm=0;for(unsigned i=0;i<4;++i){q[i]=wa*a.orientation[i]+wb*sign*b.orientation[i];norm+=q[i]*q[i];}
  norm=std::sqrt(norm);out.pose.orientation={float(q[0]/norm),float(q[1]/norm),float(q[2]/norm),float(q[3]/norm)};
  out.pose.position={a.position[0]+fraction*(b.position[0]-a.position[0]),
    a.position[1]+fraction*(b.position[1]-a.position[1]),a.position[2]+fraction*(b.position[2]-a.position[2])};
  out.fov={a.fov[0]+fraction*(b.fov[0]-a.fov[0]),a.fov[1]+fraction*(b.fov[1]-a.fov[1]),
    a.fov[2]+fraction*(b.fov[2]-a.fov[2]),a.fov[3]+fraction*(b.fov[3]-a.fov[3])};return out;
}
inline XrTime phaseTime(const EdvrFgBatch& batch,unsigned phase) {
  if(!phase)return batch.currentPoseDisplayTime;
  return batch.previousPoseDisplayTime+(batch.currentPoseDisplayTime-batch.previousPoseDisplayTime)*phase/batch.multiplier;
}
} // namespace edvr::openxr::fgschedule

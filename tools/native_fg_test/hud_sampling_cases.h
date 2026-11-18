#pragma once
#include "../../src/openxr/native_fg_projection_policy.h"
#include <cmath>

namespace edvr::openxr::fgtest {
template<class Check> void runFgHudSamplingCases(Check check) {
  using fgprojection::select;
  // Opaque white UI covers the black world texel; its white neighbor has no
  // UI. Endpoints 0/1 survive sRGB decoding exactly. All filtering and alpha
  // composition below are in linear space, with identical content and poses.
  const double world[2]={0,1},nativeComposite[2]={1,1};
  const double coverageRgb[2]={1,0},coverageAlpha[2]={1,0};
  const auto sample=[](const double* texels,double offset) {
    return texels[0]*(1-offset)+texels[1]*offset;
  };
  const auto split=[&](double scene,double offset) {
    return sample(coverageRgb,offset)+scene*(1-sample(coverageAlpha,offset));
  };
  const auto presented=[&](unsigned phase,fgprojection::Selection choice,double offset) {
    const double* base=phase==0&&!choice.nativeWorld?nativeComposite:world;
    const double scene=sample(base,offset);
    return choice.separateUi?split(scene,offset):scene;
  };
  const auto equal=[](double a,double b){return std::fabs(a-b)<1e-12;};
  const double offsets[]={0.,.25,.5,.75,1.};

  const double oldNative=sample(nativeComposite,.5);
  const double oldGenerated=split(sample(world,.5),.5);
  check(equal(oldNative,1)&&equal(oldGenerated,.75),
    "old native combined/generated split topology reproduces a quarter-linear edge pulse");

  const auto native=select(0,true,false);
  check(native.separateUi&&native.nativeWorld,
    "native phase uses separate UI over raw native world, never a second copy over composited UI");
  for(unsigned phase=1;phase<=3;++phase) {
    const auto generated=select(phase,true,false);
    check(generated.separateUi&&!generated.nativeWorld,
      "every generated phase uses separate UI over its generated world");
    for(double offset:offsets) {
      check(equal(presented(0,native,offset),presented(phase,generated,offset)),
        "equal native/generated world, content and pose have no topology-driven sampling pulse");
    }
  }
  check(equal(presented(0,native,.5),.75)&&!equal(presented(0,native,.5),oldNative),
    "consistent split filtering still differs from legacy combined filtering at the edge");

  for(unsigned phase=0;phase<=3;++phase) {
    const auto missing=select(phase,false,false);
    check(!missing.separateUi&&!missing.nativeWorld,
      "missing UI retains the normal native or generated image without auxiliary projection");
    const auto fading=select(phase,true,true);
    check(!fading.separateUi&&!fading.nativeWorld,
      "comfort fade bypasses auxiliary UI and raw-native selection");
    const auto missingFading=select(phase,false,true);
    check(!missingFading.separateUi&&!missingFading.nativeWorld,
      "fade without UI never selects a missing raw-native or auxiliary image");
  }
}
} // namespace edvr::openxr::fgtest

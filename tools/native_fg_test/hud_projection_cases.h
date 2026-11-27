#pragma once
#include "../../src/openxr/native_fg_overlay.h"
#include <cmath>
#include <limits>

namespace edvr::openxr::fgtest {
template<class Check> void runFgUiOverlayCases(Check check) {
  XrView destination{XR_TYPE_VIEW};destination.pose.orientation.w=1;
  destination.fov={-.785398163f,.785398163f,.785398163f,-.785398163f};
  XrView source=destination;
  const auto equal=[](double a,double b){return std::fabs(a-b)<1e-6;};
  auto map=fgui::makeMapping(destination,source);
  check(map.valid,"identity world/native UI rotations admit the actual production mapping");
  const double samples[]={0.,.25,.5,.75,1.};
  for(double u:samples)for(double v:samples) {
    const auto at=fgui::lookup(map,u,v);
    check(at.visible&&equal(at.u,u)&&equal(at.v,v),"native endpoint B maps every tested full-eye UI coordinate identically");
  }

  // A B-camera marker at its own center is left of center in an earlier,
  // unrotated view. The old direct-UV pass misses it at that destination.
  const float yaw=.349065850f;
  source.pose.orientation={0,std::sin(yaw*.5f),0,std::cos(yaw*.5f)};
  map=fgui::makeMapping(destination,source);
  const double markerU=.5-.5*std::tan(double(yaw));
  const auto marker=fgui::lookup(map,markerU,.5);
  check(std::fabs(markerU-.5)>.1,"old direct UV sampling demonstrably misses the rotated B-camera marker");
  check(map.valid&&marker.visible&&equal(marker.u,.5)&&equal(marker.v,.5),
    "production destination-to-B rotation finds that marker without moving it to the destination center");
  const auto center=fgui::lookup(map,.5,.5);
  check(center.visible&&center.u>.65,"relative rotation uses inverse source times destination, not the opposite direction");
  check(!fgui::lookup(map,1,.5).visible,"a destination ray outside the B camera FOV has transparent UI");

  source=destination;const float pitch=.261799388f;
  source.pose.orientation={std::sin(pitch*.5f),0,0,std::cos(pitch*.5f)};
  const auto raised=fgui::lookup(fgui::makeMapping(destination,source),.5,.5-.5*std::tan(double(pitch)));
  check(raised.visible&&equal(raised.u,.5)&&equal(raised.v,.5),"pitch respects OpenXR positive Y and texture V down");
  source=destination;
  source.pose.orientation={0,0,.707106781f,.707106781f};
  const auto rolled=fgui::lookup(fgui::makeMapping(destination,source),.5,.25);
  check(rolled.visible&&equal(rolled.u,.75)&&equal(rolled.v,.5),"roll and canted source orientations preserve the source UI ray");

  source=destination;source.fov={-.463647609f,.982793723f,.785398163f,-.463647609f};
  const auto asymmetric=fgui::lookup(fgui::makeMapping(destination,source),.5,.5);
  check(asymmetric.visible&&equal(asymmetric.u,.25)&&equal(asymmetric.v,2./3.),
    "asymmetric source FOV is projected by tangents, not a symmetric-camera approximation");
  // At 90 degrees FOV a point one metre in front of each destination eye
  // moves half the lateral displacement in source UV. This holds for both
  // a cockpit panel edge and the world landmark at that same position.
  for(float eyeX:{-.032f,.032f})for(float displacement:{0.f,.0002f,.025f}) {
    destination.pose.position={eyeX,0,0};source=destination;
    source.pose.position.x+=displacement;
    const auto at=fgui::lookup(fgui::makeMapping(destination,source),.5,.5);
    const double landmarkU=.5-.5*displacement;
    if(!displacement)check(at.visible&&equal(at.u,landmarkU),"coincident eyes align the covered panel and world landmark");
    else check(!at.visible&&landmarkU<.5,"Idle-sized and 25 mm eye translations refuse the misregistered finite-depth panel");
  }
  source=destination;
  source.pose.orientation={0,1,0,0};
  check(!fgui::lookup(fgui::makeMapping(destination,source),.5,.5).visible,"UI rays behind the source camera are transparent");

  source=destination;source.pose.orientation={0,0,0,0};
  check(!fgui::makeMapping(destination,source).valid,"zero source quaternion is refused");
  source=destination;source.pose.position.x=(std::numeric_limits<float>::quiet_NaN)();
  check(!fgui::makeMapping(destination,source).valid,"nonfinite source optical center is refused");
  source=destination;source.fov.angleLeft=(std::numeric_limits<float>::infinity)();
  check(!fgui::makeMapping(destination,source).valid,"nonfinite source FOV is refused");
  source=destination;source.fov.angleLeft=source.fov.angleRight;
  check(!fgui::makeMapping(destination,source).valid,"collapsed source FOV is refused");
  source=destination;source.fov.angleRight=1.570796327f;
  check(!fgui::makeMapping(destination,source).valid,"source FOV reaching behind its camera is refused");
  source=destination;source.next=&destination;
  check(!fgui::makeMapping(destination,source).valid,"unsupported source view extension is refused");
  source=destination;destination.pose.orientation.w=2;
  check(!fgui::makeMapping(destination,source).valid,"nonunit destination rotation is refused");
  check(!fgui::lookup(fgui::Mapping{},.5,.5).visible,"invalid mapping never samples UI");
  fgui::Mapping forged{};forged.valid=true;
  check(!fgui::lookup(forged,.5,.5).visible,"forged zero-span mapping cannot divide by zero");
}
} // namespace edvr::openxr::fgtest

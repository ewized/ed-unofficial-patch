#pragma once
#include "eye_share.h"
#include "projection_math.h"
#include "native_fg_overlay_shader.h"
#include <cmath>
#include <limits>

namespace edvr::openxr::fgui {
struct Mapping {
  bool valid=false;
  float rot[3][4]{};
  float destinationTan[4]{},sourceTan[4]{};
};
struct Constants {
  float bounds[4]{},clampUV[4]{};
  float encodeSRGB=0,pad[3]{};
  float rot[3][4]{};
  float destinationTan[4]{},sourceTan[4]{};
};
static_assert(sizeof(Constants)==128,"native UI HLSL constant-buffer packing");

inline Mapping makeMapping(const XrView& destination,const XrView& source) {
  Mapping out;
  for(const auto* view:{&destination,&source}) {
    if(view->type!=XR_TYPE_VIEW||view->next)return out;
    const auto& q=view->pose.orientation;const auto& p=view->pose.position;
    for(float v:{q.x,q.y,q.z,q.w,p.x,p.y,p.z})if(!std::isfinite(v))return out;
    const double norm=double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z+double(q.w)*q.w;
    if(std::fabs(norm-1.)>=.001)return out;
    RawFov checked{};if(!fovToRaw(view->fov,checked))return out;
  }
  const auto dst=shareview::tangentsOf(destination.fov),src=shareview::tangentsOf(source.fov);
  if(!shareview::tangentsUsable(dst)||!shareview::tangentsUsable(src))return out;
  double rotation[3][3]{};
  shareview::relativeRotation(destination.pose.orientation,source.pose.orientation,rotation);
  for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col) {
    const double v=rotation[row][col];
    if(!std::isfinite(v)||std::fabs(v)>(std::numeric_limits<float>::max)())return {};
    out.rot[row][col]=float(v);
  }
  const double a[4]={dst.left,dst.right,dst.up,dst.down},b[4]={src.left,src.right,src.up,src.down};
  for(unsigned i=0;i<4;++i){out.destinationTan[i]=float(a[i]);out.sourceTan[i]=float(b[i]);}
  // Position is validated above but deliberately does not enter this mapping.
  // A source distance/depth would be required for positional compensation.
  out.valid=true;return out;
}

struct Lookup { bool visible=false;double u=0,v=0; };
inline Lookup lookup(const Mapping& map,double u,double v) {
  if(!map.valid||!std::isfinite(u)||!std::isfinite(v)||u<0||u>1||v<0||v>1)return {};
  const double width=map.sourceTan[1]-map.sourceTan[0],height=map.sourceTan[2]-map.sourceTan[3];
  if(!std::isfinite(width)||!std::isfinite(height)||width<=1e-6||height<=1e-6)return {};
  const double d[3]={map.destinationTan[0]+u*(map.destinationTan[1]-map.destinationTan[0]),
    map.destinationTan[2]+v*(map.destinationTan[3]-map.destinationTan[2]),-1};
  double r[3]{};for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col)r[row]+=map.rot[row][col]*d[col];
  if(!(r[2]<-1e-6))return {};
  // Keep even compile-time invalid fixture values away from literal zero
  // divisors: MSVC folds their unreachable arithmetic with /O2 /WX.
  const double forward=-r[2];
  const double inverseForward=1./(forward>0?forward:1.);
  const double x=(r[0]*inverseForward-map.sourceTan[0])/(width>0?width:1.);
  const double y=(map.sourceTan[2]-r[1]*inverseForward)/(height>0?height:1.);
  if(!std::isfinite(x)||!std::isfinite(y)||x<0||x>1||y<0||y>1)return {};
  return {true,x,y};
}
} // namespace edvr::openxr::fgui

#pragma once
#include "../common/temporal_math.h"
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace edvr {
enum class SceneProjectionSource : uint32_t { None, Legacy198, Clip270 };
struct SceneProjectionCapture {
    float a = 0, b = 0, legacyA = 0, legacyB = 0;
    SceneProjectionSource source = SceneProjectionSource::None;
};
// Called on the full mapped scene CB before Unmap. Near-plane consistency is
// checked again by temporalSceneProjection at the actual eye SR boundary.
inline SceneProjectionCapture captureSceneProjection(const void* data, uint32_t bytes, float nearZ = 0) noexcept {
    SceneProjectionCapture result;
    if (!data || bytes < 796u * sizeof(float)) return result;
    const auto* source = static_cast<const unsigned char*>(data);
    std::memcpy(&result.legacyA, source + 794u * sizeof(float), sizeof(float));
    std::memcpy(&result.legacyB, source + 795u * sizeof(float), sizeof(float));
    if (temporalSceneProjection(result.legacyA, result.legacyB, nearZ, &result.a, &result.b)) {
        result.source = SceneProjectionSource::Legacy198;
        return result;
    }
    result.a = result.b = 0;
    if (bytes < 274u * 16u) return result;
    // Existing draw_record_tag::worldClip and flatJitterForwardColumns own
    // this layout: clip = x*r270 + y*r271 + z*r272 + r273. Therefore
    // z_clip=A*w_clip+B iff the spatial z column is A times the w column,
    // and B=z.w-A*w.w. This uses this exact write, never a near constant.
    float rows[16];
    std::memcpy(rows,source+270u*16u,sizeof(rows));
    for (float value:rows) if (!std::isfinite(value)) return result;
    const double x[3]={rows[0],rows[4],rows[8]},y[3]={rows[1],rows[5],rows[9]};
    const double z[3]={rows[2],rows[6],rows[10]},w[3]={rows[3],rows[7],rows[11]};
    const auto dot=[](const double* a,const double* b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const double ww=dot(w,w),xx=dot(x,x),yy=dot(y,y);
    if (!(ww>.95*.95 && ww<1.05*1.05) || xx<=1e-12 || yy<=1e-12) return result;
    const double cross[3]={y[1]*w[2]-y[2]*w[1],y[2]*w[0]-y[0]*w[2],y[0]*w[1]-y[1]*w[0]};
    if (std::fabs(dot(x,cross))<=1e-6*std::sqrt(xx*yy*ww)) return result;
    const double a=dot(z,w)/ww,b=double(rows[14])-a*rows[15];
    const double scale=(std::max)({std::fabs(z[0]),std::fabs(z[1]),std::fabs(z[2])});
    for(unsigned i=0;i<3;++i) if(std::fabs(z[i]-a*w[i])>1e-5*scale+1e-12) return result;
    if (!std::isfinite(a) || !std::isfinite(b) || a>0 || b<=0 ||
        !temporalSceneProjection(static_cast<float>(a),static_cast<float>(b),nearZ,&result.a,&result.b)) {
        result.a=result.b=0;return result;
    }
    result.source=SceneProjectionSource::Clip270;
    return result;
}
inline const char* sceneProjectionSourceName(SceneProjectionSource source) noexcept {
    switch (source) {
    case SceneProjectionSource::Legacy198: return "legacy-row-198";
    case SceneProjectionSource::Clip270: return "clip-rows-270";
    default: return "unmeasured";
    }
}
}

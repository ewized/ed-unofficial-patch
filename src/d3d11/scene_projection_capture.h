#pragma once
#include "../common/temporal_math.h"
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace edvr {
enum class SceneProjectionSource : uint32_t { None, Legacy198, Clip270 };
struct SceneProjectionCapture {
    float legacyA = 0, legacyB = 0;
    float clipA = 0, clipB = 0;
};

// Keep both candidates from this exact mapped write. The eye whose depth
// buffer is being decoded supplies its near plane later at the SR boundary.
inline SceneProjectionCapture captureSceneProjection(const void* data, uint32_t bytes) noexcept {
    SceneProjectionCapture result;
    if (!data || bytes < 796u * sizeof(float)) return result;
    const auto* source = static_cast<const unsigned char*>(data);
    std::memcpy(&result.legacyA, source + 794u * sizeof(float), sizeof(float));
    std::memcpy(&result.legacyB, source + 795u * sizeof(float), sizeof(float));
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
    if (std::isfinite(a) && std::isfinite(b) && a <= 0 && b > 0) {
        result.clipA = static_cast<float>(a);
        result.clipB = static_cast<float>(b);
    }
    return result;
}

// Prefer the legacy row when both candidates describe this eye's depth.
// temporalSceneProjection also supplies SR's infinite-far fallback on a miss.
inline SceneProjectionSource resolveSceneProjection(const SceneProjectionCapture& captured,
                                                    float nearZ, float* a, float* b) noexcept {
    if (temporalSceneProjection(captured.legacyA, captured.legacyB, nearZ, a, b))
        return SceneProjectionSource::Legacy198;
    if (temporalSceneProjection(captured.clipA, captured.clipB, nearZ, a, b))
        return SceneProjectionSource::Clip270;
    return SceneProjectionSource::None;
}
inline const char* sceneProjectionSourceName(SceneProjectionSource source) noexcept {
    switch (source) {
    case SceneProjectionSource::Legacy198: return "legacy-row-198";
    case SceneProjectionSource::Clip270: return "clip-rows-270";
    default: return "unmeasured";
    }
}
}

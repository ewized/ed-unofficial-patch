#pragma once

namespace witness_gpu {
// Full-rate HLSL checks the exact interpolated bits separately. Here the
// smooth E.z labels the winning triangle and E.xy follows its screen gradient.
inline bool fixtureSkinOwner(float encodedOwner,const float skin[4],float nx,float ny){
    const bool visibleOwner=encodedOwner==3.f || encodedOwner==5.f || encodedOwner==9.f;
    return visibleOwner && skin[2]==(encodedOwner-1.f)*.5f &&
        skin[3]>0.f && skin[0]*nx>0.f && skin[1]*ny>0.f;
}
}

#pragma once

namespace edvr {
// ED-UP's measured guide metadata, independent of SDK headers and ABI layout.
// The backend maps this into private SDK options retained through GPU completion;
// evaluation indices and resource subrectangles remain backend-owned.
struct FrameGenerationCamera {
    float cameraViewToClip[4][4]{},clipToCameraView[4][4]{},clipToLensClip[4][4]{};
    float clipToPrevClip[4][4]{},prevClipToClip[4][4]{};
    float jitterOffset[2]{},mvecScale[2]{},cameraPinholeOffset[2]{};
    float cameraPos[3]{},cameraUp[3]{},cameraRight[3]{},cameraFwd[3]{};
    float cameraNear=0,cameraFar=0,cameraFOV=0,cameraAspectRatio=0;
    float motionVectorsInvalidValue=0,minRelativeLinearDepthObjectSeparation=40.f;
    bool depthInverted=false,cameraMotionIncluded=false,reset=false,orthoProjection=false;
    bool motionVectorsDilated=false,menuDetectionEnabled=false;
};
} // namespace edvr

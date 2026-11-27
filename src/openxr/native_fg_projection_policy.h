#pragma once
#include <openxr/openxr.h>

namespace edvr::openxr::fgprojection {
// With no per-pixel geometry, only rays from the same optical center have
// a depth-independent correspondence. Use each actual eye, including its IPD.
inline bool sameOpticalCenter(const XrVector3f& a,const XrVector3f& b) noexcept {
  return a.x==b.x&&a.y==b.y&&a.z==b.z;
}
struct Selection {
  bool separateUi=false;
  bool nativeWorld=false;
};

// Keep the same composition topology throughout a batch. Phase zero needs
// raw native world here: its fully composited fallback already contains UI.
inline Selection select(unsigned phase,bool nativeUiAvailable,bool fading) noexcept {
  const bool separate=nativeUiAvailable&&!fading;
  return {separate,separate&&phase==0};
}
} // namespace edvr::openxr::fgprojection

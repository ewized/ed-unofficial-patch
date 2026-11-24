#pragma once

namespace edvr::openxr::fgprojection {
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

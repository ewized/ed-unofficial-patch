#pragma once
// A clear does not bind the target or change pipeline state. Shared with the
// WARP regression so black mode needs no mirror shaders, SRVs or eye draws.
namespace edvr {
template<class Context, class Target>
inline void desktopWindowClearBlack(Context* ctx, Target* target) {
    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    ctx->ClearRenderTargetView(target, black);
}
} // namespace edvr

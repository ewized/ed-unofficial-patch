#pragma once
// Indirect and auto draws have no CPU counts. Recording on another context
// can neither establish nor invalidate the immediate context's shading rate.
template<class Draw>
void nativeUncountedMaterialIssue(ID3D11DeviceContext* context, char kind, Draw draw) {
    if (!nativeVrsLive() || context != g_state->ownerCtx) { draw(); return; }
    const auto decision = nativeSceneMaterialDecision(context, true, false, kind, 0, 0);
    nativeWorldIssue(decision, [&]{
        if(nativeSplitOriginal(context,[&](bool){draw();}))return true;
        nativeBeforeOriginal(context);
        draw();
        return true;
    });
}

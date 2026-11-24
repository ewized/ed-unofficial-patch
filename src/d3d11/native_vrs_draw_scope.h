#pragma once
// The draw hook supplies its current NativeWorldDecision and binding reads.
// Ineligible draws still service pending measurements without classifying
// shaders or preparing geometry; nativeVrsPsBefore enforces the query owner.
struct NativePsDrawScope {
    ID3D11DeviceContext* context;NativeVrsPsToken token{};
    NativePsDrawScope(ID3D11DeviceContext* c,char kind,uint32_t count,uint32_t instances,
        uint32_t start,int32_t base=0,uint32_t startInstance=0):context(c) {
        if(!nativeVrsPsMeasureLive())return;
        const auto* d=g_nativeWorldDecision;
        if(!d||d->context!=c||d->eye>=2){
            // Retire pending measurements even when coarse shading or world
            // admission is absent. The empty draw services only the owner;
            // it cannot read draw metadata or begin a new query.
            nativeVrsPsBefore(c,{});return;
        }
        token=nativeVrsPsBefore(c,{bindingShaderHash(BindSlot::Vs),bindingShaderHash(BindSlot::Ps),d->eye,
            count,instances,start,startInstance,base,kind,d->rtv,d->dsv});
    }
    ~NativePsDrawScope(){if(token.active)nativeVrsPsAfter(context,token);}
};

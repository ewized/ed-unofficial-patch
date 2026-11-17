#pragma once
#include <atomic>
#include <cstdint>
#include "vrs_coverage_policy.h"
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace edvr {
extern std::atomic<bool> g_vrsCoverageEnabled;
extern std::atomic<bool> g_vrsWorkReferenceRequested;
inline bool vrsCoverageEnabled() noexcept {return g_vrsCoverageEnabled.load(std::memory_order_relaxed);}
inline bool vrsWorkReferenceEnabled() noexcept {return g_vrsWorkReferenceRequested.load(std::memory_order_relaxed);}
void vrsCoverageConfigure(bool enabled);
void vrsWorkReferenceConfigure(bool requested);
void vrsCoverageRegister(ID3D11Device*,ID3D11DeviceContext*);
void vrsCoverageTick(ID3D11DeviceContext*);
void vrsCoverageInvalidate(ID3D11DeviceContext*);
struct VrsCoverageDraw {
    uint64_t id=0,epoch=0;
    unsigned eye=2,ring=3,pipelineRing=3;
    bool active=false,enabled=false,known=false,substituted=false,nested=false;
};
// Direct-call arguments from draw_census.h's owner; original shader hashes
// identify the qualified material, not necessarily an EDUP replacement shader.
struct VrsCoverageSignature {
    uint64_t vs=0,ps=0;
    uint32_t count=0,instances=0,start=0,startInstance=0;
    int32_t base=0;
    char kind='?';
};
struct VrsWorkReferenceToken {uint64_t id=0;unsigned ring=3;bool active=false;};
VrsWorkReferenceToken vrsWorkReferenceBeforeNative(ID3D11DeviceContext*,const VrsCoverageSignature&);
void vrsWorkReferenceAfterNative(ID3D11DeviceContext*,const VrsWorkReferenceToken&);
class VrsWorkReferenceScope {
    ID3D11DeviceContext* context_;VrsWorkReferenceToken token_;
public:
    VrsWorkReferenceScope(ID3D11DeviceContext* c,const VrsCoverageSignature& s):context_(c),token_(vrsWorkReferenceBeforeNative(c,s)){}
    ~VrsWorkReferenceScope(){if(token_.active)vrsWorkReferenceAfterNative(context_,token_);}
    VrsWorkReferenceScope(const VrsWorkReferenceScope&)=delete;
    VrsWorkReferenceScope& operator=(const VrsWorkReferenceScope&)=delete;
};
VrsCoverageDraw vrsCoverageBeforeDraw(ID3D11DeviceContext*,unsigned eye,
    ID3D11RenderTargetView*,ID3D11DepthStencilView*,bool motionSubstitution,const VrsCoverageSignature&);
void vrsCoverageAfterDraw(ID3D11DeviceContext*,const VrsCoverageDraw&,bool issued);
void vrsCoverageNoteGate(uint64_t vs,uint64_t ps,bool owner,bool rtvEye,bool notUi,
    bool allowedClass,int depthEye,bool beforeAccepted,bool issued);
}

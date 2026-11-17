#pragma once
// Compile the actual draw scope against counted adapter/binding dependencies.
// Recorder query retirement itself is exercised separately by the WARP cases.
namespace ps_draw_scope_fixture {
struct NativeVrsPsDraw {
    uint64_t vs=0,ps=0;unsigned eye=2;
    uint32_t count=0,instances=0,start=0,startInstance=0;int32_t base=0;char kind='?';
    ID3D11RenderTargetView* target=nullptr;ID3D11DepthStencilView* depth=nullptr;
};
struct NativeVrsPsToken {bool active=false;};
struct NativeWorldDecision {
    ID3D11DeviceContext* context=nullptr;unsigned eye=2;
    ID3D11RenderTargetView* rtv=nullptr;ID3D11DepthStencilView* dsv=nullptr;
};
struct Counts {unsigned calls=0,services=0,hashes=0,begins=0,ends=0,empty=0;};
inline Counts counts;
inline auto* owner=reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(0x301));
inline auto* foreign=reinterpret_cast<ID3D11DeviceContext*>(uintptr_t(0x302));
inline const NativeWorldDecision* g_nativeWorldDecision=nullptr;
inline bool live=true,requested=false,pending=true,resolve=true,ownerThread=true,coverage=false,reference=false;
inline bool nativeVrsPsMeasureLive(){return live;}
inline bool vrsCoverageEnabled(){return coverage;}
inline bool vrsWorkReferenceEnabled(){return reference;}
enum class BindSlot {Vs,Ps};
inline uint64_t bindingShaderHash(BindSlot slot){++counts.hashes;return slot==BindSlot::Vs?11:12;}
inline NativeVrsPsToken nativeVrsPsBefore(ID3D11DeviceContext* context,const NativeVrsPsDraw& draw){
    ++counts.calls;NativeVrsPsToken token{};
    if(context!=owner||!ownerThread)return token;
    ++counts.services;
    if(pending&&resolve)pending=false;
    if(!requested&&!pending)live=false;
    if(draw.eye>=2){++counts.empty;return token;}
    if(requested&&draw.target&&draw.depth){++counts.begins;token.active=true;}
    return token;
}
inline void nativeVrsPsAfter(ID3D11DeviceContext*,const NativeVrsPsToken&){++counts.ends;}
#include "../../src/d3d11/native_vrs_draw_scope.h"
inline void reset(){counts={};g_nativeWorldDecision=nullptr;live=true;requested=false;pending=true;resolve=true;ownerThread=true;coverage=reference=false;}
inline void issue(ID3D11DeviceContext* context){NativePsDrawScope scope(context,'D',3,1,0);}
inline void run(){
    reset();live=false;issue(owner);
    check(!counts.calls&&!counts.hashes,"unarmed production PS scope performs no service or binding read");
    NativeWorldDecision unknown{owner};
    reset();g_nativeWorldDecision=&unknown;issue(owner);
    check(counts.services==1&&counts.empty==1&&!pending&&!live&&!counts.hashes&&!counts.begins&&!counts.ends,
          "production Off/unknown decision drains through empty adapter draw without shader reads or new queries");
    reset();issue(owner);
    check(counts.services==1&&!pending&&!live&&!counts.hashes&&!counts.begins,
          "production scope also services pending queries when no world decision exists");
    reset();g_nativeWorldDecision=&unknown;issue(foreign);
    check(counts.calls==1&&!counts.services&&pending&&live&&!counts.hashes&&!counts.begins,
          "production foreign-context scope delegates owner refusal without metadata reads");
    reset();g_nativeWorldDecision=&unknown;ownerThread=false;issue(owner);
    check(counts.calls==1&&!counts.services&&pending&&live&&!counts.hashes&&!counts.begins,
          "production foreign-thread scope cannot retire or replace owner queries");
    NativeWorldDecision world{owner,0,reinterpret_cast<ID3D11RenderTargetView*>(uintptr_t(0x401)),
        reinterpret_cast<ID3D11DepthStencilView*>(uintptr_t(0x402))};
    reset();g_nativeWorldDecision=&world;coverage=true;issue(owner);
    check(counts.services==1&&counts.empty==1&&!pending&&!counts.hashes&&!counts.begins,
          "coverage exclusion still services pending PS queries without sampling");
    reset();g_nativeWorldDecision=&world;reference=true;issue(owner);
    check(counts.services==1&&counts.empty==1&&!pending&&!counts.hashes&&!counts.begins,
          "work-reference exclusion still services pending PS queries without sampling");
    reset();g_nativeWorldDecision=&unknown;resolve=false;issue(owner);
    check(counts.services==1&&pending&&live&&!counts.hashes&&!counts.begins,
          "unresolved owner query remains pending and live without starting another query");
    reset();g_nativeWorldDecision=&world;requested=true;pending=false;issue(owner);
    check(counts.services==1&&counts.hashes==2&&counts.begins==1&&counts.ends==1&&!counts.empty,
          "eligible production scope retains measurement before/after pairing");
}
} // namespace ps_draw_scope_fixture

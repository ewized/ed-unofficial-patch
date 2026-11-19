#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace edvr::native_vrs {
constexpr unsigned kTileSize=16;
constexpr unsigned kMaxWorldDimension=16384;
constexpr uint32_t kFullRate=5,kRate2x2=8,kRate4x4=11;
constexpr uint64_t kReceiptAgeMs=50;
enum class FrameDecision {New,Duplicate,Zero,Regressed,ForeignContext,ForeignThread};
inline FrameDecision ownershipDecision(uintptr_t owner,uintptr_t candidate,uint32_t ownerThread,uint32_t thread) noexcept {
    if(!owner||candidate!=owner)return FrameDecision::ForeignContext;
    return !ownerThread||thread!=ownerThread?FrameDecision::ForeignThread:FrameDecision::New;
}
inline FrameDecision ordinalDecision(uint64_t previous,uint64_t ordinal) noexcept {
    if(!ordinal)return FrameDecision::Zero;
    if(ordinal==previous)return FrameDecision::Duplicate;
    return ordinal<previous?FrameDecision::Regressed:FrameDecision::New;
}
inline FrameDecision frameDecision(uintptr_t owner,uintptr_t candidate,uint32_t ownerThread,
                                   uint32_t thread,uint64_t previous,uint64_t ordinal) noexcept {
    const auto ownership=ownershipDecision(owner,candidate,ownerThread,thread);
    return ownership==FrameDecision::New?ordinalDecision(previous,ordinal):ownership;
}
inline bool fullViewport(unsigned count,float x,float y,float width,float height,unsigned worldWidth,unsigned worldHeight) noexcept {
    return count==1&&x==0.f&&y==0.f&&width==float(worldWidth)&&height==float(worldHeight);
}

inline bool validRadii(float hq,float mid) noexcept {
    return std::isfinite(hq)&&std::isfinite(mid)&&hq>0.f&&hq<mid&&mid<=1.f;
}
struct FocusRadii {float sharp=0,transition=0;};
// Fractions of each eye's image dimensions: .04 is an 8%-wide full-rate core.
// Unknown selections refuse coarse shading rather than silently choosing one.
inline FocusRadii focusRadii(const char* profile) noexcept {
    if(!profile)return {};
    if(!std::strcmp(profile,"very_narrow"))return {.04f,.12f};
    if(!std::strcmp(profile,"narrow"))return {.08f,.24f};
    if(!std::strcmp(profile,"normal"))return {.15f,.45f};
    if(!std::strcmp(profile,"wide"))return {.25f,.60f};
    return {};
}
inline bool validPoint(float x,float y) noexcept {
    return std::isfinite(x)&&std::isfinite(y)&&x>=-1.f&&x<=1.f&&y>=-1.f&&y<=1.f;
}
// Same eye, parallel eye axes. Source NDC is tied to SteamVR's projection;
// the current game frustum includes native trim/scope narrowing and jitter.
inline bool projectGaze(float sourceX,float sourceY,const float source[4],
                        const float target[4],const float shift[2],float& x,float& y) noexcept {
    if(!source||!target||!shift||!validPoint(sourceX,sourceY))return false;
    for(unsigned i=0;i<4;++i)if(!std::isfinite(source[i])||!std::isfinite(target[i]))return false;
    if(source[0]<=0||source[1]<=0||target[0]>=target[1]||target[2]>=target[3]||
       !std::isfinite(shift[0])||!std::isfinite(shift[1]))return false;
    const double tx=(double(sourceX)+source[2])/source[0],ty=(double(sourceY)+source[3])/source[1];
    x=float((2*tx-target[0]-target[1]-2*shift[0])/(double(target[1])-target[0]));
    y=float((2*ty-target[2]-target[3]-2*shift[1])/(double(target[3])-target[2]));
    return validPoint(x,y); // outside a zoomed view is refused, never clamped
}
struct MapShape {unsigned width=0,height=0;size_t bytes=0;};
inline MapShape mapShape(unsigned worldWidth,unsigned worldHeight) noexcept {
    if(!worldWidth||!worldHeight||worldWidth>kMaxWorldDimension||worldHeight>kMaxWorldDimension)return {};
    const unsigned w=(worldWidth+kTileSize-1)/kTileSize,h=(worldHeight+kTileSize-1)/kTileSize;
    return {w,h,size_t(w)*h};
}
inline bool freshReceipt(uint64_t now,uint64_t receipt,uint64_t frequency) noexcept {
    // This is host receipt age, not an unavailable sensor timestamp.
    return now&&receipt&&frequency&&now>=receipt&&now-receipt<=frequency/20;
}
inline std::array<uint32_t,16> palette() noexcept {
    std::array<uint32_t,16> result{};
    result.fill(kFullRate);result[1]=kRate2x2;result[2]=kRate4x4;return result;
}

// Map bytes are palette indices, never NV_PIXEL_X0_CULL_RASTER_PIXELS.
// All unused palette entries also resolve to full shading. Partial edge tiles
// use the centre of their actual covered pixels, without clamping the gaze.
inline bool buildMap(uint8_t* output,size_t capacity,unsigned worldWidth,unsigned worldHeight,
                     float xNdc,float yNdc,bool usable,float hq,float mid) noexcept {
    const auto shape=mapShape(worldWidth,worldHeight);
    if(!output||!shape.bytes||capacity<shape.bytes)return false;
    for(size_t i=0;i<shape.bytes;++i)output[i]=0;
    if(!usable||!validPoint(xNdc,yNdc)||!validRadii(hq,mid))return false;
    const float gazeX=(xNdc+1.f)*.5f,gazeY=(1.f-yNdc)*.5f;
    const float invW=1.f/float(worldWidth),invH=1.f/float(worldHeight);
    const float hq2=hq*hq,mid2=mid*mid;
    for(unsigned y=0;y<shape.height;++y){
        const unsigned startY=y*kTileSize;
        const unsigned endY=(startY+kTileSize<worldHeight)?startY+kTileSize:worldHeight;
        const float dy=(float(startY)+float(endY))*.5f*invH-gazeY;
        for(unsigned x=0;x<shape.width;++x){
            const unsigned startX=x*kTileSize;
            const unsigned endX=(startX+kTileSize<worldWidth)?startX+kTileSize:worldWidth;
            const float dx=(float(startX)+float(endX))*.5f*invW-gazeX;
            const float radius2=dx*dx+dy*dy;
            output[size_t(y)*shape.width+x]=radius2<=hq2?0:radius2<=mid2?1:2;
        }
    }
    return true;
}

// Valid only while this adapter exclusively owns the vendor state on its one
// immediate context. Unknown command lists/ClearState/foreign ownership must
// invalidate it. An error never acknowledges a state transition.
struct VendorStateCache {
    bool ratesKnown=false,enabled=false,bindingKnown=false;
    uintptr_t binding=0;
    void invalidate() noexcept {ratesKnown=enabled=bindingKnown=false;binding=0;}
    bool fullRateKnown()const noexcept {return ratesKnown&&!enabled&&bindingKnown&&!binding;}
    bool active(uintptr_t view)const noexcept {return ratesKnown&&enabled&&bindingKnown&&view&&binding==view;}
    bool retainedForWitness(uintptr_t view)const noexcept {return ratesKnown&&!enabled&&bindingKnown&&view&&binding==view;}
    bool acceptRates(int result,bool on) noexcept {
        if(result){invalidate();return false;}ratesKnown=true;enabled=on;return true;
    }
    bool acceptResource(int result,uintptr_t view) noexcept {
        if(result){invalidate();return false;}bindingKnown=true;binding=view;return true;
    }
};
// NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_V1::numViewports == 0 globally
// disables VRS. Only the private witness may retain its current eye's SRRV.
template<class Rates>
bool pauseForWitness(VendorStateCache& cache,uintptr_t view,Rates&& rates) {
    return cache.active(view) && cache.acceptRates(rates(false),false);
}
template<class Rates,class Resource>
bool applyFullRate(VendorStateCache& cache,Rates&& rates,Resource&& resource) {
    bool ok=true;
    if(!cache.ratesKnown||cache.enabled)ok=cache.acceptRates(rates(false),false);
    if(!cache.bindingKnown||cache.binding)ok=cache.acceptResource(resource(0),0)&&ok;
    return ok&&cache.fullRateKnown();
}
template<class Rates,class Resource>
bool applyCoarse(VendorStateCache& cache,uintptr_t view,Rates&& rates,Resource&& resource) {
    if(!view)return false;
    if((!cache.bindingKnown||cache.binding!=view)&&!cache.acceptResource(resource(view),view))return false;
    if((!cache.ratesKnown||!cache.enabled)&&!cache.acceptRates(rates(true),true))return false;
    return cache.active(view);
}
} // namespace edvr::native_vrs

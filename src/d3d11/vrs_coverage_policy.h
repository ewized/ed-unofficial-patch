#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace edvr::vrs_coverage {
// CPU-only lifetime of one bounded asynchronous pipeline query. Cancellation
// rejects evidence, but cannot skip its matching End or GPU completion poll.
struct PipelineQueryProgress {
    bool begun=false,ended=false,settled=false,cancelled=false;
    bool begin() noexcept {if(begun||cancelled)return false;begun=true;return true;}
    bool end() noexcept {if(!begun||ended)return false;ended=true;return true;}
    bool pollable() const noexcept {return begun&&ended&&!settled;}
    bool pending() const noexcept {return begun&&!settled;}
    void cancel() noexcept {cancelled=true;}
    void complete(bool success=true) noexcept {if(pollable()){settled=true;if(!success)cancelled=true;}}
    bool evidence() const noexcept {return settled&&!cancelled;}
};
inline bool pipelineSampleDue(unsigned eye,bool nested,uint64_t now,uint64_t next) noexcept {
    return eye<2&&!nested&&now>=next; // Unknown control state can still yield diagnostic counts.
}
inline bool pipelineOffQuiescent(bool publishersAndDrawsIdle,unsigned pending) noexcept {
    return publishersAndDrawsIdle&&pending==0;
}
constexpr uint64_t kWorldVs=0xDE545DC8EE4FBB87ull;
constexpr uint64_t kWorldPs=0x60D69AB6C65C549Aull;
// E46E is the existing both-profile stock G-buffer material of this VS:
// engine_velocity_families.h and engine_velocity_test's real corpus pairs
// preserve original Target0..3/depth. It is also observed on hull windows.
// These IDs identify material families, never a particular cockpit object.
constexpr uint64_t kWorldPsE46=0xE46E3E4832B2FDB0ull;
inline bool knownWorldFamily(uint64_t vs,uint64_t ps) noexcept {return vs==kWorldVs&&(ps==kWorldPs||ps==kWorldPsE46);}

struct GateCounts {
    uint64_t knownVs=0,ps60=0,psE46=0,otherPs=0,owner=0,rtvEye=0,notUi=0,allowedClass=0,exactFamily=0;
    uint64_t depthLeft=0,depthRight=0,depthUnresolved=0,candidateBeforeAccepted=0,candidateIssued=0,familyDepthBeforeAccepted=0,familyDepthIssued=0;
    uint64_t beforeAccepted=0,contextRefused=0,deviceRefused=0,observerRefused=0,threadRefused=0;
    // Stage counts form a funnel through the unchanged predicates; shader
    // counts and candidate issue facts precede that funnel. No size inference.
    void note(uint64_t vs,uint64_t ps,bool isOwner,bool isEye,bool isNotUi,bool classAllowed,
              int depthEye,bool accepted,bool issued) noexcept {
        if(vs!=kWorldVs)return;
        ++knownVs;if(ps==kWorldPs)++ps60;else if(ps==kWorldPsE46)++psE46;else ++otherPs;
        if(accepted)++candidateBeforeAccepted;if(issued)++candidateIssued;
        if(!isOwner)return;++owner;
        if(!isEye)return;++rtvEye;
        if(!isNotUi)return;++notUi;
        if(!classAllowed)return;++allowedClass;
        if(!knownWorldFamily(vs,ps))return;++exactFamily;
        if(depthEye==0)++depthLeft;else if(depthEye==1)++depthRight;else {++depthUnresolved;return;}
        if(accepted)++familyDepthBeforeAccepted;
        if(accepted&&issued)++familyDepthIssued;
    }
};

// QueryInterface may resolve the public System32 stub into NVIDIA's separate
// DriverStore implementation. Only that exact loaded-owner topology is accepted;
// these lexical checks are not a signature verification or a DLL search path.
inline wchar_t pathFold(wchar_t c) noexcept {return c>=L'A'&&c<=L'Z'?wchar_t(c+(L'a'-L'A')):c;}
inline bool sameModulePath(std::wstring_view a,std::wstring_view b) noexcept {
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i)if(pathFold(a[i])!=pathFold(b[i]))return false;
    return true;
}
inline bool ordinaryModulePath(std::wstring_view path) noexcept {
    if(path.size()<4||!((pathFold(path[0])>=L'a')&&(pathFold(path[0])<=L'z'))||path[1]!=L':'||path[2]!=L'\\')return false;
    size_t start=3;
    for(size_t i=3;i<=path.size();++i){
        if(i==path.size()||path[i]==L'\\'){
            if(i==start||path[i-1]==L'.'||path[i-1]==L' ')return false;
            start=i+1;continue;
        }
        const auto c=path[i];
        if(c<32||c>126||c==L'/'||c==L':'||c==L'<'||c==L'>'||c==L'"'||c==L'|'||c==L'?'||c==L'*')return false;
    }
    return true;
}
inline bool nvapiSystemStubPath(std::wstring_view systemDirectory,std::wstring_view stub) noexcept {
    constexpr std::wstring_view leaf=L"\\nvapi64.dll";
    return ordinaryModulePath(systemDirectory)&&ordinaryModulePath(stub)&&stub.size()==systemDirectory.size()+leaf.size()&&
        sameModulePath(stub.substr(0,systemDirectory.size()),systemDirectory)&&sameModulePath(stub.substr(systemDirectory.size()),leaf);
}
// 0=refused, 1=both targets in the public stub, 2=both in the same recognized
// NVIDIA x64 DriverStore package. Mixed owners and arbitrary implementation DLLs
// remain refused. The caller obtains systemDirectory from GetSystemDirectoryW.
inline unsigned nvapiOwnerPathKind(std::wstring_view systemDirectory,std::wstring_view stub,
                                  std::wstring_view ratesOwner,std::wstring_view resourceOwner) noexcept {
    if(!nvapiSystemStubPath(systemDirectory,stub)||!ordinaryModulePath(ratesOwner)||!sameModulePath(ratesOwner,resourceOwner))return 0;
    if(sameModulePath(ratesOwner,stub))return 1;
    constexpr std::wstring_view prefix=L"\\driverstore\\filerepository\\",package=L"nv_dispi.inf_amd64_",leaf=L"\\nvapi64_impl.dll";
    const size_t begin=systemDirectory.size()+prefix.size();
    if(ratesOwner.size()!=begin+package.size()+16+leaf.size()||!sameModulePath(ratesOwner.substr(0,systemDirectory.size()),systemDirectory)||
       !sameModulePath(ratesOwner.substr(systemDirectory.size(),prefix.size()),prefix)||!sameModulePath(ratesOwner.substr(begin,package.size()),package)||
       !sameModulePath(ratesOwner.substr(begin+package.size()+16),leaf))return 0;
    for(size_t i=begin+package.size();i<begin+package.size()+16;++i){const auto c=pathFold(ratesOwner[i]);if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))return 0;}
    return 2;
}

// Caller serializes this CPU-only evidence. No setter result is assumed to
// apply until its original call returns NVAPI_OK. Nested/concurrent writes
// make their ordering unknown, including when their final counters look equal.
struct AcceptedState {
    uint64_t epoch=1,rateEpoch=0,bindingEpoch=0;
    unsigned writers=0;
    bool overlapping=false,ratesKnown=false,enabled=false,bindingKnown=false;
    uintptr_t binding=0;
    std::array<uint32_t,16> palette{};
    uint64_t enter() noexcept {
        if(writers++)overlapping=true;
        return ++epoch;
    }
    void invalidate() noexcept {
        ++epoch;ratesKnown=bindingKnown=enabled=false;binding=0;
    }
    bool finish(uint64_t token,int status) noexcept {
        const bool ordered=writers==1&&!overlapping&&token==epoch&&status==0;
        if(!ordered)invalidate();
        if(writers)--writers;
        if(!writers)overlapping=false;
        ++epoch;
        return ordered;
    }
    void rates(uint64_t token,int status,bool parsed,bool on,const std::array<uint32_t,16>& table) noexcept {
        if(!finish(token,status)||!parsed){ratesKnown=false;return;}
        ratesKnown=true;enabled=on;palette=table;rateEpoch=epoch;
    }
    void resource(uint64_t token,int status,uintptr_t view) noexcept {
        if(!finish(token,status))return;
        bindingKnown=true;binding=view;bindingEpoch=epoch;
    }
    bool active() const noexcept {return !writers&&ratesKnown&&enabled&&bindingKnown&&binding;}
};

struct Histogram {
    std::array<uint64_t,12> rates{};
    uint64_t tiles=0,coarse=0,hq=0;
    double hqX=0,hqY=0; // memory only; never log/export centroids
    bool invalid=false;
    void add(uint8_t index,const std::array<uint32_t,16>& palette,unsigned x,unsigned y) noexcept {
        if(index>=palette.size()||palette[index]>=rates.size()){invalid=true;return;}
        const auto rate=palette[index];++tiles;++rates[rate];
        if(rate>=6)++coarse;
        if(rate>=1&&rate<=5){++hq;hqX+=x+.5;hqY+=y+.5;}
    }
    bool nonuniform() const noexcept {
        unsigned used=0;for(auto n:rates)used+=n!=0;return !invalid&&used>1;
    }
    void finish() noexcept {if(hq){hqX/=double(hq);hqY/=double(hq);}}
};

struct DirectionCounts {
    uint64_t pairs=0,sameYAgree=0,sameYMismatch=0,flippedYAgree=0,flippedYMismatch=0;
    uint64_t xPairs=0,xAgree=0,xMismatch=0,yPairs=0,ySameAgree=0,yFlippedAgree=0;
    uint64_t noise=0;
    void add(double gx,double gy,double mx,double my,unsigned width,unsigned height) noexcept {
        if(!std::isfinite(gx)||!std::isfinite(gy)||!std::isfinite(mx)||!std::isfinite(my)||!width||!height){++noise;return;}
        const bool x=std::fabs(gx)>=1e-4&&std::fabs(mx)>=.25;
        const bool y=std::fabs(gy)>=1e-4&&std::fabs(my)>=.25;
        if(!x&&!y){++noise;return;}
        if(x){++xPairs;if(gx*mx>0)++xAgree;else ++xMismatch;}
        if(y){++yPairs;if(gy*my>0)++ySameAgree;else ++yFlippedAgree;}
        // Normalized displacement is only a directional hypothesis. It does
        // not assert calibrated PVR units, sensor timestamps or latency.
        mx/=double(width);my/=double(height);
        const double denominator=std::hypot(gx,gy)*std::hypot(mx,my);
        if(!(denominator>0)){++noise;return;}
        ++pairs;
        if((gx*mx+gy*my)/denominator>=.5)++sameYAgree;else ++sameYMismatch;
        if((gx*mx-gy*my)/denominator>=.5)++flippedYAgree;else ++flippedYMismatch;
    }
};
}

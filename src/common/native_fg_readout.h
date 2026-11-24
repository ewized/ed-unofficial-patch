#pragma once
#include "native_presentation.h"
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace edvr {
enum class NativeFgMeasureState {Unavailable,Measuring,Counting,Stale};
struct NativeFgReadout {
    bool visible=false,automatic=false,failed=false,backendUnavailable=false;
    uint32_t selectedMultiplier=0;
    NativeFgMeasureState measurement=NativeFgMeasureState::Unavailable;
    NativePresentationSnapshot totals{};
    double nativeFps=0,generatedFps=0,repeatedFps=0,submittedFps=0;
    const char* reason="";
};

// Render-thread history of receiver-stamped XR counters. Rates use observed
// successful stereo submissions, including explicit zero generation. A new
// session, counter regression or loading gap starts a baseline; duplicate UI
// reads never create samples. The getter only reads this bounded history.
class NativePresentationHistory {
    static constexpr unsigned kCapacity=256;
    struct Entry {uint64_t at=0,native=0,generated=0,repeated=0;};
    Entry entries_[kCapacity]{};
    unsigned head_=0,count_=0;
    NativePresentationSnapshot latest_{};
    static bool regressed(const NativePresentationSnapshot& a,const NativePresentationSnapshot& b) noexcept {
        return b.sequence<a.sequence||b.capturedAtMs<a.capturedAtMs||b.nativeFrames<a.nativeFrames||
            b.generatedFrames<a.generatedFrames||b.repeatedFrames<a.repeatedFrames||
            b.emptyFrames<a.emptyFrames||b.failedFrames<a.failedFrames||b.backgroundFrames<a.backgroundFrames;
    }
    const Entry& at(unsigned i)const noexcept{return entries_[(head_+kCapacity-count_+i)%kCapacity];}
public:
    void clear() noexcept {head_=count_=0;latest_={};}
    void observe(const NativePresentationSnapshot& sample,uint64_t now) noexcept {
        if(!sample.active||!sample.available){clear();latest_=sample;return;}
        if(!sample.generation||!sample.context||!sample.sequence||!sample.capturedAtMs||
           now<sample.capturedAtMs||now-sample.capturedAtMs>2000){head_=count_=0;latest_=sample;return;}
        const bool reset=!latest_.available||sample.generation!=latest_.generation||sample.context!=latest_.context||
            regressed(latest_,sample)||(latest_.capturedAtMs&&sample.capturedAtMs-latest_.capturedAtMs>2000);
        if(reset)head_=count_=0;
        else if(sample.sequence==latest_.sequence)return;
        latest_=sample;
        const Entry value{sample.capturedAtMs,sample.nativeFrames,sample.generatedFrames,sample.repeatedFrames};
        if(count_&&at(count_-1).at==value.at)entries_[(head_+kCapacity-1)%kCapacity]=value;
        else {entries_[head_]=value;head_=(head_+1)%kCapacity;if(count_<kCapacity)++count_;}
    }
    NativeFgReadout read(uint64_t now,uint64_t windowMs=1000)const noexcept {
        NativeFgReadout out;out.visible=true;out.totals=latest_;
        if(!latest_.active||!latest_.available)return out;
        if(!latest_.capturedAtMs||now<latest_.capturedAtMs||now-latest_.capturedAtMs>2000){
            out.measurement=NativeFgMeasureState::Stale;return out;
        }
        out.measurement=NativeFgMeasureState::Measuring;
        if(count_<2)return out;
        const Entry& last=at(count_-1);
        unsigned first=count_-1;
        while(first&&last.at-at(first).at<windowMs)--first;
        const Entry& start=at(first);
        const uint64_t elapsed=last.at-start.at;
        if(elapsed<200)return out;
        const double scale=1000./double(elapsed);
        out.nativeFps=double(last.native-start.native)*scale;
        out.generatedFps=double(last.generated-start.generated)*scale;
        out.repeatedFps=double(last.repeated-start.repeated)*scale;
        out.submittedFps=out.nativeFps+out.generatedFps+out.repeatedFps;
        out.measurement=NativeFgMeasureState::Counting;return out;
    }
};

inline const char* nativeFgMeasureWord(NativeFgMeasureState state) noexcept {
    switch(state){
    case NativeFgMeasureState::Unavailable:return "unavailable";
    case NativeFgMeasureState::Measuring:return "measuring";
    case NativeFgMeasureState::Counting:return "counting";
    case NativeFgMeasureState::Stale:return "stale";
    }return "unavailable";
}
// The runtime's current pump selection and observed generated count are kept
// distinct. A warmed backend does not imply that a generated frame was used.
enum class NativeFgModeKind {Native,Active,Waiting,Failed,Unavailable};
struct NativeFgModeSelection {NativeFgModeKind kind;uint32_t multiplier=0;};
inline NativeFgModeSelection nativeFgModeSelection(const NativeFgReadout& r) noexcept {
    if(r.failed)return {NativeFgModeKind::Failed};
    if(r.backendUnavailable)return {NativeFgModeKind::Unavailable};
    const bool fresh=r.measurement==NativeFgMeasureState::Counting||r.measurement==NativeFgMeasureState::Measuring;
    if(fresh&&r.totals.fgEnabled&&nativeFgFixedMultiplier(r.totals.activeMultiplier))
        return {NativeFgModeKind::Active,r.totals.activeMultiplier};
    if(r.selectedMultiplier)return {NativeFgModeKind::Waiting,r.selectedMultiplier};
    return {NativeFgModeKind::Native};
}
inline void formatNativeFgMode(const NativeFgReadout& r,char* text,size_t size) {
    if(!text||!size)return;
    const auto mode=nativeFgModeSelection(r);
    const char* prefix=r.automatic?"Auto: ":"";
    switch(mode.kind){
    case NativeFgModeKind::Failed:std::snprintf(text,size,"%sfailed",prefix);break;
    case NativeFgModeKind::Unavailable:std::snprintf(text,size,"%sunavailable",prefix);break;
    case NativeFgModeKind::Active:std::snprintf(text,size,"%s%ux",prefix,mode.multiplier);break;
    case NativeFgModeKind::Waiting:std::snprintf(text,size,"%s%ux waiting",prefix,mode.multiplier);break;
    case NativeFgModeKind::Native:std::snprintf(text,size,"%s",r.automatic?"Auto: native":"Off");break;
    }
}
// A tile's large face has room for a short value. Full mode and decision text
// stay in the wrapping Monitor note instead of ellipsizing the big number.
inline void formatNativeFgTileValue(const NativeFgReadout& r,char* text,size_t size) {
    if(!text||!size)return;
    const auto mode=nativeFgModeSelection(r);
    switch(mode.kind){
    case NativeFgModeKind::Failed:std::snprintf(text,size,"--");break;
    case NativeFgModeKind::Unavailable:std::snprintf(text,size,"--");break;
    case NativeFgModeKind::Active:std::snprintf(text,size,"%ux",mode.multiplier);break;
    case NativeFgModeKind::Waiting:std::snprintf(text,size,"--");break;
    case NativeFgModeKind::Native:std::snprintf(text,size,"%s",r.automatic?"1x":"Off");break;
    }
}

inline void formatNativeFgRates(const NativeFgReadout& r,char* text,size_t size,bool compact=false) {
    if(!text||!size)return;
    if(r.measurement!=NativeFgMeasureState::Counting){
        std::snprintf(text,size,"FG counts %s",nativeFgMeasureWord(r.measurement));return;
    }
    if(compact) {
        char repeats[32]="";
        if(r.repeatedFps>.05)std::snprintf(repeats,sizeof(repeats)," + %.0f repeat",r.repeatedFps);
        std::snprintf(text,size,"%.0f submit fps (%.0f native + %.0f gen%s)",r.submittedFps,r.nativeFps,r.generatedFps,repeats);
    }
    else std::snprintf(text,size,"native %.1f + gen %.1f + repeat %.1f fps",r.nativeFps,r.generatedFps,r.repeatedFps);
}
inline void formatNativeFgTotals(const NativeFgReadout& r,char* text,size_t size) {
    if(!text||!size)return;
    char mode[40];formatNativeFgMode(r,mode,sizeof(mode));
    if(r.totals.available)std::snprintf(text,size,"%s | gen total %llu | repeat total %llu",mode,
        (unsigned long long)r.totals.generatedFrames,(unsigned long long)r.totals.repeatedFrames);
    else std::snprintf(text,size,"%s | counts unavailable",mode);
}
inline void formatNativeFgCompact(const NativeFgReadout& r,char* text,size_t size,double renderFps=-1.) {
    if(!text||!size)return;
    char rates[96],mode[40];formatNativeFgRates(r,rates,sizeof(rates),true);formatNativeFgMode(r,mode,sizeof(mode));
    if(r.measurement!=NativeFgMeasureState::Counting&&renderFps>=0.)
        std::snprintf(rates,sizeof(rates),"render %.0f fps | gen %s",renderFps,nativeFgMeasureWord(r.measurement));
    if(r.totals.available)std::snprintf(text,size,"%s | %s | gen total %llu",rates,mode,(unsigned long long)r.totals.generatedFrames);
    else std::snprintf(text,size,"%s | %s",rates,mode);
}
// Limit the compact readout to 95 characters for readability. Keep all frame
// metrics intact; timings remain on the detailed card when the line is full.
constexpr size_t kNativeFgCompactMaxChars=95;
inline void formatNativeFgOverlay(const NativeFgReadout& r,double renderFps,const char* timings,char* text,size_t size) {
    if(!text||!size)return;
    char line[192];formatNativeFgCompact(r,line,sizeof(line),renderFps);
    if(std::strlen(line)>kNativeFgCompactMaxChars) {
        char mode[40];formatNativeFgMode(r,mode,sizeof(mode));
        if(r.totals.available)std::snprintf(line,sizeof(line),"%s | gen total %llu | rates unavailable",mode,(unsigned long long)r.totals.generatedFrames);
        else std::snprintf(line,sizeof(line),"%s | counts unavailable",mode);
    }
    const bool fits=timings&&std::strlen(line)+std::strlen(timings)<=kNativeFgCompactMaxChars;
    std::snprintf(text,size,"%s%s",line,fits?timings:"");
}

}

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
enum class NativeFgModeLabel {Detailed,Compact};
inline void formatNativeFgMode(const NativeFgReadout& r,char* text,size_t size,
    NativeFgModeLabel label=NativeFgModeLabel::Detailed) {
    if(!text||!size)return;
    const auto mode=nativeFgModeSelection(r);
    const bool compact=label==NativeFgModeLabel::Compact;
    const char* autoPrefix=compact?"auto: ":"Auto: ";
    const char* prefix=r.automatic?autoPrefix:"";
    const char* off=compact?"off":"Off";
    switch(mode.kind){
    case NativeFgModeKind::Failed:std::snprintf(text,size,"%sfailed",prefix);break;
    case NativeFgModeKind::Unavailable:std::snprintf(text,size,"%sunavailable",prefix);break;
    case NativeFgModeKind::Active:
        if(compact)std::snprintf(text,size,"x%u",mode.multiplier);
        else std::snprintf(text,size,"%s%ux",prefix,mode.multiplier);
        break;
    case NativeFgModeKind::Waiting:std::snprintf(text,size,compact?"%sx%u waiting":"%s%ux waiting",prefix,mode.multiplier);break;
    case NativeFgModeKind::Native:std::snprintf(text,size,"%s%s",prefix,r.automatic?"native":off);break;
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

inline void formatNativeFgRates(const NativeFgReadout& r,char* text,size_t size) {
    if(!text||!size)return;
    if(r.measurement!=NativeFgMeasureState::Counting){
        std::snprintf(text,size,"FG counts %s",nativeFgMeasureWord(r.measurement));return;
    }
    std::snprintf(text,size,"native %.1f + gen %.1f + repeat %.1f fps",r.nativeFps,r.generatedFps,r.repeatedFps);
}
inline void formatNativeFgTotals(const NativeFgReadout& r,char* text,size_t size) {
    if(!text||!size)return;
    char mode[40];formatNativeFgMode(r,mode,sizeof(mode));
    if(r.totals.available)std::snprintf(text,size,"%s | gen total %llu | repeat total %llu",mode,
        (unsigned long long)r.totals.generatedFrames,(unsigned long long)r.totals.repeatedFrames);
    else std::snprintf(text,size,"%s | counts unavailable",mode);
}
// Use successful native and generated submissions for the compact rate.
// Without measured counts, use rendered FPS if available. Return true when
// the line includes the active multiplier and measured breakdown.
inline bool formatNativeFgCompact(const NativeFgReadout& r,char* text,size_t size,double renderFps=-1.) {
    if(!text||!size)return false;
    const bool counting=r.measurement==NativeFgMeasureState::Counting;
    const double fps=counting?r.nativeFps+r.generatedFps:renderFps;
    const auto mode=nativeFgModeSelection(r);
    if(counting&&mode.kind==NativeFgModeKind::Active){
        std::snprintf(text,size,"%.0f fps | %.0f native + %.0f gen | x%u",
            fps,r.nativeFps,r.generatedFps,mode.multiplier);
        return true;
    }
    char label[40];formatNativeFgMode(r,label,sizeof(label),NativeFgModeLabel::Compact);
    if(fps>=0.)std::snprintf(text,size,"%.0f fps | %s",fps,label);
    else std::snprintf(text,size,"fps %s | %s",nativeFgMeasureWord(r.measurement),label);
    return false;
}
// Include timings on the fallback line only when all its fields fit.
constexpr size_t kNativeFgCompactMaxChars=95;
inline void formatNativeFgOverlay(const NativeFgReadout& r,double renderFps,const char* timings,char* text,size_t size) {
    if(!text||!size)return;
    char line[kNativeFgCompactMaxChars+1];
    const bool breakdown=formatNativeFgCompact(r,line,sizeof(line),renderFps);
    const bool includeTimings=!breakdown&&timings&&std::strlen(line)+std::strlen(timings)<=kNativeFgCompactMaxChars;
    std::snprintf(text,size,"%s%s",line,includeTimings?timings:"");
}

}

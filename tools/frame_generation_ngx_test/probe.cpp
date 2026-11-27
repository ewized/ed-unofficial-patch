#include "../../src/d3d11/frame_generation_ngx.h"
#include "../../src/common/native_fg_multiplier.h"
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace edvr;
namespace {
bool waitFence(ID3D12Fence* fence,uint64_t value,DWORD limit){
    if(!fence||!value)return true;const auto done=fence->GetCompletedValue();
    if(done==UINT64_MAX)return false;if(done>=value)return true;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return false;
    if(FAILED(fence->SetEventOnCompletion(value,event))){CloseHandle(event);return false;}
    if(WaitForSingleObject(event,limit)!=WAIT_OBJECT_0){
        // The fence still owns a pending event registration. Keep this handle
        // alive with the quarantined GPU owner instead of closing it early.
        return false;
    }
    CloseHandle(event);return fence->GetCompletedValue()!=UINT64_MAX&&fence->GetCompletedValue()>=value;
}
struct ProducerHold {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> rtvs;ComPtr<ID3D12Resource> readback;std::shared_ptr<void> inputs,consumerLease;
};
struct ProducerGuard {
    ProducerHold hold;uint64_t value=0;bool submitted=false,unfenced=false;
    ~ProducerGuard(){
        if(submitted&&(unfenced||!waitFence(hold.fence.Get(),value,5000))){
            auto* retained=new ProducerHold(std::move(hold));(void)retained;
            std::puts("producer quarantine: unfinished/unfenced GPU resources retained until process exit");
        }
    }
};
void printInitTrace(const FrameGenerationNgxStatus& status,bool initialized){
    std::printf("init %s failure=%u ngx=0x%08x hr=0x%08lx available=%u maxGenerated=%u method=%s suppression_layout=%s stage=%s eye=%u failedStage=%s failedEye=%u\n",
        initialized?"submitted":"refused",unsigned(status.failure),status.lastNgxResult,static_cast<unsigned long>(status.lastHresult),status.available,status.maximumGenerated,
        frameGenerationNgxCreationModeName(status.creationMode),frameGenerationNgxSuppressionLayoutName(status.suppressionLayout),frameGenerationNgxInitStageName(status.initStage),status.initEye,frameGenerationNgxInitStageName(status.failedInitStage),status.failedInitEye);
    for(unsigned eye=0;eye<2;++eye){const auto& call=status.creation[eye];
        std::printf("init_call eye=%u scratch_called=%u scratch_result=0x%08x scratch_bytes=%llu create_called=%u create_result=0x%08x handle_returned=%u\n",
            eye,call.scratchCalled,call.scratchResult,static_cast<unsigned long long>(call.scratchBytes),call.createCalled,call.createResult,call.handleReturned);
    }
}
void printSuppression(const FrameGenerationNgxTicket& ticket){
    FrameGenerationNgxSuppressionSnapshot snapshot;
    if(!ticket.suppressionSnapshot(snapshot)){std::puts("suppression_raw fenced=0");return;}
    for(unsigned e=0;e<2;++e)for(unsigned i=0;i<snapshot.generatedCount;++i){
        const auto* b=snapshot.raw[e][i];const bool sentinel=b[0]==0xff&&b[1]==0xff&&b[2]==0xff&&b[3]==0xff;
        std::printf("suppression_raw fenced=1 native=%llu eye=%u index=%u resource=%p bytes=%02x,%02x,%02x,%02x classification=%s reset_reasons=0x%02x eval_result=0x%08x layout=%s\n",
            static_cast<unsigned long long>(snapshot.sequence),e,i+1,reinterpret_cast<void*>(snapshot.resourceIdentity[e][i]),b[0],b[1],b[2],b[3],
            b[0]==0?"zero":sentinel?"sentinel_pattern":"nonzero",snapshot.historyResetReasons,snapshot.evaluationResult[e][i],frameGenerationNgxSuppressionLayoutName(snapshot.layout));
    }
}
void barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&v);
}
bool inverse(const float (&m)[4][4],float (&out)[4][4]){
    double a[4][8]{};for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){a[y][x]=m[y][x];a[y][x+4]=x==y?1.:0.;}
    for(unsigned c=0;c<4;++c){unsigned pivot=c;for(unsigned y=c+1;y<4;++y)if(std::abs(a[y][c])>std::abs(a[pivot][c]))pivot=y;
        if(std::abs(a[pivot][c])<1e-12)return false;for(unsigned x=0;x<8;++x)std::swap(a[c][x],a[pivot][x]);
        const double scale=a[c][c];for(auto& value:a[c])value/=scale;
        for(unsigned y=0;y<4;++y)if(y!=c){const double factor=a[y][c];for(unsigned x=0;x<8;++x)a[y][x]-=factor*a[c][x];}}
    for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)out[y][x]=float(a[y][x+4]);return true;
}
// The foreground moves in both directions without wrapping at the screen edge.
// Each eye has a small disparity and a different constant background colour so
// a swapped eye cannot pass just by placing its bar at the right fraction.
int barLeft(unsigned width,unsigned frame,unsigned eye){
    const int step=std::clamp(int(width/32),16,48);
    const int tick=int((frame-1)%12);
    return int(width/4)+(eye==0?2:-2)+step*(tick<=6?tick:12-tick);
}

enum class HudFixture { None, Alpha, Opaque, Neutral, Control };
const char* hudFixtureName(HudFixture fixture) {
    switch (fixture) {
    case HudFixture::Alpha: return "alpha";
    case HudFixture::Opaque: return "opaque";
    case HudFixture::Neutral: return "neutral";
    case HudFixture::Control: return "control";
    default: return "none";
    }
}
bool hasHud(HudFixture fixture, unsigned eye) {
    return fixture != HudFixture::None && !(fixture == HudFixture::Neutral && eye == 1);
}
int hudLeft(unsigned width, unsigned frame, unsigned eye) {
    // The panel travels the opposite direction to the world edge. Its
    // translucent interior always contains at least one world edge.
    const int travel = barLeft(width, frame, eye) - barLeft(width, 1, eye);
    return int(width * 5 / 16) + (eye == 0 ? -2 : 2) - travel * 3 / 4;
}

struct EdgeMeasurement {
    int left=-1,right=-1,spread=0;
    bool eyeMatches=false;
};

template<class Score>
EdgeMeasurement measureEdges(int width, Score score, int low, int middle, int high) {
    EdgeMeasurement out;
    const auto first = [&](int threshold, bool reverse) {
        int run = 0;
        for (int i = 0; i < width; ++i) {
            const int x = reverse ? width - 1 - i : i;
            run = score(x) >= threshold ? run + 1 : 0;
            if (run == 3) return reverse ? x + 3 : x - 2; // right is exclusive
        }
        return -1;
    };
    const int lowLeft = first(low, false), highLeft = first(high, false);
    const int lowRight = first(low, true), highRight = first(high, true);
    out.left = first(middle, false);
    out.right = first(middle, true);
    if (lowLeft < 0 || highLeft < 0 || lowRight < 0 || highRight < 0) {
        out.left = out.right = -1;
        return out;
    }
    out.spread = (std::max)(highLeft - lowLeft, lowRight - highRight);
    return out;
}

// Read the green foreground silhouette on one scanline. Two contrast levels
// measure edge spread, distinguishing translation from an endpoint crossfade.
EdgeMeasurement measureBar(const uint8_t* row,int width,unsigned eye){
    auto out = measureEdges(width, [&](int x) { return row[4*x + 1]; }, 64, 120, 170);
    out.eyeMatches = out.left >= 0;
    for(int x:{width/16,width-width/16}){
        const auto* sample=row+x*4;
        const int tint=int(sample[eye==0?0:2])-int(sample[eye==0?2:0]);
        out.eyeMatches=out.eyeMatches&&tint>=20&&sample[1]<64;
    }
    return out;
}

int hudScore(const uint8_t* pixel) {
    return int(pixel[0]) + int(pixel[2]) - 2 * int(pixel[1]);
}
EdgeMeasurement measureHud(const uint8_t* row, int width, unsigned eye) {
    auto out = measureEdges(width, [&](int x) { return hudScore(row + 4*x); }, 90, 125, 150);
    if (out.left < 0) return out;
    const auto* center = row + 4 * ((out.left + out.right) / 2);
    out.eyeMatches = int(center[eye == 0 ? 0 : 2]) - int(center[eye == 0 ? 2 : 0]) >= 18;
    return out;
}

bool followsEdge(int edge, int oldEdge, int previous, int current, unsigned index, unsigned multiplier) {
    const int direction = current > previous ? 1 : -1;
    const double substep = std::abs(current - previous) / double(multiplier);
    const double expected = previous + (current - previous) * double(index) / multiplier;
    const double tolerance = (std::max)(2.0, substep * .5);
    return (edge - previous) * direction >= 2 && (current - edge) * direction >= 2 &&
           (edge - oldEdge) * direction >= 2 && std::abs(edge - expected) <= tolerance;
}

// The fixture measures image motion, independently of NGX's success code.
// Allow deviations of half an inter-phase step. Also require movement away
// from both native endpoints and forward progress from the previous phase.
bool followsMotion(const EdgeMeasurement& measured,const EdgeMeasurement& prior,
                   int previous,int current,int barWidth,unsigned index,unsigned multiplier){
    const double substep=std::abs(current-previous)/double(multiplier);
    return measured.eyeMatches&&measured.spread<=substep*.75+1&&
        followsEdge(measured.left,prior.left,previous,current,index,multiplier)&&
        followsEdge(measured.right-barWidth,prior.right-barWidth,previous,current,index,multiplier);
}

struct VisibleWorldEdge {
    int expected = -1, measured = -1;
    bool matches = false;
};
// The green channel still carries the world edge beneath the magenta panel.
// Look for a single transition away from the panel's own boundaries.
VisibleWorldEdge measureWorldThroughHud(const uint8_t* row, const EdgeMeasurement& hud,
    int previous, int current, int width, unsigned index, unsigned multiplier) {
    if (!hud.eyeMatches) return {};
    const int center = int(std::lround(previous + (current - previous) * double(index) / multiplier));
    const int radius = (std::max)(3, int(std::ceil(std::abs(current - previous) / (2.0 * multiplier))));
    VisibleWorldEdge observation{};
    for (int offset : {0, width}) {
        const int expected = center + offset;
        if (expected - radius - 3 < hud.left || expected + radius + 3 >= hud.right) continue;
        const auto bright = [&](int x) { return row[4*x + 1] >= 60; };
        int crossings = 0, measured = -1;
        for (int x = expected - radius; x <= expected + radius; ++x) {
            if (bright(x - 1) == bright(x)) continue;
            ++crossings;
            if (bright(x) == (offset == 0) && bright(x - 3) == bright(x - 1) && bright(x + 2) == bright(x))
                measured = x;
        }
        if (crossings == 1 && measured >= 0 &&
            followsEdge(measured - offset, previous, previous, current, index, multiplier))
            return {expected, measured, true};
        observation = {expected, measured, false};
    }
    return observation;
}

using ProbeColor = std::array<float, 4>;
ProbeColor worldBackground(unsigned eye) {
    return {eye == 0 ? 66.f/255.f : 10.f/255.f, 20.f/255.f, eye == 0 ? 10.f/255.f : 66.f/255.f, 1.f};
}
constexpr ProbeColor worldBar{217.f/255.f, 217.f/255.f, 166.f/255.f, 1.f};
ProbeColor translucentHud(unsigned eye) {
    return {eye == 0 ? 125.f/255.f : 63.f/255.f, 0.f, eye == 0 ? 63.f/255.f : 125.f/255.f, 128.f/255.f};
}
ProbeColor blendHud(ProbeColor world, const ProbeColor& hud) {
    for (unsigned c = 0; c < 3; ++c) world[c] = hud[c] + world[c] * (1.f - hud[3]);
    return world;
}

// Exercise the image check on the default CPU-only build gate. A valid
// halfway image must pass while either endpoint, a swapped eye, a repeated
// intermediate and a blend of the two endpoints must fail.
bool edgeCheckSelfTest(){
    static constexpr int width=256,barWidth=64,previous=64,current=96;
    const auto rowAt=[](int left,unsigned eye){
        std::array<uint8_t,width*4> row{};
        for(int x=0;x<width;++x){
            const bool foreground=x>=left&&x<left+barWidth;
            row[x*4+0]=eye==0?70:10;row[x*4+2]=eye==0?10:70;
            row[x*4+1]=foreground?217:20;row[x*4+3]=255;
        }
        return row;
    };
    const auto old=rowAt(previous,0),next=rowAt(current,0),middle=rowAt((previous+current)/2,0);
    const auto swapped=rowAt((previous+current)/2,1);
    auto blend=old;
    for(size_t i=0;i<blend.size();++i)blend[i]=uint8_t((unsigned(old[i])+next[i])/2);
    const auto native=measureBar(old.data(),width,0);
    const auto match=[&](const auto& row,const EdgeMeasurement& prior){
        return followsMotion(measureBar(row.data(),width,0),prior,previous,current,barWidth,1,2);
    };
    return match(middle,native)&&!match(old,native)&&!match(next,native)&&!match(swapped,native)&&
        !match(blend,native)&&!match(middle,measureBar(middle.data(),width,0));
}

bool hudFixtureSelfTest() {
    for (unsigned width : {256u, 1280u, 3560u}) for (unsigned eye = 0; eye < 2; ++eye) {
        const int worldWidth = int(width / 4), panelWidth = int(width * 2 / 3);
        const auto paint = [&](int world, int panel, unsigned paintedEye) {
            std::vector<uint8_t> row(width * 4);
            for (int x = 0; x < int(width); ++x) {
                auto pixel = x >= world && x < world + worldWidth ? worldBar : worldBackground(paintedEye);
                if (x >= panel && x < panel + panelWidth) pixel = blendHud(pixel, translucentHud(paintedEye));
                for (unsigned c = 0; c < 4; ++c) row[4*x + c] = uint8_t(std::lround(255.f * pixel[c]));
            }
            return row;
        };
        for (unsigned frame = 2; frame <= 13; ++frame) {
            const int worldA = barLeft(width, frame - 1, eye), worldB = barLeft(width, frame, eye);
            const int hudA = hudLeft(width, frame - 1, eye), hudB = hudLeft(width, frame, eye);
            const int middleWorld = int(std::lround((worldA + worldB) * .5));
            const int middleHud = int(std::lround((hudA + hudB) * .5));
            const EdgeMeasurement oldHud{hudA, hudA + panelWidth, 0, true};
            const auto checkHud = [&](const std::vector<uint8_t>& row) {
                auto hud = measureHud(row.data(), int(width), eye);
                return followsMotion(hud, oldHud, hudA, hudB, panelWidth, 1, 2);
            };
            const auto good = paint(middleWorld, middleHud, eye);
            const auto frozenWorld = paint(worldB, middleHud, eye);
            const auto hudAtEndpoint = paint(middleWorld, hudB, eye);
            const auto swappedHud = paint(middleWorld, middleHud, 1 - eye);
            const auto neutral = paint(middleWorld, int(width), eye);
            const auto hud = measureHud(good.data(), int(width), eye);
            if (!checkHud(good) || checkHud(hudAtEndpoint) || checkHud(swappedHud) ||
                measureHud(neutral.data(), int(width), eye).left >= 0 ||
                !measureWorldThroughHud(good.data(), hud, worldA, worldB, worldWidth, 1, 2).matches ||
                measureWorldThroughHud(frozenWorld.data(), hud, worldA, worldB, worldWidth, 1, 2).matches)
                return false;
        }
    }
    return true;
}

// Composite, HUD-less, and UI all use the same eye-sized SDR grid. In opaque
// coverage mode, a covered UI texel includes the exact B background, as ED-UP
// currently does for alpha/multiply. The native color is identical either way.
void drawColorInputs(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE handle,
    unsigned plane, unsigned width, unsigned height, unsigned frame, unsigned eye, HudFixture fixture) {
    const int left = barLeft(width, frame, eye);
    const D3D12_RECT world{left, LONG(height / 4), left + LONG(width / 4), LONG(3 * height / 4)};
    const auto background = worldBackground(eye);
    constexpr ProbeColor transparent{};
    const bool ui = plane == 4;
    const auto& clear = ui ? transparent : background;
    list->ClearRenderTargetView(handle, clear.data(), 0, nullptr);
    if (!ui) list->ClearRenderTargetView(handle, worldBar.data(), 1, &world);
    if (plane == 3 || !hasHud(fixture, eye)) return;

    const int panelLeft = hudLeft(width, frame, eye);
    const D3D12_RECT panel{panelLeft, LONG(3 * height / 8), panelLeft + LONG(width * 2 / 3), LONG(7 * height / 8)};
    const auto hud = translucentHud(eye);
    if (ui && fixture != HudFixture::Opaque) {
        list->ClearRenderTargetView(handle, hud.data(), 1, &panel);
        return;
    }
    const auto coveredBackground = blendHud(background, hud);
    const auto coveredWorld = blendHud(worldBar, hud);
    list->ClearRenderTargetView(handle, coveredBackground.data(), 1, &panel);
    const D3D12_RECT overlap{
        (std::max)(panel.left, world.left), (std::max)(panel.top, world.top),
        (std::min)(panel.right, world.right), (std::min)(panel.bottom, world.bottom),
    };
    if (overlap.left < overlap.right && overlap.top < overlap.bottom)
        list->ClearRenderTargetView(handle, coveredWorld.data(), 1, &overlap);
}

void drawInputs(ID3D12GraphicsCommandList* list,D3D12_CPU_DESCRIPTOR_HANDLE handle,
                unsigned plane,unsigned width,unsigned height,unsigned guideWidth,unsigned guideHeight,
                unsigned frame,unsigned eye,HudFixture fixture,float distantMotion){
    if (plane == 0 || plane >= 3) {
        drawColorInputs(list, handle, plane, width, height, frame, eye, fixture);
        return;
    }
    const int left=barLeft(width,frame,eye);
    const auto scaleX=[&](int x){return LONG(std::lround(x*double(guideWidth)/width));};
    const int previous=barLeft(width,frame>1?frame-1:frame,eye);
    const float background[2][4]={{.02f,0,0,0},{distantMotion,0,0,0}};
    const float foreground[2][4]={{.25f,0,0,0},{float(scaleX(previous)-scaleX(left)),0,0,0}};
    const int right=left+int(width/4);
    const LONG h=LONG(guideHeight);
    const D3D12_RECT rect{scaleX(left),h/4,scaleX(right),3*h/4};
    list->ClearRenderTargetView(handle,background[plane - 1],0,nullptr);
    list->ClearRenderTargetView(handle,foreground[plane - 1],1,&rect);
}

FrameGenerationCamera camera(unsigned w,unsigned h,unsigned eye){
    FrameGenerationCamera c{};c.cameraNear=.1f;c.cameraFar=1000.f;c.cameraFOV=1.2f;c.cameraAspectRatio=float(w)/float(h);
    const float sy=1.f/std::tan(c.cameraFOV*.5f),sx=sy/c.cameraAspectRatio;
    c.cameraViewToClip[0][0]=sx;c.cameraViewToClip[1][1]=sy;
    c.cameraViewToClip[2][2]=c.cameraNear/(c.cameraFar-c.cameraNear);
    c.cameraViewToClip[2][3]=-1;c.cameraViewToClip[3][2]=c.cameraNear*c.cameraFar/(c.cameraFar-c.cameraNear);
    inverse(c.cameraViewToClip,c.clipToCameraView);
    for(unsigned i=0;i<4;++i)c.clipToLensClip[i][i]=c.clipToPrevClip[i][i]=c.prevClipToClip[i][i]=1;
    // Displace the fixed eye so its near rectangle really shifts by the two
    // pixels in barLeft(). Its 0.25 reversed depth unprojects at this distance.
    const float nearObjectDistance=c.cameraViewToClip[3][2]/(.25f+c.cameraViewToClip[2][2]);
    const float eyeOffset=4.f*nearObjectDistance/(float(h)*sy);
    c.cameraPos[0]=eye==0?-eyeOffset:eyeOffset;c.cameraUp[1]=c.cameraRight[0]=1;c.cameraFwd[2]=-1;
    // drawInputs writes current-to-previous motion in guide pixels. The DLSS-G
    // Programming Guide specifies unit scale for pixel-valued inputs.
    c.mvecScale[0]=c.mvecScale[1]=1.f;c.depthInverted=true;
    c.motionVectorsInvalidValue=65504.f;c.cameraMotionIncluded=true;return c;
}

struct ProbeMotion {
    FrameGenerationCamera camera;
    float xDelta = 0.f, distantMotion = 0.f;
};

// Move two distinct eyes together. The scene guide is current-to-previous
// with the scene's depth. A HUD draw can still move independently of it.
ProbeMotion probeMotion(unsigned width, unsigned height, unsigned guideWidth,
    unsigned frame, unsigned eye, HudFixture fixture) {
    ProbeMotion out{camera(width, height, eye)};
    out.camera.reset = frame == 1;
    if (fixture == HudFixture::None) return out;

    auto& c = out.camera;
    const float nearDistance = c.cameraViewToClip[3][2] / (.25f + c.cameraViewToClip[2][2]);
    const float distantDistance = c.cameraViewToClip[3][2] / (.02f + c.cameraViewToClip[2][2]);
    const float pixelDistance = 2.f * nearDistance / (float(width) * c.cameraViewToClip[0][0]);
    const auto eyeShift = [&](unsigned at) {
        return float(barLeft(width, at, eye) - barLeft(width, 1, eye)) * pixelDistance * .125f;
    };
    const float shifted = eyeShift(frame);
    out.xDelta = shifted - eyeShift(frame > 1 ? frame - 1 : frame);
    c.cameraPos[0] += shifted;
    // For row vectors, clipToPrevClip = inverse(P) * shiftX(delta) * P.
    // The inverse uses the opposite translation and the same projection.
    for (unsigned row = 0; row < 4; ++row) for (unsigned col = 0; col < 4; ++col) {
        const float delta = c.clipToCameraView[row][3] * out.xDelta * c.cameraViewToClip[0][col];
        c.clipToPrevClip[row][col] += delta;
        c.prevClipToClip[row][col] -= delta;
    }
    out.distantMotion = out.xDelta * c.cameraViewToClip[0][0] * float(guideWidth) / (2.f * distantDistance);
    return out;
}

bool texture(ID3D12Device* device,unsigned w,unsigned h,DXGI_FORMAT format,ComPtr<ID3D12Resource>& out){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=d.MipLevels=1;
    d.Format=format;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)));
}
}
int wmain(int argc,wchar_t** argv){
    bool run=false,officialHelper=false,diagnosticSuppressed=false,sharedSuppression=false;
    HudFixture hudFixture = HudFixture::None;
    unsigned multiplier=2,width=1280,height=720,guideWidth=0,guideHeight=0,nativeFrames=12,warmupFrames=4;std::wstring feature,data;
    for(int i=1;i<argc;++i){
        if(wcscmp(argv[i],L"--run")==0)run=true;
        else if(wcscmp(argv[i],L"--official-fg-helper")==0)officialHelper=true;
        else if(wcscmp(argv[i],L"--diagnostic-suppressed-output")==0)diagnosticSuppressed=true;
        else if(wcscmp(argv[i],L"--shared-native-pair-suppression")==0)sharedSuppression=true;
        else if(i+1<argc&&wcscmp(argv[i],L"--multiplier")==0)multiplier=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--feature-dir")==0)feature=argv[++i];
        else if(i+1<argc&&wcscmp(argv[i],L"--data-dir")==0)data=argv[++i];
        else if(i+1<argc&&wcscmp(argv[i],L"--width")==0)width=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--height")==0)height=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--guide-width")==0)guideWidth=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--guide-height")==0)guideHeight=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--native-frames")==0)nativeFrames=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc&&wcscmp(argv[i],L"--warmup-frames")==0)warmupFrames=unsigned(_wtoi(argv[++i]));
        else if(i+1<argc && wcscmp(argv[i], L"--ui-recomposition") == 0 && hudFixture == HudFixture::None) {
            const std::wstring mode(argv[++i]);
            if (mode == L"alpha") hudFixture = HudFixture::Alpha;
            else if (mode == L"opaque") hudFixture = HudFixture::Opaque;
            else if (mode == L"neutral") hudFixture = HudFixture::Neutral;
            else if (mode == L"control") hudFixture = HudFixture::Control;
            else { std::puts("expected --ui-recomposition alpha|opaque|neutral|control"); return 2; }
        }
        else {std::puts("usage: probe [--run --multiplier 2|3|4 --feature-dir ABS --data-dir ABS [--official-fg-helper] [--shared-native-pair-suppression] [--width W --height H --guide-width GW --guide-height GH] [--native-frames N --warmup-frames N] [--diagnostic-suppressed-output] [--ui-recomposition alpha|opaque|neutral|control]]");return 2;}
    }
    if(!guideWidth)guideWidth=width;if(!guideHeight)guideHeight=height;
    if(nativeFrames<2||nativeFrames>64||warmupFrames>=nativeFrames||width<256||height<64||
       guideWidth<64||guideHeight<32||width>16384||height>16384||guideWidth>16384||guideHeight>16384){
        std::puts("moving-edge probe needs 2..64 native frames, warmup smaller than frame count, output at least 256x64 and guides at least 64x32 (maximum 16384 per dimension)");
        return 2;
    }
    const bool hudFixtureActive = hudFixture != HudFixture::None;
    const bool uirEnabled = hudFixtureActive && hudFixture != HudFixture::Control;
    if (hudFixtureActive && multiplier != 2) {
        std::puts("UI recomposition probe measures one 2x midpoint per pair");
        return 2;
    }
    if (!edgeCheckSelfTest() || !hudFixtureSelfTest()) { std::puts("moving-edge probe image self-test failed"); return 10; }
    if(!run){std::printf("DRY: moving-edge image self-test passed; method=%s layout=%s output=%ux%u guides=%ux%u nativeFrames=%u warmupFrames=%u suppressedReadback=%u ui_fixture=%s uir_enabled=%u no DXGI, device, NGX, GPU, game or XR calls. Explicit --run is required.\n",officialHelper?"official_fg_helper":"generic_scratch_query",sharedSuppression?"shared_native_pair":"private_per_index",width,height,guideWidth,guideHeight,nativeFrames,warmupFrames,diagnosticSuppressed,hudFixtureName(hudFixture),unsigned(uirEnabled));return 0;}
#ifndef EDVR_HAVE_NGX
    std::puts("REFUSED: this build has no NGX/DLSS SDK support; no GPU or driver calls were made.");return 3;
#endif
    if(!nativeFgFixedMultiplier(multiplier)||feature.empty()||data.empty()||!width||!height){std::puts("invalid explicit-run configuration");return 2;}
    // Everything below this line is an explicit GPU probe. No WARP/shader FG
    // fallback, game attachment, XR session, priority or system setting change.
    ComPtr<IDXGIFactory4> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 3;
    ComPtr<ID3D12Device> device;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 d{};adapter->GetDesc1(&d);if(d.VendorId==0x10de&&!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))break;}
    if(!device){std::puts("no NVIDIA D3D12 hardware device");return 3;}
    FrameGenerationNgx backend;FrameGenerationNgxInit init;
    init.device=device.Get();init.width=width;init.height=height;init.guideWidth=guideWidth;init.guideHeight=guideHeight;init.multiplier=multiplier;
    init.creationMode=officialHelper?FrameGenerationNgxCreationMode::OfficialFgHelper:FrameGenerationNgxCreationMode::GenericScratchQuery;
    init.suppressionLayout=sharedSuppression?FrameGenerationNgxSuppressionLayout::SharedNativePair:FrameGenerationNgxSuppressionLayout::PrivatePerIndex;
    init.uiRecomposition = uirEnabled;
    init.featureDirectory=feature.c_str();init.dataDirectory=data.c_str();init.privateNgxDevice=true;init.timeoutMs=5000;
    const bool initialized=backend.initialize(init);printInitTrace(backend.status(),initialized);
    if(!initialized){const bool drained=backend.drain(5000);std::printf("init_cleanup drain=%u quarantined=%u\n",drained,backend.status().quarantined);return 4;}
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;q.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    ComPtr<ID3D12Fence> producer;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    if(FAILED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)))||FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&producer)))||
       FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))||FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))))return 5;
    list->Close();
    const unsigned planes = uirEnabled ? 5u : 3u;
    const unsigned scanlines = hudFixtureActive ? 2u : 1u;
    ComPtr<ID3D12DescriptorHeap> rtvs;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2 * planes;
    if(FAILED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtvs))))return 5;
    const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    struct Inputs {ComPtr<ID3D12Resource> resources[2][5];};auto resources=std::make_shared<Inputs>();
    const DXGI_FORMAT formats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM};
    for (unsigned e = 0; e < 2; ++e) for (unsigned k = 0; k < planes; ++k) {
        const bool guide = k == 1 || k == 2;
        if (!texture(device.Get(), guide ? guideWidth : width, guide ? guideHeight : height,
                     formats[k], resources->resources[e][k])) return 5;
        auto handle = rtvs->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += SIZE_T(e * planes + k) * stride;
        device->CreateRenderTargetView(resources->resources[e][k].Get(), nullptr, handle);
    }
    ComPtr<ID3D12Resource> outputReadback;
    D3D12_HEAP_PROPERTIES readHeap{};readHeap.Type=D3D12_HEAP_TYPE_READBACK;readHeap.CreationNodeMask=readHeap.VisibleNodeMask=1;
    const UINT rowPitch=(width*4+D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-1u)&~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT-1u);
    const UINT64 rowStride=(rowPitch+D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1u)&~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT-1u);
    const SIZE_T readbackBytes=SIZE_T(6*scanlines*rowStride);
    const auto rowOffset = [&](unsigned eye, unsigned phase, unsigned scanline) {
        return ((eye * 3 + phase) * scanlines + scanline) * rowStride;
    };
    D3D12_RESOURCE_DESC readDesc{};readDesc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;readDesc.Width=readbackBytes;readDesc.Height=1;
    readDesc.DepthOrArraySize=readDesc.MipLevels=1;readDesc.SampleDesc.Count=1;readDesc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(device->CreateCommittedResource(&readHeap,D3D12_HEAP_FLAG_NONE,&readDesc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&outputReadback))))return 5;
    ProducerGuard producerGuard;producerGuard.hold={device,queue,producer,allocator,list,rtvs,outputReadback,resources};
    unsigned readyBatches=0,postWarmupReady=0,suppressedBatches=0,suppressedReadbacks=0,eligiblePhases=0;uint64_t producerValue=0;
    unsigned hudPhases = 0, throughPhases = 0, neutralPhases = 0, translatedPhases = 0;
    for(uint64_t frame=1;frame<=nativeFrames;++frame){
        if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator.Get(),nullptr)))return 6;
        ProbeMotion views[2] = {
            probeMotion(width, height, guideWidth, unsigned(frame), 0, hudFixture),
            probeMotion(width, height, guideWidth, unsigned(frame), 1, hudFixture),
        };
        for (unsigned e = 0; e < 2; ++e) for (unsigned k = 0; k < planes; ++k) {
            auto* resource = resources->resources[e][k].Get();
            barrier(list.Get(), resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto handle = rtvs->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += SIZE_T(e * planes + k) * stride;
            drawInputs(list.Get(), handle, k, width, height, guideWidth, guideHeight, unsigned(frame), e,
                hudFixture, views[e].distantMotion);
            barrier(list.Get(), resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
        }
        if(FAILED(list->Close()))return 6;ID3D12CommandList* commands[]={list.Get()};
        producerGuard.submitted=producerGuard.unfenced=true;queue->ExecuteCommandLists(1,commands);
        if(FAILED(queue->Signal(producer.Get(),++producerValue)))return 6;
        producerGuard.value=producerValue;producerGuard.unfenced=false;
        FrameGenerationNgxInput input;input.sequence=frame;input.historyGeneration=1;input.pairedFullEyeGuides=true;input.producerFence=producer.Get();input.producerValue=producerValue;input.inputLease=resources;
        for (unsigned e = 0; e < 2; ++e) {
            auto& eye = input.eyes[e];
            eye.color = resources->resources[e][0].Get();
            eye.depth = resources->resources[e][1].Get();
            eye.motion = resources->resources[e][2].Get();
            eye.hudless = resources->resources[e][3].Get();
            eye.ui = resources->resources[e][4].Get();
            eye.camera = views[e].camera;
        }
        auto ticket=backend.submit(input);auto completion=ticket.poll();
        while(completion==FrameGenerationNgxCompletion::Pending){Sleep(1);completion=ticket.poll();}
        const auto s=backend.status();std::printf("native=%llu requested=%ux indexedFrames=%u evalAccepted=%u completion=%u evaluations=%llu failure=%u ngx=0x%08x warmup=%u reset_reasons=0x%02x\n",
            static_cast<unsigned long long>(frame),multiplier,ticket.generatedCount,ticket.evaluationSucceeded,unsigned(completion),static_cast<unsigned long long>(s.evaluations),unsigned(s.failure),s.lastNgxResult,frame<=warmupFrames,ticket.historyResetReasons);
        printSuppression(ticket);
        const bool suppressedInspection=diagnosticSuppressed&&completion==FrameGenerationNgxCompletion::Suppressed;
        if(completion==FrameGenerationNgxCompletion::Ready||suppressedInspection){
            producerGuard.hold.consumerLease=ticket.lease;
            if(FAILED(allocator->Reset())||FAILED(list->Reset(allocator.Get(),nullptr)))return 9;
            for(unsigned e=0;e<2;++e)for(unsigned i=0;i<ticket.generatedCount;++i){
                auto* r=ticket.generated[e][i];barrier(list.Get(),r,ticket.outputState,D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=r;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                for (unsigned row = 0; row < scanlines; ++row) {
                    const unsigned y = !hudFixtureActive ? height / 2 : row == 0 ? 5 * height / 16 : 5 * height / 8;
                    D3D12_TEXTURE_COPY_LOCATION dst{};
                    dst.pResource = outputReadback.Get();
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint.Offset = rowOffset(e, i, row);
                    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, width, 1, 1, rowPitch};
                    const D3D12_BOX box{0, y, 0, width, y + 1, 1};
                    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                }
                barrier(list.Get(),r,D3D12_RESOURCE_STATE_COPY_SOURCE,ticket.outputState);
            }
            if(FAILED(list->Close()))return 9;ID3D12CommandList* copies[]={list.Get()};producerGuard.unfenced=true;
            queue->ExecuteCommandLists(1,copies);if(FAILED(queue->Signal(producer.Get(),++producerValue)))return 9;
            producerGuard.value=producerValue;producerGuard.unfenced=false;
            if(!waitFence(producer.Get(),producerValue,5000))return 9;
            producerGuard.hold.consumerLease.reset();
            void* mapped=nullptr;const D3D12_RANGE reads{0,readbackBytes};
            if(FAILED(outputReadback->Map(0,&reads,&mapped))||!mapped)return 9;
            const bool eligible=!suppressedInspection&&frame>1&&frame>warmupFrames;
            bool matches=ticket.generatedCount==multiplier-1;
            for(unsigned e=0;e<2;++e){
                const int previous=barLeft(width,unsigned(frame>1?frame-1:frame),e);
                const int current=barLeft(width,unsigned(frame),e);
                const int barWidth=int(width/4);
                EdgeMeasurement prior{previous,previous+barWidth,0,true};
                const auto* samples = static_cast<const uint8_t*>(mapped);
                for(unsigned i=0;i<ticket.generatedCount;++i){
                    const auto* row = samples + rowOffset(e, i, 0);
                    const auto measured=measureBar(row,int(width),e);
                    const bool pass=followsMotion(measured,prior,previous,current,barWidth,i+1,multiplier);
                    std::printf("edge frame=%llu eye=%u index=%u nativeLeft=%d,%d expectedLeft=%.2f measured=[%d,%d) spread=%d eyeMatch=%u eligible=%u match=%u\n",
                        static_cast<unsigned long long>(frame),e,i+1,previous,current,
                        previous+(current-previous)*double(i+1)/multiplier,measured.left,measured.right,
                        measured.spread,unsigned(measured.eyeMatches),unsigned(eligible),unsigned(pass));
                    if(eligible){matches=matches&&pass;++eligiblePhases;}
                    prior=measured;
                    if (!hudFixtureActive) continue;

                    const auto* hudRow = samples + rowOffset(e, i, 1);
                    const auto hud = measureHud(hudRow, int(width), e);
                    const bool translated = views[e].xDelta != 0.f;
                    if (eligible && translated) ++translatedPhases;
                    if (hasHud(hudFixture, e)) {
                        const int panelA = hudLeft(width, unsigned(frame > 1 ? frame - 1 : frame), e);
                        const int panelB = hudLeft(width, unsigned(frame), e);
                        const int panelWidth = int(width * 2 / 3);
                        const EdgeMeasurement priorHud{panelA, panelA + panelWidth, 0, true};
                        const bool hudPass = followsMotion(hud, priorHud, panelA, panelB, panelWidth, i + 1, multiplier);
                        const auto through = measureWorldThroughHud(hudRow, hud, previous, current, barWidth, i + 1, multiplier);
                        std::printf("hud_edge frame=%llu eye=%u index=%u x_delta_m=%.8g nativeLeft=%d,%d expectedLeft=%.2f measured=[%d,%d) spread=%d eyeMatch=%u throughExpected=%d throughMeasured=%d throughMatch=%u eligible=%u match=%u\n",
                            static_cast<unsigned long long>(frame), e, i + 1, double(views[e].xDelta), panelA, panelB,
                            panelA + (panelB - panelA) * double(i + 1) / multiplier, hud.left, hud.right, hud.spread,
                            unsigned(hud.eyeMatches), through.expected, through.measured, unsigned(through.matches),
                            unsigned(eligible), unsigned(hudPass));
                        if (eligible) {
                            matches = matches && translated && hudPass && through.matches;
                            if (hudPass) ++hudPhases;
                            if (through.matches) ++throughPhases;
                        }
                    } else {
                        bool clear = true;
                        for (unsigned x = 0; x < width; ++x) clear = clear && hudScore(hudRow + 4*x) < 90;
                        const EdgeMeasurement start{previous, previous + barWidth, 0, true};
                        const bool neutral = clear && followsMotion(measureBar(hudRow, int(width), e), start,
                            previous, current, barWidth, i + 1, multiplier);
                        std::printf("hud_neutral frame=%llu eye=%u index=%u x_delta_m=%.8g eligible=%u match=%u\n",
                            static_cast<unsigned long long>(frame), e, i + 1, double(views[e].xDelta),
                            unsigned(eligible), unsigned(neutral));
                        if (eligible) {
                            matches = matches && translated && neutral;
                            if (neutral) ++neutralPhases;
                        }
                    }
                }
            }
            const D3D12_RANGE noWrites{0,0};outputReadback->Unmap(0,&noWrites);
            if(suppressedInspection)++suppressedReadbacks;
            else {
                if(!matches){std::puts("moving-edge phase count, stereo identity, shape or ordered scene/HUD motion mismatch");return 10;}
                ++readyBatches;if(eligible)++postWarmupReady;
            }
        }
        if(completion==FrameGenerationNgxCompletion::Suppressed)++suppressedBatches;
        if(completion!=FrameGenerationNgxCompletion::Ready&&completion!=FrameGenerationNgxCompletion::Suppressed){backend.drain(5000);return 7;}
        // Backend completion also proves that these producer input resources
        // are restored to COMMON; the next native source may overwrite them.
        ticket={};
    }
    const bool drained=backend.drain(5000);
    std::printf("probe result readyStereoBatches=%u postWarmupReady=%u warmupFrames=%u nativeFrames=%u suppressedNativeOnly=%u suppressedDiagnosticReadbacks=%u eligibleEyePhases=%u drain=%u ui_fixture=%s uir_enabled=%u hudEyePhases=%u hudThroughEyePhases=%u neutralEyePhases=%u translatedEyePhases=%u outputPixels=moving-edge-scanline GPUtiming=not-measured game=0 OpenXR=0 presentation=0; VR qualification remains separate\n",
        readyBatches,postWarmupReady,warmupFrames,nativeFrames,suppressedBatches,suppressedReadbacks,eligiblePhases,drained,
        hudFixtureName(hudFixture),unsigned(uirEnabled),hudPhases,throughPhases,neutralPhases,translatedPhases);
    const bool hudProof = !hudFixtureActive || (hudPhases && hudPhases == throughPhases &&
        hudPhases + neutralPhases == eligiblePhases && translatedPhases == eligiblePhases);
    return postWarmupReady && eligiblePhases && drained && hudProof ? 0 : 8;
}

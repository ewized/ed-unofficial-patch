#include "../../src/common/native_input_scale.h"
#include "../../src/d3d11/dlss_floor.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <limits>

namespace ni=edvr::native_input;
static std::string read(const std::string& file){std::ifstream in(file,std::ios::binary);std::ostringstream out;out<<in.rdbuf();return out.str();}
static bool has(const std::string& s,const char* needle){return s.find(needle)!=std::string::npos;}
static std::string without(std::string s,const char* needle){const auto p=s.find(needle);if(p!=std::string::npos)s.erase(p,std::strlen(needle));return s;}
int main(int argc,char** argv){
 if(argc==2&&std::string(argv[1])=="--dry-run"){std::puts("CPU-only input policy/latch/source wiring; no runtime/GPU/input calls");return 0;}
 if(argc!=2)return 2;
 unsigned checks=0;bool good=true;const auto check=[&](bool v,const char* why){++checks;if(!v){good=false;std::printf("FAIL %s\n",why);}};
 const auto closeFloat=[](float a,float b){return std::fabs(a-b)<1e-6f;};
 check(ni::choose(1,false,0,false,false,false)==ni::Policy::Default,"default1 needs no HMD/bridge/trim lookup");
 for(float v:{ni::kMinimum,.5f,.58f,.666667f,1.f})check(ni::validScale(v),"supported startup scales");
 for(float v:{.25f,1.1f,0.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})check(!ni::validScale(v),"bad scale refused");
 check(ni::choose(.5f,false,1,true,true,true)==ni::Policy::RequiresHmdOne,"unknown HMD refused");
 check(ni::choose(.5f,true,.5f,true,true,true)==ni::Policy::RequiresHmdOne,"nonunit HMD refused");
 check(ni::choose(.5f,true,1,true,true,true,.5f)==ni::Policy::RequiresSsaaOne,"saved nonunit SSAA refused");
 check(ni::choose(.5f,true,1,false,true,true)==ni::Policy::TrimConfigured,"trim refused");
 check(ni::choose(.5f,true,1,true,false,true)==ni::Policy::RequiresDlss,"nonDLSS startup refused");
 check(ni::choose(.5f,true,1,true,true,false)==ni::Policy::PairUnavailable,"missing paired capability refused");
 check(ni::choose(.5f,true,1,true,true,true)==ni::Policy::Active,"valid paired policy accepted");
 check(ni::recommendation(3200,1)==3200&&ni::recommendation(3893,1)==3893,"default recommendation exactly unchanged");
 check(ni::recommendation(3200,ni::kMinimum)==1067&&ni::recommendation(3200,.5f)==1600,"real lower recommendations preserve odd NGX input");
 check(ni::validScale(.333333f)&&ni::recommendation(3200,.333333f)==1067,"serialized one-third remains valid with exact odd ask");
 check(ni::recommendation(3200,.666667f)==2133&&ni::recommendation(3200,.58f)==1856,"quality/balanced asks are measured dimensions, not mode labels");
 check(closeFloat(ni::mipBias(.5f,1),-1)&&closeFloat(ni::mipBias(1,ni::kMinimum),std::log2(ni::kMinimum)),"startup mip ratio includes accepted input scale");
 ni::StartupLatch first;
 check(first.capture(ni::kMinimum,ni::Policy::Active)&&ni::validSettings(first.info),"actual startup latch accepts paired scale");
 check(ni::recommendation(3200,first.info.activeScale)==1067&&closeFloat(first.info.activeScale,1067.0f/3200.0f)&&
       closeFloat(ni::mipBias(1,first.info.activeScale),std::log2(1067.0f/3200.0f)),"mip uses actual pixel-rounded3200 input ratio");
 edvr::DlssModeRange modes[edvr::kDlssModeCount]{};
 for(int k=0;k<3;++k){auto& m=modes[k];m.ok=true;m.minW=m.minH=1600;m.maxW=m.maxH=3200;m.optW=m.optH=k==0?2133:k==1?1856:1600;}
 auto& ultra=modes[3];ultra.ok=true;ultra.minW=ultra.minH=ultra.maxW=ultra.maxH=ultra.optW=ultra.optH=1067;
 uint32_t servedW=0,servedH=0;
 check(edvr::dlssFloorOutput(modes,3200,3200,1067,1067,&servedW,&servedH)&&servedW==3200&&servedH==3200,
       "actual singleton1067 keeps DLSS3200 output without a spatial tail");
 check(edvr::dlssFloorOutput(modes,3200,3200,1068,1068,&servedW,&servedH)&&servedW==2136&&servedH==2136,
       "control: old even1068 reproduces real served-floor2136 cut");
 const auto ask=ni::recommendation(3200,first.info.activeScale);const auto bias=ni::mipBias(1,first.info.activeScale);
 check(!first.capture(1,ni::Policy::Default)&&first.info.requestedScale==ni::kMinimum&&
       ni::recommendation(3200,first.info.activeScale)==ask&&ni::mipBias(1,first.info.activeScale)==bias,
       "INI/menu change and fresh samplers cannot change accepted ask/bias until restart");
 ni::StartupLatch restarted;restarted.capture(.5f,ni::Policy::Active);
 check(ni::recommendation(3200,restarted.info.activeScale)==1600&&closeFloat(ni::mipBias(1,restarted.info.activeScale),-1),"fresh process applies new request");
 ni::StartupLatch refused;refused.capture(ni::kMinimum,ni::Policy::PairUnavailable);
 check(refused.info.requestedScale==ni::kMinimum&&refused.info.activeScale==1&&ni::validSettings(refused.info),"well-formed refusal keeps working unscaled VR and explicit reason");
 ni::StartupLatch unchanged;unchanged.capture(1,ni::Policy::Default);
 check(!unchanged.capture(.5f,ni::Policy::Active)&&unchanged.info.activeScale==1,"default session also cannot recapture midrun");
 auto invalid=first.info;invalid.reserved=1;check(!ni::validSettings(invalid),"reserved protocol bits refused");
 invalid=first.info;invalid.version=2;check(!ni::validSettings(invalid),"wrong version refused");
 invalid=first.info;invalid.activeScale=ni::kMinimum;check(!ni::validSettings(invalid),"unrounded half-pair mip protocol refused");
 invalid=first.info;invalid.policy=100;check(!ni::validSettings(invalid),"unknown policy refused");
 const std::string root=argv[1],host=read(root+"/src/openxr/native_runtime_host.h"),gfx=read(root+"/src/d3d11/device_hook.cpp"),
  provider=read(root+"/src/d3d11/native_input_scale.cpp"),module=read(root+"/src/openxr/native_module.cpp");
 const auto wired=[](const std::string& h){return has(h,"geometry.recommend(gameAskWidth,gameAskHeight,gameInputScale!=1.0f)")&&
  has(h,"geometry.begin(metadata,gameInputScale!=1.0f)")&&
  has(h,"temporal.begin(treatedGeometry,")&&has(h,"(std::max)(gameAskWidth[0],gameAskWidth[1])")&&
  has(h,"gameAskWidth[eye]=edvr::native_input::recommendation(gameGeometry.width[eye],gameInputScale)")&&
  has(h,"metadata.recommendedWidth[eye]=edvr::native_input::recommendation(sizes[eye].recommendedImageRectWidth,gameInputScale)")&&
  has(h,"sizing.activeWidth[eye]=sizes[eye].recommendedImageRectWidth;")&&
  has(h,"timingFrame.outputWidth[eye]=sizes[eye].recommendedImageRectWidth;")&&
  has(h,"native_input_scale,refused=trim_changed_requires_restart");};
 check(wired(host),"both startup/live game asks scaled; temporal/XR/output geometry preserved; trim gate present");
 check(!wired(without(host,"geometry.recommend(gameAskWidth,gameAskHeight,gameInputScale!=1.0f)")),"control: unscaled live ask fails");
 check(!wired(without(host,"geometry.begin(metadata,gameInputScale!=1.0f)")),"control: rounded bootstrap ask fails");
 check(!wired(without(host,"temporal.begin(treatedGeometry,")),"control: wrong temporal output route fails");
 check(!wired(without(host,"sizing.activeWidth[eye]=sizes[eye].recommendedImageRectWidth;")),"control: altered UI/output sizing fails");
 check(has(gfx,"nativeInputScaleCapture(sentinelCfg,known,hmd,ssaa)")&&has(gfx,"inputScale==1.0f ? log2f(mult) : log2f(mult*inputScale)"),"pre-sampler latch and exact default auto-bias branch wired");
 check(has(host,"fallback=unchanged")&&has(host,"accepted_scale_requires_3200_square_output")&&
       host.find("accepted_scale_requires_3200_square_output")<host.find("renderSettingsCaptured=true;",host.find("gameInputScale=input.activeScale;")),
       "valid prerequisite refusal falls back and accepted output mismatch is guarded before adoption");
 check(has(provider,"if (startup.captured) return;")&&has(provider,"needsPair ? pairedBridge() : false")&&
       has(provider,"edvrNativeInputScaleCapability")&&has(provider,"LoadLibraryExW(bridgePath.c_str()"),"actual provider latches once and verifies explicit paired module before accepting nondefault");
 check(has(module,"uint32_t WINAPI edvrNativeInputScaleCapability(uint32_t version)")&&has(module,"version==EDVR_NATIVE_INPUT_SCALE_CAPABILITY_EXACT_ASK?EDVR_NATIVE_INPUT_SCALE_CAPABILITY_EXACT_ASK:0u"),"paired capability is a pure constant exact-input version response");
 std::printf("%s native_input_scale_checks=%u runtime_calls=0 gpu_calls=0 input_calls=0\n",good?"PASS":"FAIL",checks);return good?0:1;
}

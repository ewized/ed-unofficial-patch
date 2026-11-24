// Exercise the same scene classifier, shader receipts and uncounted dispatch as the draw hooks.
#include "../../src/d3d11/native_vrs_draw_policy.h"
#include "../../src/d3d11/native_vrs_material.h"
#include "../../src/d3d11/native_vrs_replay.h"
#include "../../src/d3d11/flat_replay_query_tracker.h"
#include "witness_pixel_reference.h"
#include "fg_withhold_cases.h"
#include "../../src/d3d11/nvapi_module_path.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
using namespace edvr::native_vrs;
unsigned checks=0;bool good=true;
void check(bool value,const char* label){++checks;if(!value){good=false;std::printf("FAIL: %s\n",label);}}
static void nvapiModulePaths(){
 using namespace edvr::nvapi;
 const std::wstring system=L"C:\\WINDOWS\\system32",stub=system+L"\\nvapi64.dll";
 const std::wstring package=system+L"\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_81cce6c273d405a6",impl=package+L"\\nvapi64_impl.dll";
 check(nvapiOwnerPathKind(system,stub,stub,stub)==1,"legacy public-stub ownership remains supported");
 check(nvapiOwnerPathKind(system,stub,impl,impl)==2,"both targets in one exact NVIDIA DriverStore implementation supported");
 check(nvapiOwnerPathKind(L"c:\\windows\\SYSTEM32",L"c:\\windows\\System32\\NVAPI64.DLL",impl,impl)==2,"module topology comparison is case insensitive");
 check(nvapiOwnerPathKind(system,stub,stub,impl)==0,"mixed stub/implementation owners refused");
 check(nvapiOwnerPathKind(system,stub,impl,system+L"\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_0000000000000000\\nvapi64_impl.dll")==0,"different NVIDIA package owners refused");
 check(nvapiOwnerPathKind(system,L"C:\\game\\nvapi64.dll",impl,impl)==0,"app-local public stub refused");
 check(nvapiOwnerPathKind(system,stub,L"C:\\game\\nvapi64_impl.dll",L"C:\\game\\nvapi64_impl.dll")==0,"app-local implementation refused");
 check(nvapiOwnerPathKind(system,stub,system+L"\\nvapi64_impl.dll",system+L"\\nvapi64_impl.dll")==0,"implementation outside DriverStore refused");
 check(nvapiOwnerPathKind(system,stub,package+L"\\extra\\nvapi64_impl.dll",package+L"\\extra\\nvapi64_impl.dll")==0,"nested implementation path refused");
 check(nvapiOwnerPathKind(system,stub,package+L"\\..\\nvapi64_impl.dll",package+L"\\..\\nvapi64_impl.dll")==0,"traversal refused");
 check(nvapiOwnerPathKind(system,stub,package+L"\\other.dll",package+L"\\other.dll")==0,"unrelated DLL in NVIDIA package refused");
 auto invalidPackage=impl;invalidPackage.replace(invalidPackage.find(L"nv_dispi.inf_amd64_"),std::wstring(L"nv_dispi.inf_amd64_").size(),L"otherdrv.inf_amd64_");
 check(nvapiOwnerPathKind(system,stub,invalidPackage,invalidPackage)==0,"unrecognized vendor package refused");
 invalidPackage=impl;invalidPackage[invalidPackage.find(L"81cce6")]=L'g';
 check(nvapiOwnerPathKind(system,stub,invalidPackage,invalidPackage)==0,"nonhex package suffix refused");
 check(nvapiOwnerPathKind(system,stub,impl+L":stream",impl+L":stream")==0,"alternate stream refused");
 check(nvapiOwnerPathKind(system,stub,L"\\\\?\\"+impl,L"\\\\?\\"+impl)==0,"alternate path namespace refused");
 check(nvapiOwnerPathKind(system,stub,L"D"+impl.substr(1),L"D"+impl.substr(1))==0,"alternate drive root refused");
 check(nvapiOwnerPathKind(system,stub,L"",L"")==0,"missing owner paths refused");
 check(!nvapiSystemStubPath(system,system+L"\\NVAPI64.dll ")&&!nvapiSystemStubPath(system+L"\\..",stub),"Win32 trailing-space and root traversal ambiguity refused");
}
namespace uncounted {
struct ID3D11DeviceContext {};
struct {ID3D11DeviceContext* ownerCtx;} state;
auto* g_state=&state;
bool nativeVrsLive(){return true;}
unsigned classifications=0,scopes=0,preparations=0;
int nativeSceneMaterialDecision(ID3D11DeviceContext*,bool,bool,char,unsigned,unsigned){++classifications;return 0;}
template<class Draw> bool nativeWorldIssue(int,Draw draw){++scopes;return draw();}
void nativeBeforeOriginal(ID3D11DeviceContext*){++preparations;}
template<class Draw> bool nativeSplitOriginal(ID3D11DeviceContext*,edvr::native_vrs::WitnessDraw args,Draw){
    check(!args.valid(),"uncounted draws cannot provide retained direct arguments");return false;
}
#include "../../src/d3d11/native_vrs_uncounted_issue.h"
void test(){
    ID3D11DeviceContext owner{},deferred{};state.ownerCtx=&owner;
    unsigned draws=0;
    for(char kind:{'Z','Y','A'})nativeUncountedMaterialIssue(&deferred,kind,[&]{++draws;});
    check(draws==3&&!classifications&&!scopes&&!preparations,"deferred indirect and auto recording issue once without entering either VRS mutation boundary");
    nativeUncountedMaterialIssue(&owner,'Z',[&]{++draws;});
    check(draws==4&&classifications==1&&scopes==1&&preparations==1,"owner indirect issue still classifies, enters the scope and prepares its original draw");
    state.ownerCtx=nullptr;
}
}
int main(int argc, char** argv) {
 if(argc!=2)return 2;
 nvapiModulePaths();
 vrs_withhold_test::run(check);
 uncounted::test();
 const float frontSkin[]={-.75f,.5f,1.f,1.f},otherSkin[]={-.75f,.5f,2.f,1.f},tieSkin[]={-.75f,.5f,4.f,1.f};
 using witness_gpu::fixtureSkinOwner;
 check(fixtureSkinOwner(3.f,frontSkin,-.75f,.5f)&&fixtureSkinOwner(5.f,otherSkin,-.75f,.5f)&&
       fixtureSkinOwner(9.f,tieSkin,-.75f,.5f),"per-pixel skin labels include both initially visible owners and the coplanar replacement");
 check(!fixtureSkinOwner(9.f,otherSkin,-.75f,.5f)&&!fixtureSkinOwner(7.f,otherSkin,-.75f,.5f)&&
       !fixtureSkinOwner(3.f,frontSkin,.75f,.5f),"a skin sample from another owner, hidden owner or opposite screen half fails");
 edvr::FlatReplayQueryTracker queries;
 using QueryKind=edvr::FlatReplayQueryTracker::Kind;
 int disjoint=0,occlusion=0;
 queries.begin(&disjoint,QueryKind::Timing);
 check(queries.safe(),"timestamp-disjoint timing includes the split work");
 queries.begin(&occlusion,QueryKind::Count);
 check(!queries.safe(),"count-bearing queries forbid a second geometry issue");
 queries.end(&occlusion,QueryKind::Count);queries.end(&disjoint,QueryKind::Timing);
 check(queries.safe(),"observed ends return the owner to split eligibility");
 check(knownWorldFamily(kWorldVs,kWorldPs)&&!knownWorldFamily(kWorldVs,kWorldPs+1)&&!knownWorldFamily(kWorldVs+1,kWorldPs),"both exact original opaque-world shader hashes required");
 check(knownWorldFamily(kWorldVs,kWorldPsE46)&&!knownWorldFamily(kWorldVs+1,kWorldPsE46)&&!knownWorldFamily(kWorldVs,kWorldPsE46+1),"observed E46 stock world pair is exact; VS-only and adjacent PS IDs remain refused");
 using edvr::native_vrs::protectsInterfaceShader;
 using edvr::native_vrs::sceneMaterialCandidate;
 using edvr::native_vrs::SceneMaterialDraw;
 const auto eligible=[&](uint64_t vs,uint64_t ps,SceneMaterialDraw material,bool sampledUi=false,char kind='X',unsigned count=288,unsigned instances=1){
     material.shadersTracked=vs&&ps;
     material.interfaceDraw=protectsInterfaceShader(vs,ps,false,kind,count,instances,sampledUi);
     return sceneMaterialCandidate(material,vs,ps);
 };
 const SceneMaterialDraw leftMaterial{0,1,true,false,true,true,false,24},rightMaterial{1,1,true,false,true,true,false,26};
 using edvr::native_vrs::materialColorOutput;
 check(materialColorOutput("SV_Target",0) && materialColorOutput("sV_tArGeT",3),
       "SM5 render-target output spelling is ASCII case insensitive");
 check(!materialColorOutput("SV_Target",4) && !materialColorOutput("SV_Target0",0) && !materialColorOutput("SV_Targe",0) &&
       !materialColorOutput("SV_TargetX",0) && !materialColorOutput("SV_TargeU",0),
       "render-target semantic length, complete spelling and MRT index are exact");
 check(!materialColorOutput("SV_Depth",0) && !materialColorOutput("SV_Coverage",0) && !materialColorOutput("SV_StencilRef",0),
       "depth, sample mask and stencil remain outside material outputs");
 const auto capturedMaterial=[&](const char* file){
     std::ifstream stream(std::string(argv[1])+"/tools/flat_temporal_test/fixtures/"+file,std::ios::binary);
     const std::string bytes{std::istreambuf_iterator<char>(stream),{}};
     check(!bytes.empty(),"retail DXBC fixture present");
     return edvr::native_vrs::compatibleScenePixelShader(bytes.data(),bytes.size());
 };
 check(capturedMaterial("ps_4E4FF61E8A08FC7E.dxbc")&&capturedMaterial("ps_B7D50283329322C3.dxbc")&&
       capturedMaterial("ps_76849D64AC657DB9.dxbc"),"stock forward canopy, station material and planet-surface creation bytes earn material receipts");
 check(!eligible(0x53211E8C072CD02Eull,0x7EAC71963E66C5FEull,leftMaterial)&&
       !eligible(0x0357BBB2DEE43C1Full,0x81812EF97FB4A361ull,leftMaterial)&&
       !eligible(0x525D47E3D5E2EFF4ull,0xF0BAE053476F8730ull,leftMaterial)&&!capturedMaterial("ps_0DF03E64DF9DBEF1.dxbc"),
       "draw policy excludes known screen shadows, lights and filters; shader compatibility excludes discard");
 check(!eligible(0x53211E8C072CD02Eull,0x7EAC71963E66C5FEull,leftMaterial,false,'N',3)&&
       !eligible(0x7E38A6AA1269C901ull,0x7CECABDE34FFBE9Eull,rightMaterial,false,'N',4)&&
       !eligible(0x963B52C73B4143ACull,0x50364C9D994141D5ull,leftMaterial,false,'X',84,25),
       "census 054801 F1 rows 124,126,128: shared scene DSV and full-eye target do not certify resolve, deferred sun or indexed light volumes");
 auto unknown=leftMaterial;unknown.materialShader=false;
 auto witness=leftMaterial;witness.motionWitness=true;
 check(sceneMaterialCandidate(witness,kWorldVs,kWorldPs,true)&&
       !sceneMaterialCandidate(witness,0x889A5279E68F0672ull,0x2F924695596C8195ull,true)&&
       !sceneMaterialCandidate(witness,0x889A5279E68F0672ull,0xEBA95E15B0A66102ull,true),
       "a split admits eligible material while self-marks and emissive terminal panels retain their protection");
 check(!eligible(kWorldVs,kWorldPs,unknown)&&!eligible(kWorldVs,kWorldPs,witness)&&
       !eligible(0x889A5279E68F0672ull,0x2F924695596C8195ull,leftMaterial)&&
       !eligible(0x889A5279E68F0672ull,0xEBA95E15B0A66102ull,leftMaterial),
       "unknown shaders, active velocity exports, self-marking shaders and in-world terminals stay full rate");
 constexpr uint64_t otherStockPs=0xCB429E043DBB2506ull;
 check(!knownWorldFamily(kWorldVs,otherStockPs)&&eligible(kWorldVs,otherStockPs,leftMaterial),
       "production admits a third stock material in the same G-buffer family");
 check(!knownWorldFamily(edvr::kUiVsLegacyGlass,edvr::kUiPsLegacyGlass)&&
       eligible(edvr::kUiVsLegacyGlass,edvr::kUiPsLegacyGlass,rightMaterial),
       "nonredirected forward canopy material can qualify for the right eye");
 check(!eligible(kWorldVs,otherStockPs,{-1,1,true,false,true,true,false,24})&&
       !eligible(kWorldVs,otherStockPs,{0,2,true,false,true,true,false,26})&&
       !eligible(kWorldVs,otherStockPs,{0,1,true,false,false,true,false,26})&&
       !eligible(kWorldVs,otherStockPs,{0,1,true,false,true,true,false,61})&&
       !eligible(kWorldVs,0,leftMaterial),
       "unresolved depth eye, post-tonemap target, reissue and untracked pixel shader stay full rate");
 check(!eligible(edvr::kUiVsFlightHud,otherStockPs,leftMaterial)&&
       !eligible(edvr::kUiVsGuiText,otherStockPs,leftMaterial)&&
       !eligible(edvr::kUiVsMapSprite,edvr::kUiPsMapSpriteC,leftMaterial)&&
       !eligible(edvr::kScreenMotionRecognizePair.vs,edvr::kScreenMotionRecognizePair.ps,leftMaterial)&&
       !eligible(kWorldVs,otherStockPs,leftMaterial,true),
       "HUD, direct GUI, map icons, screen composites and learned UI sources stay full rate inside the scene target");
 check(!eligible(edvr::kUiVsScopeMark,edvr::kUiPsScopeMark,leftMaterial,false,'X',6)&&
       eligible(edvr::kUiVsScopeMark,edvr::kUiPsScopeMark,leftMaterial),
       "the scope reticle stays full rate while the shared weapon material qualifies");
 constexpr uint64_t lateMenuPs=0x25D5F73A4616093Bull;
 check(!eligible(edvr::srv_turret::kGunnerSheetVs,lateMenuPs,leftMaterial,false,'X',360)&&
       !eligible(edvr::srv_turret::kGunnerSheetVs,lateMenuPs,leftMaterial,false,'I',6)&&
       eligible(edvr::srv_turret::kGunnerSheetVs,otherStockPs,leftMaterial,false,'X',360),
       "the late menu pair stays full rate regardless of direct shape without excluding other pairs");
 for(char uncountedKind:{'Z','Y','A'}){
     check(!eligible(edvr::srv_turret::kGunnerSheetVs,lateMenuPs,leftMaterial,false,uncountedKind,0,0)&&
           !eligible(edvr::srv_turret::kGunnerSheetVs,otherStockPs,leftMaterial,false,uncountedKind,0,0)&&
           !eligible(edvr::srv_turret::kGunnerCardsVs,otherStockPs,leftMaterial,false,uncountedKind,0,0)&&
           !eligible(edvr::kUiVsScopeMark,edvr::kUiPsScopeMark,leftMaterial,false,uncountedKind,0,0),
           "an unknown shape keeps potential gunner and scope UI full rate");
     check(eligible(kWorldVs,otherStockPs,leftMaterial,false,uncountedKind,0,0)&&
           eligible(edvr::kUiVsScopeMark,otherStockPs,leftMaterial,false,uncountedKind,0,0),
           "an unknown shape still admits unambiguous world and nearby shader pairs");
 }
 std::printf("%s VRS material and guard checks=%u\n", good?"PASS":"FAIL", checks);
 return good?0:1;
}

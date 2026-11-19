#include "../../src/d3d11/vrs_coverage_policy.h"
#include "../../src/d3d11/native_vrs_draw_policy.h"
#include "../../src/d3d11/native_vrs_material.h"
#include "../../src/d3d11/flat_replay_query_tracker.h"
#include "../native_vrs_test/witness_pixel_reference.h"
#include "callback_dependency_tests.h"
#include "work_reference_tests.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
using namespace edvr::vrs_coverage;
unsigned checks=0;bool good=true;
void check(bool value,const char* label){++checks;if(!value){good=false;std::printf("FAIL: %s\n",label);}}
std::string read(const std::string& name){std::ifstream f(name);return {std::istreambuf_iterator<char>(f),{}};}
bool has(const std::string& a,const char* b){return a.find(b)!=std::string::npos;}
namespace uncounted {
struct ID3D11DeviceContext {};
struct {ID3D11DeviceContext* ownerCtx;} state;
auto* g_state=&state;
bool nativeVrsLive(){return true;}
unsigned classifications=0,scopes=0,preparations=0;
int nativeSceneMaterialDecision(ID3D11DeviceContext*,bool,bool,char,unsigned,unsigned){++classifications;return 0;}
template<class Draw> bool nativeWorldIssue(int,Draw draw){++scopes;return draw();}
void nativeBeforeOriginal(ID3D11DeviceContext*){++preparations;}
template<class Draw> bool nativeSplitOriginal(ID3D11DeviceContext*,Draw){return false;}
#include "../../src/d3d11/native_vrs_uncounted_issue.h"
void test(){
    ID3D11DeviceContext owner{},deferred{};state.ownerCtx=&owner;
    unsigned draws=0;
    for(char kind:{'Z','Y','A'})nativeUncountedMaterialIssue(&deferred,kind,[&]{++draws;});
    check(draws==3&&!classifications&&!scopes&&!preparations,"deferred indirect and auto recording issue once without entering either VRS mutation boundary");
    nativeUncountedMaterialIssue(&owner,'Z',[&]{++draws;});
    check(draws==4&&classifications==1&&scopes==1&&preparations==1,"owner indirect issue still classifies, enters the scope and prepares its original draw");
}
}
int main(int argc,char** argv){
 if(argc==2&&std::strcmp(argv[1],"--dry-run")==0){std::puts("CPU-only accepted-state/rate-map/direction evidence; no NVAPI, runtime, GPU or input calls");return 0;}
 if(argc!=2)return 2;
 callbackDependencyTests(check);
 workReferenceTests(check);
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
 PipelineQueryProgress pipeline;
 check(!pipeline.pending()&&!pipeline.pollable()&&!pipeline.end(),"idle pipeline cannot End or poll");
 check(pipeline.begin()&&pipeline.pending()&&!pipeline.pollable()&&!pipeline.begin(),"one Begin creates one pending query");
 pipeline.cancel();check(pipeline.pending()&&!pipeline.evidence(),"OFF cancellation retains query ownership and rejects evidence");
 check(pipeline.end()&&!pipeline.end()&&pipeline.pollable(),"OFF-raced issue still balances exactly one End");
 check(!pipelineOffQuiescent(true,pipeline.pending()?1u:0u),"OFF ack withheld while an ended query is pending");
 pipeline.complete();check(!pipeline.pending()&&!pipeline.pollable()&&!pipeline.evidence()&&pipelineOffQuiescent(true,0),"completed cancelled query drains before final OFF ack without qualifying evidence");
 PipelineQueryProgress fresh;fresh.begin();fresh.end();fresh.complete();check(fresh.evidence(),"uncancelled completed query can qualify");
 PipelineQueryProgress beforeBegin;beforeBegin.cancel();check(!beforeBegin.begin()&&!beforeBegin.pending(),"OFF before Begin creates no GPU query work");
 PipelineQueryProgress failed;failed.begin();failed.end();failed.complete(false);
 check(!failed.pending()&&!failed.evidence(),"failed GetData/device removal settles ownership but never qualifies evidence");
 std::array<PipelineQueryProgress,3> stalled{};for(auto& q:stalled){q.begin();q.end();q.cancel();}
 for(unsigned poll=0;poll<100;++poll)for(auto& q:stalled)check(q.pending()&&q.pollable()&&!q.evidence(),"S_FALSE/timeout retains one of three bounded slots and cannot fabricate evidence or a final ack");
 for(auto& q:stalled)q.complete(false);
 check(!stalled[0].pending()&&!stalled[1].pending()&&!stalled[2].pending(),"terminal query errors drain all fixed slots without waiting or allocating");
 check(!pipelineOffQuiescent(false,0),"publisher or original draw in flight prevents OFF acknowledgement even with no pending queries");
 check(pipelineSampleDue(0,false,1000,1000)&&pipelineSampleDue(1,false,1000,1000),"known disabled, enabled and explicitly unknown control states share both-eye diagnostic count eligibility");
 check(!pipelineSampleDue(2,false,1000,0)&&!pipelineSampleDue(0,true,1000,0)&&!pipelineSampleDue(0,false,999,1000),"unknown eye, nested issue and rate-limited sample are refused");
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
 std::array<uint32_t,16> table{};for(unsigned i=0;i<16;++i)table[i]=i%12;
 AcceptedState state;check(!state.active(),"no initial state assumption");
 auto t=state.enter();check(!state.active(),"in-flight state never qualifies");state.rates(t,0,true,true,table);check(!state.active(),"rate enable alone is insufficient");
 t=state.enter();state.resource(t,0,42);check(state.active(),"both accepted setters qualify their state");const auto epoch=state.epoch;
 t=state.enter();state.rates(t,0,true,false,table);check(!state.active()&&state.ratesKnown&&!state.enabled&&state.epoch!=epoch,"successful UI zero-viewports disables and changes epoch");
 t=state.enter();state.rates(t,0,true,true,table);check(state.active(),"same retained view can be re-enabled without unbinding it");
 t=state.enter();state.resource(t,0,0);check(!state.active()&&state.bindingKnown,"null SRRV observed as actual known reset");
 t=state.enter();state.resource(t,0,42);t=state.enter();state.rates(t,-1,true,true,table);check(!state.active()&&!state.ratesKnown&&!state.bindingKnown,"driver error makes proof unknown");
 t=state.enter();state.rates(t,0,false,true,table);check(!state.ratesKnown,"unknown descriptor version is unqualified");
 auto outer=state.enter(),inner=state.enter();state.resource(inner,0,99);state.rates(outer,0,true,true,table);
 check(!state.active()&&!state.writers&&!state.overlapping,"nested accepted writes never fabricate ordering");
 t=state.enter();state.rates(t,0,true,true,table);t=state.enter();state.resource(t,0,42);check(state.active(),"fresh ordered setters recover after ambiguity");
 const auto before=state.epoch;state.invalidate();check(!state.active()&&state.epoch!=before,"clear/execute invalidation forces fresh evidence");
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
 GateCounts gates;gates.note(kWorldVs,kWorldPsE46,true,true,true,true,0,true,true);
 check(gates.knownVs==1&&gates.psE46==1&&gates.exactFamily==1&&gates.depthLeft==1&&gates.familyDepthIssued==1,"E46 positive passes complete original-issue funnel for left eye");
 gates.note(kWorldVs,kWorldPs,true,true,true,true,1,true,true);
 check(gates.ps60==1&&gates.depthRight==1&&gates.familyDepthIssued==2,"prior60D pair retained and right eye counted separately");
 for(unsigned blocked=0;blocked<4;++blocked){GateCounts rejected;rejected.note(kWorldVs,kWorldPsE46,blocked!=0,blocked!=1,blocked!=2,blocked!=3,0,true,true);
   check(rejected.knownVs==1&&rejected.exactFamily==0&&rejected.familyDepthIssued==0,"owner/eye/UI/class rejection cannot turn a candidate into a qualified family issue");}
 GateCounts other;other.note(kWorldVs,kWorldPsE46+1,true,true,true,true,0,true,true);
 check(other.otherPs==1&&other.allowedClass==1&&other.exactFamily==0&&other.familyDepthIssued==0,"size/context/class evidence cannot replace an exact material PS");
 GateCounts unresolved;unresolved.note(kWorldVs,kWorldPsE46,true,true,true,true,-1,true,true);
 check(unresolved.exactFamily==1&&unresolved.depthUnresolved==1&&!unresolved.depthLeft&&!unresolved.depthRight&&!unresolved.familyDepthIssued,"unknown DSV eye is explicit unresolved evidence, never size-inferred");
 GateCounts beforeRefused;beforeRefused.note(kWorldVs,kWorldPsE46,true,true,true,true,0,false,true);
 check(beforeRefused.depthLeft==1&&beforeRefused.candidateIssued==1&&!beforeRefused.candidateBeforeAccepted&&!beforeRefused.familyDepthIssued,"real draw issue cannot qualify if BeforeDraw declined its context/thread guard");
 GateCounts notIssued;notIssued.note(kWorldVs,kWorldPsE46,true,true,true,true,0,true,false);
 check(notIssued.familyDepthBeforeAccepted==1&&!notIssued.candidateIssued&&!notIssued.familyDepthIssued,"accepted pre-draw state never counts a draw that was not issued");
 GateCounts foreign;foreign.note(kWorldVs+1,kWorldPsE46,true,true,true,true,0,true,true);
 check(!foreign.knownVs&&!foreign.familyDepthIssued,"unrelated VS family creates no candidate evidence");
 Histogram h;h.add(5,table,0,0);h.add(8,table,1,0);h.finish();check(h.nonuniform()&&h.hq==1&&h.coarse==1&&h.tiles==2&&h.hqX==.5,"palette-decoded HQ and coarse tiles, center phase");
 Histogram allFull;allFull.add(5,table,0,0);allFull.add(5,table,1,0);allFull.finish();check(!allFull.nonuniform()&&allFull.hqX==1,"full-rate map alone does not claim nonuniform foveation");
 Histogram bad;bad.add(16,table,0,0);check(bad.invalid,"out-of-palette byte rejected");table[15]=12;bad={};bad.add(15,table,0,0);check(bad.invalid,"unknown accepted-palette enum rejected");
 Histogram cull;cull.add(0,table,0,0);cull.finish();check(cull.hq==0&&cull.coarse==0,"cull is never high quality");
 DirectionCounts direction;direction.add(0,0,0,0,134,134);check(direction.pairs==0&&direction.noise==1,"zero/noisy changes are unexercised");
 direction.add(.1,.1,2,2,134,134);check(direction.pairs==1&&direction.sameYAgree==1&&direction.ySameAgree==1,"same-Y directional hypothesis");
 direction.add(.1,.1,2,-2,134,134);check(direction.pairs==2&&direction.flippedYAgree==1&&direction.yFlippedAgree==1,"flipped-Y hypothesis remains separate");
 direction.add(.1,0,-2,0,134,134);check(direction.xMismatch==1&&direction.sameYMismatch>=1,"opposite X motion cannot qualify agreement");
 const auto root=std::string(argv[1]);const auto core=read(root+"/src/d3d11/vrs_coverage.cpp"),draw=read(root+"/src/d3d11/vscreen.cpp"),ini=read(root+"/edvr.ini");
 check(has(core,"if(!track(ctx))return forward(ctx,desc);")&&has(core,"if(!track(ctx))return forward(ctx,view);"),"OFF/foreign callbacks forward without shadow bookkeeping");
 check(has(core,"prepareRates")&&has(core,"prepareResource")&&has(core,"hooksReady.store(a&&b"),"both hooks required and original forwards prepared before patch");
 check(has(core,"prepareRates,nullptr,true")&&has(core,"prepareResource,nullptr,true")&&
       has(read(root+"/src/common/code_hook.h"),"void* prepareContext = nullptr);")&&
       has(read(root+"/src/common/code_hook.h"),"bool allowFarReplacement);"),"six-argument API retained; only explicit diagnostic callers opt into the far-replacement overload");
 check(has(core,"nvapiOwnerPathKind(systemPath,stubPath,ratesPath,resourcePath)")&&has(core,"retainedOwner==ratesOwner")&&has(core,"GET_MODULE_HANDLE_EX_FLAG_PIN"),"actual recognized function owner pinned before hook installation");
 check(has(core,"rates_attempted=%u rates_installed=%u resource_attempted=%u resource_installed=%u refusal_stage=%u")&&has(core,"d.ratesHook.refusal()")&&has(core,"d.resourceHook.refusal()"),"one-shot discovery distinguishes path/pin failures from unchanged CodeHook refusals");
 const auto configure=core.find("void vrsCoverageConfigure(bool enabled)"),repeat=core.find("if(previous==enabled)",configure),tickAck=core.find("pipelineOffQuiescent(!d.needDisable&&!d.state.writers&&!d.drawDepth,pending)");
 check(repeat!=std::string::npos&&core.substr(repeat,260).find("if(!enabled)")!=std::string::npos&&core.substr(repeat,260).find("d.offNoted=false")!=std::string::npos&&tickAck!=std::string::npos,"repeated OFF reload requests fresh acknowledgement through the same quiescence guard");
 check(has(core,"D3D11_MAP_FLAG_DO_NOT_WAIT")&&!has(core,"->Flush(")&&has(core,"D3D11_ASYNC_GETDATA_DONOTFLUSH")&&!has(core,"Sleep("),"readback and pipeline polls cannot flush or wait for GPU completion");
 check(has(core,"s.beforeEpoch!=s.afterEpoch")&&!has(core,"s.bindingEpoch!=d.state.bindingEpoch"),"copy tags immutable; async arrival does not compare later bindings");
 check(has(core,"selected.mip,slice,desc.MipLevels")&&has(core,"slot.sample.palette=d.state.palette"),"selected mip/slice and accepted palette captured with copy");
 check(has(core,"desc.Format!=DXGI_FORMAT_R8_UINT&&desc.Format!=DXGI_FORMAT_R8_TYPELESS")&&has(core,"!sameIdentity(mapDevice.Get(),d.device.Get())"),"actual map bytes require R8 format and exact producer device");
 check(has(core,"pipelineOffQuiescent(!d.needDisable&&!d.state.writers&&!d.drawDepth,pending)"),"OFF acknowledgement waits for setters, issues and pending query drain");
 check(has(core,"pipelineSampleDue(eye,token.nested")&&has(core,"d.nextPipeline[eye]=now+1000")&&has(core,"std::array<PipelineSlot,3> pipelineRing")&&has(core,"s.stateKnown=token.known"),"actual sampler includes known-disabled and explicitly unknown control with one query/sec/eye and fixed three-slot ownership");
 check(has(core,"if(!token.active)return;")&&has(core,"slot.progress.end())context->End(slot.query.Get())")&&!has(core,"if(!token.active||!vrsCoverageEnabled())return;"),"already-begun query receives matching End even after live OFF");
 const auto tick=core.find("void vrsCoverageTick"),offPoll=core.find("pollPipeline(d);",tick),offAck=core.find("pipelineOffQuiescent",tick),onDiscover=core.find("discover(d,now)",tick);
 check(offPoll<offAck&&offAck<onDiscover&&has(core,"++d.pipelineGeneration")&&!has(core,"for(auto& slot:d.pipelineRing)slot.active=false"),"OFF drains pending queries before final ack without destroying their render-thread ownership");
 check(core.find("if(d.offNoted&&d.lastOffAck)return;",tick)<offPoll,"stable final OFF acknowledgement bypasses every later pipeline poll");
 check(has(core,"s.status==S_FALSE")&&has(core,"s.timedOut=true;slot.progress.cancel()")&&has(core,"pending_retained=1 evidence_valid=0 off_ack_blocked=1")&&has(core,"device_removed_hr=0x%08x")&&has(core,"slot.query.Reset()"),"timeout remains bounded and invalid with ack withheld; failed GetData/device removal is explicit and releases settled query ownership");
 check(has(core,"one_hardware_draw_verified=0 draw_identity_complete=0")&&has(core,"primary_real_draw_call\":\"observed_draw_wrapper")&&has(core,"PSInvocations=%llu IAVertices=%llu IAPrimitives=%llu VSInvocations=%llu")&&has(core,"shader_depth_output=unknown")&&has(core,"shader_sample_frequency=unknown"),"actual issue scope/counts/signature and unknown shader eligibility are explicit, never inferred");
 check(has(core,"state_known=%u control=%s control_qualified=%u")&&has(core,"s.stateKnown?\"known\":\"unqualified_control\"")&&has(core,"valid&&s.stateKnown"),"unknown control can yield diagnostic counts without becoming known-disabled or qualifying a VRS saving");
 check(has(core,"shader_state_sample=%s")&&has(core,"raster_state_sample=%s")&&has(core,"before_native_primary\":\"before_draw_wrapper"),"sampled shader/raster metadata names exact native versus outer-wrapper sampling boundary");
 check(has(core,"vrsWorkReferenceConfigure(bool requested)")&&has(core,"referenceScopeActive.load")&&has(core,"referenceScopeActive.store(false")&&has(core,"w.override.restoring=true")&&has(core,"w.restoreStatus=applyReferenceRate(context,restore)"),"explicit temporary reference control retains active restoration even across mid-scope OFF");
 check(has(core,"consumeInternal(referenceInternal)")&&has(core,"w.override.request(requested,ownRequest")&&has(core,"ownRequest?nullptr:&requested"),"production NVAPI hook consumes own marker before forwarding so recursive external requests remain exact-once");
 check(has(draw,"VrsWorkReferenceScope reference(self,")&&has(core,"!vrsWorkReferenceEnabled()&&vrs_coverage::pipelineSampleDue")&&has(core,"expected_ia_vertices=1152 expected_ia_primitives=384")&&has(core,"referenceSchedule.issued"),"only approved primary native issue uses shared ring and predeclared three-arm work selector");
 check(has(ini,"vrs_work_reference = off")&&has(core,"requested=%u active=%u selector=useful_288x4")&&has(core,"scope_active=%u restore_inflight=%u faulted=%u quiescent_ack=%u")&&has(core,"skipped_unknown_state=%llu skipped_provider=%llu"),"reference defaults off, exposes selector gate failures and requires explicit restored quiescence");
 check(has(core,"RSGetScissorRects(&rectangleCount,rectangles)")&&has(core,"fill=%u cull=%u front_ccw=%u depth_clip=%u scissor_enabled=%u scissor_count=%u")&&has(core,"scissor_rect0=%d,%d,%d,%d scissor_hash=%016llX")&&has(core,"blend_factor=%.9g,%.9g,%.9g,%.9g"),"bounded raster/scissor and blend-factor evidence exposes clipping/culling differences in counter comparisons");
 check(has(core,"vrs_callback_dependency::pairEligible(previous.callback,s.callback)")&&has(core,"callbackWorld,callbackMap")&&has(core,"cb_insufficient_excitation=%llu")&&has(core,"sensor_freshness_verified=0")&&has(core,"legacy_gaze_freshness_unchanged=1"),"callback-only dependency uses retained resource identities and separate counters, preserving all legacy freshness claims");
 check(has(core,"s.gaze.cachedFallback==0")&&has(core,"s.gaze.ignored==0")&&has(core,"s.gaze.sequence>previous.gaze.sequence"),"no cached/ignored/repeated gaze qualifies direction pairs");
 check(has(core,"mapping_units_verified=0")&&has(core,"physical_calibration_verified=0")&&has(core,"coordinates_logged=0"),"limits explicit in metadata");
 check(has(core,"vrs coverage gates: schema=1 cumulative known_vs=")&&has(core,"context_refused=%llu device_refused=%llu observer_refused=%llu thread_refused=%llu")&&
       has(draw,"vrsCoverageNoteGate(vrsVs,vrsPs,owner,vrsEye,vrsNotUi,vrsClass,vrsEyeId,vrsDraw.active,issued)"),"separate metadata-only funnel reports class/depth/context refusals and actual issue result");
 check(has(ini,"vrs_coverage_diagnostic = off")&&has(draw,"vrsCoverageConfigure(cfg.getBool(\"advanced.vrs_coverage_diagnostic\",false))"),"default-off and live configuration wired");
 check(has(draw,"!uiLayerRedirecting()")&&has(draw,"depthProbeCurrentSceneEyeOf")&&has(draw,"altered.cls==AlteredDrawClass::PoolFamily"),"world subset requires recognized depth/eye and excludes UI while tagging substitution");
 const auto begin=draw.find("vrsDraw=vrsCoverageBeforeDraw"),issue=draw.find("const bool issued = native ? nativeWorldIssue",begin),end=draw.find("vrsCoverageAfterDraw",issue);
 check(begin<issue&&issue<end,"evidence brackets actual original issue after redirects, not a reissue");
 const auto native=read(root+"/src/d3d11/native_vrs.cpp"),gaze=read(root+"/src/d3d11/native_gaze.cpp"),present=read(root+"/src/d3d11/device_hook.cpp"),ui=read(root+"/src/d3d11/ui_layer.cpp");
 check(has(present,"native_vrs::rememberMaterialPs(static_cast<ID3D11PixelShader*>(*out)")&&
       has(draw,"candidate.materialShader = native_vrs::hasMaterialPsReceipt(pixel, ps)")&&
       has(draw,"candidate.motionWitness = engineVelocityDrawSubstituted()")&&
       has(draw,"actualVs==d->vs && actualPs==d->ps"),
       "material receipts originate at PS creation and only original bound shaders without a motion witness may be issued coarse");
 check(has(ini,"eye_tracked_foveation = 0")&&has(draw,"nativeVrsConfigureFocus(cfg.getBool(\"experimental.eye_tracked_foveation\",false),")&&
       has(draw,"cfg.getString(\"experimental.foveation_focus\",\"normal\").c_str()"),"native producer is explicitly default off with the original focus default");
 check(has(present,"tkNativeVrsClock.run([] { vScreenNativeFrameBoundary(g_state->frameCounter); });")&&has(present,"EDVR_BOUNDARY_TICK(tkNativeVrsClock, \"native_vrs_clock\")")&&!has(draw,"g_nativeVrsOrdinal"),"native recovery uses sole Present ordinal under its own fault budget before graphics standdown");
 check(has(draw,"nativeSceneMaterialDecision(self,")&&has(draw,"nativeBeforeOriginal(self);")&&
       has(draw,"depthProbeCurrentSceneEyeOf(dsv, &candidate.eye, nullptr)"),
       "native production uses broad original scene-material classification and a settled depth eye at issue");
 check(has(native,"OMGetRenderTargets(1,&actualRtv,&actualDsv)")&&has(native,"actualRtv.Get()!=rtv||actualDsv.Get()!=dsv")&&has(native,"RSGetViewports"),"actual native target and full viewport are conservatively verified");
 check(has(native,"OMGetDepthStencilState(&depthState,nullptr)")&&has(native,"rasterizerDesc.ForcedSampleCount")&&
       has(native,"map.worldW!=width||map.worldH!=height||map.depth.Get()!=dsv||!map.view"),
       "new color targets share only stable same-size scene maps with live depth and compatible raster state");
 check(has(draw,"nativeTemporalEyeGeometry(d->eye")&&has(draw,"nativeTemporalFrameSequence()==sequence")&&has(native,"map.projection.sequence==projection.sequence")&&has(gaze,"GetProjectionMatrix("),"source projection remap uses current game geometry and refuses same-ordinal geometry change");
 check(has(draw,"NativeMutationScope")&&has(draw,"NativeMutationScope nativeMutation(self)")&&has(ui,"nativeVrsFullRate(ctx)"),"raw mutation and UI boundaries cannot trust coarse cached state");
 check(has(native,"GetModuleHandleW(L\"LibMagicD3D1164.dll\")")&&has(native,"NativeVrsFailure::MagicPresent"),"external Magic producer is excluded before native ownership");
 std::printf("%s vrs_coverage_checks=%u gpu_calls=0 nvapi_calls=0 runtime_calls=0\n",good?"PASS":"FAIL",checks);return good?0:1;
}

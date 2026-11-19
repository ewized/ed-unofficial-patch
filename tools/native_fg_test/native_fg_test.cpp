#include "../../src/openxr/native_fg_schedule.h"
#include "../../src/openxr/native_fg_credit.h"
#include "../../src/openxr/native_fg_lifecycle.h"
#include "../../src/openxr/native_fg_handoff_trace.h"
#include "../../src/openxr/owner_service.h"
#include "../../src/d3d11/frame_generation_auto_policy.h"
#include "fg_cadence_cases.h"
#include "hud_sampling_cases.h"
#include "hud_projection_cases.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using namespace edvr::openxr;
namespace {
unsigned checks=0,failed=0;
void check(bool value,const char* label){++checks;if(!value){++failed;std::printf("FAIL: %s\n",label);}}
void automaticPolicy() {
  using namespace edvr::fg_auto;
  Policy policy;Sample s;s.valid=true;s.session=1;s.periodMs=1000./90;s.cpuMs=4;s.gpuMs=7;
  for(unsigned i=1;i<=250;++i){s.nowMs=i*11;s.cpuSequence=s.gpuSequence=i;policy.observe(s);}
  check(policy.status().multiplier==0,"Auto retains native90 with measured headroom");
  s.cpuMs=14;s.gpuMs=13;
  for(unsigned i=0;i<100;++i){s.nowMs+=11;++s.cpuSequence;++s.gpuSequence;policy.observe(s);}
  check(policy.status().multiplier==2,"sustained real work pressure enables2x");
  const auto entered=policy.status().transitions;
  for(unsigned i=0;i<50;++i){s.nowMs+=11;policy.observe(s);}
  check(policy.status().transitions==entered,"duplicate measurements cannot satisfy dwell evidence");
  s.cpuMs=4;s.gpuMs=7;
  for(unsigned i=0;i<400;++i){s.nowMs+=22;++s.cpuSequence;++s.gpuSequence;policy.observe(s);}
  check(policy.status().multiplier==0,"2x pacing wait is not mistaken for native work and does not lock Auto at45");
  s.cpuMs=s.gpuMs=100;s.nowMs+=11;++s.cpuSequence;++s.gpuSequence;policy.observe(s);
  s.cpuMs=4;s.gpuMs=7;
  for(unsigned i=0;i<80;++i){s.nowMs+=11;++s.cpuSequence;++s.gpuSequence;policy.observe(s);}
  check(policy.status().multiplier==0,"one spike does not force persistent interpolation");
  s.fatal=true;policy.observe(s);s.fatal=false;s.cpuMs=s.gpuMs=30;
  for(unsigned i=0;i<1000;++i){s.nowMs+=22;++s.cpuSequence;++s.gpuSequence;policy.observe(s);}
  check(policy.status().multiplier==0&&policy.status().reason==Reason::Fault,"terminal fault stays native until explicit reset");
  policy.reset();s.fault=false;s.valid=false;policy.observe(s);
  check(policy.status().multiplier==0,"missing measurements never guess that generation is beneficial");
  policy.reset();s={};s.valid=true;s.session=1;s.cpuMs=14;s.gpuMs=13;
  bool stable=true;
  for(unsigned i=1;i<=1800;++i){
    const auto cadence=policy.status().multiplier;
    s.nowMs+=cadence?22:11;++s.cpuSequence;++s.gpuSequence;
    s.periodMs=panelPeriodMs(90);policy.observe(s);
    if(i>200&&policy.status().multiplier!=2)stable=false;
  }
  check(stable&&policy.status().transitions==1,"panel90 budget stays fixed when native credits change from90 to45");
  check(panelPeriodMs(0)==0&&panelPeriodMs(-1)==0&&panelPeriodMs(10000)==0,
    "unknown or invalid physical refresh cannot authorize Auto");
}
void lifecycle() {
  fglifecycle::Reporter report;fglifecycle::Key key;
  bool coldQuiet=true;
  for(uint64_t now=0;now<10000;++now){key.history=now;coldQuiet=coldQuiet&&!report.observe(now,false,key).emit;}
  check(coldQuiet,"cold Off never emits even as unrelated state changes");
  key={};key.multiplier=2;
  auto first=report.observe(10000,true,key);
  check(first.emit&&first.changed&&first.transitions==1,"requested startup has evidence even before first usable pair");
  check(!report.observe(10001,true,key).emit,"stable requested state is not a per-tick trace");
  check(!report.observe(10999,true,key).emit,"stable requested heartbeat waits a full second");
  check(report.observe(11000,true,key).emit,"stable requested state reports once per second");
  key.failure=7;
  check(report.observe(11001,true,key).emit,"failure transition is visible without waiting for heartbeat");
  fglifecycle::Reporter steady;unsigned steadyLines=0;
  key={};key.multiplier=2;
  for(uint64_t now=0;now<3000;++now){key.state=fglifecycle::stateClass(uint32_t(now%3));steadyLines+=steady.observe(now,true,key).emit?1u:0u;}
  check(steadyLines==3,"normal OFF/ARMING/READY cycle stays at one heartbeat per second");
  check(fglifecycle::stateClass(3)==3&&fglifecycle::stateClass(4)==4&&fglifecycle::stateClass(5)==5,
    "unavailable failed and closing remain distinct lifecycle transitions");
  fglifecycle::Reporter churn;unsigned lines=0;fglifecycle::Decision last;
  for(uint64_t now=0;now<1000;++now){key.history=now;last=churn.observe(now,true,key);lines+=last.emit?1u:0u;}
  check(lines==5,"one-second transition storm is bounded to heartbeat plus four extra rows");
  check(last.transitions==1000&&last.suppressed==995,"suppressed transitions remain explicit cumulative evidence");
  key.multiplier=0;
  auto off=churn.observe(999,false,key);
  check(!off.emit&&churn.observing(),"budget-exhausted Off waits for one final report");
  check(churn.observe(1004,false,key).emit&&!churn.observing(),"pending Off drains at heartbeat then becomes quiescent");
  check(!churn.observe(5000,false,key).emit,"Off after final report has no ongoing heartbeat");
  check(churn.observe(5001,true,key).emit,"later requested lifetime resumes reporting");
  check(churn.observe(1,true,key).emit,"clock reversal cannot strand reporting");
}
void handoffTrace() {
  fghandoff::Recorder<2> trace;
  unsigned emissions=0;
  check(!trace.sampleWanted(),"FG handoff recorder does no work before opt-in");
  trace.finish([&](const auto*,auto,auto){++emissions;});
  trace.arm();
  for(unsigned source=1;source<=4;++source)if(trace.sampleWanted()) {
    fghandoff::Sample sample;sample.source=source;
    trace.append(sample);
  }
  trace.finish([&](const auto* rows,auto count,auto lost){
    ++emissions;
    check(count==2&&lost==2&&rows[0].source==1&&rows[1].source==2,
      "FG handoff recorder retains its first bounded block and counts overflow");
  });
  trace.finish([&](const auto*,auto,auto){++emissions;});
  check(emissions==1&&!trace.sampleWanted(),"FG handoff recorder emits exactly once per opt-in");
}
void schedule() {
  for(unsigned multiplier:{2u,3u,4u}) {
    uint64_t next=0;unsigned credits=0;
    for(uint64_t tick=1;tick<=360;++tick)if(tick>=next){++credits;next=fgschedule::nextCredit(tick,multiplier);}
    check(credits==360/multiplier,"native credits follow the selected ratio to independent XR ticks");
    EdvrFgBatch b{};b.multiplier=multiplier;b.generatedCount=multiplier-1;b.previousSequence=10;b.currentSequence=11;
    check(fgschedule::firstPhase(b,10)==1,"matched endpoint arms earliest generated phase");
    check(fgschedule::firstPhase(b,11)==0,"already displayed endpoint never followed by its interpolations");
    check(fgschedule::firstPhase(b,9)==0,"missing previous native endpoint skips interpolation");
    check(fgschedule::firstPhase(b,0)==0,"unarmed startup never guesses previous endpoint");
    std::vector<unsigned> order;
    for(unsigned phase=fgschedule::firstPhase(b,10);;phase=fgschedule::nextPhase(phase,b.generatedCount)) {
      order.push_back(phase);if(!phase)break;
    }
    check(order.size()==multiplier&&order.back()==0,"all generated phases precede current native endpoint");
    for(unsigned i=0;i+1<order.size();++i)check(order[i]==i+1,"SDK index order retained");
    b.generatedCount=0;b.previousSequence=b.currentSequence;
    check(fgschedule::firstPhase(b,10)==0,"native-only warmup ignores previous sequence");
  }
  EdvrFgView a{},b{};a.orientation[3]=1;b.orientation[3]=-1;a.position[0]=-0.032f;b.position[0]=.968f;
  a.fov[0]=b.fov[0]=-1;a.fov[1]=b.fov[1]=1;a.fov[2]=b.fov[2]=1;a.fov[3]=b.fov[3]=-1;
  const auto middle=fgschedule::view(a,b,.5f);
  check(std::fabs(middle.pose.position.x-.468f)<.000001f,"source poses interpolate without later XR pose");
  check(std::fabs(middle.pose.orientation.w-1.f)<.000001f,"quaternion sign takes shortest path");
  const auto first=fgschedule::view(a,b,0),last=fgschedule::view(a,b,1);
  check(first.pose.position.x==a.position[0]&&last.pose.position.x==b.position[0],"native endpoint positions preserved");
  EdvrFgBatch times{};times.multiplier=4;times.previousPoseDisplayTime=100;times.currentPoseDisplayTime=140;
  check(fgschedule::phaseTime(times,1)==110&&fgschedule::phaseTime(times,3)==130&&fgschedule::phaseTime(times,0)==140,"source-time convention preserves endpoints");
  times.multiplier=3;times.currentPoseDisplayTime=130;
  check(fgschedule::phaseTime(times,1)==110&&fgschedule::phaseTime(times,2)==120&&fgschedule::phaseTime(times,0)==130,
    "3x schedules the first and second generated phases at one-third and two-thirds");
}
void credits() {
  OwnerService owner;std::atomic<unsigned> ticks{0};std::atomic<bool> ownerThread{false};
  std::shared_ptr<NativeFgCreditRequest> pending;
  check(owner.start([&]{++ticks;if(pending){auto request=std::move(pending);request->run();}}),"single owner starts");
  auto request=std::make_shared<NativeFgCreditRequest>();request->callback=[&]{ownerThread=owner.isOwner();};
  check(owner.invoke([&]{pending=request;owner.setContinuousIdle(true);}),"native request is registered without owner wait");
  check(request->wait()&&ownerThread.load()&&ticks.load()>0,"caller wait resumes from existing owner idle tick");
  auto cancelled=std::make_shared<NativeFgCreditRequest>();cancelled->callback=[]{};
  std::atomic<bool> cancelledResult{true};std::thread caller([&]{cancelledResult=cancelled->wait();});
  check(owner.invoke([&]{owner.setContinuousIdle(false);}),"continuous pacing can be disabled");
  check(owner.stop([&]{cancelled->finish(false);}),"owner shutdown cancels outstanding native credit");
  caller.join();check(!cancelledResult.load(),"cancelled caller unblocks outside owner");
  NativeFgCreditRequest throwing;throwing.callback=[]{throw std::runtime_error("fixture");};throwing.run();
  check(!throwing.wait(),"throwing preparation releases caller with failure");
  NativeFgCreditRequest once;once.callback=[]{};once.run();once.finish(false);check(once.wait(),"completion decision cannot be overwritten");
}
void creditGate() {
  OwnerService owner;RuntimeGate gate,foreignGate;NativeFgCreditLoan loan;
  const auto generation=gate.beginGeneration(),foreignGeneration=foreignGate.beginGeneration();
  check(owner.start(),"gate/credit: owner starts");
  bool callbackAdmitted=false,outerExclusive=false,foreignAdmitted=true,escapedValid=true,creditSucceeded=false;
  bool nestedLoan=false,wrongGate=false,staleLoan=false,threw=false,recovered=false,ordinary=false;
  check(owner.invoke([&]{
    {auto normal=loan.admit(gate,generation,owner.isOwner());ordinary=bool(normal);}
    auto pump=gate.tryEnter(generation);auto foreign=foreignGate.tryEnter(foreignGeneration);
    check(bool(pump)&&!gate.tryEnter(generation),"gate/credit: reproduces exclusive pump's failed second acquisition");
    NativeFgCreditRequest request;NativeFgCreditLoan::Admission escaped;
    request.callback=[&]{
      auto admitted=loan.admit(gate,generation,owner.isOwner());callbackAdmitted=bool(admitted);
      outerExclusive=!gate.tryEnter(generation);escaped=std::move(admitted);
      std::thread outsider([&]{foreignAdmitted=bool(loan.admit(gate,generation,false));});outsider.join();
      nestedLoan=loan.run(gate,pump,generation,owner.isOwner(),[]{});
    };
    creditSucceeded=loan.run(gate,pump,generation,owner.isOwner(),[&]{request.run();})&&request.wait();
    escapedValid=bool(escaped);
    wrongGate=loan.run(foreignGate,pump,generation,owner.isOwner(),[]{});
    staleLoan=loan.run(gate,pump,generation+1,owner.isOwner(),[]{});
    try{loan.run(gate,pump,generation,owner.isOwner(),[]{throw std::runtime_error("fixture");});}catch(...){threw=true;}
    check(!loan.admit(gate,generation,owner.isOwner()),"gate/credit: exception clears loan, cannot bypass still-held gate");
    loan.run(gate,pump,generation,owner.isOwner(),[&]{recovered=bool(loan.admit(gate,generation,owner.isOwner()));});
  }),"gate/credit: actual owner executes credit under its pump lease");
  check(ordinary,"gate/credit: ordinary route still acquires its own gate lease");
  check(creditSucceeded&&callbackAdmitted,"gate/credit: synchronous credit borrows existing valid operation");
  check(outerExclusive,"gate/credit: borrowing never releases or weakens exclusive gate");
  check(!foreignAdmitted&&!nestedLoan,"gate/credit: foreign thread and nested loan refused");
  check(!escapedValid,"gate/credit: borrowed admission expires at callback boundary");
  check(!wrongGate&&!staleLoan,"gate/credit: foreign gate and stale generation refused");
  check(threw&&recovered,"gate/credit: exception cleanup permits later legitimate fallback credit");
  check(owner.stop(),"gate/credit: owner stops after credit completes");
}
}
int main(int argc,char** argv) {
  if(argc==2&&std::strcmp(argv[1],"--dry-run")==0){std::puts("native_fg_test: CPU-only scheduler/credit contracts; no graphics/runtime");return 0;}
  if(argc!=2||std::strcmp(argv[1],"--self-test")!=0)return 2;
  schedule();credits();creditGate();lifecycle();automaticPolicy();handoffTrace();
  test::runFgCadenceCases(check);
  edvr::openxr::fgtest::runFgHudSamplingCases(check);
  edvr::openxr::fgtest::runFgUiOverlayCases(check);
  std::printf("native_fg_test: %u checks, %u failures\n",checks,failed);return failed?1:0;
}

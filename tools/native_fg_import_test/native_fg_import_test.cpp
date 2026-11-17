#include "../../src/openxr/native_fg_import_pins.h"
#include <cstdio>
#include <memory>

int main(){
  using Pin=std::shared_ptr<int>;using Owner=edvr::openxr::NativeFgImportPins<Pin>;
  unsigned checks=0,failures=0;const auto check=[&](bool pass){++checks;if(!pass){++failures;std::printf("FAIL check=%u\n",checks);}};
  auto texture=std::make_shared<int>(1),ready=std::make_shared<int>(2);
  {
    Owner owner;check(!owner.pending());check(owner.begin(ready));check(owner.add(texture));
    check(texture.use_count()==2&&ready.use_count()==2);check(!owner.begin(ready));
    owner.readsMayStart();check(!owner.cancelBeforeReads());check(owner.signal(7));
    check(!owner.collect(6)&&texture.use_count()==2);owner.invalidate();
    check(owner.pending()&&texture.use_count()==2);check(!owner.close(6)&&texture.use_count()==2);
    check(owner.collect(7));check(texture.use_count()==1&&ready.use_count()==1&&!owner.pending());
    check(owner.close(7));check(owner.begin(ready));check(owner.add(texture));check(owner.cancelBeforeReads());
    check(texture.use_count()==1&&ready.use_count()==1);
  }
  {
    Owner owner;check(owner.begin(ready));for(unsigned i=0;i<12;++i)check(owner.add(texture));
    check(owner.count()==12&&!owner.add(texture));check(owner.cancelBeforeReads());check(texture.use_count()==1);
  }
  {
    Owner owner;owner.begin(ready);owner.add(texture);owner.readsMayStart();owner.signal(9);
    check(!owner.collect(UINT64_MAX)&&owner.quarantined()&&texture.use_count()==2);
    owner.invalidate();check(!owner.collect(9)&&!owner.close(9)&&texture.use_count()==2);
    check(!owner.begin(ready)); // production owner is retained, never destroyed in this state
  }
  {
    Owner owner;owner.begin(ready);owner.add(texture);owner.readsMayStart();owner.quarantine(); // Signal failure
    check(!owner.collect(UINT64_MAX)&&!owner.close(100)&&texture.use_count()==2);owner.invalidate();
    check(owner.pending()&&owner.count()==1);
  }
  {
    Owner owner;owner.begin(ready);owner.add(texture);owner.readsMayStart();check(!owner.signal(0)&&owner.quarantined());
    check(!owner.close(100)&&texture.use_count()==2);
  }
  std::printf("native FG actual import-pin owner CPU checks=%u failures=%u GPU=0 UI=0\n",checks,failures);
  return failures?1:0;
}

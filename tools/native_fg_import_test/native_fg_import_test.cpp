#include "../../src/openxr/native_fg_import_pins.h"
#include <cstdio>
#include <memory>
#include <array>

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
  {
    using Cache=edvr::openxr::NativeFgImportCache<Pin,std::array<unsigned,2>>;
    Cache cache;Cache::Signature signature{64,32,28,7,0};Pin first,again;
    unsigned opens=0;std::array<unsigned,2> description{64,32};
    const auto open=[&]{return std::make_shared<int>(int(++opens));};
    const auto describe=[&](const Pin&,auto& out){out=description;return true;};
    check(cache.resolve(1,signature,0,17,open,describe,first)==Cache::Result::Opened);
    check(cache.resolve(1,signature,0,17,open,describe,again)==Cache::Result::Reused);
    check(opens==1&&first==again); // Same immutable allocation opens once.
    auto changed=signature;changed[0]=128;
    check(cache.resolve(1,changed,0,17,open,describe,again)==Cache::Result::Invalid);
    check(cache.resolve(1,signature,0,18,open,describe,again)==Cache::Result::Invalid);
    check(cache.resolve(1,signature,1,17,open,describe,again)==Cache::Result::Invalid);
    description[0]=128;
    check(cache.resolve(1,signature,0,17,open,describe,again)==Cache::Result::Invalid);
    description[0]=64;
    check(cache.resolve(2,signature,0,17,open,describe,again)==Cache::Result::Opened);
    check(opens==2&&first!=again); // Recycled numeric handle, fresh allocation.
    check(cache.resolve(0,signature,0,17,open,describe,again)==Cache::Result::Invalid);
    check(cache.resolve(3,signature,0,19,open,describe,again)==Cache::Result::Opened);
    const unsigned beforeFailedOpen=opens;
    check(cache.resolve(4,signature,0,20,[]{return Pin{};},describe,again)==Cache::Result::OpenFailed&&cache.size()==3);
    check(cache.resolve(1,signature,0,17,open,describe,again)==Cache::Result::Reused&&opens==beforeFailedOpen);
    // A previously unobserved role may be learned on an already allocated set.
    check(cache.resolve(1,signature,10,21,open,describe,again)==Cache::Result::Opened);
    const unsigned beforeSubset=opens;
    check(cache.resolve(1,signature,0,17,open,describe,again)==Cache::Result::Reused&&opens==beforeSubset);
    cache.clear();check(cache.size()==0);
  }
  {
    using Cache=edvr::openxr::NativeFgImportCache<Pin,std::array<unsigned,2>,2>;
    Cache cache;Cache::Signature signature{64,32,28,7,0};Pin input;
    std::weak_ptr<int> queued;
    const auto describe=[](const Pin&,auto& out){out={64,32};return true;};
    const auto open=[] {return std::make_shared<int>(9);};
    check(cache.resolve(11,signature,0,21,open,describe,input)==Cache::Result::Opened);
    queued=input;Owner reads;check(reads.begin(ready));check(reads.add(input));
    reads.readsMayStart();check(reads.signal(12));input.reset();
    check(cache.resolve(12,signature,0,22,open,describe,input)==Cache::Result::Opened);input.reset();
    check(cache.resolve(13,signature,0,23,open,describe,input)==Cache::Result::Opened);input.reset();
    check(cache.size()==2&&!queued.expired()); // Evicted cache owner is not the queued-read owner.
    cache.clear();check(cache.size()==0&&!queued.expired());
    check(!reads.close(11)&&!queued.expired());check(reads.close(12)&&queued.expired());
  }
  std::printf("native FG actual import-pin owner CPU checks=%u failures=%u GPU=0 UI=0\n",checks,failures);
  return failures?1:0;
}

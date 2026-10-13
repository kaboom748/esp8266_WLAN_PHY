#include <cassert>
#include <cstdio>
#include "protocol_backend.h"
#define FAST_HOST_TEST
#define FAST_FS 153600
#define FAST_HALF_US 15000
#include "fast.h"

int main() {
  assert(Fast::RELEASE_TARGET_US==29833);
  assert(Fast::FINAL_LIMIT_US==30833+1000);
  for(int delay=-1999;delay<=33333;++delay){
    const uint32_t now=0xfffffff0u;
    const uint32_t due=now+uint32_t(delay);
    assert(int32_t(now-due)==-delay);
  }
  uint32_t rng=0x671382ab, tests=0;
  auto random=[&](){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;};
  for(unsigned n=0;n<2000;++n) {
    Fast::Packet p;p.role=1+n%2;p.boot=1+random()%65535;
    p.seq=random();p.ack=random();p.data=n&1;p.ackValid=n&2;p.byte=random();p.age=random();
    p.ackBoot=p.data?0:random();
    if(!p.data){p.byte=0;p.age=0;}
    p.txHz=(int(random()%187)-93)*128;
    p.tuneHz=n%4 ? (int(random()%187)-93)*128 : Fast::NO_TUNE;
    if(FAST_INTEGER){p.txHz=0;p.tuneHz=Fast::NO_TUNE;p.periodUs=33333+(int(random()%165)-82)*4;}
    uint8_t original[Fast::WIRE];Fast::encode(p,original);
    uint8_t whitened[Fast::WIRE];memcpy(whitened,original,sizeof(whitened));
    Fast::scramble(whitened);Fast::scramble(whitened);assert(!memcmp(whitened,original,sizeof(whitened)));
    for(unsigned errors=0;errors<=3;++errors) {
      uint8_t w[Fast::WIRE];memcpy(w,original,sizeof(w));
      unsigned used=0;
      for(unsigned e=0;e<errors;++e) {
        unsigned at;do{at=random()%Fast::WIRE;}while(used&(1u<<at));used|=1u<<at;
        w[at]^=uint8_t(1+random()%255);
      }
      Fast::Packet q;uint8_t fixed=0;bool ok=Fast::decode(w,q,fixed);
      if(errors<=2)assert(ok && fixed==errors);
      if(ok)assert(q.role==p.role && q.boot==p.boot && q.seq==p.seq && q.ack==p.ack &&
        q.data==p.data && q.ackValid==p.ackValid && q.byte==p.byte && q.age==p.age && q.ackBoot==p.ackBoot &&
        q.txHz==p.txHz && q.tuneHz==p.tuneHz && q.periodUs==p.periodUs);
      ++tests;
    }
    original[10]^=1;
    AirModem::Rs14::encode(original,original+14);
    Fast::Packet q;uint8_t fixed;assert(!Fast::decode(original,q,fixed));++tests;
  }
  printf("PASS %u codec checks: RS correction 0/1/2 bytes, rejection/validation for 3 errors, independent CRC\n",tests);
}

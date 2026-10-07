#include <cassert>
#include <cstdio>
#include "TdmRate.h"
using TdmRate::Controller;
static void clean(Controller& c,unsigned n=4){while(n--)c.feedback(0,0,false);}
int main(){
  Controller c;
  clean(c,3);assert(c.request(true,0)==0);
  clean(c,1);assert(c.request(true,0)==1 && c.request(false,0)==0);
  c.changed(1,0);c.failure();assert(c.request(true,0)==1);
  c.feedback(0,0,false);c.failure();assert(c.request(true,0)==0);
  c.changed(0,100);assert(c.cooldownMs(1,100)==5000);
  clean(c);assert(c.request(true,5099)==0 && c.request(true,5100)==1);
  // The timer progresses without receiving hundreds of slow frames.
  c.restart();assert(c.request(true,100000)==0);
  clean(c);assert(c.request(true,100000)==1);

  Controller repeated;
  uint32_t now=1000;
  const uint32_t waits[]={5000,10000,20000,40000,60000,60000};
  for(uint32_t delay:waits){
    repeated.changed(5,now);
    repeated.feedback(0,12,false);
    assert(repeated.request(false,now)==4);
    repeated.changed(4,now);
    assert(repeated.cooldownMs(5,now)==delay);
    clean(repeated);
    assert(repeated.request(true,now+delay-1)==4);
    assert(repeated.request(true,now+delay)==5);
    now+=delay;
  }
  repeated.changed(5,now);clean(repeated,32);
  repeated.changed(4,now,true);assert(repeated.cooldownMs(5,now)==5000);

  Controller lower;
  lower.changed(5,0);lower.changed(0,100,true);
  assert(lower.cooldownMs(5,100)==5000);
  for(uint8_t r=0;r<4;r++){
    clean(lower);assert(lower.request(true,100)==r+1);
    lower.changed(r+1,100);
  }
  clean(lower);assert(lower.request(true,100)==4);
  assert(lower.request(true,5100)==5);

  Controller cap;
  cap.changed(5,0);cap.limit=2;
  assert(cap.request(true,0)==2);
  cap.changed(2,100);assert(cap.cooldownMs(5,100)==0);
  clean(cap);assert(cap.request(true,100)==2);
  cap.limit=5;assert(cap.request(true,100)==3);
  cap.automatic=false;assert(cap.request(true,100)==2);

  Controller wrap;
  wrap.changed(1,0xfffffff0u);wrap.changed(0,0xfffffff0u,true);
  clean(wrap);
  assert(wrap.cooldownMs(1,0)==4984);
  assert(wrap.request(true,4983)==0 && wrap.request(true,4984)==1);
  assert(wrap.cooldownMs(255,0)==0);
  wrap.changed(255,0);assert(wrap.rate==0);
  puts("PASS rate: four clean cells, wall-clock bound, per-profile retry, decay, caps, rollover");
}

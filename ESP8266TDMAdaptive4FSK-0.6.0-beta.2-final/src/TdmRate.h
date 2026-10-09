#ifndef ESP8266_TDM_RATE_H
#define ESP8266_TDM_RATE_H
#include <stdint.h>

namespace TdmRate {
static const uint8_t COUNT=8;
inline uint32_t bitUs(uint8_t rate){
  // Legacy name: in the 4-FSK build these are symbol periods, two coded bits each.
  static const uint16_t periods[COUNT]={1000,800,600,500,400,300,200,140};
  return periods[rate<COUNT?rate:0];
}

// Only the master initiates upgrades; either receiver can request a downgrade.
class Controller {
public:
  uint8_t rate=0,good=0,bad=0,limit=COUNT-1,quality=0,lossWindow=0;
  bool automatic=true;
  void failure(){
    good=0;stable=0;lossWindow=(lossWindow<<1)|1;
    uint8_t losses=0;
    for(uint8_t mask=lossWindow;mask;mask>>=1)losses+=mask&1;
    if(losses>=2)bad=1;
  }
  void feedback(uint8_t local,uint8_t remote,bool gap){
    if(gap)failure();
    lossWindow<<=1;
    uint8_t worst=local>remote?local:remote;
    quality=(uint16_t(quality)*7+uint16_t(worst)*16)/8;
    if(worst>=12 || quality>=64){good=0;stable=0;bad=1;return;}
    if(!rate)bad=0;
    if(quality<=32 && !bad){
      if(good<255)good++;
      if(stable<32)stable++;
      if(stable==32)penaltyMs[rate]=0;
    }else {stable=0;if(good)good--;}
  }
  uint32_t cooldownMs(uint8_t target,uint32_t now) const {
    if(target>=COUNT)return 0;
    uint32_t elapsed=now-failedAt[target];
    return elapsed<penaltyMs[target]?penaltyMs[target]-elapsed:0;
  }
  uint8_t request(bool master,uint32_t now) const {
    if(rate>limit)return limit;
    if(!automatic)return rate;
    if(bad && rate)return rate-1;
    if(master && !bad && good>=4 && rate<limit && !cooldownMs(rate+1,now))return rate+1;
    return rate;
  }
  void changed(uint8_t next,uint32_t now,bool failed=false){
    if(next>=COUNT || next==rate)return;
    if(next<rate && (bad || failed)){
      // Penalize only the failed profile, not the working path back up to it.
      uint32_t delay=penaltyMs[rate]?penaltyMs[rate]*2:30000;
      penaltyMs[rate]=delay>240000?240000:delay;
      failedAt[rate]=now;
    }
    rate=next;restart();
  }
  void restart(){good=0;bad=0;quality=0;lossWindow=0;stable=0;}
private:
  uint8_t stable=0;
  uint32_t penaltyMs[COUNT]={};
  uint32_t failedAt[COUNT]={};
};
}
#endif

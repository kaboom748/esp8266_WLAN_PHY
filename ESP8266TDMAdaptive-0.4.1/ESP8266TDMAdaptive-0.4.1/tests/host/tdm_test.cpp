#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include "ESP8266TDM.h"
uint32_t fakeMs=0,tickMs=0;
long randomResult=1;
NullSerial Serial;
extern "C" uint32_t os_random(void){return 123;}
struct Air {
  RH_ESP8266FSK* peer=nullptr;
  std::vector<uint8_t> wire;
  uint32_t finish=0;
  unsigned sent=0,dropEvery=0,mutation=0;
  bool dropAll=false;
  uint8_t corrections=0;
  bool dropChange=false;
};
std::map<RH_ESP8266FSK*,Air> air;
RH_ESP8266FSK::RH_ESP8266FSK(uint8_t ch):_channel(ch),_toneZero(16),_toneOne(1016),_apwr(0),_ask(0),_bitUs(3000),_gapMs(120),_txRepeats(1),_pollBudgetUs(0),_preambleBits(64),_calUs(40000),_minToneSepHz(40000),_maxCorrections(16),_lastCal{},_bufValid(false),_bufLen(0){air[this]=Air();}
bool RH_ESP8266FSK::init(){_radioReady=true;_mode=RHModeIdle;return true;}
bool RH_ESP8266FSK::setModemConfig(const ModemConfig& c){_bitUs=c.bitUs;_preambleBits=c.preambleBits;_calUs=c.calUs;return true;}
void RH_ESP8266FSK::setMaxCorrections(uint8_t n){_maxCorrections=n;}
void RH_ESP8266FSK::setBitUs(uint32_t n){_bitUs=n;}
void RH_ESP8266FSK::resetReceiver(){}
bool RH_ESP8266FSK::send(const uint8_t* data,uint8_t n){
  if(_mode==RHModeTx)return false;
  if(air[this].peer && air[this].peer->_mode==RHModeTx){
    fprintf(stderr,"TX overlap at %u: role %u, peer finish=%u delta=%d\n",micros(),_thisAddress,air[air[this].peer].finish,int32_t(micros()-air[air[this].peer].finish));
    assert(false);
  }
  Air& a=air[this];a.wire.assign(data,data+n);
  a.finish=micros()+frameDurationUs(n)+80000;a.sent++;
  _mode=RHModeTx;_txSucceeded=false;return true;
}
void RH_ESP8266FSK::poll(){
  Air& a=air[this];
  if(_mode!=RHModeTx || !TdmProtocol::due(micros(),a.finish))return;
  _mode=RHModeIdle;_txSucceeded=true;
  if(a.dropAll || (a.dropEvery && a.sent%a.dropEvery==0) || !a.peer)return;
  auto& p=*a.peer;
  assert(p._mode!=RHModeTx); // Detect any overlap of the two transmitters.
  if(p._bitUs!=_bitUs)return;
  TdmProtocol::Cell sent;
  assert(TdmProtocol::decode(a.wire.data(),a.wire.size(),sent));
  if(a.dropChange && sent.rate!=sent.requestedRate){a.dropChange=false;return;}
  if(a.mutation){
    TdmProtocol::Cell c;assert(TdmProtocol::decode(a.wire.data(),a.wire.size(),c));
    if(a.mutation==1)c.seenSession++;
    if(a.mutation==2)c.ackId++;
    TdmProtocol::encode(c,a.wire.data());
  }
  memcpy(p._buf,a.wire.data(),a.wire.size());p._bufLen=a.wire.size();p._bufValid=true;
  p._rxHeaderFrom=_txHeaderFrom;p._rxHeaderTo=_txHeaderTo;p._lastReceiveUs=micros();
  p._lastCorrections=a.corrections;
}
bool RH_ESP8266FSK::available(){return _bufValid;}
bool RH_ESP8266FSK::recv(uint8_t* buf,uint8_t* length){
  if(!_bufValid)return false;
  *length=std::min(*length,_bufLen);memcpy(buf,_buf,*length);_bufValid=false;return true;
}
bool RH_ESP8266FSK::waitPacketSent(){return _txSucceeded;}
bool RH_ESP8266FSK::waitPacketSent(uint16_t){return _txSucceeded;}
void RH_ESP8266FSK::setMode(RHMode m){_mode=m;}
bool RH_ESP8266FSK::sleep(){return false;}
uint8_t RH_ESP8266FSK::maxMessageLength(){return 32;}
bool RH_ESP8266FSK::supportsAckBeacon(){return false;}
bool RH_ESP8266FSK::sendAckBeacon(uint8_t,uint16_t){return false;}
bool RH_ESP8266FSK::waitAckBeacon(uint16_t){return false;}

void run(ESP8266TDM& a,ESP8266TDM& b,uint32_t ms){
  for(uint32_t i=0;i<ms;i++){a.poll();b.poll();fakeMs++;}
}
void connect(RH_ESP8266FSK& a,RH_ESP8266FSK& b){air[&a].peer=&b;air[&b].peer=&a;}

int main(){
  using namespace TdmProtocol;
  assert(due(10,0xfffffff0u));assert(!due(0xfffffff0u,10));
  assert(newer(0,65535));assert(!newer(65535,0));
  Cell c;c.session=0x12345678;c.seenSession=0xfedcba98;c.cycle=65535;c.dataId=32768;c.ackId=65535;
  c.flags=DATA|ACK;c.length=PAYLOAD_SIZE;c.corrections=16;
  for(unsigned i=0;i<PAYLOAD_SIZE;i++)c.payload[i]=i*19;
  uint8_t wire[WIRE_SIZE];encode(c,wire);Cell decoded;
  assert(decode(wire,WIRE_SIZE,decoded));assert(decoded.session==c.session && decoded.ackId==65535);
  assert(!memcmp(decoded.payload,c.payload,PAYLOAD_SIZE));
  wire[16]=15;assert(!decode(wire,WIRE_SIZE,decoded));
  encode(c,wire);wire[1]=0;assert(!decode(wire,WIRE_SIZE,decoded));
  for(uint8_t rate=0;rate<TdmRate::COUNT;rate++){
    for(uint8_t request=0;request<TdmRate::COUNT;request++){
      c.rate=rate;c.requestedRate=request;encode(c,wire);
      assert(decode(wire,WIRE_SIZE,decoded));
      assert(decoded.rate==rate && decoded.requestedRate==request && decoded.flags==c.flags);
    }
  }
  encode(c,wire);wire[1]|=0xe0;assert(!decode(wire,WIRE_SIZE,decoded));
  encode(c,wire);wire[17]=17;assert(!decode(wire,WIRE_SIZE,decoded));
  encode(c,wire);wire[0]=0xd3;assert(!decode(wire,WIRE_SIZE,decoded));
  RH_ESP8266FSK ra,rb;ESP8266TDM a(ra),b(rb);connect(ra,rb);
  a.setAdaptive(false);b.setAdaptive(false);
  assert(a.begin(ESP8266TDM::MASTER,111));assert(b.begin(ESP8266TDM::FOLLOWER,222));
  assert(!a.begin(ESP8266TDM::MASTER));
  uint8_t data[]={0,1,0x80,0xff,5};
  assert(a.send(data,sizeof(data)));assert(b.send(data,sizeof(data)));
  air[&rb].dropEvery=3;air[&ra].dropEvery=4;
  run(a,b,160000);
  assert(a.queued()==0 && b.queued()==0);
  assert(a.statistics().delivered==1 && b.statistics().delivered==1);
  ESP8266TDM::Message m;
  assert(a.recv(m) && m.length==sizeof(data) && !memcmp(m.data,data,sizeof(data)));
  assert(!a.recv(m));assert(b.recv(m));assert(!b.recv(m));

  // Lost response: the sender retries; the receiver delivers only once.
  air[&ra].dropEvery=0;air[&rb].dropEvery=0;air[&rb].dropAll=true;
  assert(a.send(data,sizeof(data)));run(a,b,30000);
  assert(a.queued()==1 && a.state()==ESP8266TDM::SEARCH);
  assert(b.statistics().delivered==2 && b.statistics().duplicates>0);
  air[&rb].dropAll=false;run(a,b,30000);
  assert(a.queued()==0 && a.state()==ESP8266TDM::LOCKED && b.state()==ESP8266TDM::LOCKED);
  assert(b.recv(m));

  // Queue backpressure must not acknowledge undelivered messages.
  for(unsigned i=0;i<4;i++)assert(a.send(data,sizeof(data)));
  assert(!a.send(data,sizeof(data)));run(a,b,80000);
  assert(a.queued()==0 && b.pendingReceive()==4);
  assert(a.send(data,sizeof(data)));run(a,b,30000);
  assert(a.queued()==1 && b.statistics().queueFull>0);
  assert(b.recv(m));run(a,b,20000);assert(a.queued()==0);

  // Restart the follower with a new session while the master's queue persists.
  RH_ESP8266FSK rc;ESP8266TDM rebooted(rc);connect(ra,rc);
  rebooted.setAdaptive(false);
  assert(rebooted.begin(ESP8266TDM::FOLLOWER,333));
  assert(a.send(data,sizeof(data)));run(a,rebooted,30000);
  assert(a.queued()==0 && rebooted.statistics().delivered==1);

  // Cross the 32-bit micros rollover with both schedulers active.
  RH_ESP8266FSK rd,re;ESP8266TDM wrapA(rd),wrapB(re);connect(rd,re);
  fakeMs=4294950;assert(wrapA.begin(ESP8266TDM::MASTER,444));assert(wrapB.begin(ESP8266TDM::FOLLOWER,555));
  run(wrapA,wrapB,60000);
  assert(wrapA.state()==ESP8266TDM::LOCKED && wrapB.state()==ESP8266TDM::LOCKED);
  assert(wrapA.statistics().txFailures==0 && wrapB.statistics().txFailures==0);

  // Wrong ACK session or message id must never dequeue a pending message.
  for(unsigned mutation=1;mutation<=2;mutation++){
    RH_ESP8266FSK rf,rg;ESP8266TDM ackA(rf),ackB(rg);connect(rf,rg);
    ackA.setAdaptive(false);ackB.setAdaptive(false);
    assert(ackA.begin(ESP8266TDM::MASTER,666));assert(ackB.begin(ESP8266TDM::FOLLOWER,777));
    air[&rg].mutation=mutation;assert(ackA.send(data,sizeof(data)));run(ackA,ackB,40000);
    assert(ackA.queued()==1 && ackA.statistics().acknowledged==0);
    assert(ackB.statistics().delivered==1);
    air[&rg].mutation=0;run(ackA,ackB,30000);assert(ackA.queued()==0);
  }

  // Reboot A just as B starts transmitting its old response.
  RH_ESP8266FSK ri,rj;ESP8266TDM oldMaster(ri),survivor(rj);connect(ri,rj);
  assert(oldMaster.begin(ESP8266TDM::MASTER,1000));assert(survivor.begin(ESP8266TDM::FOLLOWER,1001));
  while(rj.mode()!=RHGenericDriver::RHModeTx)run(oldMaster,survivor,1);
  ri.setMode(RHGenericDriver::RHModeIdle);
  RH_ESP8266FSK rh;ESP8266TDM newMaster(rh);connect(rh,rj);
  assert(newMaster.begin(ESP8266TDM::MASTER,888));
  run(newMaster,survivor,40000);
  assert(newMaster.state()==ESP8266TDM::LOCKED && survivor.state()==ESP8266TDM::LOCKED);
  // Adaptive PHY: mismatched rates cannot hear each other in this simulator.
  RH_ESP8266FSK rk,rl;ESP8266TDM fastA(rk),fastB(rl);connect(rk,rl);
  assert(fastA.begin(ESP8266TDM::MASTER,2000));assert(fastB.begin(ESP8266TDM::FOLLOWER,2001));
  run(fastA,fastB,200000);
  assert(fastA.rate()==TdmRate::COUNT-1 && fastB.rate()==fastA.rate());
  assert(fastA.send(data,sizeof(data)));assert(fastB.send(data,sizeof(data)));
  run(fastA,fastB,10000);assert(!fastA.queued() && !fastB.queued());
  // Either endpoint can cap the rate; moderate corrected errors trigger retreat.
  fastB.setRateLimit(2);run(fastA,fastB,20000);
  assert(fastA.rate()==2 && fastB.rate()==2);
  fastB.setRateLimit(5);air[&rk].corrections=6;
  run(fastA,fastB,120000);assert(fastA.rate()==0 && fastB.rate()==0);
  air[&rk].corrections=0;
  run(fastA,fastB,240000);
  assert(fastA.rate()==5 && fastB.rate()==5);

  // Lost change confirmation leaves different rates temporarily, then rendezvous.
  for(unsigned sender=0;sender<2;sender++){
    RH_ESP8266FSK rm,rn;ESP8266TDM changeA(rm),changeB(rn);connect(rm,rn);
    assert(changeA.begin(ESP8266TDM::MASTER,3000));assert(changeB.begin(ESP8266TDM::FOLLOWER,3001));
    air[sender?&rn:&rm].dropChange=true;
    assert(changeA.send(data,sizeof(data)));assert(changeB.send(data,sizeof(data)));
    run(changeA,changeB,250000);
    assert(!changeA.queued() && !changeB.queued());
    assert(changeA.rate()==changeB.rate() && changeA.rate()>0);
    assert(changeA.statistics().delivered==1 && changeB.statistics().delivered==1);
  }
  // Reboot the fast follower into base mode; A must return without serial help.
  RH_ESP8266FSK ro;ESP8266TDM restarted(ro);connect(rk,ro);
  assert(restarted.begin(ESP8266TDM::FOLLOWER,4001));
  assert(fastA.send(data,sizeof(data)));run(fastA,restarted,90000);
  assert(!fastA.queued() && restarted.statistics().delivered==1);
  assert(fastA.rate()==restarted.rate());
  // Lose the confirming reply at every upgrade, with application data in flight.
  for(uint8_t target=1;target<TdmRate::COUNT;target++){
    RH_ESP8266FSK rp,rq;ESP8266TDM probeA(rp),probeB(rq);connect(rp,rq);
    probeA.setRateLimit(target-1);probeB.setRateLimit(target-1);
    assert(probeA.begin(ESP8266TDM::MASTER,5000+target));
    assert(probeB.begin(ESP8266TDM::FOLLOWER,6000+target));
    run(probeA,probeB,200000);
    assert(probeA.rate()==target-1 && probeB.rate()==target-1);
    air[&rq].dropChange=true;
    probeA.setRateLimit(target);probeB.setRateLimit(target);
    assert(probeA.send(data,sizeof(data)));assert(probeB.send(data,sizeof(data)));
    run(probeA,probeB,600000);
    assert(probeA.rate()==target && probeB.rate()==target);
    assert(!probeA.queued() && !probeB.queued());
    assert(probeA.statistics().delivered==1 && probeB.statistics().delivered==1);
    assert(probeA.statistics().recoveries>0);
    air[&rq].dropChange=true;
    probeB.setRateLimit(0);
    assert(probeA.send(data,sizeof(data)));assert(probeB.send(data,sizeof(data)));
    run(probeA,probeB,90000);
    assert(probeA.rate()==0 && probeB.rate()==0);
    assert(!probeA.queued() && !probeB.queued());
    assert(probeA.statistics().delivered==2 && probeB.statistics().delivered==2);
  }
  puts("PASS TDM: loss, ACK identity, queues, rollover, rates, quality retreat, lost negotiation, reboot, no TX overlap");
}

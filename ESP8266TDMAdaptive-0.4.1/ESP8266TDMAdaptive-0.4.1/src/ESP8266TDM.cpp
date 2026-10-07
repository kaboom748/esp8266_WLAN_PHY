#include "ESP8266TDM.h"
extern "C" {
#include "user_interface.h"
}
using namespace TdmProtocol;

bool ESP8266TDM::begin(Role role,uint32_t session){
  if(_ready || (role!=MASTER && role!=FOLLOWER))return false;
  _role=role;
  _session=session?session:os_random();
  if(!_session)_session=1;
  RH_ESP8266FSK::ModemConfig cfg={3000,16,1016,0,0,120,1,64,40000,40000};
  if(!_radio.setModemConfig(cfg) || !_radio.init())return false;
  _radio.setMaxCorrections(16);
  _radio.setCalibrationMemory(15000);
  _radio.setThisAddress(role);_radio.setHeaderFrom(role);_radio.setHeaderTo(3-role);
  _radio.setPromiscuous(false);
  _airUs=_radio.frameDurationUs(WIRE_SIZE);
  _slotUs=_airUs+80000+_guardUs;
  // A reboot can occur during the peer's previous response. Listen first.
  _due=micros()+2*_slotUs;
  _ready=true;
  return true;
}

const char* ESP8266TDM::stateName() const {
  return _state==LOCKED?"LOCKED":_state==SYNC?"SYNC":"SEARCH";
}

void ESP8266TDM::applyRate(uint8_t next,bool failed){
  if(next==_rate.rate)return;
  if(next>_rate.rate)_stats.rateUps++;else _stats.rateDowns++;
  _rate.changed(next,millis(),failed);
  _radio.setBitUs(TdmRate::bitUs(next));
  _airUs=_radio.frameDurationUs(WIRE_SIZE);
  _slotUs=_airUs+80000+_guardUs;
}

void ESP8266TDM::recover(bool failed){
  _stats.recoveries++;
  applyRate(0,failed);
  _rate.restart();
  _pendingReply=false;_awaiting=false;
  _state=SEARCH;
  _radio.restartReceive(true);
  // All recovery paths rendezvous at base rate; A first leaves B time to finish.
  _due=micros()+2*_slotUs;
  _expiry=_due;
}

bool ESP8266TDM::send(const uint8_t* data,uint8_t length,uint16_t* id){
  if(!_ready || !data || !length || length>MAX_PAYLOAD || _txCount==QUEUE_SIZE)return false;
  Message& m=_txQueue[(_txHead+_txCount)%QUEUE_SIZE];
  m.id=_nextData++;m.length=length;memcpy(m.data,data,length);
  if(id)*id=m.id;
  _txCount++;
  return true;
}

bool ESP8266TDM::recv(Message& message){
  if(!_rxCount)return false;
  message=_rxQueue[_rxHead];_rxHead=(_rxHead+1)%QUEUE_SIZE;_rxCount--;
  return true;
}

void ESP8266TDM::pause(bool paused){
  if(_paused==paused)return;
  _paused=paused;
  _pendingReply=false;_awaiting=false;
  if(_txActive){_radio.setMode(RHGenericDriver::RHModeIdle);_txActive=false;}
  _state=SEARCH;_stats.streak=0;_stats.missStreak=0;
  recover(false);
}

bool ESP8266TDM::maintenanceWindow() const {
  if(!_ready || _txActive)return false;
  if(_paused)return true;
  // Serial output is confined to the guard after a validated frame or TX.
  if(_role==FOLLOWER)return _pendingReply && int32_t(_due-micros())>100000;
  return !_awaiting && int32_t(_due-micros())>100000;
}

void ESP8266TDM::missed(){
  _stats.missed++;_stats.streak=0;
  _rate.failure();
  if(_stats.missStreak<255)_stats.missStreak++;
  if(_stats.missStreak>=3){
    if(_state!=SEARCH)_stats.resyncs++;
    recover();
  }else _state=SYNC;
}

void ESP8266TDM::receive(const Cell& c,uint32_t receivedAt){
  if(c.rate!=_rate.rate){_stats.invalid++;return;}
  bool newPeer=c.session!=_peerSession;
  bool alreadyMissed=_stats.missStreak!=0;
  bool gap=false;
  if(_role==MASTER){
    if(!_awaiting || c.cycle!=_cycle || c.seenSession!=_session || c.requestedRate>_offered){_stats.invalid++;return;}
    _awaiting=false;
  }else{
    if(!newPeer && _haveCycle && !newer(c.cycle,_cycle)){_stats.invalid++;return;}
    if(!newPeer && _haveCycle){
      gap=uint16_t(c.cycle-_cycle)>1;
      if(gap){_stats.streak=0;}
    }
    _cycle=c.cycle;_haveCycle=true;
    _pendingReply=!_paused;
    _due=receivedAt+_guardUs;
    _expiry=receivedAt+2*_slotUs+_guardUs;
  }
  if(newPeer){_peerSession=c.session;_haveData=false;_stats.streak=0;}
  _stats.rxCells++;_stats.lastRxMs=millis();_stats.cycle=_cycle;
  _stats.peerCorrections=c.corrections;_stats.missStreak=0;
  if(c.seenSession!=_session)_stats.streak=0;
  else if(_stats.streak<255)_stats.streak++;
  _state=_stats.streak>=3?LOCKED:SYNC;
  _rate.feedback(_radio.lastCorrections(),c.corrections,gap && !alreadyMissed);
  if(_role==MASTER){
    applyRate(c.requestedRate);
  }else{
    // B may accept one clean, eligible step; only A actually proposes it.
    uint8_t allowed=_rate.request(true,millis());
    _replyRate=c.requestedRate<allowed?c.requestedRate:allowed;
  }
  if((c.flags&ACK) && c.seenSession==_session && _txCount && _headAttempts && c.ackId==_txQueue[_txHead].id){
    _txHead=(_txHead+1)%QUEUE_SIZE;_txCount--;_headAttempts=0;_stats.acknowledged++;
  }
  if(!(c.flags&DATA))return;
  if(_haveData && c.dataId==_lastData){_stats.duplicates++;return;}
  if(_haveData && !newer(c.dataId,_lastData)){_stats.invalid++;return;}
  if(_rxCount==QUEUE_SIZE){_stats.queueFull++;return;}
  Message& m=_rxQueue[(_rxHead+_rxCount)%QUEUE_SIZE];
  m.id=c.dataId;m.length=c.length;memcpy(m.data,c.payload,c.length);
  _rxCount++;_lastData=c.dataId;_haveData=true;_stats.delivered++;
}

void ESP8266TDM::transmit(){
  Cell c;
  c.cycle=_cycle;c.session=_session;c.seenSession=_peerSession;
  c.rate=_rate.rate;
  c.requestedRate=_role==MASTER?_rate.request(true,millis()):_replyRate;
  _offered=c.requestedRate;
  c.corrections=_radio.lastCorrections();
  if(_haveData){c.flags|=ACK;c.ackId=_lastData;}
  if(_txCount){
    const Message& m=_txQueue[_txHead];
    c.flags|=DATA;c.dataId=m.id;c.length=m.length;memcpy(c.payload,m.data,m.length);
  }
  uint8_t wire[WIRE_SIZE];encode(c,wire);
  _radio.setHeaderId(uint8_t(_cycle));_radio.setHeaderFlags(0,0xff);
  if(!_radio.send(wire,sizeof(wire))){_stats.txFailures++;return;}
  if(_txCount){if(_headAttempts)_stats.retries++;if(_headAttempts<65535)_headAttempts++;}
  _txActive=true;_txStarted=micros();
}

void ESP8266TDM::poll(){
  if(!_ready)return;
  if(_txActive){
    _radio.poll();
    if(_radio.mode()==RHGenericDriver::RHModeTx){
      if(uint32_t(micros()-_txStarted)>_airUs+100000)_radio.setMode(RHGenericDriver::RHModeIdle);
      else return;
    }
    _txActive=false;
    if(_radio.waitPacketSent(0)){
      _stats.txCells++;
      if(_role==FOLLOWER){
        uint32_t oldSlot=_slotUs;
        applyRate(_offered);
        uint32_t longest=_slotUs>oldSlot?_slotUs:oldSlot;
        _expiry=micros()+2*longest+_guardUs;
      }
    }else {_stats.txFailures++;_rate.failure();}
  }
  if(_radio.available()){
    uint8_t wire[WIRE_SIZE],length=sizeof(wire);
    if(_radio.recv(wire,&length)){
      Cell c;
      if(_radio.headerFrom()==3-_role && _radio.headerTo()==_role && decode(wire,length,c))receive(c,_radio.lastReceiveUs());
      else _stats.invalid++;
    }
  }
  if(_paused)return;
  uint32_t now=micros();
  if(_role==MASTER && due(now,_due)){
    if(_awaiting){missed();if(_state==SEARCH)return;}
    _cycle++;_stats.cycle=_cycle;_awaiting=true;
    _due=now+2*_slotUs;
    transmit();
  }else if(_role==FOLLOWER){
    if(_pendingReply && due(now,_due)){
      _pendingReply=false;
      // Never send a late response into the next master's slot.
      if(uint32_t(now-_due)<_guardUs/2)transmit();else missed();
    }
    if(_haveCycle && due(now,_expiry)){
      _expiry=now+2*_slotUs;missed();
    }
  }
}

#include "ESP8266TDM.h"
extern "C" {
#include "user_interface.h"
}
using namespace TdmProtocol;
static const uint32_t FIXED_SLOT_US=1000000UL;
static const uint32_t TX_SLOT_GUARD_US=20000UL;
static const uint32_t FRAME_MARGIN_US=2000UL;
static const uint32_t SLOT_FLAG_US=16UL;

static uint16_t remainingFlag(uint32_t us){
  uint32_t ticks=(us+SLOT_FLAG_US/2)/SLOT_FLAG_US;
  return ticks>65535?65535:uint16_t(ticks);
}

bool ESP8266TDM::begin(Role role,uint32_t session){
  if(_ready || (role!=MASTER && role!=FOLLOWER))return false;
  _role=role;_session=session?session:os_random();
  if(!_session)_session=1;
  if(!_radio.setFourFsk(true))return false;
  RH_ESP8266FSK::ModemConfig cfg={TdmRate::bitUs(0),16,1016,0,0,2,1,64,40000,12000};
  if(!_radio.setModemConfig(cfg) || !_radio.init())return false;
  _radio.setMaxCorrections(16);_radio.setCalibrationMemory(15000);
  _radio.setThisAddress(role);_radio.setHeaderFrom(role);_radio.setHeaderTo(3-role);
  _radio.setPromiscuous(false);
  _airUs=_radio.frameDurationUs(WIRE_SIZE);_slotUs=FIXED_SLOT_US;
  _due=micros()+2*_slotUs;_ready=true;
  return true;
}

const char* ESP8266TDM::stateName() const {
  return _state==LOCKED?"LOCKED":_state==SYNC?"SYNC":"SEARCH";
}

void ESP8266TDM::applyRate(uint8_t next,bool failed){
  if(next==_rate.rate)return;
  if(next>_rate.rate)_stats.rateUps++;else _stats.rateDowns++;
  _rate.changed(next,millis(),failed);
  _radio.setBitUs(TdmRate::bitUs(next));_airUs=_radio.frameDurationUs(WIRE_SIZE);
}

void ESP8266TDM::recover(bool failed){
  uint8_t next=failed && _rate.rate?_rate.rate-1:0;
  _stats.recoveries++;_radio.endTxBurst();applyRate(next,failed);_rate.restart();
  // Give each lower profile three complete receive opportunities before retreating again.
  _stats.missStreak=0;
  _pendingReply=false;_awaiting=false;_txWindowFrames=0;_applyRateAfterWindow=false;
  _rxWindowOpen=false;_rxWindowSeen=false;_ratePending=false;_quiet=false;
  _state=SEARCH;_radio.restartReceive(!failed || !next);
  _due=micros()+2*_slotUs;_expiry=_due;
}

bool ESP8266TDM::send(const uint8_t* data,uint8_t length,uint16_t* id){
  if(!_ready || !data || !length || length>MAX_PAYLOAD || _txCount==QUEUE_SIZE)return false;
  uint8_t index=(_txHead+_txCount)%QUEUE_SIZE;
  Message& m=_txQueue[index];
  m.id=_nextData++;m.length=length;memcpy(m.data,data,length);_attempts[index]=0;
  if(id)*id=m.id;
  _txCount++;
  return true;
}

size_t ESP8266TDM::write(const uint8_t* data,size_t length){
  if(!data)return 0;
  size_t accepted=0;
  while(accepted<length){
    uint8_t n=length-accepted>MAX_PAYLOAD?MAX_PAYLOAD:uint8_t(length-accepted);
    if(!send(data+accepted,n))break;
    accepted+=n;
  }
  return accepted;
}

size_t ESP8266TDM::queuedBytes() const {
  size_t n=0;
  for(uint8_t i=0;i<_txCount;i++)n+=_txQueue[(_txHead+i)%QUEUE_SIZE].length;
  return n;
}

bool ESP8266TDM::recv(Message& m){
  if(!_rxCount)return false;
  m=_rxQueue[_rxHead];_rxHead=(_rxHead+1)%QUEUE_SIZE;_rxCount--;
  return true;
}

void ESP8266TDM::pause(bool paused){
  if(_paused==paused)return;
  _paused=paused;_radio.setMode(RHGenericDriver::RHModeIdle);_txActive=false;
  _stats.streak=0;_stats.missStreak=0;recover(false);
}

uint32_t ESP8266TDM::maintenanceUs() const {
  if(!_ready || _txActive)return 0;
  if(_paused)return 100000;
  uint32_t now=micros();
  return _quiet && !due(now,_quietUntil)?_quietUntil-now:0;
}

bool ESP8266TDM::maintenanceWindow() const {return maintenanceUs()>2000;}

void ESP8266TDM::missed(){
  _stats.missed++;_stats.streak=0;_rate.failure();
  if(_stats.missStreak<255)_stats.missStreak++;
  if(_stats.missStreak>=3){
    if(_state!=SEARCH)_stats.resyncs++;
    recover();
  }else _state=SYNC;
}

void ESP8266TDM::finishReceiveWindow(){
  if(!_rxWindowOpen)return;
  _rxWindowOpen=false;
  // A window is one quality observation, regardless of its packet count.
  _rate.feedback(_rxCorrections,_rxPeerCorrections,_rxGap);
  if(_role==MASTER){_pendingRate=_rxRequest;_ratePending=true;}
  else {
    uint8_t allowed=_rate.request(true,millis());
    _replyRate=_rxRequest<allowed?_rxRequest:allowed;
  }
}

void ESP8266TDM::receive(const Cell& c,uint32_t receivedAt,uint32_t remainingUs){
  bool newPeer=c.session!=_peerSession;
  if(c.rate!=_rate.rate){_stats.invalid++;return;}
  bool firstWindow=false,gap=false;
  if(_role==MASTER){
    if(c.cycle!=_cycle || c.seenSession!=_session || c.requestedRate>_offered){_stats.invalid++;return;}
    firstWindow=_awaiting;
    if(!firstWindow && (!_rxWindowOpen || newPeer)){_stats.duplicates++;return;}
  }else {
    firstWindow=newPeer || !_haveCycle || c.cycle!=_cycle || !_rxWindowSeen;
    if(firstWindow && !newPeer && _haveCycle && c.cycle!=_cycle && !newer(c.cycle,_cycle)){
      _stats.invalid++;return;
    }
    if(!firstWindow && !_rxWindowOpen){_stats.duplicates++;return;}
    if(firstWindow && !newPeer && _haveCycle && _state!=SEARCH)gap=uint16_t(c.cycle-_cycle)>1;
  }
  if(!firstWindow && c.requestedRate!=_rxRequest){_stats.invalid++;return;}
  uint32_t end=receivedAt+remainingUs;
  if(firstWindow){
    finishReceiveWindow();
    _rxWindowOpen=true;_rxWindowSeen=true;_rxRequest=c.requestedRate;
    _rxCorrections=0;_rxPeerCorrections=0;_rxGap=gap && !_stats.missStreak;
    if(gap)_stats.streak=0;
    if(newPeer){_peerSession=c.session;_haveData=false;_haveBase=false;_stats.streak=0;}
    if(c.seenSession!=_session)_stats.streak=0;
    else if(_stats.streak<255)_stats.streak++;
    _stats.missStreak=0;_state=_stats.streak>=3?LOCKED:SYNC;
  }
  if(_role==MASTER){
    _awaiting=false;_stats.clockErrorUs=int32_t(end-_due);
  }else {
    if(!newPeer && _haveCycle && _state!=SEARCH){
      uint32_t expected=_due+uint16_t(c.cycle-_cycle)*2*_slotUs;
      _stats.clockErrorUs=int32_t(end-expected);
    }
    _cycle=c.cycle;_haveCycle=true;_pendingReply=!_paused;
    _due=end;_expiry=end+_slotUs+_guardUs;
  }
  _rxWindowEnd=end;_stats.clockSyncs++;
  _stats.rxCells++;_stats.lastRxMs=millis();_stats.cycle=_cycle;_stats.peerCorrections=c.corrections;
  if(_radio.lastCorrections()>_rxCorrections)_rxCorrections=_radio.lastCorrections();
  if(c.corrections>_rxPeerCorrections)_rxPeerCorrections=c.corrections;

  // ACK only a contiguous prefix actually transmitted in this session.
  if((c.flags&ACK) && c.seenSession==_session){
    for(uint8_t i=0;i<_txCount;i++){
      uint8_t index=(_txHead+i)%QUEUE_SIZE;
      if(!_attempts[index])break;
      if(_txQueue[index].id!=c.ackId)continue;
      for(uint8_t n=0;n<=i;n++){
        _stats.acknowledgedBytes+=_txQueue[_txHead].length;
        _txHead=(_txHead+1)%QUEUE_SIZE;_txCount--;_stats.acknowledged++;
      }
      break;
    }
  }
  // A new receiver needs FIRST to learn the sender's retained queue base.
  if(!_haveBase && (c.flags&FIRST)){_rxNext=c.dataId;_haveBase=true;}
  if(c.flags&DATA){
    if(_haveData && !newer(c.dataId,_lastData))_stats.duplicates++;
    else if(!_haveBase || c.dataId!=_rxNext)_stats.outOfOrder++;
    else if(_rxCount==QUEUE_SIZE)_stats.queueFull++;
    else {
      Message& m=_rxQueue[(_rxHead+_rxCount)%QUEUE_SIZE];
      m.id=c.dataId;m.length=c.length;memcpy(m.data,c.payload,c.length);
      _rxCount++;_lastData=c.dataId;_rxNext=c.dataId+1;_haveData=true;
      _stats.delivered++;_stats.deliveredBytes+=c.length;
    }
  }
  if(c.flags&LAST){
    finishReceiveWindow();_quiet=true;_quietUntil=end;
  }
}

bool ESP8266TDM::beginTransmitWindow(uint32_t end){
  _txWindowEnd=end;_txWindowFrames=0;_txWindowBytes=0;_txCursor=0;
  _applyRateAfterWindow=false;_quiet=false;
  _offered=_role==MASTER?_rate.request(true,millis()):_replyRate;
  if(transmitFrame())return true;
  finishTransmitWindow();return false;
}

bool ESP8266TDM::transmitFrame(){
  uint32_t now=micros(),airUs=_radio.frameDurationUs(WIRE_SIZE,_txWindowFrames!=0);
  if(due(now,_txWindowEnd) || _txWindowEnd-now<airUs+TX_SLOT_GUARD_US+FRAME_MARGIN_US)return false;
  uint32_t remainingAfter=_txWindowEnd-now-airUs;
  bool hasData=_txCursor<_txCount;
  if(!hasData && _txWindowFrames)return false;
  _txLast=!hasData || _txCursor+1>=_txCount ||
    remainingAfter<_radio.frameDurationUs(WIRE_SIZE,true)+TX_SLOT_GUARD_US+2*FRAME_MARGIN_US;
  Cell c;c.cycle=_cycle;c.session=_session;c.seenSession=_peerSession;
  c.rate=_rate.rate;c.requestedRate=_offered;c.corrections=_rxCorrections;
  if(!_txWindowFrames)c.flags|=FIRST;
  if(_txLast)c.flags|=LAST;
  if(_haveData){c.flags|=ACK;c.ackId=_lastData;}
  c.dataId=_nextData;
  uint8_t index=(_txHead+_txCursor)%QUEUE_SIZE;
  if(hasData){
    const Message& m=_txQueue[index];
    c.flags|=DATA;c.dataId=m.id;c.length=m.length;memcpy(c.payload,m.data,m.length);
  }
  encode(c,_txWire);
  uint16_t ticks=remainingFlag(remainingAfter);
  _radio.setHeaderId(ticks>>8);_radio.setHeaderFlags(uint8_t(ticks),0xff);
  _radio.setTxFillUntil(0);
  if(!_radio.sendBurstFrame(_txWire,sizeof(_txWire),_txLast)){_stats.txFailures++;return false;}
  if(hasData){
    if(_attempts[index])_stats.retries++;
    if(_attempts[index]<65535)_attempts[index]++;
    _txCursor++;_txWindowBytes+=c.length;
  }
  _txWindowFrames++;_txActive=true;_txStarted=micros();
  return true;
}

void ESP8266TDM::finishTransmitWindow(){
  _radio.endTxBurst();
  _stats.windowFrames=_txWindowFrames;_stats.windowBytes=_txWindowBytes;_stats.txWindows++;
  _quiet=true;_quietUntil=_txWindowEnd;
  if(_role==FOLLOWER && _applyRateAfterWindow){
    applyRate(_offered);_expiry=_txWindowEnd+_slotUs+_guardUs;
  }
  _txWindowFrames=0;_applyRateAfterWindow=false;
}

void ESP8266TDM::poll(){
  if(!_ready)return;
  if(_txActive){
    _radio.poll();
    if(_radio.mode()==RHGenericDriver::RHModeTx){
      if(due(micros(),_txWindowEnd-TX_SLOT_GUARD_US))_radio.setMode(RHGenericDriver::RHModeIdle);
      else return;
    }
    _txActive=false;
    if(_radio.waitPacketSent(0)){
      _stats.txCells++;
      if(_role==FOLLOWER)_applyRateAfterWindow=true;
      if(!_txLast && transmitFrame())return;
    }else {_stats.txFailures++;_rate.failure();}
    finishTransmitWindow();
  }
  _radio.setReceiveDeadline((_role==MASTER || _pendingReply)?_due:0);
  if(!maintenanceWindow() && _radio.available()){
    uint8_t wire[WIRE_SIZE],length=sizeof(wire);
    if(_radio.recv(wire,&length)){
      Cell c;
      if(_radio.headerFrom()==3-_role && _radio.headerTo()==_role && decode(wire,length,c)){
        uint16_t ticks=uint16_t(_radio.headerId())<<8|_radio.headerFlags();
        receive(c,_radio.lastReceiveUs(),uint32_t(ticks)*SLOT_FLAG_US);
      }else _stats.invalid++;
    }
  }
  if(_rxWindowOpen && due(micros(),_rxWindowEnd))finishReceiveWindow();
  if(_paused)return;
  uint32_t now=micros();
  if(_role==MASTER && due(now,_due)){
    if(_awaiting){missed();if(_state==SEARCH)return;}
    finishReceiveWindow();
    if(_ratePending){applyRate(_pendingRate);_ratePending=false;}
    _cycle++;_stats.cycle=_cycle;_awaiting=true;
    uint32_t start=_due;_due+=2*_slotUs;
    beginTransmitWindow(start+_slotUs);
  }else if(_role==FOLLOWER){
    if(_pendingReply && due(now,_due)){
      finishReceiveWindow();_pendingReply=false;
      if(!beginTransmitWindow(_due+_slotUs))missed();
    }
    if(_haveCycle && due(now,_expiry)){_expiry=now+2*_slotUs;missed();}
  }
}

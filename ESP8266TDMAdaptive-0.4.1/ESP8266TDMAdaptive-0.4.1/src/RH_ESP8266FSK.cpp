#include "RH_ESP8266FSK.h"
#include "FskCodec.h"
#include <math.h>
extern "C" {
#include "user_interface.h"
}
static const uint32_t TONE1=0x600005B8u,TONE2=0x600005BCu,TONE3=0x600005C4u,PBUS_CMD=0x60000594u,PBUS_STAT=0x600005A0u,RX_CTRL=0x60009B08u;
static const uint32_t IQ_CTRL=0x6000057Cu,IQ_DCI=0x600005DCu,IQ_DCQ=0x600005E0u,IQ_E4=0x600005E4u;
static const uint32_t K_MASK=0x3FFu,SCALE_MASK=0x3FC00u,GATE_MASK=0x40000u,SCALE_SHIFT=10u,RX_STOP=0x08000000u;
static const uint32_t IQ_MODE=0x40000u,IQ_FIELD_MASK=0x3FFFCu,IQ_START=2u,IQ_ENABLE=1u,IQ_DONE31=0x80000000u,IQ_KEEP_MASK=0x7FF80000u;
static uint32_t cfgKeep=0;
static bool txSaved=false;
static uint16_t savedPbus[4];
static const uint8_t txSelectors[4]={2,7,1,6};
typedef uint32_t(*PbusReadFn)(uint32_t,uint32_t);
static PbusReadFn pbusRead=(PbusReadFn)0x400074D8u;
typedef void(*PbusModeFn)();
static PbusModeFn pbusWorkMode=(PbusModeFn)0x40007648u;
static PbusModeFn pbusDebugMode=(PbusModeFn)0x4000737Cu;
typedef void(*PbusForceFn)(uint32_t,uint32_t,uint32_t);
static PbusForceFn pbusForce=(PbusForceFn)0x4000747Cu;
static inline void memw_fsk(){__asm__ volatile("memw" ::: "memory");} static inline uint32_t rd32(uint32_t a){return *reinterpret_cast<volatile uint32_t*>(a);} static inline void wr32(uint32_t a,uint32_t v){*reinterpret_cast<volatile uint32_t*>(a)=v;} static inline uint8_t askCode(uint8_t ask){return (uint8_t)(0u-ask);} typedef void(*SetTxClkFn)(int); typedef uint8_t(*SetAnaScaleFn)(uint8_t); static SetTxClkFn set_txclk=(SetTxClkFn)0x4000650Cu; static SetAnaScaleFn set_ana_scale=(SetAnaScaleFn)0x4000678Cu;
RH_ESP8266FSK::RH_ESP8266FSK(uint8_t ch):_channel(ch),_toneZero(16),_toneOne(1016),_apwr(0),_ask(0),_bitUs(3000),_gapMs(120),_txRepeats(1),_pollBudgetUs(12000),_preambleBits(36),_calUs(40000),_minToneSepHz(40000),_maxCorrections(16),_lastCal{false,0,0,0,false,0},_bufValid(false),_bufLen(0){}
static const RH_ESP8266FSK::ModemConfig modemConfigs[] = {
  {4000,16,1016,0,0,120,1,36,40000,40000},
  {3000,16,1016,0,0,120,1,36,40000,40000},
  {2000,16,1016,0,0,120,1,36,40000,40000},
  {1500,16,1016,0,0,120,1,36,40000,40000},
  {1000,16,1016,0,0,120,1,36,40000,40000}
};
bool RH_ESP8266FSK::setModemConfig(ModemConfigChoice choice){if((uint8_t)choice>=sizeof(modemConfigs)/sizeof(modemConfigs[0]))return false;return setModemConfig(modemConfigs[(uint8_t)choice]);}
bool RH_ESP8266FSK::setModemConfig(const ModemConfig& c){if(_mode==RHModeTx||c.bitUs<300||c.bitUs>10000||c.toneZero>1023||c.toneOne>1023||c.toneZero==c.toneOne||c.preambleBits<24||c.preambleBits>64||c.calUs<20000||c.calUs>200000||c.minToneSepHz<1)return false;_bitUs=c.bitUs;_toneZero=c.toneZero;_toneOne=c.toneOne;_apwr=c.apwr;_ask=c.ask;_gapMs=c.frameGapMs;_txRepeats=c.txRepeats?c.txRepeats:1;_preambleBits=c.preambleBits;_calUs=c.calUs;_minToneSepHz=c.minToneSepHz;resetReceiver();return true;}
bool RH_ESP8266FSK::setBitRate(uint16_t bps){if(_mode==RHModeTx||!bps)return false;uint32_t us=(1000000UL+(bps/2))/bps;if(us<300||us>10000)return false;setBitUs(us);return true;}
void RH_ESP8266FSK::setTones(uint16_t z,uint16_t o){_toneZero=z;_toneOne=o;} void RH_ESP8266FSK::setPower(uint8_t a,uint8_t s){_apwr=a;_ask=s;} void RH_ESP8266FSK::setBitUs(uint32_t b){if(_mode!=RHModeTx&&b>=300&&b<=10000){_bitUs=b;resetReceiver();}} void RH_ESP8266FSK::setFrameGap(uint16_t m){_gapMs=m;} void RH_ESP8266FSK::setTxRepeats(uint8_t r){_txRepeats=r?r:1;} void RH_ESP8266FSK::setPollBudgetUs(uint16_t us){_pollBudgetUs=us?us:500;} void RH_ESP8266FSK::setMaxCorrections(uint8_t c){_maxCorrections=c;} uint32_t RH_ESP8266FSK::bitUs() const{return _bitUs;} uint16_t RH_ESP8266FSK::bitRate() const{return _bitUs?1000000UL/_bitUs:0;} RH_ESP8266FSK::Calibration RH_ESP8266FSK::lastCalibration() const{return _lastCal;} uint8_t RH_ESP8266FSK::maxCorrections() const{return _maxCorrections;} uint8_t RH_ESP8266FSK::maxMessageLength(){return RH_ESP8266FSK_MAX_MESSAGE_LEN;}
bool RH_ESP8266FSK::init(){RHGenericDriver::init(); randomSeed(ESP.getCycleCount()); _radioReady=initRadioBase() && initRxPath();return _radioReady;}
bool pbusWriteFSK(uint8_t sel,uint8_t bank,uint16_t value){uint32_t cmd=rd32(PBUS_CMD);cmd&=0xFFFF0001u;cmd|=(uint32_t)(bank&3u)<<14;cmd|=(uint32_t)(value&0x1FFu)<<5;cmd|=(uint32_t)(sel&7u)<<2;cmd|=2u;wr32(PBUS_CMD,cmd);memw_fsk();uint32_t t0=micros();while(rd32(PBUS_STAT)&0x80000000u){if((uint32_t)(micros()-t0)>2500u){uint32_t r=rd32(PBUS_CMD);r&=~2u;wr32(PBUS_CMD,r);return false;} delay(0);}uint32_t r=rd32(PBUS_CMD);r&=~2u;wr32(PBUS_CMD,r);memw_fsk();return true;}
bool RH_ESP8266FSK::initRadioBase(){wifi_station_set_auto_connect(0); if(!wifi_set_opmode_current(STATION_MODE))return false; delay(60); wifi_station_disconnect(); if(!wifi_set_sleep_type(NONE_SLEEP_T))return false; if(!wifi_set_channel(_channel))return false; delay(20); return true;}
bool RH_ESP8266FSK::initTxPath(){if(!initRadioBase())return false;
  if(!txSaved){
    for(uint8_t i=0;i<4;i++)savedPbus[i]=pbusRead(txSelectors[i],1);
    txSaved=true;
  }
  uint32_t r=rd32(RX_CTRL);r|=RX_STOP;wr32(RX_CTRL,r);memw_fsk();r=rd32(PBUS_CMD);r|=1u;wr32(PBUS_CMD,r);memw_fsk(); if(!(pbusWriteFSK(2,1,1)&&pbusWriteFSK(7,1,95)&&pbusWriteFSK(1,1,127)&&pbusWriteFSK(6,1,127)))return false; set_ana_scale(_apwr); set_txclk(1); r=rd32(TONE1);r&=~(K_MASK|SCALE_MASK|GATE_MASK);r|=_toneZero&K_MASK;r|=(uint32_t)askCode(_ask)<<SCALE_SHIFT;r|=GATE_MASK;wr32(TONE1,r); r=rd32(TONE2);r&=~GATE_MASK;wr32(TONE2,r); r=rd32(TONE3);r&=~GATE_MASK;wr32(TONE3,r);memw_fsk(); return true;}
RH_ESP8266FSK::RfState RH_ESP8266FSK::rfState() const {
  return {(uint16_t)pbusRead(2,1),(uint16_t)pbusRead(3,1),(uint16_t)pbusRead(3,2),
    bool(rd32(PBUS_CMD)&1),bool(rd32(RX_CTRL)&RX_STOP),_apwr,_ask,_lastTxAskField};
}
void RH_ESP8266FSK::applyReceiveGain(){
  if(_rxGain<0){pbusWorkMode();return;}
  static const uint8_t words[7]={0x00,0x40,0x60,0x70,0x78,0x7c,0x7f};
  pbusDebugMode();
  // The saved Wi-Fi gain state may have powered down LNA/mixer stages.
  pbusForce(2,1,0x1fe);
  pbusForce(3,1,words[_rxGain]);
  pbusForce(3,2,(pbusRead(3,2)&0x1c7u)|uint32_t(_rxBbGain)<<3);
}
bool RH_ESP8266FSK::setReceiveGain(int8_t rf,uint8_t bb){
  if(_mode==RHModeTx || rf < -1 || rf>6 || bb>7)return false;
  _rxGain=rf;_rxBbGain=bb;applyReceiveGain();resetReceiver();return true;
}
bool RH_ESP8266FSK::setIqMode(uint8_t mode){
  if(_mode==RHModeTx || mode>1)return false;
  _iqMode=mode;restartReceive(true);return true;
}
bool RH_ESP8266FSK::initRxPath(){
  wr32(TONE1,rd32(TONE1)&~GATE_MASK);
  wr32(TONE2,rd32(TONE2)&~GATE_MASK);
  wr32(TONE3,rd32(TONE3)&~GATE_MASK);
  memw_fsk();
  set_txclk(0);
  if(txSaved){
    for(uint8_t i=0;i<4;i++)pbusWriteFSK(txSelectors[i],1,savedPbus[i]);
    txSaved=false;
  }
  if(!pbusWriteFSK(2,1,0x1fe))return false;
  pbusWorkMode();
  applyReceiveGain();
  cfgKeep=rd32(IQ_CTRL)&IQ_KEEP_MASK;
  return true;
}
void RH_ESP8266FSK::txTone(uint16_t tone){uint32_t r=rd32(TONE1);r=(r&~K_MASK)|(tone&K_MASK);wr32(TONE1,r);memw_fsk();}
uint8_t RH_ESP8266FSK::ham74Encode(uint8_t value){return ESP8266FSKCodec::encode(value);}
uint8_t RH_ESP8266FSK::ham74Decode(uint8_t code,uint8_t& corrected){return ESP8266FSKCodec::decode(code,corrected);}
uint16_t RH_ESP8266FSK::crc16(const uint8_t* data,uint8_t len){return ESP8266FSKCodec::crc16(data,len);}
// Legacy diagnostics do not provide addressed, CRC-checked acknowledgements.
bool RH_ESP8266FSK::supportsAckBeacon(){return false;}
bool RH_ESP8266FSK::sendAckBeacon(uint8_t,uint16_t){return false;}
bool RH_ESP8266FSK::waitAckBeacon(uint16_t){return false;}
void RH_ESP8266FSK::appendBit(bool bit){
  if(bit)_wire[_wireBits>>3]|=1u<<(7-(_wireBits&7));
  _wireBits++;
}
bool RH_ESP8266FSK::send(const uint8_t* data,uint8_t len){
  if(!_radioReady||_mode==RHModeTx||(!data&&len)||len>maxMessageLength())return false;
  resetReceiver();
  uint8_t raw[7+RH_ESP8266FSK_MAX_MESSAGE_LEN];
  uint8_t n=0;
  raw[n++]=len+RH_ESP8266FSK_HEADER_LEN;
  raw[n++]=_txHeaderTo;raw[n++]=_txHeaderFrom;
  raw[n++]=_txHeaderId;raw[n++]=_txHeaderFlags;
  for(uint8_t i=0;i<len;i++)raw[n++]=data[i];
  uint16_t check=crc16(raw,n);
  raw[n++]=check>>8;raw[n++]=check&0xff;
  memset(_wire,0,sizeof(_wire));_wireBits=0;
  for(uint8_t i=0;i<_preambleBits;i++)appendBit(i&1);
  for(int8_t i=11;i>=0;i--)appendBit((0xb38>>i)&1);
  uint8_t hi=ham74Encode(raw[0]>>4),lo=ham74Encode(raw[0]&15);
  for(int8_t k=6;k>=0;k--)appendBit((hi>>k)&1);
  for(int8_t k=6;k>=0;k--)appendBit((lo>>k)&1);
  for(uint8_t i=1;i<n;i+=4){
    uint8_t block[7],count=n-i;
    if(count>4)count=4;
    ESP8266FSKCodec::encodeBlock(raw+i,count,block);
    for(uint8_t row=0;row<7;row++)for(int8_t bit=7;bit>=0;bit--)appendBit((block[row]>>bit)&1);
  }
  if(!initTxPath()){initRxPath();return false;}
  _lastTxAskField=(rd32(TONE1)&SCALE_MASK)>>SCALE_SHIFT;
  _mode=RHModeTx;_txSucceeded=false;_txStage=0;_txRepeatIndex=0;
  _wireIndex=0;_txDeadline=micros()+_calUs;
  return true;
}
void RH_ESP8266FSK::serviceTx(){
  if(_mode!=RHModeTx)return;
  uint32_t now=micros();
  if((int32_t)(now-_txDeadline)<0)return;
  if(_txStage==0){
    txTone(_toneOne);_txStage=1;_txDeadline=now+_calUs;return;
  }
  if(_txStage==3){
    if(!initTxPath()){initRxPath();_mode=RHModeIdle;_stats.txAborts++;return;}
    _txStage=0;_wireIndex=0;_txDeadline=micros()+_calUs;return;
  }
  if(_txStage==1){_txStage=2;_txDeadline=now;}
  uint32_t late=now-_txDeadline;
  if(late>_stats.txLateUs)_stats.txLateUs=late;
  if(late>_bitUs/2){
    initRxPath();_mode=RHModeIdle;_stats.txAborts++;return;
  }
  if(_wireIndex<_wireBits){
    bool bit=(_wire[_wireIndex>>3]>>(7-(_wireIndex&7)))&1;
    txTone(bit?_toneOne:_toneZero);
    _wireIndex++;_txDeadline+=_bitUs;return;
  }
  initRxPath();
  if(++_txRepeatIndex<_txRepeats){_txStage=3;_txDeadline=micros()+(uint32_t)_gapMs*1000;return;}
  _txGood++;_txSucceeded=true;_mode=RHModeIdle;
  resetReceiver();
  _nextSample=micros()+(uint32_t)_gapMs*1000;
}
void RH_ESP8266FSK::setMode(RHMode requested){
  // Only send() may enter TX: an enum change alone cannot start/stop RF.
  if(requested==RHModeTx)return;
  if(_mode==RHModeTx){
    initRxPath();_txSucceeded=false;_stats.txAborts++;
    resetReceiver();_nextSample=micros()+(uint32_t)_gapMs*1000;
  }
  _mode=requested==RHModeRx?RHModeRx:RHModeIdle;
}
bool RH_ESP8266FSK::sleep(){
  setMode(RHModeIdle);
  return false; // PHY power-down is not implemented; never report fake sleep.
}
void RH_ESP8266FSK::poll(){if(_mode==RHModeTx)serviceTx();else available();}
bool RH_ESP8266FSK::waitPacketSent(){
  while(_mode==RHModeTx){serviceTx();yield();}
  return _txSucceeded;
}
bool RH_ESP8266FSK::waitPacketSent(uint16_t timeout){
  uint32_t started=millis();
  while(_mode==RHModeTx){
    if((uint32_t)(millis()-started)>=timeout){
      initRxPath();_mode=RHModeIdle;_txSucceeded=false;_stats.txAborts++;return false;
    }
    serviceTx();yield();
  }
  return _txSucceeded;
}
struct IqSample {uint32_t t;int32_t i,q;uint32_t e;};
static bool IRAM_ATTR __attribute__((noinline)) sampleIq(IqSample* s,uint8_t count,uint32_t cpuMHz,uint8_t mode){
  volatile uint32_t* ctrl=reinterpret_cast<volatile uint32_t*>(IQ_CTRL);
  uint32_t cfg=cfgKeep|((uint32_t)4<<2)|(mode?IQ_MODE:0),savedPS;
  // A short fixed-cadence burst keeps aliased tones independent of flash layout
  // and ISR latency. Restore the caller's interrupt state on every exit.
  __asm__ volatile("rsil %0, 15" : "=a"(savedPS) :: "memory");
  uint32_t next=ESP.getCycleCount();
  bool ok=true;
  for(uint8_t k=0;k<count;k++){
    *ctrl=cfg;*ctrl=cfg|IQ_ENABLE;
    __asm__ volatile("memw" ::: "memory");
    while(int32_t(ESP.getCycleCount()-next)<0){}
    s[k].t=ESP.getCycleCount();
    *ctrl=cfg|IQ_ENABLE|IQ_START;
    __asm__ volatile("memw" ::: "memory");
    while(!(*ctrl&IQ_DONE31)){
      if(uint32_t(ESP.getCycleCount()-s[k].t)>100*cpuMHz){ok=false;break;}
    }
    if(!ok)break;
    s[k].i=*reinterpret_cast<volatile int32_t*>(IQ_DCI);
    s[k].q=*reinterpret_cast<volatile int32_t*>(IQ_DCQ);
    s[k].e=*reinterpret_cast<volatile uint32_t*>(IQ_E4);
    *ctrl=cfg|IQ_ENABLE;*ctrl=cfg;
    __asm__ volatile("memw" ::: "memory");
    next=s[k].t+3*cpuMHz;
  }
  *ctrl=cfg;
  __asm__ volatile("memw; wsr %0, ps; rsync" :: "a"(savedPS) : "memory");
  return ok;
}
bool RH_ESP8266FSK::acquireFreq(FreqEst& out){
  static const uint8_t N=8;
  IqSample s[N];uint32_t cpuMHz=ESP.getCpuFreqMHz();
  if(!sampleIq(s,N,cpuMHz,_iqMode)){out.valid=false;return false;}
  uint32_t sampledAt=micros();
  float ss=0,cc=0;uint32_t dtSum=0,dtMin=UINT32_MAX,dtMax=0;uint8_t pairs=0;uint64_t energy=0;
  for(uint8_t k=0;k<N;k++)energy+=s[k].e;
  for(uint8_t k=1;k<N;k++){
    uint32_t dt=s[k].t-s[k-1].t;
    if(!dt || dt>100*cpuMHz)continue;
    if(dt<dtMin)dtMin=dt;
    if(dt>dtMax)dtMax=dt;
    float cross=float(s[k-1].i)*float(s[k].q)-float(s[k-1].q)*float(s[k].i);
    float dot=float(s[k-1].i)*float(s[k].i)+float(s[k-1].q)*float(s[k].q);
    // Bounded amplitude weighting preserves phase without per-pair trig calls.
    float norm=fabsf(cross)+fabsf(dot);
    if(norm==0)continue;
    float scale=1.0f/norm;
    ss+=cross*scale;cc+=dot*scale;dtSum+=dt;pairs++;
  }
  if(pairs<4 || !dtSum){out.valid=false;return false;}
  float ph=atan2f(ss,cc),dt=float(dtSum)/(cpuMHz*pairs);
  out.t=sampledAt;out.e4=energy/N;
  out.hz=(int32_t)lroundf(ph/(2.0f*float(M_PI)*dt*1e-6f));
  _stats.sampleStepNs=dtSum*1000/(cpuMHz*pairs);
  _stats.sampleJitterNs=(dtMax-dtMin)*1000/cpuMHz;
  _stats.lastEnergy=out.e4;_stats.lastHz=out.hz;
  out.valid=true;return true;
}
void RH_ESP8266FSK::resetReceiver(){
  if(_candidateLen)_stats.candidateDrops++;
  _candidateLen=0;
  memset(_decoders,0,sizeof(_decoders));
  memset(_scores,0,sizeof(_scores));
  memset(_counts,0,sizeof(_counts));
  _trainingN=0;_trainingPos=0;_trained=false;
  _epoch=0;_bin=0;_nextSample=0;
  if(_calMemoryMs && _lastCal.valid && (uint32_t)(millis()-_lastCal.atMs)<_calMemoryMs){
    _low=min(_lastCal.zeroHz,_lastCal.oneHz);
    _high=max(_lastCal.zeroHz,_lastCal.oneHz);
    _trained=true;_lockAt=micros();
  }
}
bool RH_ESP8266FSK::train(int32_t hz){
  _training[_trainingPos++]=hz;
  _trainingPos%=16;
  if(_trainingN<16)_trainingN++;
  if(_trainingN<16||(_trainingPos%4))return false;
  int32_t sorted[16];memcpy(sorted,_training,sizeof(sorted));
  for(uint8_t i=1;i<16;i++){int32_t v=sorted[i];int8_t j=i-1;while(j>=0&&sorted[j]>v){sorted[j+1]=sorted[j];j--;}sorted[j+1]=v;}
  int32_t lo=(sorted[3]+sorted[4])/2,hi=(sorted[11]+sorted[12])/2;
  int32_t sep=hi-lo;
  int32_t spread=sep/3;if(spread>20000)spread=20000;
  if(sep<_minToneSepHz||sorted[6]-sorted[1]>spread||sorted[14]-sorted[9]>spread)return false;
  bool firstLock=!_trained;
  if(!_trained || labs(lo-_low)>sep/4 || labs(hi-_high)>sep/4){
    memset(_decoders,0,sizeof(_decoders));
    memset(_scores,0,sizeof(_scores));memset(_counts,0,sizeof(_counts));
    _epoch=0;_bin=0;
  }
  _low=lo;_high=hi;_trained=true;if(firstLock)_lockAt=micros();
  return true;
}
bool RH_ESP8266FSK::decodeBit(Decoder& d,bool bit,uint8_t* raw,uint8_t* rawLen){
  if(!d.active){
    d.shift=((d.shift<<1)|bit)&0xffffff;
    uint32_t normal=d.shift^0x555b38u,inverse=d.shift^0xaaa4c7u;
    bool normalMatch=__builtin_popcount(normal&0xfff)<=1&&__builtin_popcount(normal>>12)<=2;
    bool inverseMatch=__builtin_popcount(inverse&0xfff)<=1&&__builtin_popcount(inverse>>12)<=2;
    if(!normalMatch&&!inverseMatch)return false;
    d.inverted=inverseMatch;
    _stats.syncs++;
    // A tone lock may precede a TDM slot; only sync starts the frame deadline.
    _lockAt=micros();
    d.active=true;d.code=0;d.codeBits=0;d.nibble=0;d.count=0;d.expected=0;d.corrections=0;
    return false;
  }
  bit^=d.inverted;
  if(d.count==0){
    d.code=(d.code<<1)|bit;
    if(++d.codeBits<7)return false;
    uint8_t corrected=0,nib=ham74Decode(d.code,corrected);
    d.corrections+=corrected;d.code=0;d.codeBits=0;
    if(!d.nibble){d.high=nib;d.nibble=1;return false;}
    d.raw[0]=(d.high<<4)|nib;d.count=1;d.nibble=0;
    if(d.raw[0]<RH_ESP8266FSK_HEADER_LEN||d.raw[0]>RH_ESP8266FSK_HEADER_LEN+RH_ESP8266FSK_MAX_MESSAGE_LEN){
      d.active=false;d.shift=0;return false;
    }
    d.expected=d.raw[0]+3;memset(d.block,0,sizeof(d.block));return false;
  }
  if(bit)d.block[d.codeBits>>3]|=1u<<(7-(d.codeBits&7));
  if(++d.codeBits<56)return false;
  uint8_t bytes[4],corrected=0;
  ESP8266FSKCodec::decodeBlock(d.block,bytes,corrected);
  d.corrections+=corrected;d.codeBits=0;memset(d.block,0,sizeof(d.block));
  if(d.corrections>_maxCorrections){_stats.correctionRejects++;d.active=false;d.shift=0;return false;}
  for(uint8_t i=0;i<4&&d.count<d.expected;i++)d.raw[d.count++]=bytes[i];
  if(d.count<d.expected)return false;
  d.active=false;d.shift=0;
  if(crc16(d.raw,d.count-2)!=((uint16_t)d.raw[d.count-2]<<8|d.raw[d.count-1])){_rxBad++;return false;}
  _stats.decodedCandidates++;
  memcpy(raw,d.raw,d.count);*rawLen=d.count;
  _lastCal={true,d.inverted?_high:_low,d.inverted?_low:_high,_high-_low,d.inverted,millis()};
  _lastCorrections=d.corrections;
  _lastReceiveUs=micros();
  return true;
}
bool RH_ESP8266FSK::receiveRaw(uint8_t* raw,uint8_t* rawLen){
  uint32_t now=micros();
  if(_candidateLen && uint32_t(now-_candidateAt)>=_bitUs)return finishCandidate(raw,rawLen);
  if((int32_t)(now-_nextSample)<0)return false;
  uint32_t period=_trained?_bitUs/6:2000;
  _nextSample=now+period;
  FreqEst f;
  _stats.samples++;
  if(!acquireFreq(f)||!f.valid)return false;
  bool active=false;
  for(uint8_t i=0;i<8;i++)active|=_decoders[i].active;
  // A CRC-valid candidate must survive until its timing-phase comparison ends.
  if(!_candidateLen&&!active&&_trained&&(uint32_t)(micros()-_lockAt)>600000){resetReceiver();return false;}
  if(!active && !_candidateLen)train(f.hz);
  if(!_trained)return false;
  if(!_candidateLen&&(uint32_t)(micros()-_lockAt)>(uint32_t)(90+56*((RH_ESP8266FSK_MAX_MESSAGE_LEN+9)/4))*_bitUs){resetReceiver();return false;}
  if(!_epoch){_epoch=f.t;_bin=0;}
  uint32_t bin=(uint32_t)(f.t-_epoch)*8/_bitUs;
  if(bin-_bin>24){_stats.slowPolls++;resetReceiver();return false;}
  while(_bin<bin){
    int16_t score=0;uint8_t count=0;
    for(uint8_t k=0;k<5;k++){uint8_t idx=(_bin-k)&7;score+=_scores[idx];count+=_counts[idx];}
    Decoder& d=_decoders[_bin&7];
    if(count){
      if(decodeBit(d,score>0,raw,rawLen)){
        // Compare adjacent timing phases before using FEC corrections as quality.
        if(!_candidateLen || _lastCorrections<_candidateCorrections){
          if(!_candidateLen)_candidateAt=_lastReceiveUs;
          _candidateLen=*rawLen;memcpy(_candidate,raw,*rawLen);
          _candidateCorrections=_lastCorrections;_candidateCal=_lastCal;
        }
        if(!_candidateCorrections)return finishCandidate(raw,rawLen);
      }
    }else{d.active=false;d.shift=0;}
    _bin++;
    _scores[_bin&7]=0;_counts[_bin&7]=0;
  }
  int32_t sep=_high-_low,mid=_low+sep/2;
  if(f.hz>=_low-sep/3 && f.hz<=_high+sep/3){
    int32_t confidence=(f.hz-mid)*256/sep;
    if(confidence>128)confidence=128;
    if(confidence<-128)confidence=-128;
    _scores[_bin&7]+=(int16_t)confidence;
    _counts[_bin&7]++;
  }
  return false;
}
bool RH_ESP8266FSK::finishCandidate(uint8_t* raw,uint8_t* rawLen){
  *rawLen=_candidateLen;memcpy(raw,_candidate,_candidateLen);
  _lastCorrections=_candidateCorrections;_lastCal=_candidateCal;_lastReceiveUs=_candidateAt;
  _candidateLen=0;
  resetReceiver();
  return true;
}
bool RH_ESP8266FSK::available(){
  if(_mode==RHModeTx){serviceTx();return false;}
  if(_bufValid)return true;
  _mode=RHModeRx;
  uint8_t raw[1+RH_ESP8266FSK_HEADER_LEN+RH_ESP8266FSK_MAX_MESSAGE_LEN+2],rawLen=0;
  uint32_t started=micros();bool received=receiveRaw(raw,&rawLen);
  uint32_t spent=micros()-started;_stats.polls++;_stats.workUs+=spent;
  if(spent>_stats.maxPollUs)_stats.maxPollUs=spent;
  if(!received){_mode=RHModeIdle;return false;}
  uint8_t payloadLen=raw[0];
  _rxHeaderTo=raw[1];_rxHeaderFrom=raw[2];_rxHeaderId=raw[3];_rxHeaderFlags=raw[4];
  _stats.crcFrames++;_stats.lastTo=_rxHeaderTo;_stats.lastFrom=_rxHeaderFrom;_stats.lastLength=payloadLen;
  if(!_promiscuous&&_rxHeaderTo!=_thisAddress&&_rxHeaderTo!=RH_BROADCAST_ADDRESS){
    _stats.addressRejects++;_mode=RHModeIdle;return false;
  }
  _bufLen=payloadLen-RH_ESP8266FSK_HEADER_LEN;memcpy(_buf,raw+5,_bufLen);
  _bufValid=true;_rxGood++;_mode=RHModeIdle;return true;
}
bool RH_ESP8266FSK::recv(uint8_t* buf,uint8_t* len){
  if(!available())return false;
  if(buf&&len){
    if(*len>_bufLen)*len=_bufLen;
    memcpy(buf,_buf,*len);
  }
  _bufValid=false;
  return true;
}

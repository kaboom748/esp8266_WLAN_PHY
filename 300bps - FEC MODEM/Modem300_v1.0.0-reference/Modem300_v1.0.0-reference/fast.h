#pragma once
#ifndef FAST_BPS
#define FAST_BPS 1
#endif
#ifndef FAST_END_REPORT
#define FAST_END_REPORT 0
#endif
#ifndef FAST_SPS
#define FAST_SPS 8
#endif
#ifndef FAST_IQ
#define FAST_IQ 0
#endif
#ifndef FAST_FAULT
#define FAST_FAULT 0
#endif
#ifndef FAST_MATCH
#define FAST_MATCH 0
#endif
#ifndef FAST_GAIN
#define FAST_GAIN -1
#endif
#ifndef FAST_BB_GAIN
#define FAST_BB_GAIN 2
#endif
#ifndef FAST_INTEGER
#define FAST_INTEGER 0
#endif
#ifndef FAST_CHANNEL
#define FAST_CHANNEL 6
#endif
#ifndef FAST_TX_CHANNEL
#define FAST_TX_CHANNEL FAST_CHANNEL
#endif
#ifndef FAST_SYNC_LIMIT
#define FAST_SYNC_LIMIT 1
#endif
#ifndef FAST_ACCUM
#define FAST_ACCUM 31
#endif
#ifndef FAST_SCAN
#define FAST_SCAN 0
#endif
#ifndef FAST_PHASE_DIVERSITY
#define FAST_PHASE_DIVERSITY 0
#endif
#ifndef FAST_QUIET_SDK
#define FAST_QUIET_SDK 0
#endif
#ifndef FAST_HOST_TEST
extern "C" void pp_disable_noise_timer(void);
#endif

namespace Fast {
constexpr unsigned FS = FAST_FS, SPS = FAST_SPS;
constexpr unsigned WIRE = 18, BITS = WIRE * 8, SYNC_LEN = 24;
constexpr unsigned BPS = FAST_BPS;
constexpr unsigned SYNC_AT = 64 / BPS, SYMBOLS = BITS / BPS + SYNC_LEN;
constexpr unsigned TAPS = 10*SPS+1, DELAY = 5*SPS, CENTER = 13*SPS/2;
constexpr unsigned RX_DELAY = FAST_MATCH?7:SPS/2;
constexpr unsigned UNIT_HZ = FAST_INTEGER==2?78125/3:FAST_INTEGER?78125/6:648UL*FS*8/(38400*SPS);
constexpr unsigned HZ_SCALE = FS<=262144?64:128;
constexpr unsigned SYNC_RADIUS = 3*SPS;
constexpr unsigned TX_N = (SYMBOLS + 12) * SPS;
constexpr unsigned EARLY_US = 150;
constexpr unsigned RX_N = TX_N + (2 * EARLY_US * FS + 999999) / 1000000 + 32;
constexpr unsigned SEARCH_N = SPS<=4?1024:2048;
constexpr unsigned MAX_N = SEARCH_N > RX_N ? SEARCH_N : RX_N;
constexpr uint32_t HALF = FAST_HALF_US, CYCLE = HALF * 2;
constexpr uint32_t DEADLINE_US = 33333;
constexpr uint32_t RELEASE_GUARD_US = 3500;
constexpr uint32_t RELEASE_JITTER_US = 2000;
constexpr uint32_t RELEASE_TARGET_US = DEADLINE_US-RELEASE_GUARD_US;
constexpr uint32_t FINAL_LIMIT_US = RELEASE_TARGET_US+RELEASE_JITTER_US;
static_assert(FINAL_LIMIT_US==31833, "do not extend the original absolute output deadline");
constexpr unsigned MAX_ATTEMPTS = 4;
constexpr int16_t NO_TUNE = 32767;
static_assert(FAST_INTEGER || FS % 128 == 0, "phase conversion uses exact integer scaling");
static_assert(!FAST_INTEGER || BPS==1, "integer tones use binary FSK");
static_assert(TX_N * 1000000ULL / FS + 1000 < HALF, "slot needs processing headroom");

struct Packet {
  uint8_t role = 0, seq = 0, ack = 0, byte = 0, age = 0;
  uint16_t boot = 0, ackBoot = 0;
  bool data = false, ackValid = false;
  int16_t txHz = 0, tuneHz = NO_TUNE;
  uint16_t periodUs = 33333;
};

inline void encode(const Packet& p, uint8_t w[WIRE]) {
  memset(w, 0, WIRE);
  w[0] = uint8_t((FAST_INTEGER?0xac:0xa8) | (p.role - 1));
  w[1] = uint8_t((p.data ? 1 : 0) | (p.ackValid ? 2 : 0));
  w[2] = p.seq; w[3] = p.ack;
  w[4] = uint8_t(p.boot); w[9] = uint8_t(p.boot >> 8);
  w[5] = p.data ? p.byte : uint8_t(p.ackBoot);
  w[6] = uint8_t(int8_t(p.txHz / 128));
  w[7] = p.tuneHz == NO_TUNE ? 127 : uint8_t(int8_t(p.tuneHz / 128));
  if(FAST_INTEGER){w[6]=uint8_t(int8_t((int(p.periodUs)-33333)/4));w[7]=0x7e;}
  w[8] = p.data ? p.age : uint8_t(p.ackBoot>>8);
  AirModem::put32(w + 10, AirModem::crc32(w, 10));
  AirModem::Rs14::encode(w, w + 14);
}

enum DecodeResult { DECODE_OK, DECODE_FEC, DECODE_HEADER, DECODE_CRC, DECODE_META };
inline bool decode(uint8_t w[WIRE], Packet& p, uint8_t& fixed, DecodeResult* why=nullptr) {
  auto reject=[&](DecodeResult reason){if(why)*why=reason;return false;};
  if (!AirModem::Rs14::repair(w, fixed)) return reject(DECODE_FEC);
  if ((w[0] & 0xfe) != (FAST_INTEGER?0xac:0xa8) || (w[1] & ~3)) return reject(DECODE_HEADER);
  if (AirModem::get32(w + 10) != AirModem::crc32(w, 10)) return reject(DECODE_CRC);
  p.role = (w[0] & 1) + 1; p.data = w[1] & 1; p.ackValid = w[1] & 2;
  p.seq = w[2]; p.ack = w[3]; p.boot = uint16_t(w[4]) | uint16_t(w[9]) << 8;
  p.byte = p.data?w[5]:0; p.age = p.data?w[8]:0;
  p.ackBoot = p.data?0:uint16_t(w[5])|uint16_t(w[8])<<8;
  p.txHz = int8_t(w[6]) * 128;
  p.tuneHz = w[7] == 127 ? NO_TUNE : int8_t(w[7]) * 128;
  if(FAST_INTEGER){
    p.periodUs=33333+int8_t(w[6])*4;p.txHz=0;p.tuneHz=NO_TUNE;
    if(w[7]!=0x7e || p.periodUs<33000 || p.periodUs>33665)return reject(DECODE_META);
  }
  if(!p.boot || abs(p.txHz)>12000 || (p.tuneHz!=NO_TUNE && abs(p.tuneHz)>12000))return reject(DECODE_META);
  if(why)*why=DECODE_OK;
  return true;
}

inline void scramble(uint8_t w[WIRE]) {
  unsigned lfsr=0x1ff;
  for(unsigned n=0;n<WIRE;++n)for(unsigned b=0;b<8;++b) {
    w[n]^=(lfsr&1)<<b;
    lfsr=(lfsr>>1)|(((lfsr^(lfsr>>5))&1)<<8);
  }
}

#ifndef FAST_HOST_TEST
struct UartRx {
  struct Item { uint32_t at; uint8_t byte; };
  static volatile Item queue[4];
  static volatile uint8_t head,tail;
  static volatile uint32_t errors,overflow,received;
  static volatile uint32_t gapDrops,batchDrops,hardwareErrors,maxObservationGap;
  static uint32_t lastAt,periodMin,periodMax;
  static uint32_t periodSum,periodCount;
  static uint32_t clockSum,clockCount;
  static volatile uint32_t periodUs;
  static uint32_t lastPoll;
  static void IRAM_ATTR pump() {
    if(FAST_BENCH)return;
    const uint32_t now=micros(),gap=now-lastPoll;
    lastPoll=now;
    const unsigned count=U0S&255;
    if(!count)return;
    if(gap>maxObservationGap)maxObservationGap=gap;
    const uint32_t flags=U0IR&((1<<UIOF)|(1<<UIFR)|(1<<UIPE)|(1<<UIBD));
    if(count>1 || gap>1000 || flags) {
      if(gap>1000)++gapDrops;
      if(count>1)++batchDrops;
      if(flags)++hardwareErrors;
      U0C0|=1<<UCRXRST;U0C0&=~(1<<UCRXRST);U0IC=0xffff;++errors;return;
    }
    const uint8_t b=U0F;
    const uint32_t at=micros();
    const uint32_t period=at-lastAt;lastAt=at;
    if(period>20000 && period<50000){if(period<periodMin)periodMin=period;if(period>periodMax)periodMax=period;periodSum+=period;++periodCount;}
    if(period>32000 && period<35000){
      clockSum+=period;
      if(++clockCount==8){
        const uint32_t mean=(clockSum+4)/8;
        if(mean>=33000 && mean<=33665)periodUs=mean;
        clockSum=clockCount=0;
      }
    }else clockSum=clockCount=0;
    U0IC=0xffff;
    ++received;
    const uint8_t next=(head+1)&3;
    if(next==tail){++overflow;return;}
    queue[head].at=at;queue[head].byte=b;
    __asm__ volatile("memw" ::: "memory");
    head=next;
  }
  static bool pop(uint8_t& b,uint32_t& at) {
    noInterrupts();
    if(head==tail){interrupts();return false;}
    b=queue[tail].byte;at=queue[tail].at;tail=(tail+1)&3;
    interrupts();return true;
  }
};
volatile UartRx::Item UartRx::queue[4]={};
volatile uint8_t UartRx::head=0,UartRx::tail=0;
volatile uint32_t UartRx::errors=0,UartRx::overflow=0,UartRx::received=0;
volatile uint32_t UartRx::gapDrops=0,UartRx::batchDrops=0,UartRx::hardwareErrors=0,UartRx::maxObservationGap=0;
uint32_t UartRx::lastPoll=0;
uint32_t UartRx::lastAt=0,UartRx::periodMin=UINT32_MAX,UartRx::periodMax=0;
uint32_t UartRx::periodSum=0,UartRx::periodCount=0;
uint32_t UartRx::clockSum=0,UartRx::clockCount=0;
volatile uint32_t UartRx::periodUs=33333;

struct Release {
  // SDK channel changes skew CPU-cycle timing; UART uses wall time instead.
  static volatile bool armed;
  static volatile uint32_t dueUs, delivered, dropped, lateMax;
  static volatile uint32_t timerDrop,shiftDrop,fifoDrop,pastDrop,waitMax;
  static volatile uint8_t byte;
  static uint32_t lastStart;
  static uint32_t lastOutputUs;
  static bool haveStart;
  static volatile uint32_t wantedPeriod;
  static uint32_t byteUs,divider;
  static void IRAM_ATTR pump() {
    UartRx::pump();
    if(!armed)return;
    const int32_t late=int32_t(UartRx::lastPoll-dueUs);
    if(late<0)return;
    if(uint32_t(late)>lateMax)lateMax=late;
    if(late>int32_t(RELEASE_JITTER_US)){armed=false;++dropped;++timerDrop;return;}
    if(!FAST_BENCH) {
      const uint32_t nowUs=micros();
      if(uint32_t(nowUs-lastOutputUs)>100000)haveStart=false;
      uint32_t start=nowUs;
      // Bound the UART shift-register wait as well as the software timer.
      if(haveStart && int32_t(lastStart+byteUs-start)>0)start=lastStart+byteUs;
      if(int32_t(start-dueUs)>int32_t(waitMax))waitMax=start-dueUs;
      if(int32_t(start-dueUs)>int32_t(RELEASE_JITTER_US)){armed=false;++dropped;++shiftDrop;return;}
      if(int32_t(start-micros())>0)return;
      volatile uint32_t* status=reinterpret_cast<volatile uint32_t*>(0x6000001cu);
      if(((*status>>16)&255)!=0){armed=false;++dropped;++fifoDrop;return;}
      if(FAST_INTEGER){
        // Recovered peer cadence, with 80 us/character margin (<0.25%).
        // Change the divider only between characters; UART framing stays 8N1.
        divider=(wantedPeriod-80)*8;U0D=divider;byteUs=(divider+7)/8+2;
      }
      if(int32_t(micros()-dueUs)>int32_t(RELEASE_JITTER_US)-10){armed=false;++dropped;++timerDrop;return;}
      *reinterpret_cast<volatile uint32_t*>(0x60000000u)=byte;
      lastStart=micros();lastOutputUs=nowUs;haveStart=true;
    }
    armed=false;
    ++delivered;
  }
  static void schedule(uint8_t b,uint32_t at) {
    noInterrupts();
    const int32_t delayUs=int32_t(at-micros());
    if(delayUs<=-int32_t(RELEASE_JITTER_US)){++dropped;++pastDrop;interrupts();return;}
    byte=b;dueUs=at;
    armed=true;
    interrupts();
  }
};
volatile bool Release::armed=false;
volatile uint32_t Release::dueUs=0,Release::delivered=0,Release::dropped=0,Release::lateMax=0;
volatile uint32_t Release::timerDrop=0,Release::shiftDrop=0,Release::fifoDrop=0,Release::pastDrop=0,Release::waitMax=0;
volatile uint8_t Release::byte=0;
uint32_t Release::lastStart=0;
uint32_t Release::lastOutputUs=0;
bool Release::haveStart=false;
volatile uint32_t Release::wantedPeriod=33333;
uint32_t Release::byteUs=33336,Release::divider=266667;

class Radio : private RH_ESP8266FSK {
 public:
  Radio():RH_ESP8266FSK(FAST_CHANNEL){}
  struct Stats {
    uint32_t tx = 0, rx = 0, txAbort = 0, rxAbort = 0, invalid = 0;
    uint32_t prepMax = 0, setupMax = 0, restoreMax = 0, dspMax = 0;
    uint32_t lateCycles = 0, corrected = 0, candidates = 0;
    uint32_t demodMax = 0, filterMax = 0, searchMax = 0;
    uint32_t noSync=0, fecReject=0, headerReject=0, crcReject=0, metaReject=0;
    uint32_t captureLateMax=0, captureLateCount=0, noSyncLate=0;
    uint32_t edgeSync=0, validLate=0;
    uint32_t signalGood=0, signalBad=0;
    uint32_t adcMax=0,adcTimeout=0;
    uint32_t setupClockDeficit=0,restoreClockDeficit=0;
  } stats;
  bool begin() {
    if (ESP.getCpuFreqMHz() != 160) return false;
    reconnectBefore=wifi_station_get_reconnect_policy();
    if(!wifi_station_set_reconnect_policy(false))return false;
    setTones(0, 1);
    if (!init()) return false;
    // Internal SDK 3.1.2-core hook; keep our per-frame carrier/DC calibration.
    if(FAST_QUIET_SDK)pp_disable_noise_timer();
    if(!setReceiveGain(FAST_GAIN,FAST_BB_GAIN))return false;
    for(unsigned n=1;n<256;++n)reciprocal[n]=65535/n;
    for(unsigned k=0;k<SYNC_LEN;++k)syncLevels[k]=AirModem::Phy::syncExtreme(k);
    const double pi = 3.14159265358979323846, beta = .2;
    double taps[TAPS], sum = 0;
    for (unsigned k = 0; k < TAPS; ++k) {
      const double t = (int(k) - int(DELAY)) / double(SPS);
      double v;
      if (fabs(t) < 1e-9) v = 1 + beta * (4 / pi - 1);
      else if (fabs(fabs(4 * beta * t) - 1) < 1e-9)
        v = beta / sqrt(2.0) * ((1 + 2 / pi) * sin(pi / (4 * beta)) +
            (1 - 2 / pi) * cos(pi / (4 * beta)));
      else v = (sin(pi*t*(1-beta)) + 4*beta*t*cos(pi*t*(1+beta))) /
          (pi*t*(1-16*beta*beta*t*t));
      taps[k] = v; sum += v;
    }
    for (unsigned k = 0; k < TAPS; ++k) {
      rxTaps[k] = lround(taps[k] * 32768.0 / sum);
      txTaps[k] = lround(taps[k] * SPS / sum * UNIT_HZ * 65536.0*1024/78125);
    }
    for(unsigned p=0;p<SPS;++p)for(unsigned k=0;k<11;++k)
      txPoly[p][k]=p+k*SPS<TAPS?txTaps[p+k*SPS]:0;
    return true;
  }
  void IRAM_ATTR prepare(const Packet& p) {
    const uint32_t began = micros();
    uint8_t w[WIRE]; encode(p, w);scramble(w);
    int8_t padded[SYMBOLS+22]={};int8_t* levels=padded+11;
    for (unsigned s = 0; s < SYMBOLS; ++s) {
      if (s >= SYNC_AT && s < SYNC_AT + SYNC_LEN)
        levels[s] = AirModem::Phy::syncExtreme(s - SYNC_AT);
      else {
        const unsigned b = (s < SYNC_AT ? s : s - SYNC_LEN)*BPS;
        if(BPS==1)levels[s] = (w[b/8] & (1 << (7-b%8))) ? 3 : -3;
        else levels[s]=AirModem::Phy::symbol((w[b/8]>>(6-b%8))&3);
      }
    }
    const int32_t offset = int64_t(p.txHz) * 65536 / 78125;
    for (unsigned n = 0; n < TX_N; ++n) {
      if(FAST_INTEGER){
        const int s=(int(n)-int(CENTER-SPS/2))/int(SPS);
        buffer[n]=(s>=0 && s<int(SYMBOLS))?(levels[s]>0?65536:(FAST_INTEGER==2?-65536:0)):0;
        continue;
      }
      int32_t acc = 0;
      const int u=int(n)-int(CENTER-DELAY);
      if(u>=0) {
        const int8_t* symbols=levels+u/SPS;
        const int32_t* taps=txPoly[u%SPS];
        #pragma GCC unroll 11
        for(unsigned k=0;k<11;++k)acc+=int32_t(symbols[-int(k)])*taps[k];
      }
      buffer[n] = offset + (acc + (acc < 0 ? -512 : 512))/1024;
    }
    peak(stats.prepMax, micros()-began);
  }
  bool transmit(uint32_t at) {
    uint32_t began = micros();
    uint32_t cycles=ESP.getCycleCount();
    bool ok = FAST_TX_CHANNEL==FAST_CHANNEL || wifi_set_channel(FAST_TX_CHANNEL);
    if(ok)ok=initTxPath(false);
    const uint32_t setupUs=micros()-began,setupCpuUs=(ESP.getCycleCount()-cycles)/160;
    peak(stats.setupMax,setupUs);
    if(setupUs>setupCpuUs)peak(stats.setupClockDeficit,setupUs-setupCpuUs);
    if (ok) ok = transmitSamples(at);
    began = micros();cycles=ESP.getCycleCount();
    if(FAST_TX_CHANNEL!=FAST_CHANNEL && !wifi_set_channel(FAST_CHANNEL))ok=false;
    if (!initRxPath()) ok = false;
    const uint32_t restoreUs=micros()-began,restoreCpuUs=(ESP.getCycleCount()-cycles)/160;
    peak(stats.restoreMax,restoreUs);
    if(restoreUs>restoreCpuUs)peak(stats.restoreClockDeficit,restoreUs-restoreCpuUs);
    if (ok) ++stats.tx; else ++stats.txAbort;
    return ok;
  }
  bool receive(bool searching, Packet& packet, uint32_t& firstAt, int32_t& offsetHz,
               int32_t& unitHz, uint32_t scheduled=0) {
    const unsigned count = searching ? SEARCH_N : RX_N;
    uint32_t captured;
    if (!capture(count, captured)) { ++stats.rxAbort; return false; }
    if(FAST_IQ && !searching && iqState<2){memcpy(iq[iqState],buffer,RX_N*sizeof(int32_t));iqAt[iqState]=captured;}
    const uint32_t captureLate=searching?0:uint32_t(captured-scheduled);
    if(!searching){peak(stats.captureLateMax,captureLate);if(captureLate>200)++stats.captureLateCount;}
    const uint32_t began = micros();
    const int32_t coarse = demodulate(count);
    peak(stats.demodMax, micros()-began);
    const uint32_t filterBegan=micros();
    constexpr int32_t scale = HZ_SCALE;
    for (unsigned n=0;n<count;++n) buffer[n] /= scale;
    filter(count);
    peak(stats.filterMax,micros()-filterBegan);
    const uint32_t searchBegan=micros();
    Hyp best[3] = {};const unsigned nb=synchronize(searching,count,best);
    const unsigned expected=CENTER+RX_DELAY+EARLY_US*FS/1000000;
    const unsigned first=searching?TAPS-1-(DELAY-RX_DELAY):expected-SYNC_RADIUS;
    const unsigned limit=searching?count-(SYMBOLS-1)*SPS-(DELAY-RX_DELAY):expected+SYNC_RADIUS;
    stats.candidates += nb;
    if(!nb){++stats.noSync;if(captureLate>200)++stats.noSyncLate;}
    bool found=false;
    for(unsigned pass=0;pass<nb;++pass) {
      unsigned chosen=0;for(unsigned j=1;j<nb;++j)if(best[j].metric<best[chosen].metric)chosen=j;
      const Hyp h=best[chosen];best[chosen].metric=UINT32_MAX;
      uint8_t w[WIRE]={};
      for(unsigned b=0;b<BITS;b+=BPS) {
        const unsigned s=b/BPS<SYNC_AT?b/BPS:b/BPS+SYNC_LEN, at=h.start+s*SPS;
        int32_t v=SPS<=4?4*(int32_t(filtered[at])-h.offset):
          int32_t(filtered[at-1])+2*filtered[at]+filtered[at+1]-4*h.offset;
        if(h.unit<0)v=-v;
        if(BPS==1) {if(v>=0)w[b/8]|=1<<(7-b%8);}
        else {const unsigned d=(v<0?2:0)|(abs(v)>8*abs(h.unit)?1:0);w[b/8]|=d<<(6-b%8);}
      }
      uint8_t corrected;
      scramble(w);
      DecodeResult why;
      if(!decode(w,packet,corrected,&why)) {
        if(why==DECODE_FEC)++stats.fecReject;
        else if(why==DECODE_HEADER)++stats.headerReject;
        else if(why==DECODE_CRC)++stats.crcReject;
        else ++stats.metaReject;
        continue;
      }
      if(!searching && (h.start<first+4 || h.start+4>=limit))++stats.edgeSync;
      if(captureLate>200)++stats.validLate;
      stats.corrected+=corrected;
      firstAt=captured+int32_t(int64_t(int(h.start)-int(RX_DELAY)-int(CENTER))*1000000/FS);
      offsetHz=h.offset*scale+coarse;
      if(offsetHz>int32_t(FS/2))offsetHz-=FS;
      if(offsetHz<-int32_t(FS/2))offsetHz+=FS;
      unitHz=h.unit*scale;
      if(FAST_INTEGER==2)carrierHz=offsetHz;
      found=true;break;
    }
    peak(stats.searchMax,micros()-searchBegan);
    peak(stats.dspMax,micros()-began);
    if(found)++stats.rx;
    else {
      ++stats.invalid;
      if(FAST_END_REPORT){
        const unsigned bin=captured/100000;
        if(bin<FAST_DURATION*10 && badBins[bin]<255)++badBins[bin];
      }
    }
    if(FAST_IQ && !searching && ((iqState==0 && found) || (iqState==1 && !nb)))++iqState;
    return found;
  }
  void dumpIq(){
    if(FAST_IQ)for(unsigned j=0;j<iqState;++j){
      Serial.printf("IQ role=%u kind=%s fs=%u sps=%u bps=%u at=%u n=%u ",FAST_ROLE,j?"noSync":"valid",FS,SPS,BPS,iqAt[j],RX_N);
      for(unsigned n=0;n<RX_N;++n)Serial.printf("%08x",unsigned(iq[j][n]));
      Serial.println();
    }
  }
  void dumpTiming(){
    if(FAST_END_REPORT){
      Serial.printf("WIFI r=%u reconnectBefore=%u reconnectNow=%u autoConnect=%u channel=%u quietSdk=%u\n",
        FAST_ROLE,reconnectBefore,wifi_station_get_reconnect_policy(),wifi_station_get_auto_connect(),wifi_get_channel(),FAST_QUIET_SDK);
      Serial.flush();
      Serial.printf("CLOCK r=%u setupDeficitUs=%u restoreDeficitUs=%u\n",FAST_ROLE,stats.setupClockDeficit,stats.restoreClockDeficit);
      Serial.flush();
      for(unsigned first=0;first<FAST_DURATION*10;first+=50){
        Serial.printf("BAD100 r=%u first=%u counts=",FAST_ROLE,first);
        for(unsigned n=first;n<first+50 && n<FAST_DURATION*10;++n)Serial.printf("%02x",badBins[n]);
        Serial.println();Serial.flush();
      }
    }
  }
  void scan() {
    for(unsigned round=0;round<4;++round)for(unsigned channel=1;channel<=11;++channel){
      if(!wifi_set_channel(channel) || !initRxPath()){
        Serial.printf("SCAN r=%u round=%u channel=%u failed=1\n",FAST_ROLE,round,channel);continue;
      }
      delay(10);
      uint32_t powers[64],valid=0,failed=0;uint64_t total=0;
      for(unsigned block=0;block<64;++block){
        uint32_t at;if(!capture(512,at)){++failed;continue;}
        int32_t si=0,sq=0;uint64_t energy=0;
        for(unsigned n=0;n<512;++n){
          int32_t i=int16_t(buffer[n]),q=int16_t(uint32_t(buffer[n])>>16);
          si+=i;sq+=q;energy+=int64_t(i)*i+int64_t(q)*q;
        }
        int32_t di=si/512,dq=sq/512;
        uint32_t power=energy/512-int64_t(di)*di-int64_t(dq)*dq;
        total+=power;powers[valid++]=power;yield();
      }
      for(unsigned i=1;i<valid;++i){uint32_t value=powers[i];unsigned j=i;
        while(j && powers[j-1]>value){powers[j]=powers[j-1];--j;}powers[j]=value;}
      if(valid)Serial.printf("SCAN r=%u round=%u channel=%u blocks=%u failed=%u meanPower=%u medianPower=%u p95Power=%u maxPower=%u\n",
        FAST_ROLE,round,channel,valid,failed,unsigned(total/valid),powers[valid/2],powers[valid*95/100],powers[valid-1]);
      Serial.flush();
    }
  }
 private:
  int32_t buffer[MAX_N], txTaps[TAPS], txPoly[SPS][11];
  int16_t filtered[MAX_N], rxTaps[TAPS];
  int8_t syncLevels[SYNC_LEN];
  uint16_t reciprocal[256];
  int32_t iq[FAST_IQ?2:1][FAST_IQ?RX_N:1];
  uint32_t iqAt[2]={};unsigned iqState=0;
  int32_t carrierHz=0;
  uint8_t capturePhase=0;
  bool reconnectBefore=false;
  uint8_t badBins[FAST_END_REPORT?FAST_DURATION*10:1]={};
  struct Hyp {unsigned start;int32_t offset,unit;uint32_t metric;};
  void IRAM_ATTR filter(unsigned count) {
    if(FAST_MATCH){
      for(unsigned n=0;n<count;++n){
        if(n<12){filtered[n]=0;continue;}
        int32_t acc=int32_t(rxTaps[DELAY])*buffer[n-6];
        #pragma GCC unroll 6
        for(unsigned k=0;k<6;++k)acc+=int32_t(rxTaps[DELAY-6+k])*(buffer[n-k]+buffer[n-12+k]);
        filtered[n]=int16_t(acc/32768);
      }
    }else{
      int32_t sum=0;
      for(unsigned n=0;n<count;++n){sum+=buffer[n];if(n>=SPS)sum-=buffer[n-SPS];filtered[n]=int16_t(sum/int32_t(SPS));}
    }
  }
  unsigned IRAM_ATTR synchronize(bool searching,unsigned count,Hyp best[3]) {
    unsigned nb=0;
    const unsigned expected=CENTER+RX_DELAY+EARLY_US*FS/1000000;
    const unsigned first=searching?TAPS-1-(DELAY-RX_DELAY):expected-SYNC_RADIUS;
    const unsigned limit=searching?count-(SYMBOLS-1)*SPS-(DELAY-RX_DELAY):expected+SYNC_RADIUS;
    for(unsigned start=first;start<limit;start+=searching?2:1) {
      int32_t sy=0,sxy=0;
      for(unsigned k=0;k<SYNC_LEN;++k){int y=filtered[start+(SYNC_AT+k)*SPS];sy+=y;sxy+=y*syncLevels[k];}
      // The fixed 24-symbol sync has sum(x)=6 and sum(x*x)=216.
      const int32_t unit=(4*sxy-sy)/858;
      const int32_t mag=abs(unit),nominal=UNIT_HZ/HZ_SCALE;
      if(mag<nominal/3 || mag>nominal*3)continue;
      const int32_t offset=(sy-unit*6)/24;
      uint32_t err=0;
      for(unsigned k=0;k<SYNC_LEN;++k){int32_t e=filtered[start+(SYNC_AT+k)*SPS]-offset-unit*syncLevels[k];err+=uint32_t(e*e);}
      if(err>=uint32_t(mag)*mag*24*FAST_SYNC_LIMIT)continue;
      const uint32_t metric=err*256/(uint32_t(mag)*mag);
      bool same=false;
      for(unsigned j=0;j<nb;++j)if(abs(int(start)-int(best[j].start))<int(SPS/2)){
        if(metric<best[j].metric)best[j]={start,offset,unit,metric};same=true;break;
      }
      if(same)continue;
      if(nb<3)best[nb++]={start,offset,unit,metric};
      else{unsigned worst=0;for(unsigned j=1;j<3;++j)if(best[j].metric>best[worst].metric)worst=j;
        if(metric<best[worst].metric)best[worst]={start,offset,unit,metric};}
    }
    return nb;
  }
  int32_t IRAM_ATTR phaseHz(int32_t cross,int32_t dot) const {
    const uint32_t x=FskPhase::magnitude(dot),y=FskPhase::magnitude(cross);
    uint32_t high=x>y?x:y,low=x>y?y:x;
    if(!high)return 0;
    const int shift=24-__builtin_clz(high);
    if(shift>0){high>>=shift;low>>=shift;}else{high<<=-shift;low<<=-shift;}
    unsigned index=(low*reciprocal[high]+128)>>8;if(index>256)index=256;
    int32_t angle=AirModem::Phy::phaseTable[index];
    if(y>x)angle=16384-angle;
    if(dot<0)angle=32768-angle;
    if(cross<0)angle=-angle;
    return FAST_INTEGER?int32_t(int64_t(angle)*FS/65536):angle*int32_t(FS/128)/512;
  }
  int32_t IRAM_ATTR demodulate(unsigned count) {
    int32_t si=0,sq=0;
    for(unsigned n=0;n<count;++n){si+=int16_t(buffer[n]);sq+=int16_t(uint32_t(buffer[n])>>16);}
    const int32_t di=si/int32_t(count),dq=sq/int32_t(count);
    int32_t ai=int16_t(buffer[0])-di,aq=int16_t(uint32_t(buffer[0])>>16)-dq;
    int32_t sumCross=0,sumDot=0;buffer[0]=0;
    for(unsigned n=1;n<count;++n){
      const int32_t bi=int16_t(buffer[n])-di,bq=int16_t(uint32_t(buffer[n])>>16)-dq;
      int32_t cross,dot;FskPhase::correlate(ai,aq,bi,bq,cross,dot);
      sumCross+=cross;sumDot+=dot;buffer[n]=phaseHz(cross,dot);ai=bi;aq=bq;
    }
    const int32_t offset=FAST_INTEGER==2?carrierHz:phaseHz(sumCross,sumDot);
    for(unsigned n=1;n<count;++n){
      int32_t hz=buffer[n]-offset;
      if(hz>int32_t(FS/2))hz-=FS;if(hz<-int32_t(FS/2))hz+=FS;
      buffer[n]=hz;
    }
    return offset;
  }
  static void peak(uint32_t& p,uint32_t v){if(v>p)p=v;}
  static inline volatile uint32_t& reg(uint32_t a){return *reinterpret_cast<volatile uint32_t*>(a);}
  static inline void fence(){__asm__ volatile("memw" ::: "memory");}
  bool IRAM_ATTR transmitSamples(uint32_t at) {
    if(int32_t(micros()-at)>20)return false;
    uint32_t ps;__asm__ volatile("rsil %0, 15":"=a"(ps)::"memory");
    // Interrupts are masked here; keep observing UART during the slot wait.
    while(int32_t(micros()-at)<0)Release::pump();
    volatile uint32_t* tone=reinterpret_cast<volatile uint32_t*>(0x600005B8u);
    const uint32_t base=*tone&~0x7ffffu;
    uint32_t deadline=ESP.getCycleCount(), fraction=0,last=deadline;
    int32_t code=0,error=0;bool ok=true;
    for(unsigned n=0;n<TX_N;++n) {
      if(!FAST_INTEGER && !(n&7))Release::pump();
      deadline+=160000000UL/FS;fraction+=160000000UL%FS;
      if(fraction>=FS){fraction-=FS;++deadline;}
      const int32_t q=buffer[n], nearest=(q+32768)>>16;
      unsigned edge=n<TX_N/2?n:TX_N-1-n;
      constexpr unsigned mute=3*SPS/2,ramp=11*SPS/2;
      unsigned ask=edge<=mute?127:edge<ramp?(ramp-edge)*32/SPS:0;
      uint32_t amp=(n>=mute && n<TX_N-mute)?0x40000u|(uint32_t(uint8_t(0u-ask))<<10):0;
      if(FAST_INTEGER){
        *tone=base|(uint32_t(nearest)&1023u)|amp;fence();
        if(!(n&7))Release::pump();
        while(int32_t(ESP.getCycleCount()-deadline)<0){}
        const uint32_t late=ESP.getCycleCount()-deadline;
        if(late>stats.lateCycles)stats.lateCycles=late;
        if(late>(160000000UL/FS)/2){ok=false;break;}
        continue;
      }
      do {
        uint32_t now=ESP.getCycleCount(),dt=now-last;
        if(dt>2*(160000000UL/FS)){ok=false;break;}
        int32_t err=error+(q-code*65536)*int32_t(dt);
        if(err>(20<<16)||err<-(20<<16)) {
          bool up=err>0;int32_t residual=q-nearest*65536;
          int32_t length=(up?err:-err)/(up?65536-residual:65536+residual);
          if(length>int32_t(160000000UL/FS))length=160000000UL/FS;
          if(length>int32_t(deadline-now))length=int32_t(deadline-now);
          if(length>0) {
            *tone=base|(uint32_t(nearest+(up?1:-1))&1023u)|amp;fence();
            uint32_t t=ESP.getCycleCount();dt=t-last;
            if(dt>2*(160000000UL/FS)){ok=false;break;}
            error+=(q-code*65536)*int32_t(dt);code=nearest+(up?1:-1);
            while(int32_t(ESP.getCycleCount()-t)<length){}
            last=t;
          }
        }
        *tone=base|(uint32_t(nearest)&1023u)|amp;fence();
        uint32_t t=ESP.getCycleCount();dt=t-last;
        if(dt>2*(160000000UL/FS)){ok=false;break;}
        error+=(q-code*65536)*int32_t(dt);
        if(error>(1<<29))error=1<<29;if(error<-(1<<29))error=-(1<<29);
        code=nearest;last=t;
      } while(int32_t(ESP.getCycleCount()-deadline)<0);
      uint32_t late=ESP.getCycleCount()-deadline;
      if(late>stats.lateCycles)stats.lateCycles=late;
      if(!ok||late>(160000000UL/FS)/2){ok=false;break;}
    }
    *tone&=~0x40000u;fence();
    __asm__ volatile("wsr %0, ps; rsync"::"a"(ps):"memory");return ok;
  }
  bool IRAM_ATTR capture(unsigned count,uint32_t& at) {
    volatile uint32_t* ctrl=reinterpret_cast<volatile uint32_t*>(0x6000057Cu);
    const uint32_t cfg=(*ctrl&0x7ff80000u)|(uint32_t(FAST_ACCUM)<<2)|0x40000u;
    uint32_t ps;__asm__ volatile("rsil %0, 15":"=a"(ps)::"memory");
    const uint32_t phase=FAST_PHASE_DIVERSITY?(capturePhase++&3)*(160000000UL/FS)/4:0;
    Release::pump();at=micros()+3+phase/160;
    uint32_t next=ESP.getCycleCount()+480+phase,fraction=0;bool ok=true;
    for(unsigned n=0;n<count;++n) {
      *ctrl=cfg;*ctrl=cfg|1;fence();
      while(int32_t(ESP.getCycleCount()-next)<0){}
      uint32_t began=ESP.getCycleCount(),late=began-next;
      if(late>stats.lateCycles)stats.lateCycles=late;
      if(late>(160000000UL/FS)/2){ok=false;break;}
      *ctrl=cfg|3;fence();
      // The accumulator runs independently; service UART while it is working.
      if(n && !(n&7))Release::pump();
      while(!(*ctrl&0x80000000u))if(ESP.getCycleCount()-began>(160000000UL/FS)*3/4){ok=false;++stats.adcTimeout;break;}
      if(!ok)break;
      const uint32_t adcCycles=ESP.getCycleCount()-began;
      if(adcCycles>stats.adcMax)stats.adcMax=adcCycles;
      int32_t i=int32_t(reg(0x600005DCu))/(32*(FAST_ACCUM+1)),q=int32_t(reg(0x600005E0u))/(32*(FAST_ACCUM+1));
      if(i>32767)i=32767;if(i<-32768)i=-32768;
      if(q>32767)q=32767;if(q<-32768)q=-32768;
      buffer[n]=int32_t(uint16_t(i)|(uint32_t(uint16_t(q))<<16));
      *ctrl=cfg;fence();
      next+=160000000UL/FS;fraction+=160000000UL%FS;
      if(fraction>=FS){fraction-=FS;++next;}
    }
    *ctrl=cfg;fence();__asm__ volatile("wsr %0, ps; rsync"::"a"(ps):"memory");
    return ok;
  }
};
#endif
}

#ifndef FAST_HOST_TEST
class FastModem {
 public:
  void begin() {
    Serial.setRxBufferSize(64);
    Serial.begin((FAST_BENCH || FAST_SCAN) ? 115200 : 300, SERIAL_8N1);
    if(!FAST_BENCH){U0IE=0;U0IC=0xffff;}
    Serial.setDebugOutput(false);system_set_os_print(0);
    boot=uint16_t(os_random());if(!boot)boot=1;
    ready=radio.begin();born=micros();next=born+50000;
    if(FAST_SCAN){if(ready)radio.scan();ready=false;return;}
    timer1_isr_init();timer1_attachInterrupt(Fast::Release::pump);timer1_enable(TIM_DIV16,TIM_EDGE,TIM_LOOP);timer1_write(250);
    if(FAST_BENCH)Serial.printf("FAST_START role=%u fs=%u half=%u ram=%u heap=%u ready=%u\n",FAST_ROLE,Fast::FS,Fast::HALF,unsigned(sizeof(*this)),ESP.getFreeHeap(),ready);
  }
  void poll() {
    if(!ready){delay(1);return;}
    uint32_t now=micros();
    if((FAST_BENCH || FAST_END_REPORT) && uint32_t(now-born)>uint32_t(FAST_DURATION)*1000000UL){
      timer1_disable();
      if(!FAST_BENCH)Serial.begin(115200);
      report(true);radio.dumpIq();radio.dumpTiming();ready=false;return;
    }
    if(FAST_ROLE==2 && !locked) {
      Fast::Packet p;uint32_t start;int32_t off,unit;
      if(radio.receive(true,p,start,off,unit) && p.role==1) {
        epoch=start;locked=true;misses=0;incoming(p,start,off,unit);
        if(int32_t(epoch+Fast::HALF-micros())<int32_t(PREP)){epoch+=Fast::CYCLE;phase=0;next=epoch-Fast::EARLY_US;}
        else {phase=1;next=epoch+Fast::HALF;}
      }
      service();yield();return;
    }
    const bool txPhase=FAST_ROLE==1 ? phase==0 : phase==1;
    if(txPhase && int32_t(now-(next-PREP))>=0) {
      if(FAST_ROLE==1)epoch=next;
      if(int32_t(next-now)>(FAST_TX_CHANNEL!=FAST_CHANNEL?1150:FAST_INTEGER?350:500)) {
        Fast::Packet p=outgoing(next);radio.prepare(p);
        if(int32_t(next-micros())>100){
          // Count actual RF attempts, including an aborted RF burst, not preparations.
          if(p.data){++attemptsHistogram[attempts];if(attempts++)++retries;}
          radio.transmit(next);
        }else ++missedTx;
      } else ++missedTx;
      if(FAST_ROLE==1){phase=1;next=epoch+Fast::HALF-Fast::EARLY_US;}
      else {epoch+=Fast::CYCLE;phase=0;next=epoch-Fast::EARLY_US;}
    } else if(!txPhase && int32_t(now-next)>=0) {
      Fast::Packet p;uint32_t start;int32_t off,unit;
      if(radio.receive(false,p,start,off,unit,next) && p.role!=FAST_ROLE &&
          abs(int32_t(start-(FAST_ROLE==1?epoch+Fast::HALF:epoch)))<1500) {
        incoming(p,start,off,unit);misses=0;
        if(FAST_ROLE==2)epoch=start;
      } else if(++misses>=12 && FAST_ROLE==2)locked=false;
      if(FAST_ROLE==1){phase=0;next=epoch+Fast::CYCLE;}
      else {phase=1;next=epoch+Fast::HALF;}
    }
    service();
    if(FAST_BENCH)report(false);
    const bool nextTx=FAST_ROLE==1?phase==0:phase==1;
    if(int32_t(next-(nextTx?PREP:0)-micros())>500)yield();
  }
 private:
  static constexpr uint32_t PREP=FAST_TX_CHANNEL!=FAST_CHANNEL?1400:FAST_INTEGER?500:Fast::SPS==4?800:1800;
  Fast::Radio radio;
  bool ready=false,locked=false,pending=false,ackValid=false,haveRx=false,outputPending=false,peerConfirmed=false;
  uint8_t phase=0,misses=0,seq=0,ack=0,pendingByte=0,attempts=0,rxSeq=0,outputByte=0;
  uint16_t boot=0,peerBoot=0;
  uint32_t born=0,next=0,epoch=0,queuedAt=0,outputAt=0,lastReport=0,lastGenerated=0,lastDataAt=0;
  int16_t txHz=0,tuneHz=Fast::NO_TUNE;
  int32_t lastOffset=0,lastUnit=0;
  uint32_t offered=0,delivered=0,expired=0,retries=0,acked=0,duplicates=0,lateRx=0,overflow=0;
  uint32_t missedTx=0,cal=0,wrong=0,maxLocalAge=0;
  uint32_t attemptsHistogram[4]={},faultDrops=0;
  uint8_t faultSeq=0,faultCount=0;bool haveFault=false;
  void service() {
    uint32_t now=micros();
    if(pending && uint32_t(now-queuedAt)>=Fast::FINAL_LIMIT_US){pending=false;++seq;++expired;}
    outputPending=Fast::Release::armed;
    delivered=Fast::Release::delivered;
    if(FAST_BENCH) {
      if(uint32_t(now-born)>2000000 && uint32_t(now-lastGenerated)>=33334) {
        lastGenerated=now;
        if(!pending){pendingByte=uint8_t(seq*73+FAST_ROLE*29);queuedAt=now;pending=true;attempts=0;++offered;}
        else ++overflow;
      }
    } else {
      uint8_t b;uint32_t at;
      while(Fast::UartRx::pop(b,at)) {
        if(pending){++overflow;continue;}
        if(uint32_t(micros()-at)>=Fast::FINAL_LIMIT_US){++expired;continue;}
        pendingByte=b;queuedAt=at;pending=true;attempts=0;++offered;
      }
    }
  }
  Fast::Packet outgoing(uint32_t at) {
    service();Fast::Packet p;
    p.role=FAST_ROLE;p.boot=boot;p.ack=ack;p.ackValid=ackValid;p.ackBoot=peerBoot;
    p.txHz=txHz;p.tuneHz=tuneHz;
    p.periodUs=Fast::UartRx::periodUs;
    const uint32_t age=at-queuedAt;
    const uint32_t wireUs=Fast::TX_N*1000000UL/Fast::FS;
    if(peerConfirmed && pending && attempts<Fast::MAX_ATTEMPTS &&
       age+wireUs+1000<Fast::FINAL_LIMIT_US) {
      p.data=true;p.seq=seq;p.byte=pendingByte;
      p.age=uint8_t((age+124)/125);
      if(age>maxLocalAge)maxLocalAge=age;
    }
    return p;
  }
  void incoming(const Fast::Packet& p,uint32_t start,int32_t off,int32_t unit) {
    if(peerBoot!=p.boot){peerBoot=p.boot;haveRx=false;ackValid=false;peerConfirmed=false;}
    if(!p.data && p.ackBoot==boot)peerConfirmed=true;
    lastOffset=off;lastUnit=unit;
    if(FAST_INTEGER){
      // Each capture removes its measured carrier offset before symbol decisions.
      // Integer RF tones stay fixed; calibration is digital on both receivers.
      txHz=0;tuneHz=Fast::NO_TUNE;++cal;
      Fast::Release::wantedPeriod=p.periodUs;
    }
    // Use only independently CRC-validated frames for frequency feedback.
    else if(abs(off)>=1000 && abs(off)<int(Fast::FS/2)-5000 && abs(unit)>100) {
      constexpr int32_t targetAbs=Fast::UNIT_HZ*20/9;
      int32_t target=off<0?-targetAbs:targetAbs;
      int32_t residual=target-off;
      if(abs(residual)<450)residual=0;
      int32_t request=p.txHz+AirModem::clampCal(residual*(unit<0?-1:1),1200);
      tuneHz=int16_t(AirModem::clampCal(request,12000));++cal;
    }
    if(!FAST_INTEGER && p.tuneHz!=Fast::NO_TUNE)txHz=AirModem::boundedTxCorrection(txHz,p.tuneHz);
    if(peerConfirmed && p.ackValid && pending && attempts && p.ack==seq){pending=false;++seq;++acked;}
    if(!p.data)return;
    if(FAST_FAULT){
      if(!haveFault || faultSeq!=p.seq){faultSeq=p.seq;faultCount=0;haveFault=true;}
      if(faultCount<FAST_FAULT){++faultCount;++faultDrops;return;}
    }
    if(haveRx && uint32_t(micros()-lastDataAt)>2000000)haveRx=false;
    if(haveRx && p.seq==rxSeq){ack=p.seq;ackValid=true;++duplicates;return;}
    if(FAST_BENCH && p.byte!=uint8_t(p.seq*73+p.role*29)){++wrong;return;}
    if(haveRx && int8_t(p.seq-rxSeq)<=0){++lateRx;return;}
    const uint32_t due=start+Fast::RELEASE_TARGET_US-uint32_t(p.age)*125;
    if(int32_t(due+Fast::RELEASE_JITTER_US-micros())<=0 || Fast::Release::armed){++lateRx;return;}
    haveRx=true;rxSeq=p.seq;outputPending=true;outputByte=p.byte;outputAt=due;
    lastDataAt=micros();
    Fast::Release::schedule(p.byte,due);
    ack=p.seq;ackValid=true;
  }
  void report(bool final) {
    if(!final)return;
    lastReport=micros();
    const auto& s=radio.stats;
    Serial.printf("FAST%s r=%u fs=%u tx=%u rx=%u ta=%u ra=%u inv=%u cand=%u fec=%u prep=%u setup=%u restore=%u dsp=%u late=%u offer=%u out=%u exp=%u retry=%u ack=%u dup=%u lateRx=%u over=%u missTx=%u cal=%u off=%d unit=%d tune=%d wrong=%u heap=%u releaseLate=%u releaseDrop=%u demod=%u filter=%u search=%u uartRx=%u uartErrors=%u uartOverflow=%u\n",
      final?"_FINAL":"",FAST_ROLE,Fast::FS,s.tx,s.rx,s.txAbort,s.rxAbort,s.invalid,s.candidates,s.corrected,
      s.prepMax,s.setupMax,s.restoreMax,s.dspMax,s.lateCycles,offered,delivered,expired,retries,acked,duplicates,lateRx,overflow,
      missedTx,cal,lastOffset,lastUnit,txHz,wrong,ESP.getFreeHeap(),Fast::Release::lateMax,Fast::Release::dropped,s.demodMax,s.filterMax,s.searchMax,Fast::UartRx::received,Fast::UartRx::errors,Fast::UartRx::overflow);
    Serial.printf("STAGES r=%u noSync=%u fecReject=%u headerReject=%u crcReject=%u metaReject=%u captureLateMax=%u captureLateCount=%u noSyncLate=%u edgeSync=%u validLate=%u\n",FAST_ROLE,s.noSync,s.fecReject,s.headerReject,s.crcReject,s.metaReject,s.captureLateMax,s.captureLateCount,s.noSyncLate,s.edgeSync,s.validLate);
    Serial.printf("ARQ r=%u attempt1=%u attempt2=%u attempt3=%u attempt4=%u injectedDrops=%u maxAge=%u\n",FAST_ROLE,attemptsHistogram[0],attemptsHistogram[1],attemptsHistogram[2],attemptsHistogram[3],faultDrops,maxLocalAge);
    Serial.printf("ADC r=%u accumulation=%u maxCycles=%u timeouts=%u\n",FAST_ROLE,FAST_ACCUM+1,s.adcMax,s.adcTimeout);
    Serial.printf("UARTIN r=%u gapDrops=%u batchDrops=%u hardwareErrors=%u maxObservationGap=%u\n",FAST_ROLE,Fast::UartRx::gapDrops,Fast::UartRx::batchDrops,Fast::UartRx::hardwareErrors,Fast::UartRx::maxObservationGap);
    Serial.printf("UART r=%u timerDrop=%u shiftDrop=%u fifoDrop=%u pastDrop=%u waitMax=%u periodMin=%u periodMax=%u periodSum=%u periodCount=%u divider=%u recoveredPeriod=%u\n",FAST_ROLE,Fast::Release::timerDrop,Fast::Release::shiftDrop,Fast::Release::fifoDrop,Fast::Release::pastDrop,Fast::Release::waitMax,Fast::UartRx::periodMin,Fast::UartRx::periodMax,Fast::UartRx::periodSum,Fast::UartRx::periodCount,Fast::Release::divider,Fast::Release::wantedPeriod);
  }
};
#endif

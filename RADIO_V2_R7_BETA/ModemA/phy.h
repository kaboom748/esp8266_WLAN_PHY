#pragma once
#include "codec.h"
#include "uart_link.h"
extern "C" void pp_disable_noise_timer(void);
#ifndef MF_FS
#define MF_FS 312500
#endif
#ifndef MF_SPS
#define MF_SPS 2
#endif
#ifndef MF_BPS
#define MF_BPS 1
#endif
#ifndef MF_TONE_STEP
#define MF_TONE_STEP 1
#endif
#ifndef MF_TONE_MAP
#define MF_TONE_MAP 0
#endif
#ifndef MF_CAPTURE_FAST
#define MF_CAPTURE_FAST 0
#endif
#ifndef MF_RAW_SLICE
#define MF_RAW_SLICE 0
#endif
#ifndef MF_RF_UART_PUMP
#define MF_RF_UART_PUMP 1
#endif
#ifndef MF_LOCKED_HYPOTHESES
#define MF_LOCKED_HYPOTHESES 1
#endif
#ifndef MF_RAW_CANDIDATE_ORDER
#define MF_RAW_CANDIDATE_ORDER 1
#endif
#ifndef MF_HALF_US
#define MF_HALF_US 20000
#endif
#ifndef MF_RX_CHANNEL
#define MF_RX_CHANNEL 11
#endif
#ifndef MF_TX_CHANNEL
#define MF_TX_CHANNEL 3
#endif
#ifndef MF_GAIN
#define MF_GAIN 0
#endif
#ifndef MF_BB_GAIN
#define MF_BB_GAIN 0
#endif
#ifndef MF_ACCUM
#define MF_ACCUM 63
#endif
#define FAST_INTEGER 2
#define FAST_IQ 0
#define FAST_MATCH 0
#define FAST_SCAN 0
#define FAST_END_REPORT 0
#define FAST_DURATION 0
#define FAST_PHASE_DIVERSITY 0
#ifndef MF_QUIET_SDK
#define MF_QUIET_SDK 0
#endif
#define FAST_QUIET_SDK MF_QUIET_SDK
#define FAST_SYNC_LIMIT 4
#define FAST_CHANNEL MF_RX_CHANNEL
#define FAST_TX_CHANNEL MF_TX_CHANNEL
#define FAST_GAIN MF_GAIN
#define FAST_BB_GAIN MF_BB_GAIN
#define FAST_ACCUM MF_ACCUM
namespace Fast {
using Packet = BlockCodec::Packet;
constexpr unsigned FS=MF_FS,SPS=MF_SPS,BPS=MF_BPS;
static_assert(BPS==1 || BPS==2,"native modem supports 2FSK or 4FSK");
static_assert(MF_RAW_SLICE>=0 && MF_RAW_SLICE<=2,"raw slice must be filtered0, early1, or current2");
static_assert(MF_LOCKED_HYPOTHESES>=1 && MF_LOCKED_HYPOTHESES<=3,
              "locked receive hypotheses must be 1..3");
static_assert(MF_RAW_CANDIDATE_ORDER==0 || MF_RAW_CANDIDATE_ORDER==1,
              "raw candidate order must be 0 or 1");
static_assert(MF_TONE_STEP>=1 && MF_TONE_STEP<=3,"2FSK native tone step must be 1, 2, or 3");
static_assert(MF_TONE_MAP==0 || MF_TONE_MAP==1,"unsupported 4FSK tone mapping");
static_assert(BPS!=1 || 2ULL*MF_TONE_STEP*78125<FS,"2FSK tones must be strictly below Nyquist");
static_assert(BPS!=2 || FS==312500 || FS==400000 || FS==500000 || FS==625000,"unsupported native 4FSK rate");
static_assert(BPS!=2 || SPS>=2,"4FSK needs at least two IQ samples per symbol");
static_assert(BPS!=2 || MF_TONE_MAP!=1 || FS>=400000,"symmetric 4FSK mapping needs FS >= 400000");
constexpr unsigned WIRE=BlockCodec::WIRE,BITS=WIRE*8,SYNC_LEN=24;
constexpr unsigned SYNC_AT=64/BPS, SYMBOLS=BITS/BPS+SYNC_LEN;
constexpr unsigned TAPS=10*SPS+1, DELAY=5*SPS, CENTER=13*SPS/2;
// Binary uses native +/-MF_TONE_STEP. At FS312500, quaternary {-1,0,1,2}
// sits at levels {-3,-1,+1,+3} around +78125/2 Hz; odd native +/-1,+/-3
// would alias there. FS625000 instead resolves native +/-1,+/-3 directly.
// Optional map1 uses {-2,-1,+1,+2}, centred on zero at all supported rates.
// Its sync extremes still fit levels +/-3; inner data tones are +/-1.5 units.
constexpr unsigned RX_DELAY=SPS/2;
constexpr unsigned UNIT_HZ=BPS==2?(MF_TONE_MAP==1?156250/3:(FS==625000?78125:78125/2)):
  MF_TONE_STEP*78125/3,HZ_SCALE=128;
constexpr int32_t INITIAL_CARRIER_HZ=BPS==2 && MF_TONE_MAP==0 && FS!=625000?int32_t(78125/2):0;
constexpr unsigned SYNC_RADIUS=3*SPS, TX_N=(SYMBOLS+12)*SPS;
constexpr unsigned EARLY_US=150;
constexpr unsigned RX_N=TX_N+(2*EARLY_US*FS+999999)/1000000+32;
constexpr unsigned SEARCH_N=RX_N+512,MAX_N=SEARCH_N;
// At 8N1 each input character occupies ten line bits. The longest masked
// capture must leave at least 18 of the 128 hardware FIFO entries unused.
constexpr uint64_t SEARCH_UART_BYTES=(uint64_t(SEARCH_N)*MF_BAUD+uint64_t(FS)*10-1)/(uint64_t(FS)*10);
static_assert(MF_RF_UART_PUMP || SEARCH_UART_BYTES<=110,
              "masked capture exceeds safe UART FIFO capacity without RF pump");
constexpr uint32_t HALF=MF_HALF_US,CYCLE=2*HALF;
static_assert(TX_N*1000000ULL/FS+2000<HALF,"RF slot too short");
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
    // Build both FEC generators before any scheduled RF preparation.
    (void)BlockCodec::Detail::tables();
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
    prepared=false;
    const uint32_t began = micros();
    uint8_t w[WIRE];
    if(!BlockCodec::encode(p, w))return;
    BlockCodec::scramble(w, WIRE);
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
    const int32_t offset = 0;
    for (unsigned n = 0; n < TX_N; ++n) {
      if(FAST_INTEGER){
        const int s=(int(n)-int(CENTER-SPS/2))/int(SPS);
        if(s>=0 && s<int(SYMBOLS)) {
          const int32_t level=levels[s];
          const int32_t code=BPS==2?
            (MF_TONE_MAP==1?(abs(level)==3?(level>0?2:-2):level):
              (FS==625000?level:(level+1)/2)):
            (level>0?MF_TONE_STEP:(FAST_INTEGER==2?-MF_TONE_STEP:0));
          buffer[n]=code*65536;
        }else buffer[n]=0;
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
    prepared=true;
    peak(stats.prepMax, micros()-began);
  }
  bool transmit(uint32_t at) {
    // Never emit stale samples after an invalid packet or a consumed prepare.
    if(!prepared){++stats.txAbort;return false;}
    prepared=false;
    uint32_t began = micros();
    uint32_t cycles=ESP.getCycleCount();
    if(!UartLink::pauseTxForRf()) {
      UartLink::resumeTxAfterRf();++stats.txAbort;return false;
    }
    bool ok = FAST_TX_CHANNEL==FAST_CHANNEL || wifi_set_channel(FAST_TX_CHANNEL);
    if(ok)ok=initTxPath(false);
    // Mode2 keeps UART TX quiet across setup, the complete RF burst, and
    // restoration. Mode1 protects only the path/clock transition itself.
    if(MF_UART_RF_GUARD!=2)UartLink::resumeTxAfterRf();
    const uint32_t setupUs=micros()-began,setupCpuUs=(ESP.getCycleCount()-cycles)/160;
    peak(stats.setupMax,setupUs);
    if(setupUs>setupCpuUs)peak(stats.setupClockDeficit,setupUs-setupCpuUs);
    if (ok) ok = transmitSamples(at);
    began = micros();cycles=ESP.getCycleCount();
    if(MF_UART_RF_GUARD!=2 && !UartLink::pauseTxForRf()) {
      // Keep the RF path unchanged when UART has not drained. Its tone gate
      // is already closed; report failure and retry through the next TX slot.
      UartLink::resumeTxAfterRf();++stats.txAbort;return false;
    }
    if(FAST_TX_CHANNEL!=FAST_CHANNEL && !wifi_set_channel(FAST_CHANNEL))ok=false;
    if (!initRxPath()) ok = false;
    UartLink::resumeTxAfterRf();
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
    if(MF_UART_RF_GUARD==2) {
      if(!UartLink::pauseTxForRf()) {
        UartLink::resumeTxAfterRf();++stats.rxAbort;return false;
      }
      // The scheduler invokes locked receive early enough to drain UART TX.
      // While waiting, RX still runs and outgoing UART bytes remain queued.
      if(!searching)while(int32_t(micros()-scheduled)<0)UartLink::pump();
    }
    const bool capturedOk=capture(count, captured);
    if(MF_UART_RF_GUARD==2)UartLink::resumeTxAfterRf();
    if (!capturedOk) { ++stats.rxAbort; return false; }
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
    Hyp best[3] = {};unsigned nb=synchronize(searching,count,best);
    const unsigned expected=CENTER+RX_DELAY+EARLY_US*FS/1000000;
    const unsigned first=searching?TAPS-1-(DELAY-RX_DELAY):expected-SYNC_RADIUS;
    const unsigned limit=searching?count-(SYMBOLS-1)*SPS-(DELAY-RX_DELAY):expected+SYNC_RADIUS;
    stats.candidates += nb;
    if(!nb){++stats.noSync;if(captureLate>200)++stats.noSyncLate;}
    // When slicing raw samples, compare the retained candidates using that
    // same sample phase and fit before spending the bounded FEC attempts.
    // The filtered sync still finds the candidate set; acceptance thresholds
    // and packet validation are unchanged. Option0 preserves the old order.
    if(MF_RAW_CANDIDATE_ORDER && MF_RAW_SLICE){
      unsigned valid=0;
      for(unsigned j=0;j<nb;++j){
        Hyp h=best[j];
        if(refitRaw(h,count))best[valid++]=h;
      }
      nb=valid;
    }
    bool found=false;
    // Bound expensive FEC attempts while locked; acquisition keeps all three.
    for(unsigned pass=0;pass<nb && (searching || pass<MF_LOCKED_HYPOTHESES);++pass) {
      unsigned chosen=0;for(unsigned j=1;j<nb;++j)if(best[j].metric<best[chosen].metric)chosen=j;
      Hyp h=best[chosen];best[chosen].metric=UINT32_MAX;
      // A raw decision must use carrier/spacing fitted from the same raw
      // sample phase. Moving-average sync can attenuate four-tone spacing.
      if(MF_RAW_SLICE && !MF_RAW_CANDIDATE_ORDER && !refitRaw(h,count))continue;
      uint8_t w[WIRE]={};
      for(unsigned b=0;b<BITS;b+=BPS) {
        const unsigned s=b/BPS<SYNC_AT?b/BPS:b/BPS+SYNC_LEN, at=h.start+s*SPS;
        int32_t v=MF_RAW_SLICE?4*(buffer[at-(MF_RAW_SLICE==1?RX_DELAY:0)]-h.offset):SPS<=4?4*(int32_t(filtered[at])-h.offset):
          int32_t(filtered[at-1])+2*filtered[at]+filtered[at+1]-4*h.offset;
        if(h.unit<0)v=-v;
        if(BPS==1) {if(v>=0)w[b/8]|=1<<(7-b%8);}
        else {const unsigned d=(v<0?2:0)|(abs(v)>(MF_TONE_MAP==1?9:8)*abs(h.unit)?1:0);w[b/8]|=d<<(6-b%8);}
      }
      uint8_t corrected;
      BlockCodec::scramble(w, WIRE);
      
      if(!BlockCodec::decode(w,packet,corrected)) {
        ++stats.fecReject;
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

 private:
  int32_t buffer[MAX_N], txTaps[TAPS], txPoly[SPS][11];
  int16_t filtered[MAX_N], rxTaps[TAPS];
  int8_t syncLevels[SYNC_LEN];
  uint16_t reciprocal[256];
  int32_t iq[FAST_IQ?2:1][FAST_IQ?RX_N:1];
  uint32_t iqAt[2]={};unsigned iqState=0;
  int32_t carrierHz=INITIAL_CARRIER_HZ;
  uint8_t capturePhase=0;
  bool reconnectBefore=false;
  bool prepared=false;
  uint8_t badBins[FAST_END_REPORT?FAST_DURATION*10:1]={};
  struct Hyp {unsigned start;int32_t offset,unit;uint32_t metric;};
  bool IRAM_ATTR refitRaw(Hyp& h,unsigned count) {
    constexpr unsigned back=MF_RAW_SLICE==1?RX_DELAY:0;
    if(h.start<back)return false;
    const unsigned first=h.start-back;
    if(first+(SYNC_AT+SYNC_LEN-1)*SPS>=count)return false;
    int32_t sy=0,sxy=0;
    for(unsigned k=0;k<SYNC_LEN;++k) {
      const int32_t y=buffer[first+(SYNC_AT+k)*SPS];
      sy+=y;sxy+=y*syncLevels[k];
    }
    const int32_t unit=(4*sxy-sy)/858;
    const int32_t mag=abs(unit),nominal=UNIT_HZ/HZ_SCALE;
    if(mag<nominal/3 || mag>nominal*3)return false;
    const int32_t offset=(sy-unit*6)/24;
    uint32_t err=0;
    for(unsigned k=0;k<SYNC_LEN;++k) {
      const int32_t e=buffer[first+(SYNC_AT+k)*SPS]-offset-unit*syncLevels[k];
      err+=uint32_t(e*e);
    }
    if(err>=uint32_t(mag)*mag*24*FAST_SYNC_LIMIT)return false;
    h.offset=offset;h.unit=unit;
    h.metric=uint32_t(uint64_t(err)*256/(uint32_t(mag)*mag));
    return true;
  }
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
    const unsigned step=searching && !(BPS==2 && SPS==2)?2:1;
    for(unsigned start=first;start<limit;start+=step) {
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
      const uint32_t metric=uint32_t(uint64_t(err)*256/(uint32_t(mag)*mag));
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
    // Exact conversion at native rates; reduce the power-of-two denominator.
    if(FS==312500 || FS==400000 || FS==500000 || FS==625000 || FS==1000000){
      constexpr unsigned shift=16-__builtin_ctz(FS);
      const uint32_t hz=(FskPhase::magnitude(angle)*(FS>>__builtin_ctz(FS)))>>shift;
      return angle<0?-int32_t(hz):int32_t(hz);
    }
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
    while(int32_t(micros()-at)<0)UartLink::pump();
    volatile uint32_t* tone=reinterpret_cast<volatile uint32_t*>(0x600005B8u);
    const uint32_t base=*tone&~0x7ffffu;
    uint32_t deadline=ESP.getCycleCount(), fraction=0,last=deadline;
    int32_t code=0,error=0;bool ok=true;
    for(unsigned n=0;n<TX_N;++n) {
      if(MF_RF_UART_PUMP && !FAST_INTEGER && !(n&7))UartLink::pump();
      deadline+=160000000UL/FS;fraction+=160000000UL%FS;
      if(fraction>=FS){fraction-=FS;++deadline;}
      const int32_t q=buffer[n], nearest=(q+32768)>>16;
      unsigned edge=n<TX_N/2?n:TX_N-1-n;
      constexpr unsigned mute=3*SPS/2,ramp=11*SPS/2;
      unsigned ask=edge<=mute?127:edge<ramp?(ramp-edge)*32/SPS:0;
      uint32_t amp=(n>=mute && n<TX_N-mute)?0x40000u|(uint32_t(uint8_t(0u-ask))<<10):0;
      if(FAST_INTEGER){
        *tone=base|(uint32_t(nearest)&1023u)|amp;fence();
        if(MF_RF_UART_PUMP && !(n&7))UartLink::pump();
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
    UartLink::pump();at=micros()+3+phase/160;
    uint32_t next=ESP.getCycleCount()+480+phase,fraction=0;bool ok=true;
    if(MF_CAPTURE_FAST){*ctrl=cfg;fence();}
    for(unsigned n=0;n<count;++n) {
      if(!MF_CAPTURE_FAST)*ctrl=cfg;
      *ctrl=cfg|1;fence();
      while(int32_t(ESP.getCycleCount()-next)<0){}
      uint32_t began=ESP.getCycleCount(),late=began-next;
      if(late>stats.lateCycles)stats.lateCycles=late;
      if(late>(160000000UL/FS)/2){ok=false;break;}
      *ctrl=cfg|3;fence();
      // The accumulator runs independently; service UART while it is working.
      if(MF_RF_UART_PUMP && n && !(n&7))UartLink::pump();
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

}

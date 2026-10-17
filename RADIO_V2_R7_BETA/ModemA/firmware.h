#pragma once
#ifndef MF_TRANSPORT_HOST_TEST
#include "phy.h"
#endif
#include "compression.h"
#ifndef MF_ROLE
#define MF_ROLE 1
#endif
#ifndef MF_DROP_EVERY
#define MF_DROP_EVERY 0
#endif
#ifndef MF_UART_RF_GUARD
#define MF_UART_RF_GUARD 0
#endif
#ifndef MF_COMPRESSION
#define MF_COMPRESSION 1
#endif
#ifndef MF_ADAPTIVE_FEC
#define MF_ADAPTIVE_FEC 1
#endif
#ifndef MF_FAST_AFTER_ACKS
#define MF_FAST_AFTER_ACKS 64
#endif
#ifndef MF_DIAG_AFTER_MS
#define MF_DIAG_AFTER_MS 0
#endif
#ifndef MF_RX_BLACKOUT_AT_MS
#define MF_RX_BLACKOUT_AT_MS 0
#endif
#ifndef MF_RX_BLACKOUT_MS
#define MF_RX_BLACKOUT_MS 0
#endif
class FastBlockModem {
 public:
  void begin() {
    UartLink::begin();
    session=os_random();if(!session)session=1;
    ready=radio.begin();
    const uint32_t now=micros();born=now;next=now+50000;lastRx=now;
    UartLink::setLinkReady(false);
#if MF_RTC_DIAGNOSTICS && !defined(MF_TRANSPORT_HOST_TEST)
    captureRtc(0);
#endif
  }
  void poll() {
    UartLink::service();
#if MF_RTC_DIAGNOSTICS && !defined(MF_TRANSPORT_HOST_TEST)
    if((!UartLink::healthy() || sessionFault) && !rtcCaptured && !timedRtcCaptured){captureRtc(1);rtcCaptured=true;}
#endif
    if(UartLink::takeClearRequest())UartLink::clearStats();
    if(UartLink::takeStatsRequest())wantStats=true;
    if(!ready){report();delay(1);return;}
    uint32_t now=micros();
    UartLink::setLinkReady(confirmed && !sessionFault && UartLink::healthy() && uint32_t(now-lastRx)<2000000);
    noteInput(now);
    if(MF_ROLE==2 && !locked) {
      BlockCodec::Packet p;uint32_t start;int32_t off,unit;
      if(radio.receive(true,p,start,off,unit) && p.role==1 && acceptRadioReceive()) {
        epoch=start;locked=true;misses=0;incoming(p,start,off,unit);
        if(int32_t(epoch+Fast::HALF-micros())<int32_t(PREP)){
          epoch+=Fast::CYCLE;phase=0;next=epoch-Fast::EARLY_US;
        }else{phase=1;next=epoch+Fast::HALF;}
      }
      captureTimedRtc();report();yield();return;
    }
    const bool txPhase=MF_ROLE==1 ? phase==0 : phase==1;
    bool transmitted=false;
    if(txPhase && int32_t(now-(next-PREP))>=0) {
      if(MF_ROLE==1)epoch=next;
      if(int32_t(next-now)>int32_t(SETUP_BUDGET)) {
        BlockCodec::Packet p=outgoing();radio.prepare(p);
        if(int32_t(next-micros())>100){
          if(p.flags&BlockCodec::FLAG_DATA){
            if(attempts++)++retries;
            ++dataTx;
            if(p.flags&BlockCodec::FLAG_FRAGMENT){++fragmentTx;fragmentOffset=fragmentOffset?0:FRAGMENT_BYTES;}
          }
          if(!radio.transmit(next))++txFail;
          transmitted=true;
        }else ++missTx;
      }else ++missTx;
      if(MF_ROLE==1){phase=1;next=epoch+Fast::HALF-Fast::EARLY_US;}
      else{epoch+=Fast::CYCLE;phase=0;next=epoch-Fast::EARLY_US;}
    }else if(!txPhase && int32_t(now-(next-RX_PREP))>=0) {
      BlockCodec::Packet p;uint32_t start;int32_t off,unit;
      if(radio.receive(false,p,start,off,unit,next) && p.role!=MF_ROLE &&
         abs(int32_t(start-(MF_ROLE==1?epoch+Fast::HALF:epoch)))<1500 && acceptRadioReceive()){
        incoming(p,start,off,unit);misses=0;
        if(MF_ROLE==2)epoch=start;
      }else if(++misses>=12 && MF_ROLE==2){locked=false;++resyncs;}
      if(MF_ROLE==1){phase=0;next=epoch+Fast::CYCLE;}
      else{phase=1;next=epoch+Fast::HALF;}
    }
    UartLink::service();
    const bool nextTx=MF_ROLE==1?phase==0:phase==1;
    const int32_t spare=int32_t(next-(nextTx?PREP:RX_PREP)-micros());
    // Use the long interval after TX for compression, including while its DATA
    // awaits an ACK. RX/ACK handling only adopts a cache or copies a RAW prefix.
    // The guard exceeds the measured encode cost, not a proven worst case.
    if(transmitted && spare>int32_t(COMPRESSION_PREP_MARGIN_US))prepareAhead();
    if(int32_t(next-(nextTx?PREP:RX_PREP)-micros())>int32_t(PAYLOAD_PREP_MARGIN_US))fillPending(micros());
    if(int32_t(next-(nextTx?PREP:RX_PREP)-micros())>6000)captureTimedRtc();
    if(int32_t(next-(nextTx?PREP:RX_PREP)-micros())>1500){report();yield();}
  }
 private:
  static constexpr uint32_t PREP=3200,SETUP_BUDGET=1600;
  static constexpr uint32_t RX_PREP=MF_UART_RF_GUARD==2?1200:0;
  static constexpr uint32_t PAYLOAD_PREP_MARGIN_US=500,COMPRESSION_PREP_MARGIN_US=8000;
  static constexpr uint8_t COMPRESSION_COOLDOWN_BLOCKS=16;
  static constexpr unsigned FRAGMENT_HEADER=6,FRAGMENT_BYTES=BlockCodec::PAYLOAD-FRAGMENT_HEADER;
  static constexpr unsigned CLEAN_ACKS_TO_FAST=MF_FAST_AFTER_ACKS;
  static_assert(CLEAN_ACKS_TO_FAST>0 && CLEAN_ACKS_TO_FAST<=65535,"invalid fast FEC promotion threshold");
  Fast::Radio radio;
  bool ready=false,locked=false,confirmed=false,pending=false,haveRx=false,ackValid=false;
  bool wantStats=true,sessionFault=false;
  uint8_t phase=0,misses=0,pendingLength=0,pendingFlags=0;
  uint8_t pendingData[BlockCodec::MAX_PAYLOAD]={};
  uint8_t staged[RadioCompression::MAX_RAW]={},decoded[RadioCompression::MAX_RAW]={};
  uint16_t stagedLength=0,pendingRawLength=0;
  uint8_t cachedData[BlockCodec::MAX_PAYLOAD]={},cachedLength=0,cachedFlags=0;
  uint16_t cachedRawLength=0;
  bool cacheValid=false;
  uint8_t compressionCooldown=0;
  bool inputTimed=false,fastMode=false,pendingFallback=false;
  uint16_t cleanAcks=0;
  uint8_t fragmentOffset=0;
  uint32_t pendingEncodedCrc=0,restartSession=0;
  bool fragmentActive=false;
  uint8_t fragmentData[BlockCodec::MAX_PAYLOAD]={},fragmentLength=0,fragmentMask=0,fragmentFlags=0;
  uint16_t fragmentSeq=0;
  uint32_t fragmentCrc=0;
  uint16_t seq=0,ack=0,rxSeq=0;
  uint32_t session=0,peerSession=0,peerSeen=0,epoch=0,next=0,born=0,lastRx=0;
  uint32_t firstInput=0,attempts=0,queuedAt=0;
  uint32_t offered=0,delivered=0,acked=0,retries=0,duplicates=0,backpressure=0;
  uint32_t dataTx=0,missTx=0,txFail=0,resyncs=0,sessionChanges=0,unexpectedSeq=0;
  uint32_t injected=0,dataCandidates=0,maxAckUs=0,lastReport=0;
  uint32_t compressedTxBlocks=0,rawTxBlocks=0,compressedRxBlocks=0,rawRxBlocks=0;
  uint32_t compressedEncodedBytes=0,compressionRejects=0,fragmentTx=0,fragmentRx=0,fragmentRejects=0;
  uint32_t fastPromotions=0,robustFallbacks=0,foreignData=0;
  uint32_t blackoutIgnored=0;
  uint32_t compressionMaxUs=0,decompressionMaxUs=0,fillMaxUs=0,compressionProbes=0;
  int32_t lastOffset=0,lastUnit=0;
  bool acceptRadioReceive() {
#if MF_RX_BLACKOUT_MS > 0
    if(MF_ROLE==2 && MF_RX_BLACKOUT_MS){
      const uint32_t age=uint32_t(micros()-born)/1000;
      if(age>=uint32_t(MF_RX_BLACKOUT_AT_MS) && age-uint32_t(MF_RX_BLACKOUT_AT_MS)<uint32_t(MF_RX_BLACKOUT_MS)){
        ++blackoutIgnored;return false;
      }
    }
#endif
    return true;
  }
  void noteInput(uint32_t now) {
    if(!stagedLength && !UartLink::inputSize()){inputTimed=false;return;}
    if(!inputTimed){firstInput=now;inputTimed=true;}
  }
  void fillPending(uint32_t now) {
    const uint32_t began=micros();preparePending(now);
    const uint32_t elapsed=micros()-began;if(elapsed>fillMaxUs)fillMaxUs=elapsed;
  }
  void prepareAhead() {
    if(!MF_COMPRESSION || cacheValid || compressionCooldown || !confirmed ||
       sessionFault || !UartLink::healthy())return;
    const uint8_t flags=fastMode?BlockCodec::FLAG_FAST_FEC:0;
    const unsigned capacity=BlockCodec::packetCapacity(flags);
    // Small fresh input keeps the normal 5ms coalescing deadline. A retained
    // RAW remainder is already eligible for the next cheap fill.
    if(stagedLength+UartLink::inputSize()<=capacity)return;
    stagedLength+=UartLink::readInput(staged+stagedLength,RadioCompression::MAX_RAW-stagedLength);
    size_t length=0;const uint32_t began=micros();++compressionProbes;
    const bool encoded=RadioCompression::encode(staged,stagedLength,cachedData,capacity,&length);
    const uint32_t elapsed=micros()-began;if(elapsed>compressionMaxUs)compressionMaxUs=elapsed;
    if(encoded && length && length<=capacity && length<stagedLength){
      cachedLength=uint8_t(length);cachedRawLength=stagedLength;
      cachedFlags=flags|BlockCodec::FLAG_COMPRESSED;cacheValid=true;
    }else compressionCooldown=COMPRESSION_COOLDOWN_BLOCKS;
    // Neither pendingData nor its source prefix in staged is consumed here.
    // A valid cache seals that prefix until the preceding block is ACKed.
  }
  void preparePending(uint32_t now) {
    noteInput(now);
    if(pending || !confirmed || sessionFault || !UartLink::healthy())return;
    const unsigned capacity=BlockCodec::packetCapacity(fastMode?BlockCodec::FLAG_FAST_FEC:0);
    const unsigned available=stagedLength+UartLink::inputSize();
    if(!available || (!stagedLength && available<capacity && uint32_t(now-firstInput)<5000))return;
    pendingFlags=fastMode?BlockCodec::FLAG_FAST_FEC:0;
    pendingFallback=false;
    unsigned consumed;
    if(cacheValid){
      pendingLength=cachedLength;consumed=cachedRawLength;pendingFlags=cachedFlags;
      memcpy(pendingData,cachedData,cachedLength);cacheValid=false;
      // A previous block may have selected RS32 since this cache was made.
      // Keep its encoded bytes intact, but honor that adaptation immediately.
      if(!fastMode && (pendingFlags&BlockCodec::FLAG_FAST_FEC)){
        if(pendingLength<=BlockCodec::PAYLOAD)pendingFlags&=~BlockCodec::FLAG_FAST_FEC;
        else{pendingFallback=true;cleanAcks=0;++robustFallbacks;}
      }
      ++compressedTxBlocks;compressedEncodedBytes+=pendingLength;
    }else{
      stagedLength+=UartLink::readInput(staged+stagedLength,RadioCompression::MAX_RAW-stagedLength);
      consumed=stagedLength<capacity?stagedLength:capacity;
      memcpy(pendingData,staged,consumed);pendingLength=uint8_t(consumed);++rawTxBlocks;
      if(compressionCooldown)--compressionCooldown;
    }
    pendingRawLength=uint16_t(consumed);
    stagedLength-=consumed;
    if(stagedLength)memmove(staged,staged+consumed,stagedLength);
    else inputTimed=false;
    pending=true;attempts=0;queuedAt=now;offered+=consumed;
    fragmentOffset=0;
    pendingEncodedCrc=BlockCodec::Detail::crc32(pendingData,pendingLength);
  }
  BlockCodec::Packet outgoing() {
    BlockCodec::Packet p={};p.role=MF_ROLE;p.session=session;
    p.seq=seq;p.ack=ack;p.ackSession=peerSession;
    if(ackValid && !sessionFault && UartLink::healthy())p.flags|=BlockCodec::FLAG_ACK;
    // Echo peer session in every CTRL or DATA frame for the initial handshake.
    if(confirmed && pending && !sessionFault && UartLink::healthy()){
      if((pendingFlags&BlockCodec::FLAG_FAST_FEC) && attempts>=3 && !pendingFallback){
        pendingFallback=true;fastMode=false;cleanAcks=0;fragmentOffset=0;++robustFallbacks;
      }
      p.flags|=BlockCodec::FLAG_DATA | pendingFlags;
      if(pendingFallback){
        p.flags&=~BlockCodec::FLAG_FAST_FEC;
        if(pendingLength>BlockCodec::PAYLOAD){
          p.flags|=BlockCodec::FLAG_FRAGMENT;
          const unsigned remaining=unsigned(pendingLength)-fragmentOffset;
          const unsigned count=remaining<FRAGMENT_BYTES?remaining:FRAGMENT_BYTES;
          p.payload[0]=pendingLength;p.payload[1]=fragmentOffset;
          BlockCodec::Detail::put32(p.payload+2,pendingEncodedCrc);
          memcpy(p.payload+FRAGMENT_HEADER,pendingData+fragmentOffset,count);
          p.length=uint8_t(FRAGMENT_HEADER+count);return p;
        }
      }
      p.length=pendingLength;memcpy(p.payload,pendingData,pendingLength);
    }
    return p;
  }
  void incoming(const BlockCodec::Packet& p,uint32_t start,int32_t off,int32_t unit) {
    (void)start;
    if(p.role==MF_ROLE || (p.role!=1 && p.role!=2) || !p.session)return;
    if(peerSession!=p.session){
      // A stale DATA frame can never replace an established peer identity.
      if(p.flags&BlockCodec::FLAG_DATA){++foreignData;return;}
      if(peerSession && (pending || stagedLength || delivered || offered)){
        if(restartSession!=p.session){++sessionChanges;restartSession=p.session;}
        sessionFault=true;confirmed=false;return;
      }
      if(peerSession)++sessionChanges;
      peerSession=p.session;haveRx=false;ackValid=false;confirmed=false;
      fragmentActive=false;fragmentMask=0;fastMode=false;cleanAcks=0;
    }
    lastRx=micros();lastOffset=off;lastUnit=unit;
    if(p.ackSession==session)confirmed=true;
    if(sessionFault || !UartLink::healthy())return;
    if(confirmed && (p.flags&2) && p.ackSession==session && pending && attempts && p.ack==seq){
      pending=false;acked+=pendingRawLength;++seq;
      if(attempts==1 && !pendingFallback){
        if(MF_ADAPTIVE_FEC && !fastMode && ++cleanAcks>=CLEAN_ACKS_TO_FAST){fastMode=true;cleanAcks=0;++fastPromotions;}
      }else cleanAcks=0;
      const uint32_t age=micros()-queuedAt;if(age>maxAckUs)maxAckUs=age;
    }
    if(!(p.flags&1) || !confirmed || p.ackSession!=session)return;
#if MF_DROP_EVERY > 0
    if((++dataCandidates%MF_DROP_EVERY)==0){++injected;return;}
#endif
    if(haveRx && p.seq==rxSeq){ack=p.seq;ackValid=true;++duplicates;return;}
    if(haveRx && uint16_t(p.seq-rxSeq)!=1){++unexpectedSeq;return;}
    if(fragmentActive && p.seq!=fragmentSeq){++unexpectedSeq;return;}
    if(p.flags&BlockCodec::FLAG_FRAGMENT){receiveFragment(p);return;}
    admit(p.seq,p.flags,p.payload,p.length);
  }
  bool admit(uint16_t receivedSeq,uint8_t flags,const uint8_t* bytes,unsigned length) {
    const uint8_t* raw=bytes;size_t rawLength=length;
    if(flags&BlockCodec::FLAG_COMPRESSED){
      rawLength=0;
      const uint32_t began=micros();
      const bool ok=RadioCompression::decode(bytes,length,decoded,sizeof(decoded),&rawLength);
      const uint32_t elapsed=micros()-began;if(elapsed>decompressionMaxUs)decompressionMaxUs=elapsed;
      if(!ok || !rawLength){
        ++compressionRejects;return false;
      }
      raw=decoded;
    }
    if(!rawLength || rawLength>RadioCompression::MAX_RAW){++compressionRejects;return false;}
    if(UartLink::outputFree()<rawLength || !UartLink::writeOutput(raw,rawLength)){++backpressure;return false;}
    rxSeq=receivedSeq;haveRx=true;ack=receivedSeq;ackValid=true;delivered+=rawLength;
    if(flags&BlockCodec::FLAG_COMPRESSED)++compressedRxBlocks;else ++rawRxBlocks;
    fragmentActive=false;fragmentMask=0;return true;
  }
  void receiveFragment(const BlockCodec::Packet& p) {
    if((p.flags&BlockCodec::FLAG_FAST_FEC) || p.length<=FRAGMENT_HEADER){++fragmentRejects;return;}
    const unsigned total=p.payload[0],offset=p.payload[1],count=p.length-FRAGMENT_HEADER;
    if(total<=BlockCodec::PAYLOAD || total>BlockCodec::MAX_PAYLOAD ||
       (offset!=0 && offset!=FRAGMENT_BYTES) || offset>=total ||
       count!=(total-offset<FRAGMENT_BYTES?total-offset:FRAGMENT_BYTES)){
      ++fragmentRejects;return;
    }
    const uint32_t crc=BlockCodec::Detail::get32(p.payload+2);
    const uint8_t flags=p.flags&BlockCodec::FLAG_COMPRESSED;
    if(fragmentActive && (fragmentSeq!=p.seq || fragmentLength!=total || fragmentCrc!=crc || fragmentFlags!=flags)){
      ++fragmentRejects;return;
    }
    if(!fragmentActive){fragmentActive=true;fragmentSeq=p.seq;fragmentLength=uint8_t(total);
      fragmentCrc=crc;fragmentFlags=flags;fragmentMask=0;}
    const uint8_t mask=offset?2:1;
    if(fragmentMask&mask){
      if(memcmp(fragmentData+offset,p.payload+FRAGMENT_HEADER,count)){++fragmentRejects;return;}
    }else{memcpy(fragmentData+offset,p.payload+FRAGMENT_HEADER,count);fragmentMask|=mask;}
    ++fragmentRx;
    if(fragmentMask!=3)return;
    if(BlockCodec::Detail::crc32(fragmentData,fragmentLength)!=fragmentCrc){
      ++fragmentRejects;fragmentActive=false;fragmentMask=0;return;
    }
    admit(p.seq,fragmentFlags,fragmentData,fragmentLength);
  }
#if MF_RTC_DIAGNOSTICS && !defined(MF_TRANSPORT_HOST_TEST)
  bool rtcCaptured=false;
  bool timedRtcCaptured=false;
  void captureTimedRtc() {
#if MF_DIAG_AFTER_MS > 0
    // Preserve the first observation: a later reset/serial-tool BREAK must
    // not replace the timed capture, nor may the timer hide an earlier fault.
    if(MF_DIAG_AFTER_MS && !rtcCaptured && !timedRtcCaptured && uint32_t(micros()-born)/1000>=uint32_t(MF_DIAG_AFTER_MS)){
      captureRtc(2);timedRtcCaptured=true;
    }
#endif
  }
  void captureRtc(uint32_t cause) {
    uint32_t record[64]={};
    record[0]=0x54524157;record[1]=1;record[2]=MF_ROLE;record[3]=session;
    record[4]=cause;record[5]=micros();record[6]=MF_BAUD;
    record[7]=UartLink::healthy();record[8]=sessionFault;
    record[9]=UartLink::inputSize();record[10]=UartLink::outputFree();
    record[11]=UartLink::peerPaused();record[12]=UartLink::inputPaused();
    record[13]=UartLink::Detail::txPausedForRf;
    record[14]=ESP.getFreeHeap();record[15]=ESP.getFreeContStack();
    const UartLink::Stats snapshot=UartLink::snapshotStats();
    static_assert(sizeof(snapshot)<=176,"RTC diagnostic capacity");
    memcpy(record+16,&snapshot,sizeof(snapshot));
    record[60]=offered;record[61]=delivered;record[62]=acked;
    for(unsigned i=0;i<63;++i)record[63]^=record[i];
    ESP.rtcUserMemoryWrite(32,record,sizeof(record));
    // 32 words at offset96 occupy the last128 bytes of the512-byte RTC area.
    // The earlier UART record remains byte-for-byte compatible.
    uint32_t extra[32]={};
    extra[0]=0x54525032;extra[1]=1;extra[2]=cause;extra[3]=MF_ROLE;
    extra[4]=compressedTxBlocks;extra[5]=rawTxBlocks;extra[6]=compressedRxBlocks;extra[7]=rawRxBlocks;
    extra[8]=decompressionMaxUs;extra[9]=compressionRejects;extra[10]=fastPromotions;extra[11]=robustFallbacks;
    extra[12]=fragmentTx;extra[13]=fragmentRx;extra[14]=fragmentRejects;extra[15]=resyncs;
    extra[16]=dataTx;extra[17]=radio.stats.tx;extra[18]=radio.stats.rx;extra[19]=radio.stats.invalid;
    extra[20]=missTx;extra[21]=txFail;extra[22]=radio.stats.dspMax;extra[23]=radio.stats.prepMax;
    extra[24]=retries;extra[25]=duplicates;extra[26]=backpressure;extra[27]=blackoutIgnored;
    extra[28]=compressionMaxUs;extra[29]=fillMaxUs;extra[30]=micros();
    for(unsigned i=0;i<31;++i)extra[31]^=extra[i];
    ESP.rtcUserMemoryWrite(96,extra,sizeof(extra));
  }
#else
  void captureTimedRtc() {}
#endif
  // Transparent UART: no startup banner, status JSON, or in-band commands.
  void report() { wantStats=false; }
};

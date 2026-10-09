#ifndef ESP8266_TDM_H
#define ESP8266_TDM_H
#include "RH_ESP8266FSK.h"
#include "TdmProtocol.h"

class ESP8266TDM {
public:
  enum Role { MASTER=1, FOLLOWER=2 };
  enum State { SEARCH, SYNC, LOCKED };
  static const uint8_t MAX_PAYLOAD=TdmProtocol::PAYLOAD_SIZE, QUEUE_SIZE=32;
  struct Message {uint16_t id=0;uint8_t length=0;uint8_t data[MAX_PAYLOAD]={};};
  struct Statistics {
    uint32_t txCells=0,rxCells=0,missed=0,invalid=0,txFailures=0;
    uint32_t delivered=0,acknowledged=0,retries=0,duplicates=0,queueFull=0,resyncs=0;
    uint32_t lastRxMs=0;
    uint32_t rateUps=0,rateDowns=0,recoveries=0;
    uint32_t clockSyncs=0;
    uint32_t deliveredBytes=0,acknowledgedBytes=0,txWindows=0,outOfOrder=0;
    uint16_t windowBytes=0;
    uint8_t windowFrames=0;
    int32_t clockErrorUs=0;
    uint16_t cycle=0;
    uint8_t streak=0,missStreak=0,peerCorrections=0;
  };
  explicit ESP8266TDM(RH_ESP8266FSK& radio):_radio(radio){}
  bool begin(Role role,uint32_t session=0);
  void poll();
  bool send(const uint8_t* data,uint8_t length,uint16_t* id=nullptr);
  // Returns bytes accepted into the bounded queue. Retry the remainder later.
  size_t write(const uint8_t* data,size_t length);
  size_t availableForWrite() const {return size_t(QUEUE_SIZE-_txCount)*MAX_PAYLOAD;}
  size_t queuedBytes() const;
  bool recv(Message& message);
  uint8_t queued() const {return _txCount;}
  uint8_t pendingReceive() const {return _rxCount;}
  State state() const {return _state;}
  const char* stateName() const;
  Statistics statistics() const {return _stats;}
  uint32_t session() const {return _session;}
  uint32_t cycleUs() const {return 2*_slotUs;}
  uint8_t rate() const {return _rate.rate;}
  uint32_t bitUs() const {return TdmRate::bitUs(_rate.rate);}
  uint32_t symbolUs() const {return TdmRate::bitUs(_rate.rate);}
  uint32_t rawBitRate() const {return 2000000UL/symbolUs();}
  void setAdaptive(bool enabled){_rate.automatic=enabled;}
  void setRateLimit(uint8_t limit){if(limit<TdmRate::COUNT)_rate.limit=limit;}
  uint8_t rateLimit() const {return _rate.limit;}
  uint32_t probeWaitMs() const {return _rate.cooldownMs(_rate.rate+1,millis());}
  uint8_t rateGood() const {return _rate.good;}
  uint8_t rateQuality() const {return _rate.quality;}
  uint8_t rateBad() const {return _rate.bad;}
  uint8_t rateLossWindow() const {return _rate.lossWindow;}
  // A pause suspends RF transmission while retaining queued application data.
  void pause(bool paused);
  bool paused() const {return _paused;}
  bool maintenanceWindow() const;
  uint32_t maintenanceUs() const;
private:
  RH_ESP8266FSK& _radio;
  Role _role=MASTER;
  State _state=SEARCH;
  Statistics _stats;
  uint32_t _session=0,_peerSession=0,_slotUs=0,_due=0,_expiry=0,_txStarted=0,_txWindowEnd=0;
  uint32_t _guardUs=150000,_airUs=0;
  bool _ready=false,_paused=false,_pendingReply=false,_awaiting=false,_txActive=false,_applyRateAfterWindow=false;
  bool _haveCycle=false,_haveData=false,_haveBase=false;
  bool _rxWindowOpen=false,_rxWindowSeen=false,_rxGap=false,_txLast=false;
  bool _quiet=false,_ratePending=false;
  uint32_t _quietUntil=0,_rxWindowEnd=0;
  uint8_t _rxRequest=0,_rxCorrections=0,_rxPeerCorrections=0,_pendingRate=0;
  uint16_t _cycle=0,_lastData=0,_nextData=0,_rxNext=0;
  Message _txQueue[QUEUE_SIZE],_rxQueue[QUEUE_SIZE];
  uint8_t _txHead=0,_txCount=0,_rxHead=0,_rxCount=0;
  uint16_t _attempts[QUEUE_SIZE]={};
  uint8_t _txCursor=0;
  uint16_t _txWindowBytes=0;
  TdmRate::Controller _rate;
  uint8_t _offered=0,_replyRate=0,_txWindowFrames=0;
  uint8_t _txWire[TdmProtocol::WIRE_SIZE]={};
  void applyRate(uint8_t next,bool failed=false);
  void recover(bool failed=true);
  bool beginTransmitWindow(uint32_t windowEnd);
  bool transmitFrame();
  void finishTransmitWindow();
  void finishReceiveWindow();
  void receive(const TdmProtocol::Cell& cell,uint32_t receivedAt,uint32_t peerRemainingUs);
  void missed();
};
#endif

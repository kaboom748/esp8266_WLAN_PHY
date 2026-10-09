#ifndef RH_ESP8266FSK_h
#define RH_ESP8266FSK_h
#include "RHGenericDriver.h"
#include "Fsk4.h"
#include "FskFrame.h"
#define RH_ESP8266FSK_MAX_MESSAGE_LEN 32
#define RH_ESP8266FSK_HEADER_LEN 4
class RH_ESP8266FSK: public RHGenericDriver {
public:
  enum ModemConfigChoice {
    FSK_Rb250Reliable,
    FSK_Rb333Reliable,
    FSK_Rb500Experimental,
    FSK_Rb667Experimental,
    FSK_Rb1000Experimental,
    FSK_Rb500Conservative=FSK_Rb333Reliable,
    FSK_Rb1000Portable=FSK_Rb333Reliable,
    FSK_Rb2000Experimental=FSK_Rb500Experimental,
    FSK_Rb4000Experimental=FSK_Rb667Experimental
  };
  struct ModemConfig {
    uint32_t bitUs;
    uint16_t toneZero;
    uint16_t toneOne;
    uint8_t apwr;
    uint8_t ask;
    uint16_t frameGapMs;
    uint8_t txRepeats;
    uint8_t preambleBits;
    uint32_t calUs;
    int32_t minToneSepHz;
  };
  struct Calibration {
    bool valid;
    int32_t zeroHz;
    int32_t oneHz;
    int32_t sepHz;
    bool inverted;
    uint32_t atMs;
  };
  struct Statistics {
    uint32_t polls=0, workUs=0, maxPollUs=0, samples=0, slowPolls=0, txLateUs=0, txAborts=0, syncs=0, correctionRejects=0;
    uint32_t sampleStepNs=0,sampleJitterNs=0,lastEnergy=0;
    uint32_t crcFrames=0,addressRejects=0,decodedCandidates=0,candidateDrops=0;
    uint32_t fillBits=0,iqSamples=0;
    uint8_t lastTo=0,lastFrom=0,lastLength=0;
    int32_t lastHz=0;
  };
  Statistics statistics() const {return _stats;}
  bool setFourFsk(bool enabled){
    if(_radioReady || _mode==RHModeTx)return false;
    _fourFsk=enabled;return true;
  }
  bool fourFsk() const {return _fourFsk;}
  uint32_t symbolUs() const {return _bitUs;}
  bool fourToneCenters(int32_t centers[4]) const {
    if(!_fourFsk || !_trained)return false;
    memcpy(centers,_centers4,sizeof(_centers4));return true;
  }
  struct RfState {uint16_t path,rfWord,bbWord;bool manual,rxStopped;uint8_t apwr,ask,txAskField;};
  RfState rfState() const;
  bool setReceiveGain(int8_t rf,uint8_t bb=2);
  bool setIqMode(uint8_t mode);
  uint8_t iqMode() const {return _iqMode;}
  void resetStatistics(){_stats=Statistics();}
  void setCalibrationMemory(uint32_t ms){_calMemoryMs=ms;}
  void restartReceive(bool forget=false){if(forget)_lastCal.valid=false; resetReceiver();}
  uint32_t lastReceiveUs() const {return _lastReceiveUs;}
  void setReceiveDeadline(uint32_t deadline){_receiveDeadline=deadline;}
  uint8_t lastCorrections() const {return _lastCorrections;}
  bool receivingFrame() const {for(const auto& d:_decoders)if(d.active)return true;return false;}
  RH_ESP8266FSK(uint8_t channel=6);
  void poll();
  void setMode(RHMode mode) override;
  bool sleep() override;
  bool waitPacketSent();
  bool waitPacketSent(uint16_t timeout);
  // One calibration per burst; retain the RF TX path between its frames.
  bool sendBurstFrame(const uint8_t* data,uint8_t len,bool last);
  void endTxBurst();
  bool init(); bool available(); bool recv(uint8_t* buf,uint8_t* len); bool send(const uint8_t* data,uint8_t len); uint8_t maxMessageLength(); bool supportsAckBeacon(); bool sendAckBeacon(uint8_t repeats=3,uint16_t gapMs=80); bool waitAckBeacon(uint16_t timeoutMs);
  bool setModemConfig(ModemConfigChoice choice);
  bool setModemConfig(const ModemConfig& cfg);
  bool setBitRate(uint16_t bitsPerSecond);
  void setTones(uint16_t zero,uint16_t one); void setPower(uint8_t apwr,uint8_t ask); void setBitUs(uint32_t bitUs); void setFrameGap(uint16_t ms); void setTxRepeats(uint8_t repeats); void setPollBudgetUs(uint16_t us); void setMaxCorrections(uint8_t corrections); void setTxFillUntil(uint32_t deadlineUs);
  uint32_t frameDurationUs(uint8_t len,bool continuation=false) const {
    if(len>RH_ESP8266FSK_MAX_MESSAGE_LEN)return 0;
    uint32_t bits=FskFrame::bitCount(len,_preambleBits);
    return (continuation?0:(_fourFsk?4:2)*_calUs)+(_fourFsk?(bits+1)/2:bits)*_bitUs;
  }
  uint32_t bitUs() const; uint16_t bitRate() const; Calibration lastCalibration() const; uint8_t maxCorrections() const;
protected:
  struct FreqEst{uint32_t t; int32_t hz; uint32_t e4; bool valid;};
  bool initRadioBase(); bool initRxPath(); bool initTxPath(bool gate=true); void txTone(uint16_t tone); bool receiveRaw(uint8_t* raw,uint8_t* rawLen);
  bool acquireFreq(FreqEst& f); uint8_t ham74Encode(uint8_t n); uint8_t ham74Decode(uint8_t code,uint8_t& corrected); uint16_t crc16(const uint8_t* data,uint8_t len);
  bool _radioReady=false, _txSucceeded=true;
  bool _fourFsk=false;
  bool _txHeld=false,_holdTx=false;
  bool startFrame(const uint8_t* data,uint8_t len,bool continuation,bool holdTx);
  int8_t _rxGain=-1;
  uint8_t _rxBbGain=2,_lastTxAskField=0,_iqMode=1;
  void applyReceiveGain();
  uint8_t _wire[(64+12+14+56*((RH_ESP8266FSK_MAX_MESSAGE_LEN+9)/4)+7)/8] = {};
  uint16_t _wireBits=0,_wireIndex=0;
  uint8_t _txStage=0,_txRepeatIndex=0,_pilotIndex=0;
  uint32_t _txDeadline=0,_txFillUntil=0;
  bool _txFillBit=false;
  void serviceTx();
  bool transmitBits();
  void appendBit(bool bit);
  Statistics _stats;
  typedef FskFrame::Decoder Decoder;
  Decoder _decoders[8] = {};
  int32_t _training4[Fsk4::TRAINING_SIZE]={},_centers4[4]={};
  uint16_t _training4N=0,_training4Pos=0;
  uint8_t _votes4[8][4]={};
  int32_t _training[16] = {};
  uint8_t _trainingN=0, _trainingPos=0;
  uint32_t _nextSample=0, _nextTraining=0, _epoch=0, _bin=0, _lockAt=0,_receiveDeadline=0;
  int16_t _scores[8] = {};
  uint8_t _counts[8] = {};
  int32_t _low=0, _high=0;
  bool _trained=false;
  int64_t _dcI=0,_dcQ=0;
  uint32_t _calMemoryMs=0, _lastReceiveUs=0;
  uint8_t _lastCorrections=0;
  uint8_t _candidate[1+RH_ESP8266FSK_HEADER_LEN+RH_ESP8266FSK_MAX_MESSAGE_LEN+2]={};
  uint8_t _candidateLen=0,_candidateCorrections=0;
  uint32_t _candidateAt=0;
  Calibration _candidateCal{};
  bool finishCandidate(uint8_t* raw,uint8_t* rawLen);
  void resetReceiver();
  bool train(int32_t hz);
  bool trainFour(int32_t hz);
  bool decodeBit(Decoder& d,bool bit,uint8_t* raw,uint8_t* rawLen,bool firstInSymbol=false);
  uint8_t _channel; uint16_t _toneZero,_toneOne; uint8_t _apwr,_ask; uint32_t _bitUs; uint16_t _gapMs; uint8_t _txRepeats; uint16_t _pollBudgetUs; uint8_t _preambleBits; uint32_t _calUs; int32_t _minToneSepHz; uint8_t _maxCorrections; Calibration _lastCal; bool _bufValid; uint8_t _buf[RH_ESP8266FSK_MAX_MESSAGE_LEN]; uint8_t _bufLen;
};
#endif

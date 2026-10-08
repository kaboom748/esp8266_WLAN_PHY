#ifndef RHReliableDatagram_h
#define RHReliableDatagram_h
#include "RHDatagram.h"
#define RH_ESP8266FSK_ACK_BUFFER_LEN 32
class RHReliableDatagram: public RHDatagram {
public:
  RHReliableDatagram(RHGenericDriver& driver, uint8_t thisAddress = 0);
  void setTimeout(uint16_t timeout); void setRetries(uint8_t retries); uint8_t retries(); void setAckTiming(uint16_t delayMs, uint8_t repeats=1, uint16_t gapMs=0); void setUseAckBeacon(bool enable);
  bool sendtoWait(uint8_t* buf, uint8_t len, uint8_t address);
  bool recvfromAck(uint8_t* buf, uint8_t* len, uint8_t* from=NULL, uint8_t* to=NULL, uint8_t* id=NULL, uint8_t* flags=NULL);
  bool recvfromAckTimeout(uint8_t* buf, uint8_t* len, uint16_t timeout, uint8_t* from=NULL, uint8_t* to=NULL, uint8_t* id=NULL, uint8_t* flags=NULL);
  uint32_t retransmissions(); void resetRetransmissions();
protected:
  void acknowledge(uint8_t id, uint8_t from); bool haveNewMessage(uint8_t from,uint8_t id,uint8_t flags);
private:
  uint32_t _retransmissions; uint8_t _lastSequenceNumber; uint16_t _timeout; uint8_t _retries; uint16_t _ackDelayMs; uint8_t _ackRepeats; uint16_t _ackGapMs; bool _useAckBeacon; uint8_t _seenIds[256]; uint8_t _seenValid[32];
};
#endif

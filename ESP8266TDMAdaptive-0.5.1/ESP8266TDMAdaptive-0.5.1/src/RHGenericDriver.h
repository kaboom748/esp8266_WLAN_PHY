#ifndef RHGenericDriver_h
#define RHGenericDriver_h
#include "RHCompat.h"
class RHGenericDriver {
public:
  enum RHMode { RHModeInitialising=0, RHModeSleep, RHModeIdle, RHModeTx, RHModeRx, RHModeCad };
  RHGenericDriver();
  virtual ~RHGenericDriver() {}
  virtual bool init();
  virtual bool available() = 0;
  virtual bool recv(uint8_t* buf, uint8_t* len) = 0;
  virtual bool send(const uint8_t* data, uint8_t len) = 0;
  virtual uint8_t maxMessageLength() = 0;
  virtual void waitAvailable();
  virtual bool waitPacketSent();
  virtual bool waitPacketSent(uint16_t timeout);
  virtual bool waitAvailableTimeout(uint16_t timeout);
  virtual bool waitCAD();
  virtual bool isChannelActive();
  virtual bool supportsAckBeacon();
  virtual bool sendAckBeacon(uint8_t repeats=3, uint16_t gapMs=80);
  virtual bool waitAckBeacon(uint16_t timeoutMs);
  void setCADTimeout(unsigned long cad_timeout);
  virtual void setThisAddress(uint8_t thisAddress);
  virtual void setHeaderTo(uint8_t to);
  virtual void setHeaderFrom(uint8_t from);
  virtual void setHeaderId(uint8_t id);
  virtual void setHeaderFlags(uint8_t set, uint8_t clear = RH_FLAGS_APPLICATION_SPECIFIC);
  virtual void setPromiscuous(bool promiscuous);
  virtual uint8_t headerTo();
  virtual uint8_t headerFrom();
  virtual uint8_t headerId();
  virtual uint8_t headerFlags();
  virtual int16_t lastRssi();
  virtual RHMode mode();
  virtual void setMode(RHMode mode);
  virtual bool sleep();
  static void printBuffer(const char* prompt, const uint8_t* buf, uint8_t len);
  virtual uint16_t rxBad();
  virtual uint16_t rxGood();
  virtual uint16_t txGood();
protected:
  volatile RHMode _mode;
  uint8_t _thisAddress, _txHeaderTo, _txHeaderFrom, _txHeaderId, _txHeaderFlags;
  uint8_t _rxHeaderTo, _rxHeaderFrom, _rxHeaderId, _rxHeaderFlags;
  bool _promiscuous;
  int16_t _lastRssi;
  uint16_t _rxBad, _rxGood, _txGood;
  unsigned long _cad_timeout;
};
#endif

#ifndef RHDatagram_h
#define RHDatagram_h
#include "RHGenericDriver.h"
class RHDatagram {
public:
  RHDatagram(RHGenericDriver& driver, uint8_t thisAddress = 0);
  bool init();
  void setThisAddress(uint8_t thisAddress);
  bool sendto(uint8_t* buf, uint8_t len, uint8_t address);
  bool recvfrom(uint8_t* buf, uint8_t* len, uint8_t* from=NULL, uint8_t* to=NULL, uint8_t* id=NULL, uint8_t* flags=NULL);
  bool available(); void waitAvailable(); bool waitPacketSent(); bool waitPacketSent(uint16_t timeout); bool waitAvailableTimeout(uint16_t timeout);
  void setHeaderTo(uint8_t to); void setHeaderFrom(uint8_t from); void setHeaderId(uint8_t id); void setHeaderFlags(uint8_t set, uint8_t clear=RH_FLAGS_NONE);
  uint8_t headerTo(); uint8_t headerFrom(); uint8_t headerId(); uint8_t headerFlags(); uint8_t thisAddress();
protected:
  RHGenericDriver& _driver; uint8_t _thisAddress;
};
#endif

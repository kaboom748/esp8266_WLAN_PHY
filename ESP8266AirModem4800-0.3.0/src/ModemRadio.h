#pragma once
#include "RH_ESP8266FSK.h"
#include "ModemSignal.h"

namespace AirModem { namespace Phy {
class Radio : private RH_ESP8266FSK {
public:
  struct Stats {
    uint32_t tx = 0, txAbort = 0, captureAbort = 0, sampleLateCycles = 0;
    uint32_t prepareUs = 0, setupUs = 0, startLateUs = 0, demodUs = 0, acquireUs = 0;
  } stats;
  explicit Radio(uint8_t channel = 6) : RH_ESP8266FSK(channel) {}
  bool begin();
  bool prepare(const Frame& frame, int32_t offsetHz = 0);
  bool transmitAt(uint32_t atUs);
  bool receive(unsigned samples, Acquisition& result, uint32_t& capturedAtUs);
  bool captureIQ(int32_t* samples, unsigned count);
  bool testCarrier(uint16_t code = 0);
private:
  int32_t buffer[MAX_CAPTURE];
  Signal signal;
  bool ready = false, prepared = false;
  bool IRAM_ATTR transmitSamples(uint32_t atUs);
  bool IRAM_ATTR capture(unsigned count, uint32_t& atUs);
};
}}

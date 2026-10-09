#pragma once
#include "ModemLink.h"

namespace AirModem {
class Modem {
public:
  struct Stats {
    uint32_t xoff = 0, xon = 0, hostPauses = 0, uartErrors = 0, overflows = 0;
    uint32_t maxQueued = 0;
    uint32_t inputBytes = 0, outputBytes = 0, inputCrc = 0xffffffffu, outputCrc = 0xffffffffu;
    uint32_t cableDropped = 0;
  } stats;
  struct RadioStats {
    uint32_t tx = 0, rx = 0, aborts = 0;
    int32_t offset = 0, unit = 0, shift = 0;
  } radioStats;
  Modem(uint8_t role, uint32_t boot) : link(role, boot), random_(boot ? boot : 1) {}
  void input(uint8_t byte, uint32_t now);
  int output(uint32_t now);
  void tick(uint32_t now);
  void uartFailure(uint32_t now);
  bool online() const { return link.state() == State::Connected; }
  bool cableMode() const { return true; }
  bool inputPaused() const { return inputStopped; }
  bool hostPaused() const { return hostStopped; }
  Link link;
private:
  void updateFlow(uint32_t now);
  uint32_t nonce();
  RingBuffer<1024> cablePending;
  bool inputStopped = false, hostStopped = false, sessionEstablished = false;
  int flowPending = 0x11;
  uint32_t lastFlow = 0, random_;
};
}

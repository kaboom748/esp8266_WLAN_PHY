#pragma once
#include "AirModem.h"
#include "ModemRadio.h"

namespace AirModem {
class Firmware {
public:
  explicit Firmware(uint8_t role) : modem(role, 1), role(role) {}
  void begin();
  void poll();
private:
  static constexpr uint32_t HALF = 140000, CYCLE = 2 * HALF;
  static constexpr uint32_t PREPARE = 20000;
  Modem modem;
  Phy::Radio radio;
  uint8_t role, phase = 0, misses = 0;
  uint32_t next = 0, epoch = 0;
  int16_t txHz = 0, tuneHz = NO_TUNE;
  bool ready = false, locked = false;
  void serial();
  bool transmit(uint32_t at);
  bool receive(unsigned samples, uint32_t& start);
};
}

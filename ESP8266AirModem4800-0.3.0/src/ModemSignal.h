#pragma once
#include "ModemProtocol.h"

namespace AirModem { namespace Phy {
constexpr unsigned SPS = 8, SAMPLE_RATE = 38400, SYMBOL_RATE = 4800;
constexpr unsigned FRAME_BYTES = PACKET_BYTES + 6, SYMBOLS = FRAME_BYTES * 4;
constexpr unsigned SLOT_SAMPLES = (SYMBOLS + 12) * SPS, MAX_CAPTURE = 4096;
constexpr unsigned RRC_TAPS = 81, RRC_DELAY = 40, FIRST_CENTER = 52, SYNC_AT = 160;
constexpr uint64_t SYNC = 0xd3916ac5f027ULL;
struct Frame { uint8_t bytes[FRAME_BYTES] = {}; };
struct Acquisition { Packet packet; int32_t firstCenter = 0, offsetHz = 0, unitHz = 0; };
inline int8_t symbol(uint8_t d) { return d == 0 ? 1 : d == 1 ? 3 : d == 2 ? -1 : -3; }
void makeFrame(const Packet& packet, Frame& frame);
bool readFrame(const Frame& frame, Packet& packet);
int32_t packIq(int16_t i, int16_t q);
int32_t phaseHz(int32_t cross, int32_t dot);
int32_t demodulate(int32_t* samples, unsigned count);
class Signal {
public:
  Signal();
  void waveform(const Frame& frame, int32_t out[SLOT_SAMPLES], int32_t offsetHz = 0) const;
  bool acquire(const int32_t* hz, unsigned count, Acquisition& result);
private:
  int32_t tx[RRC_TAPS];
  int16_t rx[RRC_TAPS], filtered[MAX_CAPTURE];
};
}}

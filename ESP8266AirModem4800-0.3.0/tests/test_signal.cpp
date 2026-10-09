#include "ModemSignal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <random>
using namespace AirModem;
using namespace AirModem::Phy;
static Signal signal;
static int32_t wave[SLOT_SAMPLES], samples[MAX_CAPTURE];
int main() {
  std::mt19937 rng(347); unsigned cases = 0;
  for (int invert : {-1, 1}) for (int offset : {-19200, -19000, -16000, -8000, 0, 9000, 17000, 19000, 19200}) {
    for (unsigned delay : {0u, 137u, 900u}) {
      Packet p; p.type = Type::Data; p.role = 1; p.boot = 22; p.session = 123; p.length = PAYLOAD;
      for (auto& b : p.data) b = uint8_t(rng());
      Frame frame; makeFrame(p, frame); signal.waveform(frame, wave);
      double phase = 0;
      for (unsigned n = 0; n < MAX_CAPTURE; ++n) {
        int32_t hz = n >= delay && n < delay + SLOT_SAMPLES ? int64_t(wave[n - delay]) * 78125 / 65536 : 0;
        phase += 6.283185307179586 * (offset + invert * hz) / SAMPLE_RATE;
        int amplitude = n >= delay && n < delay + SLOT_SAMPLES ? 12000 : 0;
        int16_t i = int16_t(amplitude * cos(phase) + 700 + int(rng() % 121) - 60);
        int16_t q = int16_t(amplitude * sin(phase) - 500 + int(rng() % 121) - 60);
        samples[n] = packIq(i, q);
      }
      demodulate(samples, MAX_CAPTURE); Acquisition got;
      if (!signal.acquire(samples, MAX_CAPTURE, got)) {
        fprintf(stderr, "PHY miss invert=%d offset=%d delay=%u\n", invert, offset, delay);
        return 1;
      }
      assert(!memcmp(got.packet.data, p.data, PAYLOAD)); ++cases;
    }
  }
  for (unsigned n = 0; n < MAX_CAPTURE; ++n) samples[n] = int(rng() % 38401) - 19200;
  Acquisition absent; assert(!signal.acquire(samples, MAX_CAPTURE, absent));
  printf("PASS PHY: %u IQ cases, offsets/inversion/noise/timing; noise rejected\n", cases);
}

#include "ModemRadio.h"
#include <math.h>

namespace AirModem { namespace Phy {
namespace {
constexpr uint32_t TONE = 0x600005B8u, IQCTRL = 0x6000057Cu;
constexpr uint32_t IREG = 0x600005DCu, QREG = 0x600005E0u;
constexpr uint32_t GATE = 0x40000u, FIELDS = 0x7FFFFu;
inline void fence() { __asm__ volatile("memw" ::: "memory"); }
inline volatile uint32_t& reg(uint32_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
inline int16_t shrink(int32_t value) {
  value /= 1024;
  return value > 32767 ? 32767 : value < -32768 ? -32768 : int16_t(value);
}
}
bool Radio::begin() {
  if (ready || ESP.getCpuFreqMHz() != 160) return false;
  setTones(0, 1);
  ready = init();
  return ready;
}
bool Radio::prepare(const Frame& frame, int32_t offsetHz) {
  if (!ready || offsetHz < -12000 || offsetHz > 12000) return false;
  uint32_t began = micros();
  signal.waveform(frame, buffer, offsetHz);
  uint32_t spent = micros() - began;
  if (spent > stats.prepareUs) stats.prepareUs = spent;
  prepared = true;
  return true;
}
bool Radio::transmitAt(uint32_t atUs) {
  if (!ready || !prepared) return false;
  prepared = false;
  uint32_t began = micros();
  bool ok = initTxPath(false);
  uint32_t spent = micros() - began;
  if (spent > stats.setupUs) stats.setupUs = spent;
  if (ok) ok = transmitSamples(atUs);
  reg(TONE) &= ~GATE; fence();
  if (!initRxPath()) { ready = false; ok = false; }
  if (ok) ++stats.tx; else ++stats.txAbort;
  return ok;
}
bool IRAM_ATTR Radio::transmitSamples(uint32_t atUs) {
  int32_t lateStart = int32_t(micros() - atUs);
  if (lateStart > 0 && uint32_t(lateStart) > stats.startLateUs) stats.startLateUs = lateStart;
  if (lateStart > 100) return false;
  uint32_t ps;
  __asm__ volatile("rsil %0, 15" : "=a"(ps) :: "memory");
  while (int32_t(micros() - atUs) < 0) {}
  volatile uint32_t* tone = reinterpret_cast<volatile uint32_t*>(TONE);
  uint32_t base = *tone & ~FIELDS;
  uint32_t deadline = ESP.getCycleCount(), fraction = 0, last = deadline;
  int32_t code = 0;
  int32_t error = 0;
  bool ok = true;
  // Fractional tone generation adapted from SP8ESA's MIT-licensed ditherBurst.
  // The integrated frequency error is kept across all shaped waveform samples.
  for (unsigned n = 0; n < SLOT_SAMPLES; ++n) {
    deadline += 4166; fraction += 25600;
    if (fraction >= 38400) { fraction -= 38400; ++deadline; }
    const int32_t q = buffer[n];
    int32_t nearest = (q + 32768) >> 16;
    bool enabled = n >= 12 && n < SLOT_SAMPLES - 12;
    unsigned edge = n < SLOT_SAMPLES / 2 ? n : SLOT_SAMPLES - 1 - n;
    unsigned ask = edge <= 12 ? 127 : edge < 44 ? (44 - edge) * 4 : 0;
    uint32_t amplitude = enabled ? GATE | (uint32_t(uint8_t(0u - ask)) << 10) : 0;
    do {
      uint32_t now = ESP.getCycleCount(), dt = now - last;
      if (dt > 8192) { ok = false; break; }
      int32_t errNow = error + (q - code * 65536) * int32_t(dt);
      if (errNow > (20 << 16) || errNow < -(20 << 16)) {
        bool up = errNow > 0;
        int32_t residual = q - nearest * 65536;
        int32_t rate = up ? 65536 - residual : 65536 + residual;
        int32_t length = (up ? errNow : -errNow) / rate;
        if (length > 4000) length = 4000;
        int32_t remaining = int32_t(deadline - now);
        if (length > remaining) length = remaining;
        if (length > 0) {
          *tone = base | (uint32_t(nearest + (up ? 1 : -1)) & 1023u) | amplitude;
          fence();
          uint32_t pulseAt = ESP.getCycleCount();
          dt = pulseAt - last;
          if (dt > 8192) { ok = false; break; }
          error += (q - code * 65536) * int32_t(dt);
          code = nearest + (up ? 1 : -1);
          while (int32_t(ESP.getCycleCount() - pulseAt) < length) {}
          last = pulseAt;
        }
      }
      *tone = base | (uint32_t(nearest) & 1023u) | amplitude;
      fence();
      uint32_t writtenAt = ESP.getCycleCount();
      dt = writtenAt - last;
      if (dt > 8192) { ok = false; break; }
      error += (q - code * 65536) * int32_t(dt);
      if (error > (1 << 29)) error = 1 << 29;
      if (error < -(1 << 29)) error = -(1 << 29);
      code = nearest;
      last = writtenAt;
    } while (int32_t(ESP.getCycleCount() - deadline) < 0);
    uint32_t late = ESP.getCycleCount() - deadline;
    if (late > stats.sampleLateCycles) stats.sampleLateCycles = late;
    if (!ok || late > 1600) { ok = false; break; }
  }
  *tone &= ~GATE; fence();
  __asm__ volatile("wsr %0, ps; rsync" :: "a"(ps) : "memory");
  return ok;
}
bool IRAM_ATTR Radio::capture(unsigned count, uint32_t& atUs) {
  volatile uint32_t* ctrl = reinterpret_cast<volatile uint32_t*>(IQCTRL);
  const uint32_t cfg = (*ctrl & 0x7FF80000u) | (127u << 2) | 0x40000u;
  uint32_t ps;
  __asm__ volatile("rsil %0, 15" : "=a"(ps) :: "memory");
  atUs = micros();
  uint32_t next = ESP.getCycleCount(), fraction = 0;
  bool ok = true;
  for (unsigned n = 0; n < count; ++n) {
    *ctrl = cfg; *ctrl = cfg | 1; fence();
    while (int32_t(ESP.getCycleCount() - next) < 0) {}
    uint32_t started = ESP.getCycleCount(), late = started - next;
    if (late > stats.sampleLateCycles) stats.sampleLateCycles = late;
    if (late > 800) { ok = false; break; }
    *ctrl = cfg | 3; fence();
    while (!(*ctrl & 0x80000000u)) {
      if (ESP.getCycleCount() - started > 3200) { ok = false; break; }
    }
    if (!ok) break;
    uint16_t i = uint16_t(shrink(int32_t(reg(IREG))));
    uint16_t q = uint16_t(shrink(int32_t(reg(QREG))));
    buffer[n] = int32_t(i | (uint32_t(q) << 16));
    *ctrl = cfg; fence();
    next += 4166; fraction += 25600;
    if (fraction >= 38400) { fraction -= 38400; ++next; }
  }
  *ctrl = cfg; fence();
  __asm__ volatile("wsr %0, ps; rsync" :: "a"(ps) : "memory");
  return ok;
}
bool Radio::testCarrier(uint16_t code) {
  if (!ready || (code != 0 && code != 4)) return false;
  prepared = false;
  bool ok = initTxPath(false);
  if (ok) {
    reg(TONE) = (reg(TONE) & ~FIELDS) | GATE | code;
    fence();
    delay(2000);
  }
  reg(TONE) &= ~GATE; fence();
  if (!initRxPath()) { ready = false; ok = false; }
  return ok;
}
bool Radio::captureIQ(int32_t* samples, unsigned count) {
  prepared = false;
  if (!ready || !samples || !count || count > MAX_CAPTURE) return false;
  uint32_t atUs;
  if (!capture(count, atUs)) { ++stats.captureAbort; return false; }
  memcpy(samples, buffer, count * sizeof(*samples));
  return true;
}
bool Radio::receive(unsigned samples, Acquisition& out, uint32_t& capturedAtUs) {
  prepared = false;
  if (!ready || samples > MAX_CAPTURE || samples < 1200) return false;
  if (!capture(samples, capturedAtUs)) { ++stats.captureAbort; return false; }
  uint32_t began = micros();
  int32_t coarseHz = demodulate(buffer, samples);
  uint32_t spent = micros() - began;
  if (spent > stats.demodUs) stats.demodUs = spent;
  began = micros();
  bool got = signal.acquire(buffer, samples, out);
  if (got) {
    out.offsetHz += coarseHz;
    if (out.offsetHz > int32_t(SAMPLE_RATE / 2)) out.offsetHz -= SAMPLE_RATE;
    if (out.offsetHz < -int32_t(SAMPLE_RATE / 2)) out.offsetHz += SAMPLE_RATE;
  }
  spent = micros() - began;
  if (spent > stats.acquireUs) stats.acquireUs = spent;
  return got;
}
}}

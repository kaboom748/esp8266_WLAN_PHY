#include "ModemSignal.h"
#include <math.h>
#include <string.h>
#include <assert.h>
#include "FskPhase.h"

namespace AirModem { namespace Phy {
static void whiten(uint8_t* bytes) {
  unsigned lfsr = 0x1ff;
  for (unsigned n = 0; n < PACKET_BYTES; ++n) for (unsigned b = 0; b < 8; ++b) {
    bytes[n] ^= (lfsr & 1) << b;
    lfsr = (lfsr >> 1) | (((lfsr ^ (lfsr >> 5)) & 1) << 8);
  }
}
void makeFrame(const Packet& packet, Frame& frame) {
  uint8_t raw[PACKET_BYTES] = {};
  encode(packet, raw); whiten(raw);
  memcpy(frame.bytes, raw, 40);
  for (unsigned n = 0; n < 6; ++n) frame.bytes[40 + n] = uint8_t(SYNC >> (40 - 8 * n));
  memcpy(frame.bytes + 46, raw + 40, 40);
}
bool readFrame(const Frame& frame, Packet& packet) {
  uint8_t raw[PACKET_BYTES]; memcpy(raw, frame.bytes, 40); memcpy(raw + 40, frame.bytes + 46, 40);
  whiten(raw); return decode(raw, packet);
}
static const int16_t phaseTable[257] = {
  0,41,81,122,163,204,244,285,326,367,407,448,489,529,570,610,
  651,692,732,773,813,854,894,935,975,1015,1056,1096,1136,1177,1217,1257,
  1297,1337,1377,1417,1457,1497,1537,1577,1617,1656,1696,1736,1775,1815,1854,1894,
  1933,1973,2012,2051,2090,2129,2168,2207,2246,2285,2324,2363,2401,2440,2478,2517,
  2555,2594,2632,2670,2708,2746,2784,2822,2860,2897,2935,2973,3010,3047,3085,3122,
  3159,3196,3233,3270,3307,3344,3380,3417,3453,3490,3526,3562,3599,3635,3670,3706,
  3742,3778,3813,3849,3884,3920,3955,3990,4025,4060,4095,4129,4164,4199,4233,4267,
  4302,4336,4370,4404,4438,4471,4505,4539,4572,4605,4639,4672,4705,4738,4771,4803,
  4836,4869,4901,4933,4966,4998,5030,5062,5094,5125,5157,5188,5220,5251,5282,5313,
  5344,5375,5406,5437,5467,5498,5528,5559,5589,5619,5649,5679,5708,5738,5768,5797,
  5826,5856,5885,5914,5943,5972,6000,6029,6058,6086,6114,6142,6171,6199,6227,6254,
  6282,6310,6337,6365,6392,6419,6446,6473,6500,6527,6554,6580,6607,6633,6660,6686,
  6712,6738,6764,6790,6815,6841,6867,6892,6917,6943,6968,6993,7018,7043,7068,7092,
  7117,7141,7166,7190,7214,7238,7262,7286,7310,7334,7358,7381,7405,7428,7451,7475,
  7498,7521,7544,7566,7589,7612,7635,7657,7679,7702,7724,7746,7768,7790,7812,7834,
  7856,7877,7899,7920,7942,7963,7984,8005,8026,8047,8068,8089,8110,8131,8151,8172,
  8192
};
int32_t phaseHz(int32_t cross, int32_t dot) {
  uint32_t x = FskPhase::magnitude(dot), y = FskPhase::magnitude(cross);
  uint32_t high = x > y ? x : y, low = x > y ? y : x;
  if (!high) return 0;
  if (high > 65535) {
    unsigned shift = 16 - __builtin_clz(high);
    low >>= shift; high >>= shift;
  }
  uint32_t ratio = (low << 16) / high;
  unsigned index = ratio >> 8;
  int32_t angle = phaseTable[index];
  if (index < 256) angle += ((phaseTable[index+1] - angle) * int32_t(ratio & 255) + 128) / 256;
  if (y > x) angle = 16384 - angle;
  if (dot < 0) angle = 32768 - angle;
  if (cross < 0) angle = -angle;
  return angle * 75 / 128;
}
int32_t packIq(int16_t i, int16_t q) { return int32_t(uint16_t(i) | (uint32_t(uint16_t(q)) << 16)); }
int32_t demodulate(int32_t* samples, unsigned count) {
  if (!samples || !count || count > MAX_CAPTURE) return 0;
  int32_t si = 0, sq = 0;
  for (unsigned n = 0; n < count; ++n) {
    si += int16_t(samples[n] & 0xFFFF); sq += int16_t(uint32_t(samples[n]) >> 16);
  }
  int32_t dcI = si / int32_t(count), dcQ = sq / int32_t(count);
  int32_t lastI = int16_t(samples[0] & 0xFFFF) - dcI;
  int32_t lastQ = int16_t(uint32_t(samples[0]) >> 16) - dcQ;
  samples[0] = 0;
  int32_t sumCross = 0, sumDot = 0;
  for (unsigned n = 1; n < count; ++n) {
    int32_t i = int16_t(samples[n] & 0xFFFF) - dcI;
    int32_t q = int16_t(uint32_t(samples[n]) >> 16) - dcQ;
    int32_t cross, dot;
    FskPhase::correlate(lastI, lastQ, i, q, cross, dot);
    sumCross += cross; sumDot += dot;
    samples[n] = phaseHz(cross, dot);
    lastI = i; lastQ = q;
  }
  // Center circular phase differences before the linear matched filter. Without
  // this, CFO near Nyquist turns valid FSK into +/- Fs/2 discontinuities.
  int32_t offset = phaseHz(sumCross, sumDot);
  for (unsigned n = 1; n < count; ++n) {
    int32_t hz = samples[n] - offset;
    if (hz > int32_t(SAMPLE_RATE / 2)) hz -= SAMPLE_RATE;
    if (hz < -int32_t(SAMPLE_RATE / 2)) hz += SAMPLE_RATE;
    samples[n] = hz;
  }
  return offset;
}
Signal::Signal() {
  const double pi = 3.14159265358979323846, beta = 0.2;
  double sum = 0;
  double taps[RRC_TAPS];
  for (unsigned k = 0; k < RRC_TAPS; ++k) {
    double t = (int(k) - int(RRC_DELAY)) / double(SPS), v;
    if (fabs(t) < 1e-9) v = 1 + beta * (4 / pi - 1);
    else if (fabs(fabs(4 * beta * t) - 1) < 1e-9)
      v = beta / sqrt(2.0) * ((1 + 2 / pi) * sin(pi / (4 * beta)) + (1 - 2 / pi) * cos(pi / (4 * beta)));
    else v = (sin(pi * t * (1 - beta)) + 4 * beta * t * cos(pi * t * (1 + beta))) /
                 (pi * t * (1 - 16 * beta * beta * t * t));
    taps[k] = v; sum += v;
  }
  int32_t absoluteSum = 0;
  for (unsigned k = 0; k < RRC_TAPS; ++k) {
    rx[k] = int16_t(lround(taps[k] * 32768.0 / sum));
    tx[k] = lround(taps[k] * SPS / sum * 648.0 * 65536 * 1024 / 78125);
    absoluteSum += rx[k] < 0 ? -rx[k] : rx[k];
  }
  // Together with bounded discriminator output this proves int32 FIR sums fit.
  assert(absoluteSum <= 65536);
}
void Signal::waveform(const Frame& frame, int32_t q16[SLOT_SAMPLES], int32_t offsetHz) const {
  int32_t offset = int64_t(offsetHz) * 65536 / 78125;
  for (unsigned n = 0; n < SLOT_SAMPLES; ++n) {
    int32_t level = 0;
    int first = (int(n) - int(FIRST_CENTER) + int(RRC_DELAY) - int(RRC_TAPS) + 1 + int(SPS) - 1) / int(SPS);
    if (first < 0) first = 0;
    for (unsigned s = unsigned(first); s < SYMBOLS; ++s) {
      int tap = int(n) - int(FIRST_CENTER) - int(s * SPS) + int(RRC_DELAY);
      if (tap < 0) break;
      if (tap < int(RRC_TAPS)) {
        uint8_t d = (frame.bytes[s / 4] >> (6 - 2 * (s % 4))) & 3;
        level += symbol(d) * tx[tap];
      }
    }
    q16[n] = offset + (level + (level < 0 ? -512 : 512)) / 1024;
  }
}
bool Signal::acquire(const int32_t* hz, unsigned count, Acquisition& result) {
  if (!hz || count > MAX_CAPTURE || count < (SYMBOLS - 1) * SPS + RRC_TAPS) return false;
  for (unsigned n = 0; n < count; ++n) if (hz[n] < -19200 || hz[n] > 19200) return false;
  for (unsigned n = 0; n < count; ++n) {
    int32_t sum = 0;
    unsigned taps = n + 1 < RRC_TAPS ? n + 1 : RRC_TAPS;
    for (unsigned k = 0; k < taps; ++k) sum += hz[n - k] * rx[k];
    sum /= 32768;
    filtered[n] = int16_t(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
  }
  int8_t syncLevels[24];
  uint64_t sync = SYNC;
  int32_t sumX = 0, sumXX = 0;
  for (unsigned i = 0; i < 24; ++i) {
    syncLevels[i] = symbol((sync >> (46 - 2 * i)) & 3);
    sumX += syncLevels[i]; sumXX += syncLevels[i] * syncLevels[i];
  }
  const int32_t denominator = 24 * sumXX - sumX * sumX;
  uint32_t bestError = 160000;
  bool found = false;
  for (unsigned start = RRC_TAPS - 1; start + (SYMBOLS - 1) * SPS < count; ++start) {
    int32_t sumY = 0, sumXY = 0;
    for (unsigned i = 0; i < 24; ++i) {
      int32_t y = filtered[start + (SYNC_AT + i) * SPS];
      sumY += y; sumXY += y * syncLevels[i];
    }
    int32_t unit = (24 * sumXY - sumX * sumY) / denominator;
    int32_t magnitude = unit < 0 ? -unit : unit;
    if (magnitude < 400 || magnitude > 950) continue;
    int32_t offset = (sumY - unit * sumX) / 24;
    if (offset < -16000 || offset > 16000) continue;
    uint32_t sumSquared = 0;
    for (unsigned i = 0; i < 24; ++i) {
      int32_t error = filtered[start + (SYNC_AT + i) * SPS] - offset - unit * syncLevels[i];
      if (error < -2 * magnitude || error > 2 * magnitude) { sumSquared = UINT32_MAX; break; }
      sumSquared += error * error;
    }
    if (sumSquared >= uint32_t(unit * unit * 384 / 100)) continue;
    uint32_t mse = uint64_t(sumSquared) * 1000000 / uint32_t(24 * unit * unit);
    if (mse >= bestError) continue;
    Frame frame;
    for (unsigned i = 0; i < SYMBOLS; ++i) {
      int32_t value = filtered[start + i * SPS] - offset;
      if (unit < 0) value = -value;
      uint8_t d = value >= 0 ? (value > 2 * magnitude ? 1 : 0) : (value < -2 * magnitude ? 3 : 2);
      frame.bytes[i / 4] |= d << (6 - 2 * (i % 4));
    }
    Packet packet;
    if (!readFrame(frame, packet)) continue;
    result.packet = packet; result.firstCenter = int32_t(start) - int32_t(RRC_DELAY);
    result.offsetHz = offset; result.unitHz = unit;
    bestError = mse; found = true;
  }
  return found;
}
}}

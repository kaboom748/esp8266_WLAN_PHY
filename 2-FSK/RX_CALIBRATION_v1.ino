/*
  ESP8266_FSK_FAST_RX_1500US_MAPFIX.ino

  Modem RX corrige pour le TX:
    sync A 20 ms
    sync B 20 ms
    payload 32 bits, 1500 us/symbole
    mot attendu D3A5C69B MSB first

  Calibration confirmee:
    TONE_A=1016 / bit 0 -> f_eff POSITIF ~ +20 kHz
    TONE_B=0    / bit 1 -> f_eff NEGATIF ~ -27 kHz

  Classification:
    bit 1 si f_eff < -3000 Hz
    bit 0 sinon

  Capture:
    une estimation f_eff pres du centre de chaque symbole.
    Aucun Serial pendant les 48 ms de payload.

  Serial: 115200
*/

#include <Arduino.h>
#include <math.h>

extern "C" {
  #include "user_interface.h"
}

static constexpr uint8_t  RF_CHANNEL = 6;
static constexpr uint16_t IQ_FIELD = 4;
static constexpr uint8_t  IQ_MODE_SEL = 0;
static constexpr uint32_t TIMEOUT_US = 1000;
static constexpr uint32_t SERIAL_BAUD = 115200;

static constexpr int32_t FREQ_THRESHOLD_HZ = -3000;

static constexpr uint32_t SYNC_US   = 20000;
static constexpr uint32_t SYMBOL_US = 750;
static constexpr uint8_t  N_BITS    = 32;
static constexpr uint32_t EXPECTED  = 0xD3A5C69Bu;

static constexpr uint32_t IQ_CTRL = 0x6000057Cu;
static constexpr uint32_t IQ_DCI  = 0x600005DCu;
static constexpr uint32_t IQ_DCQ  = 0x600005E0u;
static constexpr uint32_t IQ_E4   = 0x600005E4u;

static constexpr uint32_t IQ_MODE       = 0x00040000u;
static constexpr uint32_t IQ_FIELD_MASK = 0x0003FFFCu;
static constexpr uint32_t IQ_START      = 2u;
static constexpr uint32_t IQ_ENABLE     = 1u;
static constexpr uint32_t IQ_DONE31     = 0x80000000u;
static constexpr uint32_t IQ_KEEP_MASK  = 0x7FF80000u;

struct IQSample {
  uint32_t t;
  int32_t i;
  int32_t q;
  uint32_t e4;
  bool valid;
};

struct FreqEst {
  uint32_t t;
  int32_t hz;
  uint32_t e4;
  bool valid;
};

static uint32_t cfgKeep = 0;

static inline void memw() {
  __asm__ volatile("memw" ::: "memory");
}
static inline uint32_t rd32(uint32_t a) {
  return *reinterpret_cast<volatile uint32_t*>(a);
}
static inline void wr32(uint32_t a, uint32_t v) {
  *reinterpret_cast<volatile uint32_t*>(a) = v;
}

static uint32_t makeCfg() {
  return cfgKeep |
         (IQ_MODE_SEL ? IQ_MODE : 0u) |
         (((uint32_t)IQ_FIELD << 2) & IQ_FIELD_MASK);
}

static bool initRx() {
  wifi_station_set_auto_connect(0);
  if (!wifi_set_opmode_current(STATION_MODE)) return false;
  delay(80);
  wifi_station_disconnect();
  if (!wifi_set_sleep_type(NONE_SLEEP_T)) return false;
  if (!wifi_set_channel(RF_CHANNEL)) return false;
  delay(40);

  cfgKeep = rd32(IQ_CTRL) & IQ_KEEP_MASK;
  return true;
}

static bool acquireIQ(IQSample &s) {
  const uint32_t cfg = makeCfg();

  wr32(IQ_CTRL, cfg);
  memw();
  wr32(IQ_CTRL, cfg | IQ_ENABLE);
  memw();
  wr32(IQ_CTRL, cfg | IQ_ENABLE | IQ_START);
  memw();

  const uint32_t t0 = micros();
  uint32_t c = rd32(IQ_CTRL);

  while (!(c & IQ_DONE31)) {
    if ((uint32_t)(micros() - t0) >= TIMEOUT_US) {
      s.valid = false;
      return false;
    }
    c = rd32(IQ_CTRL);
  }

  s.t = micros();
  s.i = (int32_t)rd32(IQ_DCI);
  s.q = (int32_t)rd32(IQ_DCQ);
  s.e4 = rd32(IQ_E4);
  s.valid = true;

  wr32(IQ_CTRL, cfg | IQ_ENABLE);
  memw();
  wr32(IQ_CTRL, cfg);
  memw();

  return true;
}

static bool estimateFreq(FreqEst &out) {
  static constexpr uint8_t N = 8;
  IQSample s[N];

  for (uint8_t k = 0; k < N; ++k) {
    if (!acquireIQ(s[k])) {
      out.valid = false;
      return false;
    }
  }

  float sumSin = 0.0f;
  float sumCos = 0.0f;
  uint32_t sumDt = 0;
  uint64_t sumE4 = 0;
  uint8_t pairs = 0;

  for (uint8_t k = 0; k < N; ++k)
    sumE4 += s[k].e4;

  for (uint8_t k = 1; k < N; ++k) {
    const uint32_t dt = s[k].t - s[k - 1].t;
    if (dt == 0 || dt > 100u) continue;

    const double cross =
      (double)s[k - 1].i * (double)s[k].q -
      (double)s[k - 1].q * (double)s[k].i;

    const double dot =
      (double)s[k - 1].i * (double)s[k].i +
      (double)s[k - 1].q * (double)s[k].q;

    if (cross == 0.0 && dot == 0.0) continue;

    const float dp = atan2f((float)cross, (float)dot);
    sumSin += sinf(dp);
    sumCos += cosf(dp);
    sumDt += dt;
    ++pairs;
  }

  out.t = s[N - 1].t;
  out.e4 = (uint32_t)(sumE4 / N);
  out.hz = 0;
  out.valid = false;

  if (pairs < 4 || sumDt == 0)
    return false;

  const float meanPhase = atan2f(sumSin, sumCos);
  const float meanDtUs = (float)sumDt / (float)pairs;

  out.hz = (int32_t)lroundf(
    meanPhase /
    (2.0f * (float)M_PI * meanDtUs * 1.0e-6f)
  );

  out.valid = true;
  return true;
}

static inline uint8_t classify(const FreqEst &f) {
  // B/bit1 = groupe negatif
  return (f.hz < FREQ_THRESHOLD_HZ) ? 1u : 0u;
}

static bool waitForSync(uint32_t &dataStartUs) {
  // 0: chercher A stable (positif / bit0)
  // 1: A qualifie, attendre transition vers B
  // 2: B commence, exiger B stable
  uint8_t state = 0;
  uint32_t aStart = 0;
  uint32_t bStart = 0;

  while (true) {
    FreqEst f;

    if (!estimateFreq(f)) {
      yield();
      continue;
    }

    const uint8_t st = classify(f);

    if (state == 0) {
      if (st == 0) {
        if (!aStart)
          aStart = f.t;

        if ((uint32_t)(f.t - aStart) >= 12000u)
          state = 1;
      } else {
        aStart = 0;
      }
    }
    else if (state == 1) {
      if (st == 1) {
        bStart = f.t;
        state = 2;
      }
    }
    else {
      if (st == 1) {
        if ((uint32_t)(f.t - bStart) >= 12000u) {
          dataStartUs = bStart + SYNC_US;
          return true;
        }
      } else {
        state = 1;
        bStart = 0;
      }
    }

    yield();
  }
}

static inline void waitUntil(uint32_t target) {
  while ((int32_t)(micros() - target) < 0) {}
}

static void decodeFrame(uint32_t dataStartUs) {
  uint32_t rxWord = 0;
  uint8_t errors = 0;
  uint8_t valid = 0;

  int32_t min0 =  2147483647;
  int32_t max0 = -2147483647;
  int32_t min1 =  2147483647;
  int32_t max1 = -2147483647;

  for (uint8_t b = 0; b < N_BITS; ++b) {
    const uint32_t center =
      dataStartUs + (uint32_t)b * SYMBOL_US + SYMBOL_US / 2u;

    waitUntil(center - 40u);

    FreqEst f;
    uint8_t bit = 0;

    if (estimateFreq(f) && f.valid) {
      bit = classify(f);
      ++valid;

      if (bit == 0) {
        if (f.hz < min0) min0 = f.hz;
        if (f.hz > max0) max0 = f.hz;
      } else {
        if (f.hz < min1) min1 = f.hz;
        if (f.hz > max1) max1 = f.hz;
      }
    }

    rxWord = (rxWord << 1) | bit;

    const uint8_t exp =
      (EXPECTED >> (31 - b)) & 1u;

    if (bit != exp)
      ++errors;
  }

  Serial.printf(
    "RX=%08lX EXP=%08lX ERR=%u/32 VALID=%u "
    "F0=[%ld,%ld] F1=[%ld,%ld]\r\n",
    (unsigned long)rxWord,
    (unsigned long)EXPECTED,
    errors,
    valid,
    (long)min0, (long)max0,
    (long)min1, (long)max1
  );
  Serial.flush();
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);

  Serial.println();
  Serial.println(F("BOOT FAST RX MAPFIX"));

  if (!initRx()) {
    Serial.println(F("ERR RX INIT"));
    return;
  }

  Serial.printf(
    "READY MAPFIX CH=%u N=%u SYM=%luus TH=%ld EXP=%08lX\r\n",
    RF_CHANNEL,
    IQ_FIELD,
    (unsigned long)SYMBOL_US,
    (long)FREQ_THRESHOLD_HZ,
    (unsigned long)EXPECTED
  );
  Serial.flush();
}

void loop() {
  uint32_t dataStartUs = 0;

  if (waitForSync(dataStartUs)) {
    decodeFrame(dataStartUs);
  }

  yield();
}

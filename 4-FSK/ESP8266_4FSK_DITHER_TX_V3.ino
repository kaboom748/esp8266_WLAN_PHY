/*
  ESP8266_4FSK_DITHER_TX_V3.ino

  Trame autonome:
    SYNC A (TONE 1016) : 20 ms
    SYNC B (TONE 0)    : 20 ms
    DATA                : 16 dibits, 750 us/symbole (code effectif)
    GAP A               : 20 ms
  Puis repetition.

  Mapping 4-FSK:
    dibit 00 -> K moyen  0.000 (RX observe ~ -26 kHz)
    dibit 01 -> K moyen -2.667 (RX observe ~ +97 kHz)
    dibit 10 -> K moyen -5.333 (RX observe ~ -106 kHz)
    dibit 11 -> K moyen -8.000 (RX observe ~ +20 kHz)
  Frequences observees avec IQ_FIELD=4; elles ne sont pas lineaires en Hz.

  Dithering sigma-delta adapte du projet SSB fourni.
  Les frequences des deux niveaux intermediaires sont mesurees IQ et non
  lineaires en Hz; recalibrer avant tout changement de carte ou de IQ_FIELD.
  Le mot connu MSB first: 0xD3A5C69B

  Serial: 115200 (messages seulement entre trames)
*/

#include <Arduino.h>
extern "C" {
  #include "user_interface.h"
}

static constexpr uint8_t  RF_CHANNEL = 6;
static constexpr uint16_t TONE_A = 1016;   // sync haut, K signe -8
static constexpr uint16_t TONE_B = 0;      // sync bas, K signe 0
static constexpr uint8_t  APWR = 64;
static constexpr uint8_t  ASK  = 32;

static constexpr uint32_t SYNC_US   = 20000;
static constexpr uint32_t SYMBOL_US = 750; // prototype conserve la cadence 2-FSK source
static constexpr uint32_t GAP_US    = 20000;
static constexpr uint32_t WORD      = 0xD3A5C69Bu;

static constexpr uint32_t TONE1      = 0x600005B8u;
static constexpr uint32_t TONE2      = 0x600005BCu;
static constexpr uint32_t TONE3      = 0x600005C4u;
static constexpr uint32_t PBUS_CMD   = 0x60000594u;
static constexpr uint32_t PBUS_STAT  = 0x600005A0u;
static constexpr uint32_t RX_CTRL    = 0x60009B08u;

static constexpr uint32_t K_MASK      = 0x000003FFu;
static constexpr uint32_t SCALE_MASK  = 0x0003FC00u;
static constexpr uint32_t GATE_MASK   = 0x00040000u;
static constexpr uint32_t SCALE_SHIFT = 10u;
static constexpr uint32_t RX_STOP     = 0x08000000u;

static constexpr uint32_t ROM_SET_TXCLK_EN         = 0x4000650Cu;
static constexpr uint32_t ROM_SET_ANA_INF_TX_SCALE = 0x4000678Cu;

using SetTxClkFn    = void(*)(int);
using SetAnaScaleFn = uint8_t(*)(uint8_t);

static SetTxClkFn set_txclk =
  reinterpret_cast<SetTxClkFn>(ROM_SET_TXCLK_EN);
static SetAnaScaleFn set_ana_scale =
  reinterpret_cast<SetAnaScaleFn>(ROM_SET_ANA_INF_TX_SCALE);

static inline void memw() { __asm__ volatile("memw" ::: "memory"); }
static inline uint32_t rd32(uint32_t a) {
  return *reinterpret_cast<volatile uint32_t*>(a);
}
static inline void wr32(uint32_t a, uint32_t v) {
  *reinterpret_cast<volatile uint32_t*>(a) = v;
}
static inline uint8_t askCode(uint8_t ask) {
  return static_cast<uint8_t>(0u - ask);
}

static bool pbusWrite(uint8_t sel, uint8_t bank, uint16_t value) {
  uint32_t cmd = rd32(PBUS_CMD);
  cmd &= 0xFFFF0001u;
  cmd |= (uint32_t)(bank & 3u) << 14;
  cmd |= (uint32_t)(value & 0x1FFu) << 5;
  cmd |= (uint32_t)(sel & 7u) << 2;
  cmd |= 2u;
  wr32(PBUS_CMD, cmd);
  memw();

  const uint32_t t0 = micros();
  while (rd32(PBUS_STAT) & 0x80000000u) {
    if ((uint32_t)(micros() - t0) > 2500u) {
      uint32_t r = rd32(PBUS_CMD);
      r &= ~2u;
      wr32(PBUS_CMD, r);
      memw();
      return false;
    }
    delay(0);
  }

  uint32_t r = rd32(PBUS_CMD);
  r &= ~2u;
  wr32(PBUS_CMD, r);
  memw();
  return true;
}

static void enterManual() {
  uint32_t r = rd32(RX_CTRL);
  r |= RX_STOP;
  wr32(RX_CTRL, r);
  memw();

  r = rd32(PBUS_CMD);
  r |= 1u;
  wr32(PBUS_CMD, r);
  memw();
}

static bool txPathOn() {
  return pbusWrite(2,1,1) &&
         pbusWrite(7,1,95) &&
         pbusWrite(1,1,127) &&
         pbusWrite(6,1,127);
}

static void programBase() {
  uint32_t r = rd32(TONE1);
  r &= ~(K_MASK | SCALE_MASK | GATE_MASK);
  r |= (uint32_t)TONE_A & K_MASK;
  r |= (uint32_t)askCode(ASK) << SCALE_SHIFT;
  r |= GATE_MASK;
  wr32(TONE1, r);

  r = rd32(TONE2);
  r &= ~GATE_MASK;
  wr32(TONE2, r);

  r = rd32(TONE3);
  r &= ~GATE_MASK;
  wr32(TONE3, r);
  memw();
}

static inline void setTone(uint16_t tone) {
  uint32_t r = rd32(TONE1);
  r = (r & ~K_MASK) | ((uint32_t)tone & K_MASK);
  wr32(TONE1, r);
  memw();
}

/*
  Dithering adapte de CW_CARRIER_TEST.ino, projet SSB fourni.
  Copyright (c) 2026 SP8ESA. Licence MIT.

  MIT License
  Copyright (c) 2026 SP8ESA

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/
// q is the signed target K in Q16.
static constexpr int32_t K_LEVEL_Q16[4] = {0, -(8 * 65536) / 3, -(16 * 65536) / 3, -(8 * 65536)};
static int32_t sdErr = 0;
static int32_t sdCode = -8;
static uint32_t sdLast = 0;
static constexpr int32_t SD_THR = 20 << 16;

static void ditherKUntil(int32_t q, uint32_t endCycle) {
  const int32_t kb = (q + 0x8000) >> 16;
  const int32_t r = q - kb * 65536;
  const uint32_t base = rd32(TONE1) & ~K_MASK;
  const uint32_t kN = base | ((uint32_t)kb & K_MASK);
  const uint32_t kU = base | ((uint32_t)(kb + 1) & K_MASK);
  const uint32_t kD = base | ((uint32_t)(kb - 1) & K_MASK);
  int32_t err = sdErr, code = sdCode;
  uint32_t last = sdLast;
  const uint32_t ps = xt_rsil(15);
  do {
    uint32_t dt = ESP.getCycleCount() - last;
    if (dt > 8192u) dt = 8192u;
    const int32_t errNow = err + (q - code * 65536) * (int32_t)dt;
    if (errNow > SD_THR || errNow < -SD_THR) {
      const bool up = errNow > 0;
      const int32_t rate = up ? 65536 - r : 65536 + r;
      int32_t len = (up ? errNow : -errNow) / rate;
      if (len > 4000) len = 4000;
      wr32(TONE1, up ? kU : kD); memw();
      const uint32_t tp = ESP.getCycleCount();
      dt = tp - last; if (dt > 8192u) dt = 8192u;
      err += (q - code * 65536) * (int32_t)dt;
      code = up ? kb + 1 : kb - 1;
      while ((int32_t)(ESP.getCycleCount() - tp) < len) {}
      last = tp;
    }
    wr32(TONE1, kN); memw();
    const uint32_t tw = ESP.getCycleCount();
    dt = tw - last; if (dt > 8192u) dt = 8192u;
    err += (q - code * 65536) * (int32_t)dt;
    if (err > (1 << 29)) err = (1 << 29);
    if (err < -(1 << 29)) err = -(1 << 29);
    code = kb; last = tw;
  } while ((int32_t)(last - endCycle) < 0);
  xt_wsr_ps(ps); memw();
  sdErr = err; sdCode = code; sdLast = last;
}

static bool initRf() {
  wifi_station_set_auto_connect(0);
  if (!wifi_set_opmode_current(STATION_MODE)) return false;
  delay(80);
  wifi_station_disconnect();
  if (!wifi_set_sleep_type(NONE_SLEEP_T)) return false;
  if (!wifi_set_channel(RF_CHANNEL)) return false;
  delay(40);

  enterManual();
  if (!txPathOn()) return false;
  set_ana_scale(APWR);
  set_txclk(1);
  programBase();
  return true;
}

static inline void waitUntil(uint32_t target) {
  while ((int32_t)(micros() - target) < 0) {
    // busy wait for deterministic symbol timing
  }
}

static void sendFrame() {
  setTone(TONE_A);
  uint32_t t = micros() + SYNC_US; waitUntil(t);
  setTone(TONE_B);
  t += SYNC_US; waitUntil(t);

  // Reprendre l'etat K=0 du ton B, puis transmettre 16 dibits.
  sdErr = 0; sdCode = 0; sdLast = ESP.getCycleCount();
  const uint32_t cyclesPerSymbol = ESP.getCpuFreqMHz() * SYMBOL_US;
  uint32_t boundary = sdLast;
  for (uint8_t sym = 0; sym < 16; ++sym) {
    const uint8_t dibit = (WORD >> (30 - 2 * sym)) & 3u;
    boundary += cyclesPerSymbol;
    ditherKUntil(K_LEVEL_Q16[dibit], boundary);
    ESP.wdtFeed();
  }
  setTone(TONE_A);
  t += 16u * SYMBOL_US + GAP_US; waitUntil(t);
}

void setup() {
  Serial.begin(115200);
  delay(150);

  if (!initRf()) {
    Serial.println(F("ERR TX INIT"));
    return;
  }

  Serial.printf(
    "READY 4FSK_DITHER_TX_V3 CH=%u SYM=%luus BITRATE=%lu WORD=%08lX A=%u B=%u\r\n",
    RF_CHANNEL, (unsigned long)SYMBOL_US,
    (unsigned long)(2000000u / SYMBOL_US), (unsigned long)WORD, TONE_A, TONE_B
  );
}

void loop() {
  sendFrame();
  Serial.printf("FRAME T_US=%lu\r\n", (unsigned long)micros());
  delay(0);
}

/*
  ESP8266_FSK_FAST_TX_1500US.ino

  Trame autonome:
    SYNC A (TONE 1016) : 20 ms
    SYNC B (TONE 0)    : 20 ms
    DATA                : 32 bits, 1500 us/bit
    GAP A               : 20 ms
  Puis repetition.

  Mapping:
    bit 0 -> TONE 1016
    bit 1 -> TONE 0

  Mot connu MSB first: 0xD3A5C69B

  Serial: 115200 (messages seulement entre trames)
*/

#include <Arduino.h>
extern "C" {
  #include "user_interface.h"
}

static constexpr uint8_t  RF_CHANNEL = 6;
static constexpr uint16_t TONE_A = 1016;   // bit 0
static constexpr uint16_t TONE_B = 0;      // bit 1
static constexpr uint8_t  APWR = 64;
static constexpr uint8_t  ASK  = 32;

static constexpr uint32_t SYNC_US   = 20000;
static constexpr uint32_t SYMBOL_US = 750;
static constexpr uint32_t GAP_US    = 20000;
static constexpr uint32_t WORD      = 0xD3A5C69Bu;
static constexpr uint8_t  N_BITS    = 32;

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
  uint32_t t = micros() + SYNC_US;
  waitUntil(t);

  setTone(TONE_B);
  t += SYNC_US;
  waitUntil(t);

  // 32 bits MSB first
  for (uint8_t b = 0; b < N_BITS; ++b) {
    const bool bit = (WORD >> (31 - b)) & 1u;
    setTone(bit ? TONE_B : TONE_A);
    t += SYMBOL_US;
    waitUntil(t);
  }

  setTone(TONE_A);
  t += GAP_US;
  waitUntil(t);
}

void setup() {
  Serial.begin(115200);
  delay(150);

  if (!initRf()) {
    Serial.println(F("ERR TX INIT"));
    return;
  }

  Serial.printf(
    "READY FAST_TX CH=%u SYM=%luus WORD=%08lX A=%u B=%u\r\n",
    RF_CHANNEL, (unsigned long)SYMBOL_US,
    (unsigned long)WORD, TONE_A, TONE_B
  );
}

void loop() {
  sendFrame();
  Serial.printf("FRAME T_US=%lu\r\n", (unsigned long)micros());
  delay(0);
}

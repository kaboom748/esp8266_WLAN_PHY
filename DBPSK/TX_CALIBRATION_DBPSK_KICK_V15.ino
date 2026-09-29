/*
  TX_REFERENCE_DBPSK_FIXED_V4.ino

  ESP8266 - liaison DBPSK par phase-kick.

  IMPORTANT:
    - La FSK A/B/A sert UNIQUEMENT a synchroniser la trame.
    - Le payload reste sur UNE SEULE porteuse: TONE_A=1016.
    - bit 0 = aucune transition de phase.
    - bit 1 = transition de phase ~pi obtenue par une courte excursion
              TONE_A -> 1017/1015 -> TONE_A.

  Trame:
    20 ms TONE_A=1016   sync A
    20 ms TONE_B=0      sync B
    10 ms TONE_A=1016   garde
    PHASE FLIP connu    marqueur DBPSK de debut
    80 ms TONE_A=1016   temps de traitement RX (V4 calibration)
    32 bits DBPSK       1000 us/symbole
    20 ms TONE_A=1016   gap

  Payload de test: 0xD3A5C69B, MSB first.
*/

#include <Arduino.h>

extern "C" {
  #include "user_interface.h"
}

static constexpr uint8_t RF_CHANNEL = 6;

static constexpr uint16_t TONE_A     = 1016; // porteuse DBPSK / sync A
static constexpr uint16_t TONE_B     = 0;    // sync FSK seulement
static constexpr uint16_t PLUS_TONE  = 1017;
static constexpr uint16_t MINUS_TONE = 1015;

static constexpr uint8_t ASK  = 32;
static constexpr uint8_t APWR = 64;

static constexpr uint32_t SYNC_A_US = 20000;
static constexpr uint32_t SYNC_B_US = 20000;
static constexpr uint32_t GUARD_US  = 10000;
static constexpr uint32_t MARKER_TO_PAYLOAD_US = 80000;
static constexpr uint32_t SYMBOL_US = 1000;
static constexpr uint32_t GAP_US    = 20000;

static constexpr uint32_t PAYLOAD = 0xD3A5C69Bu;

// Valeurs empiriques du test PHASE_KICK fonctionnel.
static constexpr uint32_t PLUS_KICK_TENTHS_US  = 55; // ~5.5 us
static constexpr uint32_t MINUS_KICK_TENTHS_US = 67; // ~6.7 us

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

static uint32_t plusKickCycles  = 440;
static uint32_t minusKickCycles = 536;
static bool nextKickPlus = true;
static uint32_t frameCounter = 0;

static inline void memw() {
  __asm__ volatile("memw" ::: "memory");
}

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

  if (!wifi_set_opmode_current(STATION_MODE))
    return false;

  delay(80);
  wifi_station_disconnect();

  if (!wifi_set_sleep_type(NONE_SLEEP_T))
    return false;

  if (!wifi_set_channel(RF_CHANNEL))
    return false;

  delay(40);

  enterManual();

  if (!txPathOn())
    return false;

  set_ana_scale(APWR);
  set_txclk(1);
  programBase();

  return true;
}

static inline void waitUntil(uint32_t t) {
  while ((int32_t)(micros() - t) < 0) {}
}

// Un flip +/-pi. Le sens alterne; modulo 2pi, +pi et -pi sont
// la meme transition DBPSK.
static inline void phaseFlip() {
  const bool plus = nextKickPlus;
  const uint16_t excursion = plus ? PLUS_TONE : MINUS_TONE;
  const uint32_t cycles = plus ? plusKickCycles : minusKickCycles;

  setTone(excursion);

  const uint32_t c0 = ESP.getCycleCount();
  while ((uint32_t)(ESP.getCycleCount() - c0) < cycles) {}

  setTone(TONE_A);
  nextKickPlus = !nextKickPlus;
}

static void sendFrame() {
  // A long. Avec le gap precedent, A reste assez longtemps pour l'auto-sync RX.
  setTone(TONE_A);
  uint32_t t = micros() + SYNC_A_US;
  waitUntil(t);

  // B: uniquement ancre FSK.
  setTone(TONE_B);
  t += SYNC_B_US;
  waitUntil(t);

  // Retour definitif sur la porteuse DBPSK.
  setTone(TONE_A);
  t += GUARD_US;
  waitUntil(t);

  // Marqueur de phase connu. Ce n'est PAS un symbole FSK.
  // Il sert a recaler precisement le RX sur le temps des phase-kicks.
  phaseFlip();

  // Laisse au RX le temps de detecter/analyser le marqueur.
  t += MARKER_TO_PAYLOAD_US;
  waitUntil(t);

  // DBPSK: changement de phase seulement si bit=1.
  for (uint8_t b = 0; b < 32; ++b) {
    const uint8_t bit = (PAYLOAD >> (31 - b)) & 1u;

    if (bit)
      phaseFlip();

    t += SYMBOL_US;
    waitUntil(t);
  }

  // Gap sur la MEME porteuse; aucune FSK de donnees.
  t += GAP_US;
  waitUntil(t);
}

void setup() {
  Serial.begin(115200);
  delay(250);

  Serial.println();
  Serial.println(F("BOOT TX_REFERENCE_DBPSK_FIXED_V4"));

  if (!initRf()) {
    Serial.println(F("ERR TX INIT"));
    Serial.flush();
    return;
  }

  const uint32_t cpuMHz = ESP.getCpuFreqMHz();

  plusKickCycles =
    (cpuMHz * PLUS_KICK_TENTHS_US + 5u) / 10u;

  minusKickCycles =
    (cpuMHz * MINUS_KICK_TENTHS_US + 5u) / 10u;

  Serial.printf(
    "READY DBPSK CH=%u CARRIER=%u FSKSYNC_B=%u SYM=%luus PAYLOAD=%08lX "
    "MARKERGAP=%luus K+=%lu K-=%lu\r\n",
    RF_CHANNEL,
    TONE_A,
    TONE_B,
    (unsigned long)SYMBOL_US,
    (unsigned long)PAYLOAD,
    (unsigned long)MARKER_TO_PAYLOAD_US,
    (unsigned long)plusKickCycles,
    (unsigned long)minusKickCycles
  );
  Serial.flush();

  delay(100);
}

void loop() {
  sendFrame();

  ++frameCounter;
  Serial.printf(
    "FRAME %lu DBPSK_TX=%08lX\r\n",
    (unsigned long)frameCounter,
    (unsigned long)PAYLOAD
  );
  Serial.flush();

  yield();
}

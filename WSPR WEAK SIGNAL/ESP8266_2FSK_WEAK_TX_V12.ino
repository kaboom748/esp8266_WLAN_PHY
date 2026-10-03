/*
  ESP8266_2FSK_WEAK_TX_V12.ino — prototype faible signal, inspiré de WSPR.
  Modulation 2-FSK, K=-3/-5 fixes, APWR=0, ASK=0, aucun dithering.
  Ce protocole est propre au projet, incompatible avec WSPR et les V1..V10.
  Un symbole = 1 seconde = 1 bit de canal/s = 0.001 kbit/s brut.
  32 bits utiles + CRC16 + FEC K7 1/2, entrelacement et pilotes.
  Trame nominale 163.06 s; débit utile maximal 0.000196 kbit/s.
  Les marqueurs A/B et pause de 20 ms sont conservés. L'acquisition faible
  signal utilise en plus les tons d'apprentissage et 31 symboles connus.
  Arduino ESP8266 core 3.1.2, CPU 160 MHz automatique. Série 230400 bauds, 8N1.
  Sensibilité et puissance RF non mesurées; aucun seuil -31 dB garanti.
*/
#include <Arduino.h>
extern "C" {
  #include "user_interface.h"
  void preloop_update_frequency();
}

// Sélection automatique, y compris si le sketch est compilé avec F_CPU=80 MHz.
// Le core ESP8266 rappelle ce hook weak avant setup()/loop(); notre version
// remplace sa sélection basée sur l'IDE et conserve 160 MHz entre les boucles.
static bool ensureCpu160MHz() {
  if (system_get_cpu_freq() != SYS_CPU_160MHZ &&
      !system_update_cpu_freq(SYS_CPU_160MHZ)) return false;
  return system_get_cpu_freq() == SYS_CPU_160MHZ;
}
extern "C" void preloop_update_frequency() {
  (void)ensureCpu160MHz();
}

// Protocole commun aux deux sketches V12 (copié ici, aucun .h requis).
static constexpr uint8_t RF_CHANNEL = 6;
static constexpr uint32_t SERIAL_BAUD = 230400;
static constexpr uint32_t SYMBOL_US = 1000000;
static constexpr uint32_t MARKER_US = 20000;
static constexpr uint32_t GAP_US = 20000;
static constexpr uint8_t TRAIN_SYMBOLS = 4; // 4 s par ton
static constexpr uint8_t SYNC_BITS = 31;
static constexpr uint32_t SYNC_WORD = 0x7CD215D8u; // m-sequence x^5+x^2+1
static constexpr uint32_t TEST_WORD = 0xD3A5C69Bu;
static constexpr uint8_t INPUT_BITS = 54; // 32 data + CRC16 + 6 zeros
static constexpr uint8_t CODE_BITS = 108;
static constexpr uint8_t PILOT_EVERY = 12;
static constexpr uint8_t PILOT_PAIRS = (CODE_BITS - 1) / PILOT_EVERY;
static constexpr uint16_t DATA_SLOTS = CODE_BITS + 2 * PILOT_PAIRS;
static constexpr uint32_t FRAME_US =
    (2 * TRAIN_SYMBOLS + SYNC_BITS + DATA_SLOTS) * SYMBOL_US +
    2 * MARKER_US + GAP_US;
static constexpr uint16_t TONE_BIT0 = 1021; // K=-3
static constexpr uint16_t TONE_BIT1 = 1019; // K=-5
static constexpr uint16_t TONE_A = 1016;
static constexpr uint16_t TONE_B = 0;
static constexpr uint8_t APWR = 0;
static constexpr uint8_t ASK = 0;
static_assert(SYMBOL_US % 8 == 0, "Symbole divisible par 8 requis");
static_assert(FRAME_US < 0x7fffffffu, "Trame trop longue pour micros");

static uint8_t parity7(uint8_t v) {
  v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
  return v & 1u;
}

static uint16_t crcWord(uint32_t word) {
  // CRC-16/CCITT-FALSE: poly 0x1021, init FFFF, MSB first, xorout=0.
  uint16_t crc = 0xffffu;
  for (uint8_t b = 0; b < 4; ++b) {
    crc ^= (uint16_t)((word >> (24 - 8 * b)) & 255u) << 8;
    for (uint8_t k = 0; k < 8; ++k)
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                            : (uint16_t)(crc << 1);
  }
  return crc;
}

static void encodeWord(uint32_t word, uint8_t *coded) {
  const uint16_t crc = crcWord(word);
  uint8_t state = 0;
  for (uint8_t t = 0; t < INPUT_BITS; ++t) {
    const uint8_t bit = t < 32 ? ((word >> (31 - t)) & 1u) :
                        t < 48 ? ((crc >> (47 - t)) & 1u) : 0;
    const uint8_t reg = (uint8_t)((state << 1) | bit);
    // K=7, r=1/2, polynomes 171 et 133 octal. Convention propre V12:
    // aucune inversion du deuxième flux, ordre G1 puis G2.
    coded[2 * t] = parity7(reg & 0x79u);
    coded[2 * t + 1] = parity7(reg & 0x5bu);
    state = reg & 63u;
  }
}

static uint8_t codedIndex(uint8_t airIndex) {
  // 31 et 108 sont premiers entre eux: permutation bijective.
  return ((uint16_t)airIndex * 31u) % CODE_BITS;
}

static uint8_t syncBit(uint8_t n) {
  return (SYNC_WORD >> (SYNC_BITS - 1 - n)) & 1u;
}

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

static inline void memw() {
#ifndef MODEM_HOST_TEST
  __asm__ volatile("memw" ::: "memory");
#endif
}
static inline uint32_t rd32(uint32_t a) {
#ifdef MODEM_HOST_TEST
  extern uint32_t modemHostReadReg(uint32_t);
  return modemHostReadReg(a);
#else
  return *reinterpret_cast<volatile uint32_t*>(a);
#endif
}
static inline void wr32(uint32_t a, uint32_t v) {
#ifdef MODEM_HOST_TEST
  extern void modemHostWriteReg(uint32_t, uint32_t);
  modemHostWriteReg(a, v);
#else
  *reinterpret_cast<volatile uint32_t*>(a) = v;
#endif
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

// 2-FSK sans dithering : un code K entier fixe par bit.
// Ces codes doivent être validés par mesure IQ sur le matériel cible.

static bool initRf() {
  if (!ensureCpu160MHz()) {
    Serial.printf("ERR TX INIT CODE=CPU_160_FAILED CPU=%uMHz\r\n",
                  (unsigned)ESP.getCpuFreqMHz());
    return false;
  }
  Serial.printf("INIT CPU=%uMHz AUTO=1\r\n", (unsigned)ESP.getCpuFreqMHz());
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


static bool txReady = false;
static uint32_t frameNumber = 0;
static uint8_t coded[CODE_BITS];

static void waitUntil(uint32_t target) {
  while ((int32_t)(micros() - target) < 0) {
    // Une attente de 1 s ne doit pas bloquer le watchdog.
    if ((int32_t)(target - micros()) > 2000) delay(1);
  }
}

static void sendBit(uint8_t bit, uint32_t &boundary) {
  setTone(bit ? TONE_BIT1 : TONE_BIT0);
  boundary += SYMBOL_US;
  waitUntil(boundary);
  ESP.wdtFeed();
}

static void sendFrame() {
  ++frameNumber;
  const uint32_t start = micros();
  uint32_t boundary = start;
  setTone(TONE_A); boundary += MARKER_US; waitUntil(boundary);
  setTone(TONE_B); boundary += MARKER_US; waitUntil(boundary);
  Serial.printf("WS TYPE=TX ST=TRAIN FRAME=%lu WORD=%08lX\r\n",
                (unsigned long)frameNumber, (unsigned long)TEST_WORD);
  for (uint8_t b = 0; b < 2; ++b)
    for (uint8_t n = 0; n < TRAIN_SYMBOLS; ++n) sendBit(b, boundary);
  Serial.println(F("WS TYPE=TX ST=SYNC"));
  for (uint8_t n = 0; n < SYNC_BITS; ++n) sendBit(syncBit(n), boundary);
  for (uint8_t n = 0; n < CODE_BITS; ++n) {
    sendBit(coded[codedIndex(n)], boundary);
    if ((n + 1) % PILOT_EVERY == 0 && n + 1 < CODE_BITS) {
      sendBit(0, boundary);
      sendBit(1, boundary);
      Serial.printf("WS TYPE=TX ST=DATA POS=%u TOTAL=%u\r\n", n + 1, CODE_BITS);
    }
  }
  setTone(TONE_A); boundary += GAP_US; waitUntil(boundary);
  Serial.printf("WS TYPE=TX ST=END FRAME=%lu ELAPSED_US=%lu\r\n",
                (unsigned long)frameNumber, (unsigned long)(micros() - start));
}

void setup() {
  (void)ensureCpu160MHz();
  Serial.begin(SERIAL_BAUD);
  Serial.println(F("BOOT 2FSK_WEAK_TX_V12 BAUD=230400"));
  Serial.flush();
  delay(150);
  txReady = initRf();
  if (!txReady) { Serial.println(F("ERR TX INIT")); return; }
  encodeWord(TEST_WORD, coded);
  Serial.printf("READY 2FSK_WEAK_TX_V12 CH=%u SYM=%luus RAW_BPS=1 "
                "CODED=%u FRAME_US=%lu APWR=%u ASK=%u T0=%u T1=%u WORD=%08lX CPU=%uMHz\r\n",
                RF_CHANNEL, (unsigned long)SYMBOL_US, CODE_BITS,
                (unsigned long)FRAME_US, APWR, ASK, TONE_BIT0, TONE_BIT1,
                (unsigned long)TEST_WORD, (unsigned)ESP.getCpuFreqMHz());
  Serial.flush();
}

void loop() {
  if (!ensureCpu160MHz()) {
    Serial.printf("ERR TX INIT CODE=CPU_160_FAILED CPU=%uMHz\r\n",
                  (unsigned)ESP.getCpuFreqMHz());
    delay(1000); return;
  }
  if (!txReady) { Serial.println(F("ERR TX INIT")); delay(1000); return; }
  sendFrame();
  yield();
}

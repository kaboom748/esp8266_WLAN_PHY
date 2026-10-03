/*
  ESP8266_2FSK_WEAK_RX_V13.ino — prototype faible signal inspiré de WSPR.
  AUTONOME: pas de décodage radio sur le PC; HTML = affichage seulement.
  Core ESP8266 3.1.2, CPU 160 MHz sélectionné automatiquement par le sketch.
  UART 230400 bauds, 8N1. Trame compatible avec TX WEAK V13.

  Principes: accumulation complexe IQ par blocs courts, combinaison de
  puissances sur symboles data de 500 ms, décisions souples, entrelacement,
  Viterbi K7 r=1/2 et CRC16. Le mot attendu n'aide PAS le décodage.
  La longue séquence connue sert à la synchronisation; des pilotes
  0/1 sont ajoutés tous les 12 symboles codés, sauf après le dernier groupe.
  Pas d'AGC matériel piloté; normalisation DSP par des filtres de référence.
  E4 est une énergie brute, pas un SNR ni une puissance en dBm.

  Le PLL RX est centré une fois entre les tons TX K=-3/-5; les quartz sont
  cherchés dans +/-60 kHz puis suivis numériquement. Aucun dithering TX.
  Les filtres sont cohérents sur une RAFale de 1024 IQ, pas sur 1 s entière.
  Leurs puissances sont ensuite combinées: largeur dépend de CAP_US, souvent
  quelques centaines de Hz. Ce n'est ni la bande RF de 6 Hz ni le code K32
  de WSPR. Aucun seuil -31 dB ou fiabilité 100% n'a été mesuré sur ce matériel.
  Compteurs SEARCH/MET chaque seconde même sans signal; '?' rejoue READY.
*/
#include <Arduino.h>
#include <math.h>
extern "C" {
  #include "user_interface.h"
  void preloop_update_frequency();
}

// Déclarations pour le prétraitement automatique des sketches Arduino.
struct IQPoint;
struct BlockMetric;
struct TimeBin;

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

// Protocole commun aux deux sketches V13 (copié ici, aucun .h requis).
static constexpr uint8_t RF_CHANNEL = 6;
static constexpr uint32_t SERIAL_BAUD = 230400;
static constexpr uint32_t SYMBOL_US = 1000000; // apprentissage + synchronisation
static constexpr uint32_t DATA_SYMBOL_US = 500000;
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
    (2 * TRAIN_SYMBOLS + SYNC_BITS) * SYMBOL_US +
    DATA_SLOTS * DATA_SYMBOL_US + 2 * MARKER_US + GAP_US;
static constexpr uint16_t TONE_BIT0 = 1021; // K=-3
static constexpr uint16_t TONE_BIT1 = 1019; // K=-5
static constexpr uint16_t TONE_A = 1016;
static constexpr uint16_t TONE_B = 0;
static constexpr uint8_t APWR = 0;
static constexpr uint8_t ASK = 0;
static_assert(SYMBOL_US % (DATA_SYMBOL_US / 8) == 0,
              "Grille de sync incompatible avec les bins de données");
static_assert(DATA_SYMBOL_US % 8 == 0, "Symbole data divisible par 8 requis");
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
    // K=7, r=1/2, polynomes 171 et 133 octal. Convention propre V13:
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

// Accès radio repris des registres du dépôt fourni. Le PLL est réglé UNE
// fois au milieu des deux tons, jamais commuté entre symboles.
extern "C" void set_rf_freq_offset(int xtal, int mhz, int off);
extern "C" uint8_t chip6_phy_init_ctrl[];
static constexpr uint32_t IQ_CTRL = 0x6000057cu;
static constexpr uint32_t IQ_DCI = 0x600005dcu;
static constexpr uint32_t IQ_DCQ = 0x600005e0u;
static constexpr uint32_t IQ_E4 = 0x600005e4u;
static constexpr uint32_t IQ_DONE = 0x80000000u;
static constexpr uint16_t SCAN_FIELD = 31; // 0.8 us, recherche +/-60 kHz
static constexpr uint16_t TRACK_FIELD = 127; // 3.2 us, reception filtrée
static constexpr uint16_t CAPTURE_N = 1024;
static constexpr uint16_t LUT_N = 1024;
static constexpr uint32_t BLOCK_PERIOD_US = 20000;
static constexpr uint32_t BIN_US = DATA_SYMBOL_US / 8;
static constexpr uint16_t SYNC_BINS_PER_SYMBOL = SYMBOL_US / BIN_US;
static constexpr uint16_t SYNC_BINS = SYNC_BITS * SYNC_BINS_PER_SYMBOL;
static constexpr uint16_t RING_N = 512;
static constexpr int32_t CFO_LIMIT = 60000;
static constexpr int32_t CFO_GRID = 500;
static constexpr uint16_t CFO_N = 2 * CFO_LIMIT / CFO_GRID + 1;
static constexpr uint8_t FILTER_N = 13;
static constexpr uint8_t CFO_WINDOW_BINS = SYMBOL_US / BIN_US;
static constexpr int32_t FILTER_STEP = 100;
static constexpr float SYNC_CORR_MIN = 0.65f;
static constexpr uint8_t SYNC_LOOKAHEAD = SYNC_BINS_PER_SYMBOL / 2;
static constexpr uint32_t SYNC_SEARCH_TIMEOUT_US = 2 * FRAME_US;
static constexpr int32_t TONE_HZ[2] = {78125, -78125};

struct IQPoint { uint32_t cycles; int32_t i, q; };
struct BlockMetric {
  float soft, p0, p1, floor0, floor1;
  float bank[FILTER_N];
  uint32_t atUs, e4, spanUs;
  uint16_t clipped;
  bool valid;
};
struct TimeBin { float soft; uint16_t blocks; };
static IQPoint iq[CAPTURE_N];
static float fftI[CAPTURE_N], fftQ[CAPTURE_N];
static int16_t sineLut[LUT_N];
static float scanSum[CFO_N], scanSumSq[CFO_N];
static TimeBin ringBins[RING_N];
static int16_t receivedSoft[CODE_BITS];
static uint8_t referenceCode[CODE_BITS];
static uint64_t survivors[INPUT_BITS];
static int32_t pathMetric[64], nextMetric[64];
static uint32_t cfgKeep = 0, cpuMHz = 160;
static bool rxReady = false, tracking = false, receiving = false;
static const char *initError = "NONE";
static int32_t cfoHz = 0;
static uint32_t scanBlocks = 0, scanFailures = 0, iqTimeouts = 0;
static uint32_t scanSampleHz = 0, scanJitterNs = 0;
static const char *scanReason = "NONE";
static float scanBestZ = 0, scanBestRatio = 0;
static int32_t scanBestCfo = 0;
static uint32_t nextBlockUs = 0, lastPrintUs = 0, trackingSinceUs = 0;
static uint32_t nextBinUs = 0, binId = 0, candidateEnd = 0;
static uint32_t dataStartBin = 0, frameCount = 0, goodCount = 0;
static float candidateCorr = 0, latestCorr = 0, frameCorr = 0;
static float binSoftSum = 0, binBank[FILTER_N];
static uint16_t binBlocks = 0;
static float frequencyBank[FILTER_N];
static uint16_t frequencyBlocks = 0;
static uint8_t frequencyBins = 0;
static uint16_t slotNumber = 0, codedReceived = 0, pilotErrors = 0;
static uint8_t pilotRemaining = 0;
static int8_t framePolarity = 1;
static BlockMetric lastBlock = {};
static uint32_t captureMaxUs = 0, processMaxUs = 0, blockOverruns = 0;

static inline __attribute__((always_inline)) void memw() {
#ifndef MODEM_HOST_TEST
  __asm__ volatile("memw" ::: "memory");
#endif
}
static inline __attribute__((always_inline)) uint32_t cyclesNow() {
#ifdef MODEM_HOST_TEST
  extern uint32_t modemHostReadCycles();
  return modemHostReadCycles();
#else
  uint32_t c; __asm__ volatile("rsr %0, ccount" : "=a"(c)); return c;
#endif
}
static inline __attribute__((always_inline)) uint32_t rd32(uint32_t a) {
#ifdef MODEM_HOST_TEST
  extern uint32_t modemHostReadReg(uint32_t);
  return modemHostReadReg(a);
#else
  return *reinterpret_cast<volatile uint32_t *>(a);
#endif
}
static inline __attribute__((always_inline)) void wr32(uint32_t a, uint32_t v) {
#ifdef MODEM_HOST_TEST
  extern void modemHostWriteReg(uint32_t, uint32_t);
  modemHostWriteReg(a, v);
#else
  *reinterpret_cast<volatile uint32_t *>(a) = v;
#endif
}

static bool initRx() {
  initError = "CPU_160_FAILED";
  const bool selected = ensureCpu160MHz();
  cpuMHz = ESP.getCpuFreqMHz();
  Serial.printf("INIT CPU=%luMHz AUTO=1\r\n", (unsigned long)cpuMHz); Serial.flush();
  if (!selected || cpuMHz != 160) return false;
  initError = "OPMODE";
  Serial.println(F("INIT OPMODE")); Serial.flush();
  wifi_station_set_auto_connect(0);
  if (!wifi_set_opmode_current(STATION_MODE)) return false;
  delay(80); wifi_station_disconnect();
  initError = "SLEEP";
  if (!wifi_set_sleep_type(NONE_SLEEP_T)) return false;
  initError = "CHANNEL";
  Serial.println(F("INIT CHANNEL")); Serial.flush();
  if (!wifi_set_channel(RF_CHANNEL)) return false;
  delay(40);
  initError = "XTAL";
  const uint8_t xtal = chip6_phy_init_ctrl[1];
  if (xtal > 2) return false;
  Serial.println(F("INIT PLL_MIDPOINT OFF=-320")); Serial.flush();
  // -320 / 1024 MHz = -312500 Hz, exactement le milieu des K=-3/-5.
  set_rf_freq_offset(xtal, 2412 + 5 * (RF_CHANNEL - 1), -320);
  delay(5);
  cfgKeep = rd32(IQ_CTRL) & 0x7ff80000u;
  initError = "NONE";
  return true;
}

static void initDsp() {
  for (uint16_t n = 0; n < LUT_N; ++n)
    sineLut[n] = (int16_t)lroundf(32767.0f * sinf(2.0f * (float)M_PI * n / LUT_N));
  encodeWord(TEST_WORD, referenceCode);
}

static IRAM_ATTR bool captureIQ(uint16_t field, uint32_t &startUs,
                                uint32_t &spanUs, uint32_t &meanE4) {
  // Aucune FFT, trigonométrie ou sortie série pendant cette courte rafale.
  // Interruptions rétablies même en cas de timeout; maximum 12 ms/rafale.
  const uint32_t cfg = cfgKeep | ((uint32_t)field << 2);
  const uint32_t anchorUs = micros();
  const uint32_t anchorCycles = cyclesNow();
  uint64_t sumE = 0;
  bool ok = true;
  noInterrupts();
  for (uint16_t n = 0; n < CAPTURE_N; ++n) {
    const uint32_t before = cyclesNow();
    wr32(IQ_CTRL, cfg); memw();
    wr32(IQ_CTRL, cfg | 1u); memw();
    wr32(IQ_CTRL, cfg | 3u); memw();
    while (!(rd32(IQ_CTRL) & IQ_DONE)) {
      if ((uint32_t)(cyclesNow() - before) > 20u * cpuMHz) { ok = false; break; }
    }
    if (!ok) break;
    iq[n].cycles = cyclesNow();
    iq[n].i = (int32_t)rd32(IQ_DCI);
    iq[n].q = (int32_t)rd32(IQ_DCQ);
    sumE += rd32(IQ_E4);
    wr32(IQ_CTRL, cfg | 1u); memw();
    wr32(IQ_CTRL, cfg); memw();
    if ((uint32_t)(cyclesNow() - anchorCycles) > 12000u * cpuMHz) {
      ok = false; break;
    }
  }
  wr32(IQ_CTRL, cfg); memw();
  interrupts();
  if (!ok) { ++iqTimeouts; return false; }
  startUs = anchorUs + (uint32_t)(iq[0].cycles - anchorCycles) / cpuMHz;
  spanUs = (uint32_t)(iq[CAPTURE_N - 1].cycles - iq[0].cycles) / cpuMHz;
  meanE4 = (uint32_t)(sumE / CAPTURE_N);
  if (spanUs > captureMaxUs) captureMaxUs = spanUs;
  return spanUs != 0;
}

static void fftForward() {
  for (uint16_t i = 1, j = 0; i < CAPTURE_N; ++i) {
    uint16_t bit = CAPTURE_N >> 1;
    while (j & bit) { j ^= bit; bit >>= 1; }
    j ^= bit;
    if (i < j) {
      float t = fftI[i]; fftI[i] = fftI[j]; fftI[j] = t;
      t = fftQ[i]; fftQ[i] = fftQ[j]; fftQ[j] = t;
    }
  }
  for (uint16_t len = 2; len <= CAPTURE_N; len <<= 1) {
    const float a = -2.0f * (float)M_PI / len;
    const float cr = cosf(a), ci = sinf(a);
    for (uint16_t base = 0; base < CAPTURE_N; base += len) {
      float wr = 1, wi = 0;
      for (uint16_t k = 0; k < len / 2; ++k) {
        const uint16_t x = base + k, y = x + len / 2;
        const float vr = wr * fftI[y] - wi * fftQ[y];
        const float vi = wr * fftQ[y] + wi * fftI[y];
        fftI[y] = fftI[x] - vr; fftQ[y] = fftQ[x] - vi;
        fftI[x] += vr; fftQ[x] += vi;
        const float t = wr * cr - wi * ci;
        wi = wr * ci + wi * cr; wr = t;
      }
    }
  }
}

static uint16_t fftIndex(int32_t n) { return (uint32_t)n & (CAPTURE_N - 1); }
static float fftPower(int32_t n) {
  const uint16_t k = fftIndex(n);
  return fftI[k] * fftI[k] + fftQ[k] * fftQ[k];
}
static float interpolatedPower(float bin) {
  const int32_t lo = (int32_t)floorf(bin);
  const float f = bin - lo;
  return (1 - f) * fftPower(lo) + f * fftPower(lo + 1);
}

static void clearBins() {
  for (uint16_t n = 0; n < RING_N; ++n) ringBins[n] = {};
  binId = 0; binBlocks = 0; binSoftSum = 0;
  for (uint8_t k = 0; k < FILTER_N; ++k) binBank[k] = 0;
  for (uint8_t k = 0; k < FILTER_N; ++k) frequencyBank[k] = 0;
  frequencyBlocks = 0; frequencyBins = 0;
  candidateCorr = 0; latestCorr = 0;
  receiving = false; codedReceived = 0; pilotRemaining = 0;
  nextBinUs = micros() + BIN_US;
}

static void enterScan() {
  tracking = false; receiving = false;
  scanBlocks = 0; scanBestZ = 0; scanBestRatio = 0;
  for (uint16_t n = 0; n < CFO_N; ++n) scanSum[n] = scanSumSq[n] = 0;
  clearBins();
}

static bool searchCarrier() {
  uint32_t startUs, spanUs, e4;
  scanReason = "IQ_TIMEOUT";
  if (!captureIQ(SCAN_FIELD, startUs, spanUs, e4)) { ++scanFailures; return false; }
  lastBlock = {}; lastBlock.e4 = e4; lastBlock.spanUs = spanUs;
  const uint32_t span = iq[CAPTURE_N - 1].cycles - iq[0].cycles;
  const float averageDt = (float)span / (CAPTURE_N - 1);
  const float sampleHz = cpuMHz * 1.0e6f / averageDt;
  scanSampleHz = (uint32_t)lroundf(sampleHz);
  // L'acquisition FFT suppose une cadence régulière et sans alias dans la
  // zone recherchée. La détection suivante utilise les temps réels.
  if (sampleHz < 280000.0f) {
    scanReason = "IQ_TOO_SLOW"; ++scanFailures; return false;
  }
  float jitterSq = 0;
  double mi = 0, mq = 0;
  for (uint16_t n = 0; n < CAPTURE_N; ++n) {
    mi += iq[n].i; mq += iq[n].q;
    if (n) {
      const float d = (float)(iq[n].cycles - iq[n - 1].cycles) - averageDt;
      jitterSq += d * d;
    }
  }
  const float jitterCycles = sqrtf(jitterSq / (CAPTURE_N - 1));
  scanJitterNs = (uint32_t)lroundf(jitterCycles * 1000 / cpuMHz);
  if (jitterCycles > 0.20f * cpuMHz) {
    scanReason = "IQ_JITTER"; ++scanFailures; return false;
  }
  scanReason = "NONE";
  mi /= CAPTURE_N; mq /= CAPTURE_N;
  for (uint16_t n = 0; n < CAPTURE_N; ++n) {
    fftI[n] = (float)(((double)iq[n].i - mi) / 64.0);
    fftQ[n] = (float)(((double)iq[n].q - mq) / 64.0);
  }
  fftForward();
  ++scanBlocks;
  float bestZ = -1.0e9f, bestRatio = 0;
  uint16_t best = 0;
  for (uint16_t c = 0; c < CFO_N; ++c) {
    const int32_t offset = (int32_t)c * CFO_GRID - CFO_LIMIT;
    float signal = 0, floor = 0;
    for (uint8_t b = 0; b < 2; ++b) {
      const float bin = (TONE_HZ[b] + offset) * CAPTURE_N / sampleHz;
      signal += interpolatedPower(bin);
      const int32_t center = (int32_t)lroundf(bin);
      for (uint8_t d = 6; d <= 10; ++d)
        floor += (fftPower(center + d) + fftPower(center - d)) / 10.0f;
    }
    // Le score est un rapport dans les filtres, pas un SNR calibré 2.5 kHz.
    const float ratio = floor > 1.0e-12f ? signal / floor : 0;
    const float limited = ratio > 1000 ? 1000 : ratio;
    scanSum[c] += limited; scanSumSq[c] += limited * limited;
    const float mean = scanSum[c] / scanBlocks;
    const float variance = scanSumSq[c] / scanBlocks - mean * mean;
    const float v = variance > 0.25f ? variance : 0.25f;
    const float z = (mean - 1) * sqrtf((float)scanBlocks / v);
    if (z > bestZ) { bestZ = z; bestRatio = mean; best = c; }
  }
  scanBestZ = bestZ; scanBestRatio = bestRatio;
  scanBestCfo = (int32_t)best * CFO_GRID - CFO_LIMIT;
  if (scanBlocks >= 48 && bestZ >= 6 && bestRatio >= 1.10f) {
    cfoHz = scanBestCfo; tracking = true; trackingSinceUs = micros();
    clearBins();
    Serial.printf("WS TYPE=LOCK CFO=%ld Z100=%ld R100=%ld N=%lu\r\n",
                  (long)cfoHz, (long)lroundf(bestZ * 100),
                  (long)lroundf(bestRatio * 100), (unsigned long)scanBlocks);
    return true;
  }
  // Fenêtre bornée pour ne pas conserver indéfiniment une fréquence ancienne.
  if (scanBlocks >= 4096) enterScan();
  return false;
}

static uint32_t phaseStep(int32_t hz) {
  const int64_t v = (int64_t)llround((double)hz * 4294967296.0 /
                                   ((double)cpuMHz * 1.0e6));
  return (uint32_t)v;
}

static float matchedPower(int32_t hz) {
  const uint32_t step = phaseStep(hz), ref = iq[0].cycles;
  int64_t ri = 0, rq = 0;
  for (uint16_t n = 0; n < CAPTURE_N; ++n) {
    const uint32_t phase = (iq[n].cycles - ref) * step;
    const uint16_t k = phase >> 22;
    const int32_t sn = sineLut[k], cs = sineLut[(k + LUT_N / 4) & (LUT_N - 1)];
    // IQ a été ramené à +/-8191: produits signés 32 bits sans overflow.
    ri += iq[n].i * cs + iq[n].q * sn;
    rq += iq[n].q * cs - iq[n].i * sn;
  }
  const float a = (float)ri / (32768.0f * CAPTURE_N);
  const float b = (float)rq / (32768.0f * CAPTURE_N);
  return a * a + b * b;
}

static void prepareIQ(uint16_t &clipped) {
  // Dimensionner la conversion avec le niveau habituel, avant tout écrêtage.
  // Un seul INT32 extrême ne doit pas arrondir toutes les autres mesures à 0.
  uint16_t histogram[32] = {};
  for (uint16_t n = 0; n < CAPTURE_N; ++n) {
    const uint32_t a = (uint32_t)(iq[n].i < 0 ? -(int64_t)iq[n].i : iq[n].i);
    const uint32_t b = (uint32_t)(iq[n].q < 0 ? -(int64_t)iq[n].q : iq[n].q);
    const uint32_t maximum = a > b ? a : b;
    const uint8_t bucket = maximum ? 31u - __builtin_clz(maximum) : 0;
    ++histogram[bucket];
  }
  uint16_t count = 0;
  uint8_t bucket = 0;
  for (; bucket < 31; ++bucket) {
    count += histogram[bucket];
    if (count >= CAPTURE_N * 3 / 4) break;
  }
  const uint8_t exponent = bucket + 4 < 31 ? bucket + 4 : 31;
  const uint32_t clipLimit = exponent == 31 ? 0x7fffffffu : 1u << exponent;
  clipped = 0;
  for (uint16_t n = 0; n < CAPTURE_N; ++n) {
    const uint32_t a = (uint32_t)(iq[n].i < 0 ? -(int64_t)iq[n].i : iq[n].i);
    const uint32_t b = (uint32_t)(iq[n].q < 0 ? -(int64_t)iq[n].q : iq[n].q);
    const uint32_t maximum = a > b ? a : b;
    if (maximum > clipLimit) {
      iq[n].i = (int32_t)((int64_t)iq[n].i * clipLimit / maximum);
      iq[n].q = (int32_t)((int64_t)iq[n].q * clipLimit / maximum);
      ++clipped;
    }
  }
  int64_t sumI = 0, sumQ = 0;
  uint32_t maximum = 0;
  for (uint16_t n = 0; n < CAPTURE_N; ++n) {
    sumI += iq[n].i; sumQ += iq[n].q;
    const uint32_t a = (uint32_t)(iq[n].i < 0 ? -(int64_t)iq[n].i : iq[n].i);
    const uint32_t b = (uint32_t)(iq[n].q < 0 ? -(int64_t)iq[n].q : iq[n].q);
    if (a > maximum) maximum = a;
    if (b > maximum) maximum = b;
  }
  uint8_t shift = 6;
  while ((maximum >> shift) > 4095u && shift < 31) ++shift;
  const int32_t divisor = (int32_t)(1u << shift);
  const int32_t meanI = (int32_t)(sumI / CAPTURE_N / divisor);
  const int32_t meanQ = (int32_t)(sumQ / CAPTURE_N / divisor);
  for (uint16_t n = 0; n < CAPTURE_N; ++n) {
    iq[n].i = iq[n].i / divisor - meanI;
    iq[n].q = iq[n].q / divisor - meanQ;
  }
}

static BlockMetric measureBlock() {
  BlockMetric m{};
  uint32_t start;
  if (!captureIQ(TRACK_FIELD, start, m.spanUs, m.e4)) return m;
  m.atUs = start + m.spanUs / 2;
  prepareIQ(m.clipped);
  if (m.clipped > CAPTURE_N / 20) return m;
  float p[2][FILTER_N];
  float floor[2];
  for (uint8_t b = 0; b < 2; ++b) {
    const int32_t center = TONE_HZ[b] + cfoHz;
    floor[b] = 0.5f * (matchedPower(center - 3000) + matchedPower(center + 3000));
    if (floor[b] < 1.0e-8f) floor[b] = 1.0e-8f;
    for (uint8_t k = 0; k < FILTER_N; ++k)
      p[b][k] = matchedPower(center + ((int32_t)k - FILTER_N / 2) * FILTER_STEP);
  }
  uint8_t best = 0;
  for (uint8_t k = 0; k < FILTER_N; ++k) {
    if (receiving && pilotRemaining) {
      const uint8_t protocolBit = pilotRemaining == 2 ? 0 : 1;
      const uint8_t expected = framePolarity == 1 ? protocolBit : 1 - protocolBit;
      m.bank[k] = 2.0f * p[expected][k] / floor[expected];
    } else {
      m.bank[k] = p[0][k] / floor[0] + p[1][k] / floor[1];
    }
    if (m.bank[k] > m.bank[best]) best = k;
  }
  m.p0 = p[0][best] / floor[0]; m.p1 = p[1][best] / floor[1];
  m.floor0 = floor[0]; m.floor1 = floor[1];
  float soft = m.p1 - m.p0; // positif = bit 1; décision conservée souple
  if (soft > 8) soft = 8;
  if (soft < -8) soft = -8;
  m.soft = soft;
  m.valid = isfinite(soft) && isfinite(m.bank[best]);
  return m;
}

static bool decodeSoft(const int16_t *soft, uint32_t &word, uint16_t &crcRx,
                       uint8_t &changed, uint8_t &erasures) {
  // Viterbi à décisions souples, 64 états, état initial et final zéro.
  for (uint8_t s = 0; s < 64; ++s) pathMetric[s] = s ? -100000000 : 0;
  for (uint8_t t = 0; t < INPUT_BITS; ++t) {
    uint64_t decisions = 0;
    for (uint8_t s = 0; s < 64; ++s) {
      const uint8_t input = s & 1u;
      const uint8_t p0 = s >> 1, p1 = p0 | 32u;
      const uint8_t r0 = (uint8_t)((p0 << 1) | input);
      const uint8_t r1 = (uint8_t)((p1 << 1) | input);
      const int32_t m0 = pathMetric[p0] +
          (parity7(r0 & 0x79u) ? soft[2*t] : -soft[2*t]) +
          (parity7(r0 & 0x5bu) ? soft[2*t+1] : -soft[2*t+1]);
      const int32_t m1 = pathMetric[p1] +
          (parity7(r1 & 0x79u) ? soft[2*t] : -soft[2*t]) +
          (parity7(r1 & 0x5bu) ? soft[2*t+1] : -soft[2*t+1]);
      nextMetric[s] = m1 > m0 ? m1 : m0;
      if (m1 > m0) decisions |= (uint64_t)1 << s;
    }
    survivors[t] = decisions;
    for (uint8_t s = 0; s < 64; ++s) pathMetric[s] = nextMetric[s];
  }
  uint8_t bits[INPUT_BITS], state = 0;
  for (int16_t t = INPUT_BITS - 1; t >= 0; --t) {
    bits[t] = state & 1u;
    state = (state >> 1) | (uint8_t)(((survivors[t] >> state) & 1u) << 5);
  }
  word = 0; crcRx = 0;
  for (uint8_t t = 0; t < 32; ++t) word = (word << 1) | bits[t];
  for (uint8_t t = 32; t < 48; ++t) crcRx = (uint16_t)((crcRx << 1) | bits[t]);
  uint8_t recoded[CODE_BITS]; encodeWord(word, recoded);
  changed = erasures = 0;
  for (uint8_t n = 0; n < CODE_BITS; ++n) {
    if (!soft[n]) ++erasures;
    else if ((soft[n] > 0) != recoded[n]) ++changed;
  }
  return crcRx == crcWord(word);
}

static float syncCorrelation(uint32_t end) {
  if (end < SYNC_BINS) return 0;
  float dot = 0, sq = 0;
  uint16_t valid = 0;
  for (uint16_t n = 0; n < SYNC_BINS; ++n) {
    const TimeBin &b = ringBins[(end - SYNC_BINS + n) % RING_N];
    if (!b.blocks) continue;
    // Saturation souple: un brouilleur très fort ne doit pas imposer la sync.
    float x = b.soft;
    if (x > 2) x = 2;
    if (x < -2) x = -2;
    dot += (syncBit(n / SYNC_BINS_PER_SYMBOL) ? x : -x);
    sq += x * x; ++valid;
  }
  if (valid < SYNC_BINS * 3 / 4 || sq < 1.0e-12f) return 0;
  return dot / sqrtf((float)SYNC_BINS * sq);
}

static void reportFrame() {
  uint32_t word;
  uint16_t crcRx;
  uint8_t changed, erasures;
  const bool crcOk = decodeSoft(receivedSoft, word, crcRx, changed, erasures);
  uint16_t rawErrors = 0, observed = 0;
  for (uint8_t n = 0; n < CODE_BITS; ++n) {
    if (receivedSoft[n]) {
      ++observed;
      if ((receivedSoft[n] > 0) != referenceCode[n]) ++rawErrors;
    }
  }
  // CRC autorise un message quelconque. MATCH compare uniquement au mot test.
  // Un message CRC valide mais différent n'est pas une réception du mot test.
  const bool ok = crcOk && observed >= 16;
  ++frameCount; if (ok) ++goodCount;
  Serial.printf("WS TYPE=FRAME N=%lu OK=%u MATCH=%u WORD=%08lX CRC_RX=%04X "
                "CRC_CALC=%04X RAW_ERR=%u OBS=%u ERA=%u CHANGED=%u "
                "PILOT_ERR=%u CORR1000=%ld CFO=%ld POL=%d GOOD=%lu\r\n",
                (unsigned long)frameCount, ok ? 1u : 0u,
                ok && word == TEST_WORD ? 1u : 0u, (unsigned long)word,
                crcRx, crcWord(word), rawErrors, observed, erasures, changed,
                pilotErrors, (long)lroundf(frameCorr * 1000), (long)cfoHz, (int)framePolarity,
                (unsigned long)goodCount);
  // Les décisions erronées sont conservées dans les logs du moniteur.
  // Deux lignes de 54 métriques au maximum, hors capture IQ.
  for (uint8_t base = 0; base < CODE_BITS; base += 54) {
    Serial.printf("WS TYPE=SOFT N=%lu BASE=%u VALUES=", (unsigned long)frameCount, base);
    for (uint8_t n = base; n < base + 54; ++n) {
      if (n != base) Serial.print(',');
      Serial.print(receivedSoft[n]);
    }
    Serial.println();
  }
  receiving = false; candidateCorr = 0;
  trackingSinceUs = micros();
}

static void consumeData() {
  while (receiving && binId - dataStartBin >= (uint32_t)(slotNumber + 1) * 8u) {
    const uint32_t start = dataStartBin + (uint32_t)slotNumber * 8u;
    float sum = 0; uint8_t valid = 0;
    // Les 62,5 ms de chaque bord sont exclus.
    for (uint8_t b = 1; b <= 6; ++b) {
      const TimeBin &v = ringBins[(start + b) % RING_N];
      if (v.blocks) { sum += v.soft; ++valid; }
    }
    const float soft = valid >= 4 ? framePolarity * sum / valid : 0;
    if (pilotRemaining) {
      const uint8_t expected = pilotRemaining == 2 ? 0 : 1;
      if (fabsf(soft) < 0.05f || (soft > 0) != expected) ++pilotErrors;
      --pilotRemaining;
    } else {
      int32_t quantized = (int32_t)lroundf(soft * 32);
      if (quantized > 255) quantized = 255;
      if (quantized < -255) quantized = -255;
      receivedSoft[codedIndex(codedReceived)] = (int16_t)quantized;
      ++codedReceived;
      if (codedReceived % PILOT_EVERY == 0 && codedReceived < CODE_BITS) pilotRemaining = 2;
    }
    ++slotNumber;
    if (codedReceived == CODE_BITS) reportFrame();
  }
}

static void inspectSync() {
  if (receiving) { consumeData(); return; }
  latestCorr = syncCorrelation(binId);
  if (fabsf(latestCorr) >= SYNC_CORR_MIN && fabsf(latestCorr) > fabsf(candidateCorr)) {
    candidateCorr = latestCorr; candidateEnd = binId;
  }
  if (fabsf(candidateCorr) >= SYNC_CORR_MIN && binId - candidateEnd >= SYNC_LOOKAHEAD) {
    dataStartBin = candidateEnd;
    framePolarity = candidateCorr >= 0 ? 1 : -1;
    frameCorr = fabsf(candidateCorr);
    receiving = true; slotNumber = codedReceived = 0; pilotErrors = 0; pilotRemaining = 0;
    for (uint8_t n = 0; n < CODE_BITS; ++n) receivedSoft[n] = 0;
    Serial.printf("WS TYPE=SYNC CORR1000=%ld CFO=%ld POL=%d DATA_BIN=%lu\r\n",
                  (long)lroundf(frameCorr * 1000), (long)cfoHz, (int)framePolarity,
                  (unsigned long)dataStartBin);
    candidateCorr = 0;
    consumeData();
  }
}

static void finishBin() {
  ringBins[binId % RING_N] = {binBlocks ? binSoftSum / binBlocks : 0, binBlocks};
  // Suivi prudent du décalage des quartz, avec la même correction sur les
  // deux tons. Accumule 1 s avant de déplacer le centre; les pilotes utilisent
  // seulement l'énergie du ton connu. Les observations trop faibles gardent
  // le réglage précédent plutôt que de suivre les fluctuations du bruit.
  for (uint8_t k = 0; k < FILTER_N; ++k) frequencyBank[k] += binBank[k];
  frequencyBlocks += binBlocks;
  ++frequencyBins;
  if (frequencyBins == CFO_WINDOW_BINS && frequencyBlocks >= 24) {
    uint8_t best = 0;
    for (uint8_t k = 1; k < FILTER_N; ++k)
      if (frequencyBank[k] > frequencyBank[best]) best = k;
    const float strength = frequencyBank[best] / frequencyBlocks;
    const float improvement = frequencyBank[best] - frequencyBank[FILTER_N / 2];
    if (strength > 6 && improvement > 0.15f * frequencyBank[best]) {
      cfoHz += ((int32_t)best - FILTER_N / 2) * FILTER_STEP / 4;
      if (cfoHz > CFO_LIMIT) cfoHz = CFO_LIMIT;
      if (cfoHz < -CFO_LIMIT) cfoHz = -CFO_LIMIT;
    }
  }
  if (frequencyBins == CFO_WINDOW_BINS) {
    frequencyBins = 0; frequencyBlocks = 0;
    for (uint8_t k = 0; k < FILTER_N; ++k) frequencyBank[k] = 0;
  }
  ++binId;
  inspectSync();
  binSoftSum = 0; binBlocks = 0;
  for (uint8_t k = 0; k < FILTER_N; ++k) binBank[k] = 0;
}

static void addBlock(const BlockMetric &m) {
  // Un bloc est attribué selon son milieu RF, pas selon la fin du calcul DSP.
  const uint32_t at = m.valid ? m.atUs : micros();
  while ((int32_t)(at - nextBinUs) >= 0) { finishBin(); nextBinUs += BIN_US; }
  if (m.valid) {
    binSoftSum += m.soft; ++binBlocks;
    for (uint8_t k = 0; k < FILTER_N; ++k) binBank[k] += m.bank[k];
  }
}

static void printReady() {
  Serial.printf("READY 2FSK_WEAK_RX_V13 CH=%u SYM=%luus DATA_SYM=%luus RAW_BPS=2 "
                "CODED=%u FRAME_US=%lu BAUD=%lu CPU=%luMHz SCAN_ADC=%u "
                "TRACK_ADC=%u LO_OFF_HZ=-312500 CFO_LIMIT=%ld\r\n",
                RF_CHANNEL, (unsigned long)SYMBOL_US,
                (unsigned long)DATA_SYMBOL_US, CODE_BITS,
                (unsigned long)FRAME_US, (unsigned long)SERIAL_BAUD,
                (unsigned long)cpuMHz, SCAN_FIELD + 1, TRACK_FIELD + 1,
                (long)CFO_LIMIT);
}

static void printStatus() {
  if (!rxReady) {
    Serial.printf("ERR RX INIT CODE=%s BAUD=%lu CPU=%luMHz\r\n", initError,
                  (unsigned long)SERIAL_BAUD, (unsigned long)cpuMHz);
  } else if (!tracking) {
    Serial.printf("WS TYPE=SEARCH N=%lu FAIL=%lu TO=%lu E4=%lu CFO=%ld "
                  "Z100=%ld R100=%ld CAP_US=%lu ADC=%u FS_HZ=%lu JIT_NS=%lu WHY=%s\r\n",
                  (unsigned long)scanBlocks, (unsigned long)scanFailures,
                  (unsigned long)iqTimeouts, (unsigned long)lastBlock.e4,
                  (long)scanBestCfo, (long)lroundf(scanBestZ * 100),
                  (long)lroundf(scanBestRatio * 100),
                  (unsigned long)lastBlock.spanUs, SCAN_FIELD + 1,
                  (unsigned long)scanSampleHz, (unsigned long)scanJitterNs, scanReason);
  } else {
    Serial.printf("WS TYPE=MET ST=%s POL=%d POS=%u TOTAL=%u SOFT100=%ld "
                  "P0100=%ld P1100=%ld CFO=%ld E4=%lu CAP_US=%lu "
                  "PROC_US=%lu OVER=%lu TO=%lu CLIP=%u CORR1000=%ld ADC=%u\r\n",
                  receiving ? "DATA" : "SYNC", (int)framePolarity, codedReceived, CODE_BITS,
                  (long)lroundf(lastBlock.soft * 100),
                  (long)lroundf(fminf(lastBlock.p0, 100000.0f) * 100),
                  (long)lroundf(fminf(lastBlock.p1, 100000.0f) * 100),
                  (long)cfoHz, (unsigned long)lastBlock.e4,
                  (unsigned long)lastBlock.spanUs, (unsigned long)processMaxUs,
                  (unsigned long)blockOverruns, (unsigned long)iqTimeouts,
                  lastBlock.clipped, (long)lroundf(latestCorr * 1000), TRACK_FIELD + 1);
  }
  lastPrintUs = micros();
}

void setup() {
  (void)ensureCpu160MHz();
  Serial.begin(SERIAL_BAUD);
  Serial.println(F("BOOT 2FSK_WEAK_RX_V13 BAUD=230400")); Serial.flush();
  delay(150);
  initDsp();
  rxReady = initRx();
  if (!rxReady) { printStatus(); return; }
  printReady(); Serial.flush();
  enterScan(); nextBlockUs = micros(); lastPrintUs = micros();
}

void loop() {
  // Vérifier la fréquence avant les captures utilisant le compteur CPU.
  if (!ensureCpu160MHz() && rxReady) {
    cpuMHz = ESP.getCpuFreqMHz();
    initError = "CPU_160_FAILED"; rxReady = false; printStatus();
  }
  bool request = false;
  for (uint8_t n = 0; n < 32 && Serial.available(); ++n)
    if (Serial.read() == '?') request = true;
  if (request) { if (rxReady) printReady(); printStatus(); }
  if ((uint32_t)(micros() - lastPrintUs) >= 1000000u) printStatus();
  if (!rxReady) { delay(10); return; }
  const uint32_t now = micros();
  if ((int32_t)(now - nextBlockUs) < 0) { delay(0); return; }
  const uint32_t before = micros();
  if (!tracking) searchCarrier();
  else {
    lastBlock = measureBlock(); addBlock(lastBlock);
    if (!receiving && (uint32_t)(micros() - trackingSinceUs) > SYNC_SEARCH_TIMEOUT_US) {
      Serial.println(F("WS TYPE=RESET REASON=NO_SYNC")); enterScan();
    }
  }
  const uint32_t elapsed = micros() - before;
  if (elapsed > processMaxUs) processMaxUs = elapsed;
  nextBlockUs = before + BLOCK_PERIOD_US;
  if ((int32_t)(micros() - nextBlockUs) >= 0) {
    ++blockOverruns; nextBlockUs = micros();
  }
  ESP.wdtFeed(); yield();
}

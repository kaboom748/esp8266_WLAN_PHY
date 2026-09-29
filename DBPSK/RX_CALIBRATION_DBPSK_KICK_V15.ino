/*
  RX_CALIBRATION_DBPSK_KICK_V15.ino

  RX DBPSK pour TX_REFERENCE_DBPSK_FIXED_V4.ino.

  V13 revient volontairement a la methode qui a deja fonctionne sur le
  materiel reel:
    - FSK A/B/A uniquement pour la synchronisation de trame;
    - detection locale du phase-kick par IQ consecutifs;
    - baseline robuste = mediane de dphi/dt;
    - residual = wrapPi(dphi - omega*dt);
    - somme sur 9 paires autour du kick;
    - capture des 32 fenetres en temps reel, analyse seulement apres.

  V14 conserve strictement ce detecteur et ne raffine que les validations
  confirmees par les logs V13:
    1) marqueur: score minimal realiste + fenetre temporelle asymetrique;
    2) payload: recherche locale resserree a +/-18 us autour du front;
    3) sequence +/- utilisee uniquement sur cette mesure locale;
    4) RAW reste affiche comme reference historique, CTR comme decision locale,
       FINAL comme decision sequence sur les kicks temporellement plausibles.

  V15:
    - resserre uniquement la borne positive du marqueur de +40 us a +20 us;
    - ajoute un statut humain [RÉUSSITE] / [ÉCHEC] sur chaque trame decodee.

  Aucun phasor longue duree, aucune recuperation de porteuse absolue,
  aucune comparaison de phase a 1 ms.
*/

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>

extern "C" {
  #include "user_interface.h"
}

// ------------------------------------------------------------
// Configuration
// ------------------------------------------------------------

static constexpr uint8_t  RF_CHANNEL = 6;
static constexpr uint16_t IQ_FIELD = 4;
static constexpr uint8_t  IQ_MODE_SEL = 0;
static constexpr uint32_t TIMEOUT_US = 1000;

static constexpr uint32_t GUARD_US = 10000;
static constexpr uint32_t MARKER_TO_PAYLOAD_US = 80000;
static constexpr uint32_t SYMBOL_US = 1000;
static constexpr uint32_t EXPECTED = 0xD3A5C69Bu;

// FSK: synchronisation uniquement.
static constexpr uint8_t LEVEL_MEDIAN_N = 5;
static constexpr uint8_t RAW_IQ_PER_FAST_EST = 6;
static constexpr uint32_t A_STABLE_US = 30000;
static constexpr uint32_t B_STABLE_US = 12000;
static constexpr int32_t SAME_LEVEL_TOL_HZ = 9000;
static constexpr int32_t DIFFERENT_LEVEL_HZ = 13000;
static constexpr uint8_t A_RETURN_COUNT = 2;
// Logs V13: vraies trames typiquement OFF=-160..0 us; faux verrouillages
// catastrophiques observes a +70..+191 us ou au-dela.
static constexpr int32_t MARKER_OFFSET_MIN_US = -180;
static constexpr int32_t MARKER_OFFSET_MAX_US =   20;

// Marqueur connu autour de guardStart + GUARD_US.
static constexpr uint32_t MARKER_BEFORE_US = 1200;
static constexpr uint32_t MARKER_AFTER_US  = 1200;
static constexpr uint16_t MARKER_MAX_SAMPLES = 512;

// Petite fenetre capturee autour de chaque front symbole.
// 280 us donne typiquement ~55..65 acquisitions IQ_EST N=4.
static constexpr uint32_t PAY_BEFORE_US = 180;
static constexpr uint32_t PAY_AFTER_US  = 220;
static constexpr uint8_t  PAY_MAX_SAMPLES = 96;

// Detecteur de kick, derive du PHASE_KICK_RX_V2 fonctionnel.
static constexpr float STRONG_PAIR_RAD = 0.75f;
// Le score marqueur est une somme locale, pas un simple pic.
// V13: vrais marqueurs ~8.8..9.6 rad, faux verrous catastrophiques ~5.9..6.0.
static constexpr float MARKER_THRESHOLD_RAD = 7.50f;
static constexpr float DBPSK_THRESHOLD_RAD  = 1.55f;
static constexpr uint8_t CLUSTER_HALF_WIDTH = 4; // 9 paires

// Le vrai phase-kick est centre sur la frontiere symbole.
// On garde une marge large pour la latence IQ_EST, mais on ne laisse plus
// un parasite situe a +/-180..220 us gagner simplement parce qu'il est gros.
// V13: les vrais kicks payload sont presque tous dans -17..+17 us.
// Les erreurs restantes etaient des parasites a -21,-26,+34,+91,+119,-63,-82 us.
static constexpr int16_t PAY_EVENT_MIN_US = -18;
static constexpr int16_t PAY_EVENT_MAX_US =  18;

// Decodeur de sequence: la magnitude reste l'information principale.
// Le signe +/- n'est qu'une contrainte souple pour rejeter les gros pics
// parasites incompatibles avec l'alternance physique des kicks.
static constexpr float SEQ_MAG_SCALE_RAD = 0.55f;
static constexpr float SEQ_MAX_EVIDENCE = 2.50f;
static constexpr float SEQ_SIGN_MISMATCH_COST = 2.80f;
static constexpr float SEQ_NO_SIGN_COST = 1.20f;

// ------------------------------------------------------------
// IQ_EST
// ------------------------------------------------------------

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

struct RawIQ {
  uint32_t t;
  int32_t i;
  int32_t q;
  uint32_t e4;
};

struct FreqEst {
  uint32_t t;
  int32_t hz;
  uint32_t e4;
  uint16_t spanUs;
  bool valid;
};

struct KickResult {
  // score/signe/offset = detecteur V7 original: meilleur evenement partout
  // dans la fenetre capturee.
  float score;
  float signedSum;
  float peak;
  int16_t eventOffsetUs;
  uint8_t support;

  // Variante centree: meme detecteur, mais candidat limite autour de la
  // frontiere symbole. RAW continue d'utiliser score; CTR/SEQ utilisent ceci.
  float centerScore;
  float centerSignedSum;
  float centerPeak;
  int16_t centerOffsetUs;
  uint8_t centerSupport;

  float baselineHz;
  uint32_t eventUs;
  uint32_t avgE4;
  uint16_t sampleCount;
  bool valid;
};

// Format compact pour stocker les 32 fenetres sans exploser la RAM.
// dt = temps depuis l'echantillon precedent de LA MEME fenetre.
struct __attribute__((packed)) CompactIQ {
  int32_t i;
  int32_t q;
  uint16_t dt;
};

static uint32_t cfgKeep = 0;

// Buffer marqueur (analyse avant payload).
static RawIQ markerIQ[MARKER_MAX_SAMPLES];

// Capture payload: ~23 ko (72 * 32 * 10 octets).
static CompactIQ payloadIQ[32][PAY_MAX_SAMPLES];
static uint8_t payloadN[32];

// Buffers de travail reutilises pendant l'analyse offline.
static float omegaBuf[MARKER_MAX_SAMPLES - 1];
static float residualBuf[MARKER_MAX_SAMPLES - 1];

static inline void memw() {
  __asm__ volatile("memw" ::: "memory");
}

static inline uint32_t rd32(uint32_t a) {
  return *reinterpret_cast<volatile uint32_t*>(a);
}

static inline void wr32(uint32_t a, uint32_t v) {
  *reinterpret_cast<volatile uint32_t*>(a) = v;
}

static inline float wrapPi(float x) {
  while (x > (float)M_PI)  x -= 2.0f * (float)M_PI;
  while (x < -(float)M_PI) x += 2.0f * (float)M_PI;
  return x;
}

static inline float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

static inline int8_t signOf(float x) {
  return (x > 0.0f) ? 1 : ((x < 0.0f) ? -1 : 0);
}

static inline int32_t absI32(int32_t x) {
  return (x < 0) ? -x : x;
}

static int cmpI32(const void *a, const void *b) {
  const int32_t x = *(const int32_t*)a;
  const int32_t y = *(const int32_t*)b;
  return (x > y) - (x < y);
}

static int cmpFloat(const void *a, const void *b) {
  const float x = *(const float*)a;
  const float y = *(const float*)b;
  return (x > y) - (x < y);
}

static uint32_t makeCfg() {
  return cfgKeep |
         (IQ_MODE_SEL ? IQ_MODE : 0u) |
         (((uint32_t)IQ_FIELD << 2) & IQ_FIELD_MASK);
}

static bool initRx() {
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
  cfgKeep = rd32(IQ_CTRL) & IQ_KEEP_MASK;
  return true;
}

static bool acquireIQ(RawIQ &s) {
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
    if ((uint32_t)(micros() - t0) >= TIMEOUT_US)
      return false;
    c = rd32(IQ_CTRL);
  }

  s.t  = micros();
  s.i  = (int32_t)rd32(IQ_DCI);
  s.q  = (int32_t)rd32(IQ_DCQ);
  s.e4 = rd32(IQ_E4);

  wr32(IQ_CTRL, cfg | IQ_ENABLE);
  memw();
  wr32(IQ_CTRL, cfg);
  memw();

  return true;
}

static inline float phaseRaw(
  int32_t ai, int32_t aq,
  int32_t bi, int32_t bq
) {
  const double cross =
    (double)ai * (double)bq -
    (double)aq * (double)bi;

  const double dot =
    (double)ai * (double)bi +
    (double)aq * (double)bq;

  return atan2f((float)cross, (float)dot);
}

static inline float phaseBetween(const RawIQ &a, const RawIQ &b) {
  return phaseRaw(a.i, a.q, b.i, b.q);
}

// ------------------------------------------------------------
// FSK A/B/A: SYNCHRONISATION seulement
// ------------------------------------------------------------

static bool estimateFreqFast(FreqEst &out) {
  RawIQ s[RAW_IQ_PER_FAST_EST];

  for (uint8_t k = 0; k < RAW_IQ_PER_FAST_EST; ++k) {
    if (!acquireIQ(s[k])) {
      out.valid = false;
      return false;
    }
  }

  double sumCross = 0.0;
  double sumDot = 0.0;
  uint32_t sumDt = 0;
  uint64_t sumE4 = 0;
  uint8_t pairs = 0;

  for (uint8_t k = 0; k < RAW_IQ_PER_FAST_EST; ++k)
    sumE4 += s[k].e4;

  for (uint8_t k = 1; k < RAW_IQ_PER_FAST_EST; ++k) {
    const uint32_t dt = s[k].t - s[k - 1].t;
    if (dt == 0 || dt > 100u)
      continue;

    const double cross =
      (double)s[k - 1].i * (double)s[k].q -
      (double)s[k - 1].q * (double)s[k].i;

    const double dot =
      (double)s[k - 1].i * (double)s[k].i +
      (double)s[k - 1].q * (double)s[k].q;

    sumCross += cross;
    sumDot += dot;
    sumDt += dt;
    ++pairs;
  }

  out.t = s[RAW_IQ_PER_FAST_EST - 1].t;
  out.e4 = (uint32_t)(sumE4 / RAW_IQ_PER_FAST_EST);
  out.spanUs = (uint16_t)(s[RAW_IQ_PER_FAST_EST - 1].t - s[0].t);
  out.hz = 0;
  out.valid = false;

  if (pairs < 3 || sumDt == 0)
    return false;

  const float meanPhase = atan2f((float)sumCross, (float)sumDot);
  const float meanDtUs = (float)sumDt / (float)pairs;

  out.hz = (int32_t)lroundf(
    meanPhase /
    (2.0f * (float)M_PI * meanDtUs * 1.0e-6f)
  );

  out.valid = true;
  return true;
}

static bool measureLevel(FreqEst &out) {
  int32_t hz[LEVEL_MEDIAN_N];
  uint32_t tLast = 0;
  uint64_t e4sum = 0;
  uint32_t spanSum = 0;

  for (uint8_t k = 0; k < LEVEL_MEDIAN_N; ++k) {
    FreqEst f;
    if (!estimateFreqFast(f))
      return false;

    hz[k] = f.hz;
    tLast = f.t;
    e4sum += f.e4;
    spanSum += f.spanUs;
  }

  qsort(hz, LEVEL_MEDIAN_N, sizeof(int32_t), cmpI32);

  out.hz = hz[LEVEL_MEDIAN_N / 2];
  out.t = tLast;
  out.e4 = (uint32_t)(e4sum / LEVEL_MEDIAN_N);
  out.spanUs = (uint16_t)(spanSum / LEVEL_MEDIAN_N);
  out.valid = true;
  return true;
}

static bool waitForAutoFskSync(
  uint32_t &guardStartUs,
  int32_t &aLevel,
  int32_t &bLevel
) {
  enum {
    FIND_LONG_A = 0,
    WAIT_B,
    QUALIFY_B,
    WAIT_RETURN_A
  };

  uint8_t state = FIND_LONG_A;
  int32_t aRef = 0;
  int32_t bRef = 0;
  uint32_t aStart = 0;
  uint32_t bStart = 0;
  bool haveARef = false;
  uint32_t lastPrint = millis();

  // Variables du verrouillage rapide B->A.
  uint8_t aConfirm = 0;
  uint32_t firstAEdgeUs = 0;

  while (true) {
    FreqEst f;

    // IMPORTANT V6:
    // pendant le retour B->A on n'utilise PLUS la mesure mediane lente.
    // On enchaine des estimations rapides afin de dater le front reel.
    const bool ok = (state == WAIT_RETURN_A)
      ? estimateFreqFast(f)
      : measureLevel(f);

    if (!ok) {
      yield();
      continue;
    }

    if (state == FIND_LONG_A) {
      if (!haveARef) {
        aRef = f.hz;
        aStart = f.t;
        haveARef = true;
      }
      else if (absI32(f.hz - aRef) <= SAME_LEVEL_TOL_HZ) {
        aRef = (aRef * 7 + f.hz) / 8;
        if ((uint32_t)(f.t - aStart) >= A_STABLE_US)
          state = WAIT_B;
      }
      else {
        aRef = f.hz;
        aStart = f.t;
      }
    }
    else if (state == WAIT_B) {
      if (absI32(f.hz - aRef) >= DIFFERENT_LEVEL_HZ) {
        bRef = f.hz;
        bStart = f.t;
        state = QUALIFY_B;
      }
      else {
        aRef = (aRef * 7 + f.hz) / 8;
      }
    }
    else if (state == QUALIFY_B) {
      const bool nearB = absI32(f.hz - bRef) <= SAME_LEVEL_TOL_HZ;
      const bool farA = absI32(f.hz - aRef) >= DIFFERENT_LEVEL_HZ;

      if (nearB && farA) {
        bRef = (bRef * 7 + f.hz) / 8;
        if ((uint32_t)(f.t - bStart) >= B_STABLE_US) {
          state = WAIT_RETURN_A;
          aConfirm = 0;
          firstAEdgeUs = 0;
        }
      }
      else if (absI32(f.hz - aRef) <= SAME_LEVEL_TOL_HZ) {
        state = WAIT_B;
      }
      else {
        bRef = f.hz;
        bStart = f.t;
      }
    }
    else {
      // Classification par proximite des deux niveaux appris.
      const int32_t da = absI32(f.hz - aRef);
      const int32_t db = absI32(f.hz - bRef);
      const bool isA = (da < db) && (da <= SAME_LEVEL_TOL_HZ);

      if (isA) {
        if (aConfirm == 0) {
          // f.t est la FIN de la courte estimation. Le vrai front B->A
          // est quelque part dans cette petite fenetre; son milieu est une
          // bien meilleure estimation que f.t lui-meme.
          firstAEdgeUs = f.t - (uint32_t)(f.spanUs / 2u);
        }

        ++aConfirm;

        if (aConfirm >= A_RETURN_COUNT) {
          guardStartUs = firstAEdgeUs;
          aLevel = aRef;
          bLevel = bRef;
          return true;
        }
      }
      else {
        aConfirm = 0;
        firstAEdgeUs = 0;
      }
    }

    if ((uint32_t)(millis() - lastPrint) >= 1000u) {
      Serial.printf(
        "SYNC state=%u hz=%ld A=%ld B=%ld\r\n",
        state,
        (long)f.hz,
        (long)aRef,
        (long)bRef
      );
      Serial.flush();
      lastPrint = millis();
    }

    yield();
  }
}

// ------------------------------------------------------------
// Detecteur robuste pour le marqueur
// ------------------------------------------------------------

static inline void waitUntil(uint32_t t) {
  while ((int32_t)(micros() - t) < 0) {}
}

static bool captureMarkerWindow(
  uint32_t centerUs,
  uint16_t &n
) {
  const uint32_t startUs = centerUs - MARKER_BEFORE_US;
  const uint32_t stopUs  = centerUs + MARKER_AFTER_US;

  if ((int32_t)(micros() - stopUs) >= 0)
    return false;

  waitUntil(startUs);

  n = 0;
  while ((int32_t)(micros() - stopUs) < 0 &&
         n < MARKER_MAX_SAMPLES) {
    if (!acquireIQ(markerIQ[n]))
      return false;
    ++n;
  }

  return n >= 20;
}

static bool analyzeMarker(uint16_t n, KickResult &out) {
  out.score = 0.0f;
  out.signedSum = 0.0f;
  out.peak = 0.0f;
  out.baselineHz = 0.0f;
  out.eventUs = 0;
  out.eventOffsetUs = 0;
  out.support = 0;
  out.centerScore = 0.0f;
  out.centerSignedSum = 0.0f;
  out.centerPeak = 0.0f;
  out.centerOffsetUs = 0;
  out.centerSupport = 0;
  out.avgE4 = 0;
  out.sampleCount = n;
  out.valid = false;

  if (n < 20)
    return false;

  uint16_t nOmega = 0;
  uint64_t e4sum = 0;

  for (uint16_t k = 0; k < n; ++k)
    e4sum += markerIQ[k].e4;

  for (uint16_t k = 1; k < n; ++k) {
    const uint32_t dt = markerIQ[k].t - markerIQ[k - 1].t;
    if (dt == 0 || dt > 100u)
      continue;

    const float dp = phaseBetween(markerIQ[k - 1], markerIQ[k]);
    omegaBuf[nOmega++] = dp / (float)dt;
  }

  if (nOmega < 12)
    return false;

  qsort(omegaBuf, nOmega, sizeof(float), cmpFloat);

  const float omega =
    (nOmega & 1u) ?
      omegaBuf[nOmega / 2] :
      0.5f * (omegaBuf[nOmega / 2 - 1] + omegaBuf[nOmega / 2]);

  out.baselineHz = omega * 1.0e6f / (2.0f * (float)M_PI);
  out.avgE4 = (uint32_t)(e4sum / n);

  const uint16_t pairs = n - 1;

  for (uint16_t k = 0; k < pairs; ++k) {
    const uint32_t dt = markerIQ[k + 1].t - markerIQ[k].t;

    if (dt == 0 || dt > 100u) {
      residualBuf[k] = 0.0f;
      continue;
    }

    const float dp = phaseBetween(markerIQ[k], markerIQ[k + 1]);
    residualBuf[k] = wrapPi(dp - omega * (float)dt);
  }

  float bestAbs = 0.0f;
  float bestSigned = 0.0f;
  float bestPeak = 0.0f;
  uint16_t bestK = 0;

  for (uint16_t k = CLUSTER_HALF_WIDTH;
       k + CLUSTER_HALF_WIDTH < pairs;
       ++k) {

    if (fabsf(residualBuf[k]) < STRONG_PAIR_RAD)
      continue;

    float sum = 0.0f;
    float peak = residualBuf[k];

    for (int8_t j = -(int8_t)CLUSTER_HALF_WIDTH;
         j <= (int8_t)CLUSTER_HALF_WIDTH;
         ++j) {
      const float r = residualBuf[(int)k + j];
      sum += r;
      if (fabsf(r) > fabsf(peak))
        peak = r;
    }

    const float a = fabsf(sum);
    if (a > bestAbs) {
      bestAbs = a;
      bestSigned = sum;
      bestPeak = peak;
      bestK = k;
    }
  }

  out.score = bestAbs;
  out.signedSum = bestSigned;
  out.peak = bestPeak;

  if (bestAbs > 0.0f)
    out.eventUs = markerIQ[bestK + 1].t;

  out.valid = true;
  return true;
}

// ------------------------------------------------------------
// Capture payload TEMPS REEL, analyse APRES la trame
// ------------------------------------------------------------

static bool capturePayloadSymbol(
  uint8_t bitIndex,
  uint32_t boundaryUs
) {
  const uint32_t startUs = boundaryUs - PAY_BEFORE_US;
  const uint32_t stopUs  = boundaryUs + PAY_AFTER_US;

  if ((int32_t)(micros() - stopUs) >= 0) {
    payloadN[bitIndex] = 0;
    return false;
  }

  waitUntil(startUs);

  uint8_t n = 0;
  uint32_t prevT = 0;

  while ((int32_t)(micros() - stopUs) < 0 &&
         n < PAY_MAX_SAMPLES) {
    RawIQ s;
    if (!acquireIQ(s)) {
      payloadN[bitIndex] = 0;
      return false;
    }

    payloadIQ[bitIndex][n].i = s.i;
    payloadIQ[bitIndex][n].q = s.q;

    if (n == 0) {
      payloadIQ[bitIndex][n].dt = 0;
    }
    else {
      uint32_t dt = s.t - prevT;
      if (dt > 65535u) dt = 65535u;
      payloadIQ[bitIndex][n].dt = (uint16_t)dt;
    }

    prevT = s.t;
    ++n;
  }

  payloadN[bitIndex] = n;
  return n >= 20;
}

static bool analyzePayloadSymbol(
  uint8_t bitIndex,
  KickResult &out
) {
  const uint8_t n = payloadN[bitIndex];

  out.score = 0.0f;
  out.signedSum = 0.0f;
  out.peak = 0.0f;
  out.baselineHz = 0.0f;
  out.eventUs = 0;
  out.eventOffsetUs = 0;
  out.support = 0;
  out.centerScore = 0.0f;
  out.centerSignedSum = 0.0f;
  out.centerPeak = 0.0f;
  out.centerOffsetUs = 0;
  out.centerSupport = 0;
  out.avgE4 = 0;
  out.sampleCount = n;
  out.valid = false;

  if (n < 20)
    return false;

  uint16_t nOmega = 0;
  uint16_t relUs[PAY_MAX_SAMPLES];
  relUs[0] = 0;

  for (uint8_t k = 1; k < n; ++k) {
    const uint16_t dt = payloadIQ[bitIndex][k].dt;

    uint32_t r = (uint32_t)relUs[k - 1] + dt;
    if (r > 65535u) r = 65535u;
    relUs[k] = (uint16_t)r;

    if (dt == 0 || dt > 100u)
      continue;

    const CompactIQ &a = payloadIQ[bitIndex][k - 1];
    const CompactIQ &b = payloadIQ[bitIndex][k];

    const float dp = phaseRaw(a.i, a.q, b.i, b.q);
    omegaBuf[nOmega++] = dp / (float)dt;
  }

  if (nOmega < 12)
    return false;

  qsort(omegaBuf, nOmega, sizeof(float), cmpFloat);

  const float omega =
    (nOmega & 1u) ?
      omegaBuf[nOmega / 2] :
      0.5f * (omegaBuf[nOmega / 2 - 1] + omegaBuf[nOmega / 2]);

  out.baselineHz = omega * 1.0e6f / (2.0f * (float)M_PI);

  const uint8_t pairs = n - 1;

  for (uint8_t k = 0; k < pairs; ++k) {
    const uint16_t dt = payloadIQ[bitIndex][k + 1].dt;

    if (dt == 0 || dt > 100u) {
      residualBuf[k] = 0.0f;
      continue;
    }

    const CompactIQ &a = payloadIQ[bitIndex][k];
    const CompactIQ &b = payloadIQ[bitIndex][k + 1];

    const float dp = phaseRaw(a.i, a.q, b.i, b.q);
    residualBuf[k] = wrapPi(dp - omega * (float)dt);
  }

  float bestAbs = 0.0f;
  float bestSigned = 0.0f;
  float bestPeak = 0.0f;
  int16_t bestOffset = 0;
  uint8_t bestSupport = 0;

  float centerBestAbs = 0.0f;
  float centerBestSigned = 0.0f;
  float centerBestPeak = 0.0f;
  int16_t centerBestOffset = 0;
  uint8_t centerBestSupport = 0;

  for (uint8_t k = CLUSTER_HALF_WIDTH;
       k + CLUSTER_HALF_WIDTH < pairs;
       ++k) {

    if (fabsf(residualBuf[k]) < STRONG_PAIR_RAD)
      continue;

    float sum = 0.0f;
    float peak = residualBuf[k];

    for (int8_t j = -(int8_t)CLUSTER_HALF_WIDTH;
         j <= (int8_t)CLUSTER_HALF_WIDTH;
         ++j) {
      const float r = residualBuf[(int)k + j];
      sum += r;
      if (fabsf(r) > fabsf(peak))
        peak = r;
    }

    uint8_t support = 0;
    const int8_t sgn = signOf(sum);
    for (int8_t j = -(int8_t)CLUSTER_HALF_WIDTH;
         j <= (int8_t)CLUSTER_HALF_WIDTH;
         ++j) {
      const float r = residualBuf[(int)k + j];
      if (sgn != 0 && signOf(r) == sgn && fabsf(r) >= 0.20f)
        ++support;
    }

    const int16_t eventOffset =
      (int16_t)((int32_t)relUs[k + 1] - (int32_t)PAY_BEFORE_US);
    const float a = fabsf(sum);

    // EXACTEMENT le choix historique V7: plus grosse somme de la fenetre.
    if (a > bestAbs) {
      bestAbs = a;
      bestSigned = sum;
      bestPeak = peak;
      bestOffset = eventOffset;
      bestSupport = support;
    }

    // Variante: meme mesure, seulement pres de la frontiere attendue.
    if (eventOffset >= PAY_EVENT_MIN_US &&
        eventOffset <= PAY_EVENT_MAX_US &&
        a > centerBestAbs) {
      centerBestAbs = a;
      centerBestSigned = sum;
      centerBestPeak = peak;
      centerBestOffset = eventOffset;
      centerBestSupport = support;
    }
  }

  out.score = bestAbs;
  out.signedSum = bestSigned;
  out.peak = bestPeak;
  out.eventOffsetUs = bestOffset;
  out.support = bestSupport;

  out.centerScore = centerBestAbs;
  out.centerSignedSum = centerBestSigned;
  out.centerPeak = centerBestPeak;
  out.centerOffsetUs = centerBestOffset;
  out.centerSupport = centerBestSupport;
  out.valid = true;
  return true;
}

// Decodeur de sequence tres leger. Il n'invente aucune nouvelle mesure:
// il reutilise uniquement score + signe produits par le detecteur V2/V7.
// Etat = signe attendu du prochain vrai kick. bit 0 conserve l'etat;
// bit 1 bascule l'etat. Le signe est une penalite souple, jamais un veto.
static uint32_t decodeKickSequence(
  const KickResult *kr,
  int8_t markerSign,
  float &pathCost,
  int8_t &chosenInitialSign
) {
  static constexpr float INF = 1.0e9f;

  float dp[33][2];
  uint8_t prevState[33][2];
  uint8_t prevBit[33][2];

  // state 0 => prochain kick attendu negatif; state 1 => positif.
  // Le TX alterne a chaque phaseFlip(). Apres le marqueur, le prochain
  // kick payload devrait donc avoir le signe oppose. On laisse toutefois
  // l'autre hypothese possible avec une petite penalite, afin qu'un signe
  // de marqueur imparfait ne puisse pas casser toute la trame.
  const int8_t preferred = (markerSign >= 0) ? -1 : 1;
  dp[0][0] = (preferred == -1) ? 0.0f : 0.60f;
  dp[0][1] = (preferred ==  1) ? 0.0f : 0.60f;

  for (uint8_t b = 0; b < 32; ++b) {
    dp[b + 1][0] = INF;
    dp[b + 1][1] = INF;

    const float score = kr[b].valid ? kr[b].centerScore : 0.0f;
    float e = (score - DBPSK_THRESHOLD_RAD) / SEQ_MAG_SCALE_RAD;
    e = clampf(e, -SEQ_MAX_EVIDENCE, SEQ_MAX_EVIDENCE);

    // Cout symetrique: score faible favorise 0, score fort favorise 1.
    const float cost0 = 0.5f * (SEQ_MAX_EVIDENCE + e);
    const float cost1Mag = 0.5f * (SEQ_MAX_EVIDENCE - e);

    for (uint8_t st = 0; st < 2; ++st) {
      const float here = dp[b][st];
      if (here >= INF * 0.5f)
        continue;

      // Hypothese bit 0: pas de kick, signe attendu inchange.
      const float c0 = here + cost0;
      if (c0 < dp[b + 1][st]) {
        dp[b + 1][st] = c0;
        prevState[b + 1][st] = st;
        prevBit[b + 1][st] = 0;
      }

      // Hypothese bit 1: vrai kick, puis alternance du signe.
      const int8_t expectedSign = st ? 1 : -1;
      const int8_t observedSign = signOf(kr[b].centerSignedSum);
      float signCost = 0.0f;

      if (observedSign == 0)
        signCost = SEQ_NO_SIGN_COST;
      else if (observedSign != expectedSign)
        signCost = SEQ_SIGN_MISMATCH_COST;

      const uint8_t next = st ^ 1u;
      const float c1 = here + cost1Mag + signCost;
      if (c1 < dp[b + 1][next]) {
        dp[b + 1][next] = c1;
        prevState[b + 1][next] = st;
        prevBit[b + 1][next] = 1;
      }
    }
  }

  uint8_t st = (dp[32][1] < dp[32][0]) ? 1u : 0u;
  pathCost = dp[32][st];

  uint8_t bits[32];
  for (int b = 32; b > 0; --b) {
    bits[b - 1] = prevBit[b][st];
    st = prevState[b][st];
  }

  chosenInitialSign = st ? 1 : -1;

  uint32_t word = 0;
  for (uint8_t b = 0; b < 32; ++b)
    word = (word << 1) | bits[b];

  return word;
}

// ------------------------------------------------------------
// Trame DBPSK
// ------------------------------------------------------------

static void decodeDbpskFrame(
  uint32_t guardStartUs,
  int32_t aLevel,
  int32_t bLevel
) {
  const uint32_t markerExpectedUs = guardStartUs + GUARD_US;

  uint16_t markerN = 0;
  const uint32_t markerProcStart = micros();

  if (!captureMarkerWindow(markerExpectedUs, markerN)) {
    Serial.println(F("MARKER CAPTURE FAIL"));
    Serial.flush();
    return;
  }

  KickResult marker;
  if (!analyzeMarker(markerN, marker)) {
    Serial.println(F("MARKER ANALYZE FAIL"));
    Serial.flush();
    return;
  }

  const uint32_t markerProcUs = micros() - markerProcStart;

  if (!marker.valid ||
      marker.eventUs == 0 ||
      marker.score < MARKER_THRESHOLD_RAD) {
    Serial.printf(
      "MARKER FAIL score=%ldmrad base=%ldHz N=%u PROC=%luus A=%ld B=%ld\r\n",
      (long)lroundf(marker.score * 1000.0f),
      (long)lroundf(marker.baselineHz),
      marker.sampleCount,
      (unsigned long)markerProcUs,
      (long)aLevel,
      (long)bLevel
    );
    Serial.flush();
    return;
  }

  const int32_t markerOffset =
    (int32_t)(marker.eventUs - markerExpectedUs);

  // V6: le front FSK B->A est maintenant date avec l'estimateur rapide.
  // Le marqueur doit donc tomber pres de son instant theorique. Si ce n'est
  // pas le cas, mieux vaut jeter la trame que decoder avec un glissement
  // entier de symboles. Quand il est coherent, son eventUs donne l'ancre
  // la plus precise pour les 32 frontieres DBPSK.
  if (markerOffset < MARKER_OFFSET_MIN_US ||
      markerOffset > MARKER_OFFSET_MAX_US) {
    Serial.printf(
      "MARKER OFFSET FAIL score=%ldmrad OFF=%ldus base=%ldHz A=%ld B=%ld\r\n",
      (long)lroundf(marker.score * 1000.0f),
      (long)markerOffset,
      (long)lroundf(marker.baselineHz),
      (long)aLevel,
      (long)bLevel
    );
    Serial.flush();
    return;
  }

  const uint32_t payloadStartUs =
    marker.eventUs + MARKER_TO_PAYLOAD_US;

  // Il faut etre pret avant le debut de la premiere fenetre.
  const int32_t slackUs =
    (int32_t)((payloadStartUs - PAY_BEFORE_US) - micros());

  if (slackUs <= 0) {
    Serial.printf(
      "PAYLOAD TOO LATE MARK=%ldmrad PROC=%luus SLACK=%ldus\r\n",
      (long)lroundf(marker.score * 1000.0f),
      (unsigned long)markerProcUs,
      (long)slackUs
    );
    Serial.flush();
    return;
  }

  // Phase temps reel: CAPTURE SEULEMENT.
  uint8_t captured = 0;
  int32_t maxCaptureLate = 0;

  for (uint8_t b = 0; b < 32; ++b) {
    const uint32_t boundaryUs =
      payloadStartUs + (uint32_t)b * SYMBOL_US;

    if (capturePayloadSymbol(b, boundaryUs))
      ++captured;

    const int32_t late =
      (int32_t)(micros() - (boundaryUs + PAY_AFTER_US));

    if (late > maxCaptureLate)
      maxCaptureLate = late;
  }

  // A partir d'ici la trame RF est deja capturee. RAW reste la reference V7.
  // CTR n'accepte qu'un kick dans +/-18 us du front. FINAL applique ensuite
  // l'alternance physique +/- uniquement a ces candidats temporellement valides.
  KickResult kr[32];
  uint8_t validBits = 0;
  uint8_t nMin = 255;
  uint8_t nMax = 0;

  uint32_t rawWord = 0;
  uint32_t ctrWord = 0;
  uint8_t rawErrors = 0;
  uint8_t ctrErrors = 0;
  float zeroMax = 0.0f;
  float oneMin = 1000.0f;

  for (uint8_t b = 0; b < 32; ++b) {
    const bool ok = analyzePayloadSymbol(b, kr[b]) && kr[b].valid;
    uint8_t rawBit = 0;
    uint8_t ctrBit = 0;

    if (ok) {
      ++validBits;
      if (payloadN[b] < nMin) nMin = payloadN[b];
      if (payloadN[b] > nMax) nMax = payloadN[b];

      rawBit = (kr[b].score >= DBPSK_THRESHOLD_RAD) ? 1u : 0u;
      ctrBit = (kr[b].centerScore >= DBPSK_THRESHOLD_RAD) ? 1u : 0u;

      const uint8_t expBit = (EXPECTED >> (31 - b)) & 1u;
      if (rawBit != expBit) ++rawErrors;
      if (ctrBit != expBit) ++ctrErrors;

      if (expBit == 0 && kr[b].score > zeroMax)
        zeroMax = kr[b].score;
      if (expBit == 1 && kr[b].score < oneMin)
        oneMin = kr[b].score;
    }
    else {
      kr[b].score = 0.0f;
      kr[b].signedSum = 0.0f;
      kr[b].peak = 0.0f;
      kr[b].eventOffsetUs = 0;
      kr[b].support = 0;
      kr[b].centerScore = 0.0f;
      kr[b].centerSignedSum = 0.0f;
      kr[b].centerPeak = 0.0f;
      kr[b].centerOffsetUs = 0;
      kr[b].centerSupport = 0;
      kr[b].valid = false;
      ++rawErrors;
      ++ctrErrors;
    }

    rawWord = (rawWord << 1) | rawBit;
    ctrWord = (ctrWord << 1) | ctrBit;
  }

  float seqCost = 0.0f;
  int8_t seqInitialSign = 0;
  const int8_t markerSign = signOf(marker.signedSum);
  const uint32_t seqWord =
    decodeKickSequence(kr, markerSign, seqCost, seqInitialSign);

  uint8_t seqErrors = 0;
  for (uint8_t b = 0; b < 32; ++b) {
    const uint8_t got = (seqWord >> (31 - b)) & 1u;
    const uint8_t exp = (EXPECTED >> (31 - b)) & 1u;
    if (got != exp)
      ++seqErrors;
  }

  Serial.printf(
    "DBPSK-KICK MARK=%ldmrad MSIGN=%c OFF=%ldus base=%ldHz "
    "PROC=%luus SLACK=%ldus A=%ld B=%ld "
    "RAW=%08lX RERR=%u CTR=%08lX CERR=%u FINAL=%08lX FERR=%u "
    "COST=%ld INIT=%c VALID=%u CAP=%u Zmax=%ld Omin=%ld "
    "N=[%u,%u] CAP_LATE=%ldus %s\r\n",
    (long)lroundf(marker.score * 1000.0f),
    (markerSign >= 0) ? '+' : '-',
    (long)markerOffset,
    (long)lroundf(marker.baselineHz),
    (unsigned long)markerProcUs,
    (long)slackUs,
    (long)aLevel,
    (long)bLevel,
    (unsigned long)rawWord,
    rawErrors,
    (unsigned long)ctrWord,
    ctrErrors,
    (unsigned long)seqWord,
    seqErrors,
    (long)lroundf(seqCost * 1000.0f),
    (seqInitialSign >= 0) ? '+' : '-',
    validBits,
    captured,
    (long)lroundf(zeroMax * 1000.0f),
    (long)lroundf(oneMin * 1000.0f),
    nMin,
    nMax,
    (long)maxCaptureLate,
    (seqErrors == 0 && validBits == 32 && captured == 32)
      ? "[RÉUSSITE]"
      : "[ÉCHEC]"
  );

  // Par bit:
  //   FULL/CENTER signe@offset/support
  // FULL est exactement la recherche V7. CENTER est la meme recherche mais
  // limitee pres de la frontiere; le signe/offset/support affiches sont CENTER.
  Serial.print(F("KICK:"));
  for (uint8_t b = 0; b < 32; ++b) {
    Serial.print(' ');
    if (!kr[b].valid) {
      Serial.print(F("FAIL"));
      continue;
    }

    Serial.print((long)lroundf(kr[b].score * 1000.0f));
    Serial.print('/');
    Serial.print((long)lroundf(kr[b].centerScore * 1000.0f));

    const int8_t sgn = signOf(kr[b].centerSignedSum);
    Serial.print((sgn > 0) ? '+' : ((sgn < 0) ? '-' : '.'));
    Serial.print('@');
    Serial.print((long)kr[b].centerOffsetUs);
    Serial.print('/');
    Serial.print((unsigned int)kr[b].centerSupport);
  }
  Serial.println();
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(250);

  Serial.println();
  Serial.println(F("BOOT RX_CALIBRATION_DBPSK_KICK_V15"));

  if (!initRx()) {
    Serial.println(F("ERR RX INIT"));
    Serial.flush();
    return;
  }

  Serial.printf(
    "READY DBPSK KICK V15 CH=%u N=%u MODE=%u SYM=%luus MARKGAP=%luus "
    "TH=%ldmrad MARKTH=%ldmrad MARKOFF=[%ld,%ld]us SEARCH=[%d,%d]us EXP=%08lX\r\n",
    RF_CHANNEL,
    IQ_FIELD,
    IQ_MODE_SEL,
    (unsigned long)SYMBOL_US,
    (unsigned long)MARKER_TO_PAYLOAD_US,
    (long)lroundf(DBPSK_THRESHOLD_RAD * 1000.0f),
    (long)lroundf(MARKER_THRESHOLD_RAD * 1000.0f),
    (long)MARKER_OFFSET_MIN_US,
    (long)MARKER_OFFSET_MAX_US,
    (int)PAY_EVENT_MIN_US,
    (int)PAY_EVENT_MAX_US,
    (unsigned long)EXPECTED
  );
  Serial.flush();
}

void loop() {
  uint32_t guardStartUs = 0;
  int32_t aLevel = 0;
  int32_t bLevel = 0;

  if (!waitForAutoFskSync(guardStartUs, aLevel, bLevel)) {
    Serial.println(F("SYNC FAIL"));
    delay(20);
    return;
  }

  decodeDbpskFrame(guardStartUs, aLevel, bLevel);
  yield();
}

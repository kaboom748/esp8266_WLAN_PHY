#include <Arduino.h>
extern "C" {
  #include <user_interface.h>

// ===== 1 kHz continuous subcarrier presence test =====
// Flash ROLE_TX=1 on the transmitter; change to 0 and flash the receiver.
#define ROLE_TX 0
// Fixed ASK level being measured. Change only this value on the TX.
#define ASK_TEST_LEVEL 0
static constexpr uint32_t SUBCARRIER_HZ = 1000u;
static constexpr uint32_t HALF_PERIOD_US = 1000000u / (2u * SUBCARRIER_HZ);

// RX detector: compares short-window E4 peak-to-floor ratio.
// It auto-learns the local floor; no packet/framing decoder is involved.
static constexpr uint32_t DETECT_REPORT_MS = 250u;
static constexpr uint8_t  DETECT_HITS_REQUIRED = 6u;
}
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ============================================================
// ESP8266 OOK v32.18 - 1500 BPS + IQ_N=192 + FROZEN DATA THRESHOLD + FAILBLOCK E4 DIAG + RESTORED TX + PREAMBLE QUALIFIER + fixed RX gain 0x7C03
//
// Goals:
//   * no session, no peer, no ACK, no CSMA
//   * APWR deliberately left at 255 as requested
//   * 1500 bit/s OOK raw line rate with exact fractional TX/RX scheduling
//   * 36-bit alternating training preamble + 12-bit start symbol 0xB38
//   * 4-to-6 bit balanced symbols
//   * length + arbitrary payload + CRC16-CCITT/FCS
//   * analog E4 receiver with RH_ASK-style fractional-tick 8x digital PLL
//   * diagnostics kept visible
//
// This is architecture-compatible in spirit with RH_ASK/VirtualWire, but the
// application frame intentionally omits RadioHead addressing headers.
// ============================================================

// ---------------- User tuning ----------------
static constexpr uint8_t  RF_CHANNEL       = 6;
static constexpr uint16_t TONE_K           = 8;
static constexpr uint8_t  TX_ASK = (uint8_t)ASK_TEST_LEVEL;
static constexpr uint8_t  TX_APWR          = 255; // deliberately NOT clamped

static constexpr uint16_t RX_GAIN_CODE     = 0x7C03u; // fixed: field logs show this is the good gain
static constexpr uint16_t IQ_N             = 192;
static constexpr uint32_t TX_SETTLE_US     = 400u;
static constexpr uint32_t RX_SETTLE_US     = 1000u;
static constexpr uint32_t POST_TX_HOLDOFF_US = 2500u;

// 1500 raw OOK bits/s. Keep the proven 8 samples/bit RH_ASK PLL, but
// schedule both TX and RX fractionally so there is no integer-us drift.
// TX bit period = 1e6/1500 = 666.666... us -> 666/667 us pattern.
// RX tick period = 1e6/(1500*8) = 83.333... us -> 83/84 us pattern.
static constexpr uint16_t BIT_RATE            = 1500u;
static constexpr uint8_t  RX_SAMPLES_PER_BIT  = 8u;

static constexpr uint32_t TX_TICK_DEN         = BIT_RATE;
static constexpr uint32_t TX_BIT_US_FLOOR     = 1000000UL / TX_TICK_DEN;
static constexpr uint32_t TX_BIT_US_REM       = 1000000UL % TX_TICK_DEN;

static constexpr uint32_t RX_TICK_DEN         = (uint32_t)BIT_RATE * RX_SAMPLES_PER_BIT;
static constexpr uint32_t RX_TICK_US_FLOOR    = 1000000UL / RX_TICK_DEN;
static constexpr uint32_t RX_TICK_US_REM      = 1000000UL % RX_TICK_DEN;
static constexpr uint32_t RX_TICK_US_CEIL     = RX_TICK_US_FLOOR + (RX_TICK_US_REM ? 1u : 0u);

// Match the classic RH_ASK / VirtualWire digital PLL ramp.
static constexpr uint16_t PLL_RAMP_LEN      = 160u;
static constexpr uint16_t PLL_RAMP_TRANSITION = PLL_RAMP_LEN / 2u;
static constexpr uint16_t PLL_RAMP_INC      = PLL_RAMP_LEN / RX_SAMPLES_PER_BIT; // 20
static constexpr uint16_t PLL_RAMP_ADJUST   = 9u;
static constexpr uint16_t PLL_RAMP_RETARD   = PLL_RAMP_INC - PLL_RAMP_ADJUST;   // 11
static constexpr uint16_t PLL_RAMP_ADVANCE  = PLL_RAMP_INC + PLL_RAMP_ADJUST;   // 29

static constexpr uint32_t RX_ACTIVE_TIMEOUT_US = 750000u;

// Test traffic. It is intentionally sparse enough to validate the PHY before
// trying to saturate the channel. Both modules use a randomized interval.
static constexpr uint8_t  TEST_PAYLOAD_LEN  = 8u;
static constexpr uint8_t  MAX_PAYLOAD_LEN   = 32u;
static constexpr uint32_t SEND_MIN_MS       = 900u;
static constexpr uint32_t SEND_MAX_MS       = 1800u;
static constexpr uint32_t IDLE_YIELD_US     = 250000u;

// Search qualifier: require a real alternating training run before accepting START.
// The wire preamble has 36 alternating bits = 35 transitions, so 20 leaves
// substantial room for PLL acquisition while making random START hits unlikely.
static constexpr uint8_t PREAMBLE_MIN_TRANSITIONS = 20u;
static constexpr uint8_t PREAMBLE_START_WINDOW_BITS = 24u;

// ============================================================
// 4b/6b framing, derived from the VirtualWire/RH_ASK wire format
// ============================================================
namespace AskProto {
  static constexpr uint16_t START_SYMBOL = 0x0B38u;
  static constexpr uint8_t PREAMBLE_SYMBOLS = 8u;
  static constexpr uint8_t PREAMBLE[PREAMBLE_SYMBOLS] = {
    0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x38, 0x2c
  };
  static constexpr uint8_t SYMBOLS[16] = {
    0x0d, 0x0e, 0x13, 0x15, 0x16, 0x19, 0x1a, 0x1c,
    0x23, 0x25, 0x26, 0x29, 0x2a, 0x2c, 0x32, 0x34
  };

  // Count includes itself and the two FCS bytes.
  static constexpr uint8_t MAX_COUNT = (uint8_t)(MAX_PAYLOAD_LEN + 3u);
  static constexpr uint8_t MAX_ENCODED_SYMBOLS =
      (uint8_t)(PREAMBLE_SYMBOLS + 2u * MAX_COUNT);

  inline uint16_t crcCcittUpdate(uint16_t crc, uint8_t data) {
    crc ^= data;
    for (uint8_t i = 0; i < 8; ++i)
      crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u)
                       : (uint16_t)(crc >> 1);
    return crc;
  }

  inline bool decode6(uint8_t symbol, uint8_t &nibble) {
    symbol &= 0x3fu;
    for (uint8_t i = 0; i < 16; ++i) {
      if (SYMBOLS[i] == symbol) { nibble = i; return true; }
    }
    return false;
  }

  inline uint8_t encodeNibble(uint8_t n) { return SYMBOLS[n & 0x0fu]; }
}

// ============================================================
// Analog E4 decision + RH_ASK-style 8x digital PLL decoder
// ============================================================
class AskDecoder {
public:
  struct Diagnostics {
    uint32_t samples = 0;
    uint32_t transitions = 0;
    uint32_t bits = 0;
    uint32_t rawStartHits = 0;
    uint32_t startHits = 0;
    uint32_t preambleArms = 0;
    uint32_t preambleRejects = 0;
    uint32_t preambleRejectNotArmed = 0;
    uint32_t preambleRejectExpired = 0;
    uint32_t preambleExpired = 0;
    uint8_t preambleAltMax = 0;
    // v32.11 diagnostic only: transition density in the exact 36 decisions
    // immediately preceding each detected 12-bit START symbol. Maximum = 35.
    uint32_t pre36AcceptCount = 0;
    uint32_t pre36AcceptSum = 0;
    uint8_t pre36AcceptMin = 0xffu;
    uint8_t pre36AcceptMax = 0u;
    uint32_t pre36RejectNACount = 0;
    uint32_t pre36RejectNASum = 0;
    uint8_t pre36RejectNAMin = 0xffu;
    uint8_t pre36RejectNAMax = 0u;
    uint32_t pre36RejectEXPCount = 0;
    uint32_t pre36RejectEXPSum = 0;
    uint8_t pre36RejectEXPMin = 0xffu;
    uint8_t pre36RejectEXPMax = 0u;
    uint32_t symbolErrors = 0;
    uint32_t analogMlAttempts = 0;
    uint32_t analogMlRescuedBlocks = 0;
    uint32_t analogMlPacketsOk = 0;
    uint32_t lengthErrors = 0;
    uint32_t crcErrors = 0;
    uint32_t timeouts = 0;
    uint32_t packetsOk = 0;
    uint8_t lastBadSymHi = 0;
    uint8_t lastBadSymLo = 0;
    uint8_t lastBadLen = 0;
    uint32_t pllRetard = 0;
    uint32_t pllAdvance = 0;
    uint32_t weakSamples = 0;
    uint32_t envLow = 0;
    uint32_t envHigh = 0;
    uint32_t threshold = 0;
    uint32_t contrast = 0;
    uint32_t contrastMinActive = 0xffffffffu;
    uint32_t contrastMax = 0;
    uint16_t lastCount = 0;
    uint16_t lastPayloadLen = 0;
    uint8_t bitSamplesMin = 0xffu;
    uint8_t bitSamplesMax = 0u;
  };

  enum FailureReason : uint8_t {
    FAIL_NONE = 0,
    FAIL_SYMBOL = 1,
    FAIL_LENGTH = 2,
    FAIL_CRC = 3,
    FAIL_TIMEOUT = 4
  };

  struct FailureSnapshot {
    bool valid = false;
    uint8_t reason = FAIL_NONE;
    uint8_t blockIndex = 0;       // 0=count, 1=payload[0], ...
    uint8_t count = 0;            // number of captured bits in failing/current block
    bool bits[12]{};
    uint8_t highVotes[12]{};
    uint8_t totalVotes[12]{};
    uint8_t phaseResidual[12]{};
    uint16_t sampleGapUs[12]{};   // largest RX sample gap observed inside each decided bit
    uint32_t e4Avg[12]{};         // raw E4 average across PLL samples for each decided bit
    uint32_t e4Min[12]{};         // raw E4 minimum inside each decided bit
    uint32_t e4Max[12]{};         // raw E4 maximum inside each decided bit
    uint32_t frozenThreshold = 0; // base slicer threshold held for DATA+CRC
    uint32_t slicerHysteresis = 0;// +/- hysteresis used around frozenThreshold
    uint16_t blockGapMaxUs = 0;
    uint8_t badSymHi = 0;
    uint8_t badSymLo = 0;
    uint8_t badLen = 0;
  };

  AskDecoder() { resetAll(); }

  void resetAll() {
    diag_ = Diagnostics{};
    failurePending_ = FailureSnapshot{};
    resetSearch();
    envValid_ = false;
  }

  void resetSearch() {
    active_ = false;
    bitCount_ = 0;
    rxBits_ = 0;
    resetPreambleQualifier();
    resetPre36History();
    rxBufLen_ = 0;
    rxCount_ = 0;
    analogMlUsedInPacket_ = false;
    packetReady_ = false;
    pllRamp_ = 0;
    highSamples_ = 0;
    totalSamples_ = 0;
    prevSampleUs_ = 0;
    currentBitGapMaxUs_ = 0u;
    resetCurrentBitE4();
    activeStartUs_ = 0;
    resetBlockTrace();
  }

  void resetTimingOnly() {
    active_ = false;
    bitCount_ = 0;
    rxBits_ = 0;
    resetPreambleQualifier();
    resetPre36History();
    rxBufLen_ = 0;
    rxCount_ = 0;
    analogMlUsedInPacket_ = false;
    packetReady_ = false;
    pllRamp_ = 0;
    highSamples_ = 0;
    totalSamples_ = 0;
    prevSampleUs_ = 0;
    currentBitGapMaxUs_ = 0u;
    resetCurrentBitE4();
    activeStartUs_ = 0;
    resetBlockTrace();
    // Keep envelopes: they contain useful information about the current gain.
  }

  bool searching() const { return !active_; }
  bool active() const { return active_; }
  const Diagnostics& diagnostics() const { return diag_; }

  void resetEnvelope() {
    envValid_ = false;
    envLow_ = envHigh_ = 0u;
    diag_.envLow = diag_.envHigh = diag_.threshold = diag_.contrast = 0u;
  }

  bool takePacket(uint8_t *out, uint8_t &len) {
    if (!packetReady_) return false;
    len = readyLen_;
    if (out && len) memcpy(out, readyPayload_, len);
    packetReady_ = false;
    return true;
  }

  bool takeFailureSnapshot(FailureSnapshot &out) {
    if (!failurePending_.valid) return false;
    out = failurePending_;
    failurePending_.valid = false;
    return true;
  }

  void addSample(uint32_t tUs, uint32_t e) {
    ++diag_.samples;

    // v32.18: keep the proven v32.11 slicer behavior; add E4-only failure diagnostics.
    // Once START is accepted, freeze envLow/envHigh/threshold/contrast for the
    // complete DATA+CRC frame so packet energy cannot drag the slicer threshold.
    if (!active_) updateEnvelope(e);

    sampleHigh_ = classify(e, prevSampleUs_ != 0u);

    if (!prevSampleUs_) {
      prevSampleUs_ = tUs;
      lastHigh_ = sampleHigh_;
      highSamples_ = sampleHigh_ ? 1u : 0u;
      totalSamples_ = 1u;
      currentBitGapMaxUs_ = 0u;
      currentBitE4Sum_ = e;
      currentBitE4Min_ = e;
      currentBitE4Max_ = e;
      return;
    }
    const uint32_t sampleGap = (uint32_t)(tUs - prevSampleUs_);
    if (sampleGap > currentBitGapMaxUs_)
      currentBitGapMaxUs_ = (sampleGap > 0xffffu) ? 0xffffu : (uint16_t)sampleGap;
    prevSampleUs_ = tUs;

    // Integrate one fixed-cadence decision sample.
    if (sampleHigh_) ++highSamples_;
    if (totalSamples_ < 255u) ++totalSamples_;
    currentBitE4Sum_ += e;
    if (e < currentBitE4Min_) currentBitE4Min_ = e;
    if (e > currentBitE4Max_) currentBitE4Max_ = e;

    const bool transition = (sampleHigh_ != lastHigh_);
    if (transition) {
      ++diag_.transitions;

      // This is the same ramp rule used by RH_ASK:
      // transition before the midpoint -> retard (11 instead of 20)
      // transition after  the midpoint -> advance (29 instead of 20)
      if (pllRamp_ < PLL_RAMP_TRANSITION) {
        pllRamp_ += PLL_RAMP_RETARD;
        ++diag_.pllRetard;
      } else {
        pllRamp_ += PLL_RAMP_ADVANCE;
        ++diag_.pllAdvance;
      }
      lastHigh_ = sampleHigh_;
    } else {
      pllRamp_ += PLL_RAMP_INC;
    }

    if (diag_.contrast < 120u) ++diag_.weakSamples;
    if (active_) {
      if (diag_.contrast < diag_.contrastMinActive)
        diag_.contrastMinActive = diag_.contrast;
      if ((uint32_t)(tUs - activeStartUs_) > RX_ACTIVE_TIMEOUT_US) {
        ++diag_.timeouts;
        snapshotFailure(FAIL_TIMEOUT);
        dropPacket();
      }
    }

    if (pllRamp_ >= PLL_RAMP_LEN) {
      pllRamp_ -= PLL_RAMP_LEN;

      if (totalSamples_ < diag_.bitSamplesMin) diag_.bitSamplesMin = totalSamples_;
      if (totalSamples_ > diag_.bitSamplesMax) diag_.bitSamplesMax = totalSamples_;

      // RH_ASK-compatible integrate-and-dump decision.
      // IMPORTANT: the threshold is a fixed 5 HIGH samples, not a majority
      // of however many samples the PLL happened to place in this bit.
      // The PLL deliberately varies the cycle length (typically ~6..11 ticks)
      // while acquiring phase; RH_ASK keeps the slicer threshold fixed at 5.
      const bool bit = (highSamples_ >= 5u);
      const uint32_t bitE4Avg = totalSamples_
                                  ? (uint32_t)(currentBitE4Sum_ / (uint64_t)totalSamples_)
                                  : 0u;
      consumeBit(bit, tUs, highSamples_, totalSamples_, (uint8_t)pllRamp_, currentBitGapMaxUs_,
                 bitE4Avg, currentBitE4Min_, currentBitE4Max_);

      highSamples_ = 0;
      totalSamples_ = 0;
      currentBitGapMaxUs_ = 0u;
      resetCurrentBitE4();
    }
  }

private:
  bool active_ = false;
  bool packetReady_ = false;
  bool envValid_ = false;
  bool sampleHigh_ = false;
  bool lastHigh_ = false;
  uint32_t envLow_ = 0, envHigh_ = 0;
  uint32_t prevSampleUs_ = 0;
  uint16_t pllRamp_ = 0;
  uint8_t highSamples_ = 0;
  uint8_t totalSamples_ = 0;
  uint16_t rxBits_ = 0;
  uint8_t bitCount_ = 0;
  bool preambleLastBitValid_ = false;
  bool preambleLastBit_ = false;
  bool preambleArmed_ = false;
  bool preambleWindowExpired_ = false;
  uint8_t preambleAltRun_ = 0;
  uint8_t preambleAge_ = 0;
  // Rolling 48 decided bits while searching. When START is detected, these are
  // exactly 36 preceding training decisions + the 12 START decisions.
  bool pre36Hist_[48]{};
  uint8_t pre36HistCount_ = 0u;
  uint8_t pre36HistPos_ = 0u;
  uint8_t rxBuf_[AskProto::MAX_COUNT]{};
  uint8_t rxBufLen_ = 0;
  uint8_t rxCount_ = 0;
  bool analogMlUsedInPacket_ = false;
  uint32_t activeStartUs_ = 0;
  uint8_t readyPayload_[MAX_PAYLOAD_LEN]{};
  uint8_t readyLen_ = 0;
  uint8_t blockTraceCount_ = 0;
  bool blockTraceBits_[12]{};
  uint8_t blockTraceHigh_[12]{};
  uint8_t blockTraceTotal_[12]{};
  uint8_t blockTracePhase_[12]{};
  uint16_t blockTraceGapUs_[12]{};
  uint32_t blockTraceE4Avg_[12]{};
  uint32_t blockTraceE4Min_[12]{};
  uint32_t blockTraceE4Max_[12]{};
  uint16_t currentBitGapMaxUs_ = 0;
  uint64_t currentBitE4Sum_ = 0u;
  uint32_t currentBitE4Min_ = 0xffffffffu;
  uint32_t currentBitE4Max_ = 0u;
  FailureSnapshot failurePending_{};
  Diagnostics diag_{};

  void resetCurrentBitE4() {
    currentBitE4Sum_ = 0u;
    currentBitE4Min_ = 0xffffffffu;
    currentBitE4Max_ = 0u;
  }

  void resetBlockTrace() {
    blockTraceCount_ = 0u;
    memset(blockTraceBits_, 0, sizeof(blockTraceBits_));
    memset(blockTraceHigh_, 0, sizeof(blockTraceHigh_));
    memset(blockTraceTotal_, 0, sizeof(blockTraceTotal_));
    memset(blockTracePhase_, 0, sizeof(blockTracePhase_));
    memset(blockTraceGapUs_, 0, sizeof(blockTraceGapUs_));
    memset(blockTraceE4Avg_, 0, sizeof(blockTraceE4Avg_));
    memset(blockTraceE4Min_, 0, sizeof(blockTraceE4Min_));
    memset(blockTraceE4Max_, 0, sizeof(blockTraceE4Max_));
  }

  void captureBlockBit(bool bit, uint8_t highVotes, uint8_t totalVotes,
                       uint8_t phaseResidual, uint16_t sampleGapUs,
                       uint32_t e4Avg, uint32_t e4Min, uint32_t e4Max) {
    if (blockTraceCount_ >= 12u) return;
    const uint8_t i = blockTraceCount_++;
    blockTraceBits_[i] = bit;
    blockTraceHigh_[i] = highVotes;
    blockTraceTotal_[i] = totalVotes;
    blockTracePhase_[i] = phaseResidual;
    blockTraceGapUs_[i] = sampleGapUs;
    blockTraceE4Avg_[i] = e4Avg;
    blockTraceE4Min_[i] = e4Min;
    blockTraceE4Max_[i] = e4Max;
  }

  void snapshotFailure(uint8_t reason) {
    // Keep the most recent failure until the application prints it.
    // rxBufLen_ is exactly the encoded-byte/block index currently being received.
    failurePending_ = FailureSnapshot{};
    failurePending_.valid = true;
    failurePending_.reason = reason;
    failurePending_.blockIndex = (reason == FAIL_CRC && rxBufLen_ > 0u)
                                   ? (uint8_t)(rxBufLen_ - 1u) : rxBufLen_;
    failurePending_.count = blockTraceCount_;
    for (uint8_t i=0; i<blockTraceCount_ && i<12u; ++i) {
      failurePending_.bits[i] = blockTraceBits_[i];
      failurePending_.highVotes[i] = blockTraceHigh_[i];
      failurePending_.totalVotes[i] = blockTraceTotal_[i];
      failurePending_.phaseResidual[i] = blockTracePhase_[i];
      failurePending_.sampleGapUs[i] = blockTraceGapUs_[i];
      failurePending_.e4Avg[i] = blockTraceE4Avg_[i];
      failurePending_.e4Min[i] = blockTraceE4Min_[i];
      failurePending_.e4Max[i] = blockTraceE4Max_[i];
      if (blockTraceGapUs_[i] > failurePending_.blockGapMaxUs)
        failurePending_.blockGapMaxUs = blockTraceGapUs_[i];
    }
    failurePending_.frozenThreshold = diag_.threshold;
    failurePending_.slicerHysteresis = diag_.contrast / 48u;
    if (failurePending_.slicerHysteresis < 20u) failurePending_.slicerHysteresis = 20u;
    // Snapshot the actual rolling 12-bit word for THIS block, not stale diagnostics.
    failurePending_.badSymHi = (uint8_t)(rxBits_ & 0x3fu);
    failurePending_.badSymLo = (uint8_t)((rxBits_ >> 6) & 0x3fu);
    failurePending_.badLen = diag_.lastBadLen;
  }

  void updateEnvelope(uint32_t e) {
    if (!envValid_) {
      envLow_ = envHigh_ = e;
      envValid_ = true;
    } else {
      // Fast attack toward a new extreme, slow release toward the centre.
      if (e < envLow_) envLow_ = (uint32_t)(((uint64_t)envLow_ * 3u + e) / 4u);
      else             envLow_ = (uint32_t)(((uint64_t)envLow_ * 127u + e) / 128u);
      if (e > envHigh_) envHigh_ = (uint32_t)(((uint64_t)envHigh_ * 3u + e) / 4u);
      else              envHigh_ = (uint32_t)(((uint64_t)envHigh_ * 127u + e) / 128u);
      if (envHigh_ < envLow_) { uint32_t x=envHigh_; envHigh_=envLow_; envLow_=x; }
    }
    diag_.envLow = envLow_;
    diag_.envHigh = envHigh_;
    diag_.contrast = envHigh_ - envLow_;
    diag_.threshold = envLow_ + diag_.contrast / 2u;
    if (diag_.contrast > diag_.contrastMax) diag_.contrastMax = diag_.contrast;
  }

  uint32_t robustBlockE4(uint8_t idx) const {
    // v32.18: robust per-bit E4 for analog rescue. The ordinary RX slicer is
    // unchanged. For ML only, remove the single minimum and maximum sample
    // from each decided bit when at least 4 samples were collected. This
    // suppresses the large one-sample E4 impulses observed in FAILBLOCK logs.
    const uint8_t n = blockTraceTotal_[idx];
    const uint32_t avg = blockTraceE4Avg_[idx];
    if (n < 4u) return avg;
    const uint64_t approxSum = (uint64_t)avg * (uint64_t)n;
    const uint64_t extremes = (uint64_t)blockTraceE4Min_[idx] +
                              (uint64_t)blockTraceE4Max_[idx];
    if (approxSum <= extremes) return avg;
    return (uint32_t)((approxSum - extremes) / (uint64_t)(n - 2u));
  }

  bool decode6AnalogMl(uint8_t offset, uint8_t &nibbleOut, uint64_t &marginOut) const {
    // All legal 4b/6b codewords contain exactly three 1s and three 0s.
    // Score each legal codeword from robust per-bit analog E4. Common-mode
    // E4 offset cancels because every candidate is balanced.
    int64_t bestScore = -(1LL << 60);
    int64_t secondScore = -(1LL << 60);
    uint8_t bestNibble = 0u;
    for (uint8_t n=0u; n<16u; ++n) {
      const uint8_t sym = AskProto::SYMBOLS[n];
      int64_t score = 0;
      for (uint8_t i=0u; i<6u; ++i) {
        const int64_t v = (int64_t)robustBlockE4((uint8_t)(offset + i));
        score += (sym & (1u << i)) ? v : -v;
      }
      if (score > bestScore) {
        secondScore = bestScore;
        bestScore = score;
        bestNibble = n;
      } else if (score > secondScore) {
        secondScore = score;
      }
    }
    if (secondScore <= -(1LL << 59)) return false;
    nibbleOut = bestNibble;
    marginOut = (uint64_t)(bestScore - secondScore);
    return true;
  }

  bool classify(uint32_t e, bool useHysteresis) const {
    const uint32_t c = diag_.contrast;
    // SEARCH/PREAMBLE keeps the proven c/12 hysteresis.
    // During DATA+CRC the threshold is frozen, but use a much narrower
    // hysteresis (c/48) so a preamble-derived contrast cannot make
    // legitimate weakened HIGH/LOW levels "stick" on the wrong side.
    uint32_t h = c / (active_ ? 48u : 12u);
    if (h < 20u) h = 20u;
    if (!useHysteresis) return e >= diag_.threshold;
    if (lastHigh_) {
      const uint32_t lo = (diag_.threshold > h) ? diag_.threshold - h : 0u;
      return e >= lo;
    }
    // v32.18: SEARCH/PREAMBLE keeps the proven symmetric +h rising threshold.
    // During DATA+CRC only, allow LOW->HIGH at the frozen centre threshold T.
    // This targets the measured HIGH droop without moving the frozen threshold
    // or weakening SEARCH/PREAMBLE qualification.
    return e > (active_ ? diag_.threshold : (diag_.threshold + h));
  }

  void resetPre36History() {
    memset(pre36Hist_, 0, sizeof(pre36Hist_));
    pre36HistCount_ = 0u;
    pre36HistPos_ = 0u;
  }

  void pushPre36History(bool bit) {
    pre36Hist_[pre36HistPos_] = bit;
    pre36HistPos_ = (uint8_t)((pre36HistPos_ + 1u) % 48u);
    if (pre36HistCount_ < 48u) ++pre36HistCount_;
  }

  bool pre36TransitionCount(uint8_t &transitionsOut) const {
    // At raw START, a full history is [36 bits immediately before START][12 START bits].
    if (pre36HistCount_ < 48u) return false;
    const uint8_t oldest = pre36HistPos_; // next write position == oldest when full
    bool prev = pre36Hist_[oldest];
    uint8_t transitions = 0u;
    for (uint8_t i=1u; i<36u; ++i) {
      const bool b = pre36Hist_[(uint8_t)((oldest + i) % 48u)];
      if (b != prev) ++transitions;
      prev = b;
    }
    transitionsOut = transitions;
    return true;
  }

  static void addPre36Stat(uint8_t transitions, uint32_t &count, uint32_t &sum,
                           uint8_t &minValue, uint8_t &maxValue) {
    ++count;
    sum += transitions;
    if (transitions < minValue) minValue = transitions;
    if (transitions > maxValue) maxValue = transitions;
  }

  void resetPreambleQualifier() {
    preambleLastBitValid_ = false;
    preambleLastBit_ = false;
    preambleArmed_ = false;
    preambleWindowExpired_ = false;
    preambleAltRun_ = 0u;
    preambleAge_ = 0u;
  }

  void updatePreambleQualifier(bool bit) {
    if (!preambleLastBitValid_) {
      preambleLastBitValid_ = true;
      preambleLastBit_ = bit;
      preambleAltRun_ = 0u;
      return;
    }

    const bool transition = (bit != preambleLastBit_);
    preambleLastBit_ = bit;

    if (transition) {
      if (preambleAltRun_ < 255u) ++preambleAltRun_;
      if (preambleAltRun_ > diag_.preambleAltMax)
        diag_.preambleAltMax = preambleAltRun_;

      if (preambleAltRun_ >= PREAMBLE_MIN_TRANSITIONS) {
        if (!preambleArmed_) ++diag_.preambleArms;
        preambleArmed_ = true;
        preambleWindowExpired_ = false;
        preambleAge_ = 0u;
      } else if (preambleArmed_ && preambleAge_ < 255u) {
        ++preambleAge_;
      }
    } else {
      preambleAltRun_ = 0u;
      if (preambleArmed_ && preambleAge_ < 255u) ++preambleAge_;
    }

    if (preambleArmed_ && preambleAge_ > PREAMBLE_START_WINDOW_BITS) {
      preambleArmed_ = false;
      preambleWindowExpired_ = true;
      preambleAge_ = 0u;
      ++diag_.preambleExpired;
    }
  }

  void dropPacket() {
    active_ = false;
    bitCount_ = 0;
    rxBufLen_ = 0;
    rxCount_ = 0;
    analogMlUsedInPacket_ = false;
    activeStartUs_ = 0;
    resetPreambleQualifier();
    resetPre36History();
  }

  void consumeBit(bool bit, uint32_t nowUs, uint8_t highVotes, uint8_t totalVotes,
                  uint8_t phaseResidual, uint16_t sampleGapUs,
                  uint32_t e4Avg, uint32_t e4Min, uint32_t e4Max) {
    ++diag_.bits;
    rxBits_ >>= 1;
    if (bit) rxBits_ |= 0x0800u;

    if (!active_) {
      pushPre36History(bit);
      updatePreambleQualifier(bit);

      if (rxBits_ == AskProto::START_SYMBOL) {
        ++diag_.rawStartHits;
        uint8_t pre36Transitions = 0u;
        const bool pre36Valid = pre36TransitionCount(pre36Transitions);
        if (preambleArmed_) {
          if (pre36Valid)
            addPre36Stat(pre36Transitions, diag_.pre36AcceptCount, diag_.pre36AcceptSum,
                         diag_.pre36AcceptMin, diag_.pre36AcceptMax);
          ++diag_.startHits;
          active_ = true;
          bitCount_ = 0;
          rxBufLen_ = 0;
          rxCount_ = 0;
          analogMlUsedInPacket_ = false;
          activeStartUs_ = nowUs;
          resetBlockTrace();
          diag_.contrastMinActive = 0xffffffffu;
          // The qualifier has served its purpose; DATA must not re-arm it.
          preambleArmed_ = false;
          preambleWindowExpired_ = false;
          preambleAltRun_ = 0u;
          preambleAge_ = 0u;
        } else {
          ++diag_.preambleRejects;
          if (preambleWindowExpired_) {
            ++diag_.preambleRejectExpired;
            if (pre36Valid)
              addPre36Stat(pre36Transitions, diag_.pre36RejectEXPCount, diag_.pre36RejectEXPSum,
                           diag_.pre36RejectEXPMin, diag_.pre36RejectEXPMax);
          } else {
            ++diag_.preambleRejectNotArmed;
            if (pre36Valid)
              addPre36Stat(pre36Transitions, diag_.pre36RejectNACount, diag_.pre36RejectNASum,
                           diag_.pre36RejectNAMin, diag_.pre36RejectNAMax);
          }
        }
      }
      return;
    }

    captureBlockBit(bit, highVotes, totalVotes, phaseResidual, sampleGapUs, e4Avg, e4Min, e4Max);

    if (++bitCount_ < 12u) return;
    bitCount_ = 0;

    uint8_t hiNib=0, loNib=0;
    const uint8_t symHi = (uint8_t)(rxBits_ & 0x3fu);
    const uint8_t symLo = (uint8_t)((rxBits_ >> 6) & 0x3fu);
    bool hiOk = AskProto::decode6(symHi, hiNib);
    bool loOk = AskProto::decode6(symLo, loNib);

    // v32.18: preserve every already-valid binary symbol. Only an invalid
    // 6-bit symbol may be rescued from the analog E4 bit averages.
    if ((!hiOk || !loOk) && blockTraceCount_ == 12u) {
      ++diag_.analogMlAttempts;
      uint8_t mlHi = hiNib, mlLo = loNib;
      uint64_t marginHi = 0u, marginLo = 0u;
      bool mlHiOk = hiOk;
      bool mlLoOk = loOk;
      if (!hiOk) mlHiOk = decode6AnalogMl(0u, mlHi, marginHi);
      if (!loOk) mlLoOk = decode6AnalogMl(6u, mlLo, marginLo);

      // v32.18 confidence gate fitted against the recorded count-byte failures.
      // With min/max-trimmed E4, contrast/12 recovered 9/10 known count-byte
      // failures while the one wrong/ambiguous ML candidate remained rejected.
      const uint64_t minMargin = (uint64_t)diag_.contrast / 12u;
      const bool hiConf = hiOk || (mlHiOk && marginHi >= minMargin);
      const bool loConf = loOk || (mlLoOk && marginLo >= minMargin);
      if (hiConf && loConf) {
        if (!hiOk) hiNib = mlHi;
        if (!loOk) loNib = mlLo;
        hiOk = loOk = true;
        analogMlUsedInPacket_ = true;
        ++diag_.analogMlRescuedBlocks;
      }
    }

    if (!hiOk || !loOk) {
      diag_.lastBadSymHi = symHi;
      diag_.lastBadSymLo = symLo;
      ++diag_.symbolErrors;
      snapshotFailure(FAIL_SYMBOL);
      dropPacket();
      return;
    }
    const uint8_t b = (uint8_t)((hiNib << 4) | loNib);

    if (rxBufLen_ == 0u) {
      rxCount_ = b;
      if (rxCount_ < 3u || rxCount_ > AskProto::MAX_COUNT) {
        diag_.lastBadLen = rxCount_;
        ++diag_.lengthErrors;
        snapshotFailure(FAIL_LENGTH);
        dropPacket();
        return;
      }
      diag_.lastCount = rxCount_;
    }

    if (rxBufLen_ >= AskProto::MAX_COUNT) {
      ++diag_.lengthErrors;
      snapshotFailure(FAIL_LENGTH);
      dropPacket();
      return;
    }
    rxBuf_[rxBufLen_++] = b;

    if (rxBufLen_ < rxCount_) {
      // Move microscope to the next encoded byte only after this block decoded cleanly.
      resetBlockTrace();
      return;
    }

    uint16_t crc = 0xffffu;
    for (uint8_t i=0; i<rxCount_; ++i)
      crc = AskProto::crcCcittUpdate(crc, rxBuf_[i]);

    if (crc != 0xf0b8u) {
      ++diag_.crcErrors;
      snapshotFailure(FAIL_CRC);
      dropPacket();
      return;
    }

    const uint8_t payloadLen = (uint8_t)(rxCount_ - 3u);
    if (payloadLen) memcpy(readyPayload_, &rxBuf_[1], payloadLen);
    readyLen_ = payloadLen;
    packetReady_ = true;
    ++diag_.packetsOk;
    if (analogMlUsedInPacket_) ++diag_.analogMlPacketsOk;
    diag_.lastPayloadLen = payloadLen;
    dropPacket();
  }
};

// Forward declarations used below.
static inline void feedSoftWdtNow();
static inline void serviceSoftWdt();
static bool transmitPacket(const uint8_t *data, uint8_t len);
static void serviceRx();
static void serviceApplication();
static void printDiagnostics();
void setup();
void loop();

// ============================================================
// Low-level PHY
// ============================================================
namespace Phy {
  static constexpr uint32_t TONE1       = 0x600005B8u;
  static constexpr uint32_t TONE2       = 0x600005BCu;
  static constexpr uint32_t TONE3       = 0x600005C4u;
  static constexpr uint32_t GATE_MASK   = 0x00040000u;
  static constexpr uint32_t K_MASK      = 0x000003FFu;
  static constexpr uint32_t SCALE_MASK  = 0x0003FC00u;
  static constexpr uint32_t SCALE_SHIFT = 10u;

  static constexpr uint32_t PBUS_CMD    = 0x60000594u;
  static constexpr uint32_t PBUS_STATUS = 0x600005A0u;
  static constexpr uint32_t RX_CTRL     = 0x60009B08u;
  static constexpr uint32_t RX_STOP     = 0x08000000u;
  static constexpr uint32_t IQ_POWER_E4 = 0x600005E4u;

  static constexpr uint32_t ROM_IQ_EST_DISABLE   = 0x40006400u;
  static constexpr uint32_t ROM_IQ_EST_ENABLE    = 0x40006430u;
  static constexpr uint32_t ROM_SET_TXCLK_EN     = 0x4000650Cu;
  static constexpr uint32_t ROM_SET_RXCLK_EN     = 0x40006550u;
  static constexpr uint32_t ROM_SET_ANA_TX_SCALE = 0x4000678Cu;
  static constexpr uint32_t ROM_PBUS_RD          = 0x400074D8u;
  static constexpr uint32_t ROM_PBUS_SET_RXGAIN  = 0x4000754Cu;
  static constexpr uint32_t ROM_PBUS_XPD_RX_OFF  = 0x40007688u;
  static constexpr uint32_t ROM_PBUS_XPD_RX_ON   = 0x400076CCu;

  using IqEnableFn    = void    (*)(uint32_t, uint32_t);
  using IqDisableFn   = void    (*)(void);
  using SetClkFn      = void    (*)(int);
  using SetAnaScaleFn = uint8_t (*)(uint8_t);
  using PbusReadFn    = uint32_t(*)(uint32_t, uint32_t);
  using SetRxGainFn   = void    (*)(uint32_t);
  using RxOffFn       = void    (*)(uint32_t);
  using RxOnFn        = void    (*)(void);

  static IqEnableFn    iqEnable    = reinterpret_cast<IqEnableFn>(ROM_IQ_EST_ENABLE);
  static IqDisableFn   iqDisable   = reinterpret_cast<IqDisableFn>(ROM_IQ_EST_DISABLE);
  static SetClkFn      setTxClock  = reinterpret_cast<SetClkFn>(ROM_SET_TXCLK_EN);
  static SetClkFn      setRxClock  = reinterpret_cast<SetClkFn>(ROM_SET_RXCLK_EN);
  static SetAnaScaleFn setAnaScale = reinterpret_cast<SetAnaScaleFn>(ROM_SET_ANA_TX_SCALE);
  static PbusReadFn    pbusReadRom = reinterpret_cast<PbusReadFn>(ROM_PBUS_RD);
  static SetRxGainFn   setRxGain   = reinterpret_cast<SetRxGainFn>(ROM_PBUS_SET_RXGAIN);
  static RxOffFn       rxOff       = reinterpret_cast<RxOffFn>(ROM_PBUS_XPD_RX_OFF);
  static RxOnFn        rxOn        = reinterpret_cast<RxOnFn>(ROM_PBUS_XPD_RX_ON);

  enum Mode : uint8_t { MODE_RX, MODE_TX };
  static Mode mode = MODE_RX;
  static uint32_t toneShadow = 0;
  static uint16_t savedRxGain = 0;
  static bool savedRxGainValid = false;
  static uint8_t currentTxApwr = TX_APWR;

  static inline void memw() {
#ifdef HOST_COMPILE
    asm volatile("" ::: "memory");
#else
    __asm__ volatile("memw" ::: "memory");
#endif
  }

  static inline uint32_t rd32(uint32_t a) {
#ifdef HOST_COMPILE
    (void)a; return 0;
#else
    return *(volatile uint32_t *)a;
#endif
  }

  static inline void wr32(uint32_t a, uint32_t v) {
#ifdef HOST_COMPILE
    (void)a; (void)v;
#else
    *(volatile uint32_t *)a = v;
#endif
  }

  static bool pbusWrite(uint8_t sel, uint8_t bank, uint16_t value) {
    uint32_t cmd = rd32(PBUS_CMD);
    cmd &= 0xFFFF0001u;
    cmd |= ((uint32_t)(bank & 3u) << 14);
    cmd |= ((uint32_t)(value & 0x1FFu) << 5);
    cmd |= ((uint32_t)(sel & 7u) << 2);
    cmd |= 2u;
    memw(); wr32(PBUS_CMD, cmd); memw();

    const uint32_t t0 = micros();
    while (rd32(PBUS_STATUS) & 0x80000000u) {
      if ((uint32_t)(micros() - t0) > 2000u) {
        uint32_t r = rd32(PBUS_CMD);
        wr32(PBUS_CMD, r & ~2u); memw();
        return false;
      }
    }
    uint32_t r = rd32(PBUS_CMD);
    wr32(PBUS_CMD, r & ~2u); memw();
    return true;
  }

  static inline void forceManualRxDigitalOff() {
    uint32_t r = rd32(RX_CTRL);
    wr32(RX_CTRL, r | RX_STOP); memw();
    r = rd32(PBUS_CMD);
    wr32(PBUS_CMD, r | 1u); memw();
  }

  static uint16_t decodeRxGain(uint32_t p32, uint32_t p31, uint32_t p21) {
    uint16_t g = 0;
    g |= (uint16_t)((p32 >> 3) & 0x7u);
    for (uint8_t bit = 3; bit <= 9; ++bit) {
      const uint8_t pbit = (uint8_t)(9u - bit);
      if (p31 & (1u << pbit)) g |= (uint16_t)(1u << bit);
    }
    if (p21 & (1u << 1)) g |= (uint16_t)(1u << 10);
    for (uint8_t bit = 11; bit <= 14; ++bit) {
      const uint8_t pbit = (uint8_t)(17u - bit);
      if (p21 & (1u << pbit)) g |= (uint16_t)(1u << bit);
    }
    return g;
  }

  static bool captureRxGain() {
#ifdef HOST_COMPILE
    savedRxGain = RX_GAIN_CODE;
    savedRxGainValid = true;
    return true;
#else
    memw(); const uint32_t p32 = pbusReadRom(3u,2u) & 0x1FFu;
    memw(); const uint32_t p31 = pbusReadRom(3u,1u) & 0x1FFu;
    memw(); const uint32_t p21 = pbusReadRom(2u,1u) & 0x1FFu;
    memw();
    const uint16_t rawGain = decodeRxGain(p32,p31,p21);
    (void)rawGain;
    savedRxGain = RX_GAIN_CODE;
    savedRxGainValid = true;
    return true;
#endif
  }

  static inline void restoreRxGain() {
    if (!savedRxGainValid) return;
#ifndef HOST_COMPILE
    memw(); setRxGain((uint32_t)savedRxGain); memw();
#endif
  }

  static inline void applyRxGain(uint16_t g) {
    savedRxGain = g;
    savedRxGainValid = true;
#ifndef HOST_COMPILE
    memw(); setRxGain((uint32_t)g); memw();
#endif
  }

  static bool txPathOn() {
    if (!pbusWrite(2,1,1)) return false;
    if (!pbusWrite(7,1,95)) return false;
    if (!pbusWrite(1,1,127)) return false;
    if (!pbusWrite(6,1,127)) return false;
    return true;
  }

  static void txPathOff() {
    pbusWrite(6,1,0);
    pbusWrite(1,1,12);
    pbusWrite(2,1,0);
  }

  static void programToneOff() {
    const uint8_t askCode = (uint8_t)(0u - TX_ASK);
    uint32_t v = rd32(TONE1) & 0xF0000000u;
    v |= ((uint32_t)TONE_K & K_MASK);
    v |= (((uint32_t)askCode << SCALE_SHIFT) & SCALE_MASK);
    v &= ~GATE_MASK;
    toneShadow = v;
    memw(); wr32(TONE1,v); memw();
    uint32_t r=rd32(TONE2); wr32(TONE2,r & ~GATE_MASK); memw();
    r=rd32(TONE3); wr32(TONE3,r & ~GATE_MASK); memw();
  }

  static inline void gate(bool on) {
    uint32_t v=toneShadow;
    if(on)v|=GATE_MASK; else v&=~GATE_MASK;
    toneShadow=v; wr32(TONE1,v); memw();
  }

  static bool begin() {
#ifdef HOST_COMPILE
    savedRxGain=RX_GAIN_CODE; savedRxGainValid=true; mode=MODE_RX; return true;
#else
    wifi_station_set_auto_connect(0);
    if (!wifi_set_opmode_current(STATION_MODE)) return false;
    delay(300);
    wifi_station_disconnect();
    if (!wifi_set_sleep_type(NONE_SLEEP_T)) return false;
    if (!wifi_set_channel(RF_CHANNEL)) return false;
    delay(100);
    memw(); rxOn(); memw();
    memw(); setRxClock(1); memw();
    delay(10);
    forceManualRxDigitalOff();
    captureRxGain(); restoreRxGain();
    delayMicroseconds(RX_SETTLE_US);
    mode=MODE_RX;
    return true;
#endif
  }

  static bool enterTx(uint8_t apwr) {
    currentTxApwr = apwr;
    if (mode==MODE_TX) return true;
#ifndef HOST_COMPILE
    memw(); iqDisable(); memw();
    memw(); setRxClock(0); memw();
    memw(); rxOff(1u); memw();
#endif
    forceManualRxDigitalOff();
    if (!txPathOn()) {
#ifndef HOST_COMPILE
      memw(); rxOn(); memw();
      memw(); setRxClock(1); memw();
#endif
      forceManualRxDigitalOff(); restoreRxGain();
      delayMicroseconds(RX_SETTLE_US); mode=MODE_RX; return false;
    }
#ifndef HOST_COMPILE
    memw(); setAnaScale(currentTxApwr); memw();
    memw(); setTxClock(1); memw();
#endif
    programToneOff();
    delayMicroseconds(TX_SETTLE_US); mode=MODE_TX; return true;
  }

  static void enterRx() {
    if (mode==MODE_RX) return;
    gate(false);
#ifndef HOST_COMPILE
    memw(); setTxClock(0); memw();
#endif
    txPathOff();
#ifndef HOST_COMPILE
    memw(); rxOn(); memw();
    memw(); setRxClock(1); memw();
#endif
    forceManualRxDigitalOff(); restoreRxGain();
    delayMicroseconds(RX_SETTLE_US); mode=MODE_RX;
  }

  static inline void txLevel(bool on) {
#ifndef HOST_COMPILE
    if (on) {
      memw(); setAnaScale(currentTxApwr); memw(); gate(true);
    } else {
      gate(false); memw(); setAnaScale(0); memw();
    }
#else
    (void)on;
#endif
  }

  static uint32_t measureE4() {
#ifdef HOST_COMPILE
    return 100000;
#else
    // v32.16 baseline: one E4 integration per PLL tick with IQ_N=192; DATA threshold frozen after START.
    // RX still uses exactly 8 decisions per bit; only the hardware
    // integration window is increased. No multi-read averaging.
    memw(); iqEnable(1u,IQ_N); memw();
    const uint32_t e=rd32(IQ_POWER_E4);
    memw(); iqDisable(); memw();
    return e;
#endif
  }
}


// ============================================================
// Soft-WDT service
// ============================================================
static constexpr uint32_t WDT_FEED_INTERVAL_US = 20000u;
static uint32_t lastSoftWdtFeedUs = 0;
static uint32_t maxSoftWdtGapUs = 0;

static inline void feedSoftWdtNow() {
  const uint32_t now = micros();
  if (lastSoftWdtFeedUs != 0u) {
    const uint32_t gap = now - lastSoftWdtFeedUs;
    if (gap > maxSoftWdtGapUs) maxSoftWdtGapUs = gap;
  }
#ifndef HOST_COMPILE
  ESP.wdtFeed();
#endif
  lastSoftWdtFeedUs = now;
}

static inline void serviceSoftWdt() {
  const uint32_t now = micros();
  if (lastSoftWdtFeedUs == 0u ||
      (uint32_t)(now - lastSoftWdtFeedUs) >= WDT_FEED_INTERVAL_US) {
    feedSoftWdtNow();
  }
}

// ============================================================
// TX: RH_ASK style symbol stream at exact-average 1500 raw bit/s
// ============================================================
static uint32_t txDeadlineUs = 0;
static uint32_t txFracAccum = 0;

static inline uint32_t nextTxBitIntervalUs() {
  uint32_t dt = TX_BIT_US_FLOOR;
  txFracAccum += TX_BIT_US_REM;
  if (txFracAccum >= TX_TICK_DEN) {
    txFracAccum -= TX_TICK_DEN;
    ++dt;
  }
  return dt;
}

static inline void txWaitDeadline() {
  while (true) {
    const uint32_t now = micros();
    if ((int32_t)(now - txDeadlineUs) >= 0) break;
    if (lastSoftWdtFeedUs == 0u ||
        (uint32_t)(now - lastSoftWdtFeedUs) >= WDT_FEED_INTERVAL_US)
      feedSoftWdtNow();
  }
}

static inline void txBit(bool on) {
  Phy::txLevel(on);
  txDeadlineUs += nextTxBitIntervalUs();
  txWaitDeadline();
}

static inline void txSymbol6(uint8_t symbol) {
  // RH_ASK/VirtualWire transmits each 6-bit symbol LSB first.
  for (uint8_t bit=0; bit<6u; ++bit)
    txBit((symbol & (1u << bit)) != 0u);
}

static bool buildEncodedPacket(const uint8_t *data, uint8_t len,
                               uint8_t *symbolsOut, uint8_t &symbolCount) {
  if (len > MAX_PAYLOAD_LEN) return false;
  uint8_t n=0;
  for (uint8_t i=0; i<AskProto::PREAMBLE_SYMBOLS; ++i)
    symbolsOut[n++] = AskProto::PREAMBLE[i];

  const uint8_t count = (uint8_t)(len + 3u); // count + payload + 2 FCS
  uint16_t crc = 0xffffu;
  crc = AskProto::crcCcittUpdate(crc, count);
  symbolsOut[n++] = AskProto::encodeNibble(count >> 4);
  symbolsOut[n++] = AskProto::encodeNibble(count);

  for (uint8_t i=0; i<len; ++i) {
    crc = AskProto::crcCcittUpdate(crc, data[i]);
    symbolsOut[n++] = AskProto::encodeNibble(data[i] >> 4);
    symbolsOut[n++] = AskProto::encodeNibble(data[i]);
  }

  crc = (uint16_t)~crc;
  const uint8_t fcsLo = (uint8_t)(crc & 0xffu);
  const uint8_t fcsHi = (uint8_t)(crc >> 8);
  symbolsOut[n++] = AskProto::encodeNibble(fcsLo >> 4);
  symbolsOut[n++] = AskProto::encodeNibble(fcsLo);
  symbolsOut[n++] = AskProto::encodeNibble(fcsHi >> 4);
  symbolsOut[n++] = AskProto::encodeNibble(fcsHi);

  symbolCount = n;
  return true;
}

static bool transmitPacket(const uint8_t *data, uint8_t len) {
  uint8_t symbols[AskProto::MAX_ENCODED_SYMBOLS]{};
  uint8_t symbolCount=0;
  if (!buildEncodedPacket(data,len,symbols,symbolCount)) return false;

  feedSoftWdtNow();
  if (!Phy::enterTx(TX_APWR)) {
    Phy::enterRx();
    feedSoftWdtNow();
    return false;
  }
  txDeadlineUs = micros();
  txFracAccum = 0u;

  // One quiet bit lets the analog path settle before training begins.
  txBit(false);
  for (uint8_t i=0; i<symbolCount; ++i) txSymbol6(symbols[i]);
  txBit(false); // one trailing quiet bit

  Phy::enterRx();
  feedSoftWdtNow();
  return true;
}

// ============================================================
// RX sampling - fixed gain 0x7C03
// ============================================================
static AskDecoder decoder;
static uint32_t rxHoldoffUntilUs=0;
static uint32_t nextSampleDueUs=0;
static uint32_t rxFracAccum=0;
static uint32_t sampleCount=0, sampleDtMin=0xffffffffu, sampleDtMax=0, prevSampleUs=0;
static uint64_t sampleDtSum=0;
static uint32_t minE4=0xffffffffu,maxE4=0;
static uint32_t rxGainSamples=0;
static uint32_t e4DtCount=0, e4DtMin=0xffffffffu, e4DtMax=0;
static uint64_t e4DtSum=0;

static inline uint32_t nextRxTickIntervalUs() {
  uint32_t dt = RX_TICK_US_FLOOR;
  rxFracAccum += RX_TICK_US_REM;
  if (rxFracAccum >= RX_TICK_DEN) {
    rxFracAccum -= RX_TICK_DEN;
    ++dt;
  }
  return dt;
}

static void resetReceiverAfterOwnTx() {
  const uint32_t now=micros();
  rxHoldoffUntilUs=now+POST_TX_HOLDOFF_US;
  nextSampleDueUs=0u;
  rxFracAccum=0u;
  decoder.resetTimingOnly();
  prevSampleUs=0;
  // Gain is deliberately never changed: 0x7C03 stays applied at all times.
}

static void serviceRx() {
  if (Phy::mode != Phy::MODE_RX) return;
  const uint32_t now=micros();
  if ((int32_t)(now-rxHoldoffUntilUs)<0) return;

  // Fractional fixed-rate scheduler, equivalent to a 12 kHz hardware timer.
  // Preserve absolute cadence: 83,83,84 us... averages exactly 83.333 us.
  if (!nextSampleDueUs) {
    rxFracAccum=0u;
    nextSampleDueUs=now + nextRxTickIntervalUs();
    return;
  }
  if ((int32_t)(now-nextSampleDueUs)<0) return;

  const uint32_t late = (uint32_t)(now-nextSampleDueUs);
  if (late >= RX_TICK_US_CEIL * 3u) {
    decoder.resetTimingOnly();
    prevSampleUs=0;
    rxFracAccum=0u;
    nextSampleDueUs=now + nextRxTickIntervalUs();
  } else {
    nextSampleDueUs += nextRxTickIntervalUs();
  }

  const uint32_t t0=micros();
  const uint32_t e=Phy::measureE4();
  ++rxGainSamples;
  const uint32_t t1=micros();
  const uint32_t e4dt=(uint32_t)(t1-t0);
  ++e4DtCount; e4DtSum += e4dt;
  if(e4dt < e4DtMin) e4DtMin = e4dt;
  if(e4dt > e4DtMax) e4DtMax = e4dt;
  const uint32_t ts=t0+e4dt/2u;

  if(prevSampleUs){
    const uint32_t dt=ts-prevSampleUs;
    sampleDtSum+=dt; ++sampleCount;
    if(dt<sampleDtMin)sampleDtMin=dt;
    if(dt>sampleDtMax)sampleDtMax=dt;
  }
  prevSampleUs=ts;
  if(e<minE4)minE4=e;
  if(e>maxE4)maxE4=e;

  decoder.addSample(ts,e);
  serviceSoftWdt();
}

// ============================================================
// Simple arbitrary-data application
// ============================================================
static uint32_t nextTxMs=0;
static uint32_t txPackets=0;
static uint32_t rxPackets=0;
static uint32_t txBytes=0;
static uint32_t rxBytes=0;

static void scheduleNextTx() {
  nextTxMs=millis()+(uint32_t)random(SEND_MIN_MS,SEND_MAX_MS+1u);
}

static void printHex(const uint8_t *p, uint8_t n) {
  for(uint8_t i=0;i<n;++i) Serial.printf("%02X",(unsigned)p[i]);
}

static void sendTestPacket() {
  if (!decoder.searching()) { nextTxMs=millis()+20u; return; }

  uint8_t payload[TEST_PAYLOAD_LEN]{};
  for(uint8_t i=0;i<TEST_PAYLOAD_LEN;++i)
    payload[i]=(uint8_t)random(0,256);

  decoder.resetTimingOnly();
  if(transmitPacket(payload,TEST_PAYLOAD_LEN)) {
    ++txPackets; txBytes+=TEST_PAYLOAD_LEN;
    Serial.printf("TX len=%u data=",(unsigned)TEST_PAYLOAD_LEN);
    printHex(payload,TEST_PAYLOAD_LEN);
    const uint32_t wireBits = (uint32_t)(2u + 6u*(AskProto::PREAMBLE_SYMBOLS + 2u*(TEST_PAYLOAD_LEN+3u)));
    const uint32_t airtimeUs = (uint32_t)(((uint64_t)wireBits * 1000000ULL + BIT_RATE - 1u) / BIT_RATE);
    Serial.printf(" APWR=%u DS=0 TXMODE=ANA-BIT airtime=%lu.%03lums\n",(unsigned)TX_APWR,
                  (unsigned long)(airtimeUs/1000u),(unsigned long)(airtimeUs%1000u));
  } else {
    Serial.println("TX ERROR");
  }
  resetReceiverAfterOwnTx();
  scheduleNextTx();
}

static const char* failureReasonName(uint8_t reason) {
  switch (reason) {
    case AskDecoder::FAIL_SYMBOL: return "SYM";
    case AskDecoder::FAIL_LENGTH: return "LEN";
    case AskDecoder::FAIL_CRC: return "CRC";
    case AskDecoder::FAIL_TIMEOUT: return "TIMEOUT";
    default: return "?";
  }
}

static void buildExpectedCountBits(char out[13]) {
  const uint8_t count = (uint8_t)(TEST_PAYLOAD_LEN + 3u);
  const uint8_t s0 = AskProto::encodeNibble(count >> 4);
  const uint8_t s1 = AskProto::encodeNibble(count);
  uint8_t k = 0u;
  for (uint8_t i=0; i<6u; ++i) out[k++] = (s0 & (1u << i)) ? '1' : '0';
  for (uint8_t i=0; i<6u; ++i) out[k++] = (s1 & (1u << i)) ? '1' : '0';
  out[12] = '\0';
}

static void printPendingFailureDiag() {
  AskDecoder::FailureSnapshot f{};
  if (!decoder.takeFailureSnapshot(f)) return;

  char expected[13] = "------------";
  // Only block 0 (count byte) is predictable locally for randomized payloads.
  if (f.blockIndex == 0u) buildExpectedCountBits(expected);

  char got[13];
  for (uint8_t i=0; i<12u; ++i)
    got[i] = (i < f.count) ? (f.bits[i] ? '1' : '0') : '-';
  got[12] = '\0';

  Serial.printf("FAILBLOCK reason=%s byte=%u expected=%s bits=%s n=%u symbols=%02X/%02X len=%u sampGapMax=%uus\n",
                failureReasonName(f.reason), (unsigned)f.blockIndex, expected, got, (unsigned)f.count,
                (unsigned)f.badSymHi, (unsigned)f.badSymLo, (unsigned)f.badLen,
                (unsigned)f.blockGapMaxUs);
  Serial.printf("  votes=");
  for (uint8_t i=0; i<f.count; ++i) {
    if (i) Serial.printf(",");
    Serial.printf("%u/%u", (unsigned)f.highVotes[i], (unsigned)f.totalVotes[i]);
  }
  Serial.println();
  Serial.printf("  phase=");
  for (uint8_t i=0; i<f.count; ++i) {
    if (i) Serial.printf(",");
    Serial.printf("%u", (unsigned)f.phaseResidual[i]);
  }
  Serial.println();
  Serial.printf("  gapUs=");
  for (uint8_t i=0; i<f.count; ++i) {
    if (i) Serial.printf(",");
    Serial.printf("%u", (unsigned)f.sampleGapUs[i]);
  }
  Serial.println();
  Serial.printf("  slicer thr=%lu hyst=%lu effectiveLOW=%lu effectiveHIGH=%lu(rise=T)\n",
                (unsigned long)f.frozenThreshold, (unsigned long)f.slicerHysteresis,
                (unsigned long)((f.frozenThreshold > f.slicerHysteresis)
                                  ? (f.frozenThreshold - f.slicerHysteresis) : 0u),
                (unsigned long)f.frozenThreshold);
  Serial.printf("  e4avg=");
  for (uint8_t i=0; i<f.count; ++i) {
    if (i) Serial.printf(",");
    Serial.printf("%lu", (unsigned long)f.e4Avg[i]);
  }
  Serial.println();
  Serial.printf("  e4min=");
  for (uint8_t i=0; i<f.count; ++i) {
    if (i) Serial.printf(",");
    Serial.printf("%lu", (unsigned long)f.e4Min[i]);
  }
  Serial.println();
  Serial.printf("  e4max=");
  for (uint8_t i=0; i<f.count; ++i) {
    if (i) Serial.printf(",");
    Serial.printf("%lu", (unsigned long)f.e4Max[i]);
  }
  Serial.println();
  // The diagnostic print itself is intentionally outside the decoder's sampling path.
  // Prevent it from being counted as an RX sample-period outlier.
  prevSampleUs = 0u;
}

static void serviceApplication() {
  printPendingFailureDiag();
  uint8_t payload[MAX_PAYLOAD_LEN]{};
  uint8_t len=0;
  if(decoder.takePacket(payload,len)) {
    ++rxPackets; rxBytes+=len;
    Serial.printf("RX OK len=%u data=",(unsigned)len);
    printHex(payload,len);
    Serial.println();
  }

  if((int32_t)(millis()-nextTxMs)>=0) sendTestPacket();
}

static void printDiagnostics() {
  const auto &d=decoder.diagnostics();
  const uint32_t dtAvg=sampleCount?(uint32_t)(sampleDtSum/sampleCount):0u;
  const uint32_t dtMin=(sampleDtMin==0xffffffffu)?0u:sampleDtMin;
  const uint32_t eMin=(minE4==0xffffffffu)?0u:minE4;
  const uint32_t cMin=(d.contrastMinActive==0xffffffffu)?0u:d.contrastMinActive;
  const uint32_t qPermille=d.startHits?(uint32_t)(((uint64_t)d.packetsOk*1000u)/d.startHits):0u;
  const uint32_t e4DtAvg=e4DtCount?(uint32_t)(e4DtSum/e4DtCount):0u;
  const uint32_t e4DtMinOut=(e4DtMin==0xffffffffu)?0u:e4DtMin;
  const uint32_t p36AAvg=d.pre36AcceptCount?(d.pre36AcceptSum/d.pre36AcceptCount):0u;
  const uint32_t p36NAAvg=d.pre36RejectNACount?(d.pre36RejectNASum/d.pre36RejectNACount):0u;
  const uint32_t p36EXAvg=d.pre36RejectEXPCount?(d.pre36RejectEXPSum/d.pre36RejectEXPCount):0u;
  const uint8_t p36AMin=(d.pre36AcceptMin==0xffu)?0u:d.pre36AcceptMin;
  const uint8_t p36NAMin=(d.pre36RejectNAMin==0xffu)?0u:d.pre36RejectNAMin;
  const uint8_t p36EXMin=(d.pre36RejectEXPMin==0xffu)?0u:d.pre36RejectEXPMin;

  Serial.printf(
    "STAT tx=%lu/%luB rx=%lu/%luB "
    "ASK[samp=%lu bit=%lu edge=%lu start=%lu ok=%lu symBad=%lu lenBad=%lu crc=%lu to=%lu] "
    "Q[ok/start=%lu/%lu=%lu.%lu%%] "
    "ML[try=%lu use=%lu ok=%lu] "
    "PRE[arm=%lu raw=%lu accept=%lu reject=%lu rejNA=%lu rejEXP=%lu expEvt=%lu altMax=%u] "
    "P36[A=%lu:%lu[%u..%u] NA=%lu:%lu[%u..%u] EX=%lu:%lu[%u..%u]] "
    "PLL[ret=%lu adv=%lu bs=%u..%u] BAD[s=%02X/%02X len=%u] "
    "E4[lo=%lu hi=%lu th=%lu sep=%lu minPkt=%lu maxSep=%lu weak=%lu raw=%lu..%lu] "
    "GAIN[fixed=%04X samples=%lu] "
    "E4dt=%lu[%lu..%lu]us "
    "sampdt=%lu[%lu..%lu]us WDTmax=%luus\n",
    (unsigned long)txPackets,(unsigned long)txBytes,
    (unsigned long)rxPackets,(unsigned long)rxBytes,
    (unsigned long)d.samples,(unsigned long)d.bits,(unsigned long)d.transitions,
    (unsigned long)d.startHits,(unsigned long)d.packetsOk,
    (unsigned long)d.symbolErrors,(unsigned long)d.lengthErrors,
    (unsigned long)d.crcErrors,(unsigned long)d.timeouts,
    (unsigned long)d.packetsOk,(unsigned long)d.startHits,
    (unsigned long)(qPermille/10u),(unsigned long)(qPermille%10u),
    (unsigned long)d.analogMlAttempts,(unsigned long)d.analogMlRescuedBlocks,
    (unsigned long)d.analogMlPacketsOk,
    (unsigned long)d.preambleArms,(unsigned long)d.rawStartHits,
    (unsigned long)d.startHits,(unsigned long)d.preambleRejects,
    (unsigned long)d.preambleRejectNotArmed,(unsigned long)d.preambleRejectExpired,
    (unsigned long)d.preambleExpired,(unsigned)d.preambleAltMax,
    (unsigned long)d.pre36AcceptCount,(unsigned long)p36AAvg,(unsigned)p36AMin,(unsigned)d.pre36AcceptMax,
    (unsigned long)d.pre36RejectNACount,(unsigned long)p36NAAvg,(unsigned)p36NAMin,(unsigned)d.pre36RejectNAMax,
    (unsigned long)d.pre36RejectEXPCount,(unsigned long)p36EXAvg,(unsigned)p36EXMin,(unsigned)d.pre36RejectEXPMax,
    (unsigned long)d.pllRetard,(unsigned long)d.pllAdvance,
    (unsigned)(d.bitSamplesMin==0xffu?0u:d.bitSamplesMin),(unsigned)d.bitSamplesMax,
    (unsigned)d.lastBadSymHi,(unsigned)d.lastBadSymLo,(unsigned)d.lastBadLen,
    (unsigned long)d.envLow,(unsigned long)d.envHigh,(unsigned long)d.threshold,
    (unsigned long)d.contrast,(unsigned long)cMin,(unsigned long)d.contrastMax,
    (unsigned long)d.weakSamples,(unsigned long)eMin,(unsigned long)maxE4,
    (unsigned)RX_GAIN_CODE,(unsigned long)rxGainSamples,
    (unsigned long)e4DtAvg,(unsigned long)e4DtMinOut,(unsigned long)e4DtMax,
    (unsigned long)dtAvg,(unsigned long)dtMin,(unsigned long)sampleDtMax,
    (unsigned long)maxSoftWdtGapUs);
}









static const uint8_t ASK_LEVELS[4] = {70,102,114,127};
static const uint16_t FSK_HALF_US[32] = {500,423,367,324,290,262,239,220,203,189,176,165,155,146,138,131,125,119,114,109,104,100,96,92,89,86,83,80,77,74,72,63};
static const uint16_t FSK_HZ_DISPLAY[32] = {1000,1182,1362,1543,1724,1908,2092,2273,2463,2646,2841,3030,3226,3425,3623,3817,4000,4202,4386,4587,4808,5000,5208,5435,5618,5814,6024,6250,6494,6757,6944,7937};
static const uint8_t TEST_PATTERN[256] = {231,129,193,216,7,11,51,128,120,138,157,194,117,90,131,78,191,217,84,185,196,159,178,179,145,44,245,149,199,142,95,154,9,158,4,229,212,182,206,238,19,81,140,122,250,180,244,203,188,80,109,50,189,228,68,57,40,121,220,85,38,175,225,249,201,42,1,143,88,58,26,23,83,37,79,226,86,47,74,56,108,247,141,184,104,125,252,163,20,92,124,16,136,93,218,112,119,3,27,146,155,204,222,24,87,130,253,25,113,33,32,59,99,102,65,118,133,91,150,45,153,190,5,168,208,166,186,100,177,224,61,235,223,192,12,115,198,89,6,34,165,200,103,147,66,173,54,46,0,174,36,132,28,41,97,48,243,49,13,69,214,39,183,55,207,75,151,8,72,213,139,210,232,126,171,71,98,134,30,18,169,22,176,67,17,52,106,242,246,219,181,2,254,35,251,105,156,21,15,76,172,31,111,96,135,237,60,70,170,195,197,248,148,110,167,64,162,144,10,209,202,116,114,205,53,234,43,227,137,221,63,215,164,107,101,77,14,29,211,94,160,187,127,239,62,241,161,73,152,123,255,240,82,233,236,230};

// Payload mapping:
//   bits [7:3] = FSK index
//   bits [2:1] = ASK index
//   bit  [0]   = PSK bit
//
// 2PSK is decoded DIFFERENTIALLY inside each symbol:
//   pilot window  : before the possible TX phase flip
//   data window   : after the possible TX phase flip
// Both correlations use the SAME local symbol reference.
// dot(pilotIQ, dataIQ) >= 0 -> PSK 0
// dot(pilotIQ, dataIQ) <  0 -> PSK 1
//
// This avoids requiring long-term coherent phase lock between ESP8266s.

static constexpr uint32_t SYMBOL_US = 30000u;
static constexpr uint16_t PAYLOAD_SYMBOLS = 1024u;
static constexpr uint8_t  SYNC_SYMBOLS = 32u;

static constexpr uint32_t CAL_STATE_US = 120000u;
static constexpr uint32_t CAL_EDGE_GUARD_US = 18000u;
static constexpr uint32_t INTER_GAP_US = 100000u;

static constexpr uint32_t WINDOW_US = 8000u;
static constexpr uint32_t PILOT_CENTER_US = 7500u;   // 3.5 .. 11.5 ms
static constexpr uint32_t PSK_FLIP_US = 13000u;
static constexpr uint32_t DATA_CENTER_US = 19500u;   // 15.5 .. 23.5 ms
static constexpr uint16_t TRAIN_MAX_WINDOWS = 220u;

struct MultiWindow {
  uint32_t startUs;
  uint32_t endUs;
  uint16_t samples;
  uint32_t amp[32];
  int32_t iv[32];
  int32_t qv[32];
};

static uint32_t mT0 = 0;
static uint32_t mRefUs = 0;
static uint64_t mSum = 0;
static uint32_t mN = 0;
static int64_t mI[32] = {0};
static int64_t mQ[32] = {0};

static inline uint64_t abs64(int64_t v) {
  return (uint64_t)(v < 0 ? -v : v);
}

static inline void correlateSquareHalfPeriod(
    uint32_t now, uint32_t refUs, uint16_t halfUs,
    int32_t x, int64_t &iv, int64_t &qv) {
  const uint32_t elapsed = now - refUs;
  const uint32_t phase =
      (uint32_t)(((elapsed * 2UL) / (uint32_t)halfUs) & 3UL);

  const int32_t ri = (phase < 2u) ? 1 : -1;
  const int32_t rq = (phase == 0u || phase == 3u) ? 1 : -1;

  iv += (int64_t)x * ri;
  qv += (int64_t)x * rq;
}

static void resetMeasurement(uint32_t now) {
  mT0 = now;
  mRefUs = now;
  mSum = 0;
  mN = 0;
  for (uint8_t i=0;i<32u;++i) {
    mI[i]=0;
    mQ[i]=0;
  }
}

static void resetMeasurementWithRef(uint32_t now, uint32_t refUs) {
  mT0 = now;
  mRefUs = refUs;
  mSum = 0;
  mN = 0;
  for (uint8_t i=0;i<32u;++i) {
    mI[i]=0;
    mQ[i]=0;
  }
}

static bool serviceMultiMeasurement(struct MultiWindow &out) {
  const uint32_t now = micros();
  const uint32_t e = Phy::measureE4();

  mSum += e;
  ++mN;
  const int32_t mean = (int32_t)(mSum / mN);
  const int32_t x = (int32_t)e - mean;

  for (uint8_t i=0;i<32u;++i) {
    correlateSquareHalfPeriod(now, mRefUs, FSK_HALF_US[i], x, mI[i], mQ[i]);
  }

  if ((uint32_t)(now - mT0) < WINDOW_US || mN < 8u) return false;

  out.startUs = mT0;
  out.endUs = now;
  out.samples = (mN > 65535u) ? 65535u : (uint16_t)mN;

  for (uint8_t i=0;i<32u;++i) {
    const int64_t ni = mI[i] / (int64_t)mN;
    const int64_t nq = mQ[i] / (int64_t)mN;

    out.iv[i] = (int32_t)ni;
    out.qv[i] = (int32_t)nq;
    out.amp[i] = (uint32_t)(abs64(ni) + abs64(nq));
  }

  resetMeasurement(micros());
  return true;
}

static inline uint32_t maxAmp32(const struct MultiWindow &w) {
  uint32_t v=w.amp[0];
  for (uint8_t i=1;i<32u;++i) if (w.amp[i]>v) v=w.amp[i];
  return v;
}

// Same magnitude calibration architecture as V27.
static uint64_t calSum[32][4];
static uint16_t calCount[32][4];
static uint32_t askCenter[32][4];
static uint32_t freqRef[32];
static bool calibrationValid = false;

static uint32_t offMetricSum = 0;
static uint32_t offMetricCount = 0;
static uint32_t offMetricAvg = 0;
static uint32_t onThreshold = 0;

static bool candidate = false;
static uint32_t candidateStartUs = 0;
static uint32_t lowStartUs = 0;
static uint32_t calStartUs = 0;

static uint32_t syncCoarseStartUs = 0;
static uint32_t syncStartUs = 0;
static uint16_t syncScoreX1000 = 0;

static uint32_t trainStartUs[TRAIN_MAX_WINDOWS];
static uint32_t trainEndUs[TRAIN_MAX_WINDOWS];
static uint8_t trainPresent[TRAIN_MAX_WINDOWS];
static uint16_t trainCount = 0;

static uint32_t payloadStartUs = 0;
static uint16_t payloadIndex = 0;
static uint8_t payloadStage = 0;   // 0=pilot wait, 1=data wait
static bool payloadMeasurementActive = false;
static struct MultiWindow pilotWindow;

static uint16_t decisions = 0;
static uint16_t correct = 0;

static uint16_t freqSent[32] = {0};
static uint16_t freqCorrect[32] = {0};
static uint16_t askSent[4] = {0};
static uint16_t askCorrect[4] = {0};
static uint16_t pskSent[2] = {0,0};
static uint16_t pskCorrect[2] = {0,0};

static uint32_t sampleSum = 0;
static uint16_t sampleMin = 65535u;
static uint16_t sampleMax = 0;

static inline uint8_t payloadPskBit(uint8_t state) { return state & 1u; }
static inline uint8_t payloadAskIndex(uint8_t state) { return (state >> 1) & 3u; }
static inline uint8_t payloadFreqIndex(uint8_t state) { return (state >> 3) & 31u; }

static inline uint8_t calAskIndex(uint8_t state) { return state & 3u; }
static inline uint8_t calFreqIndex(uint8_t state) { return (state >> 2) & 31u; }

enum RxState : uint8_t {
  RX_WAIT_LONG_OFF,
  RX_WAIT_MARK,
  RX_WAIT_MARK_END,
  RX_WAIT_CAL,
  RX_CALIBRATE,
  RX_WAIT_SYNC,
  RX_TRAINING,
  RX_WAIT_PAYLOAD,
  RX_PAYLOAD
};

static RxState rxState = RX_WAIT_LONG_OFF;

static void clearCalibration() {
  for (uint8_t f=0;f<32u;++f) {
    freqRef[f]=1u;
    for (uint8_t a=0;a<4u;++a) {
      calSum[f][a]=0;
      calCount[f][a]=0;
      askCenter[f][a]=0;
    }
  }
  calibrationValid=false;
}

static void clearTraining() {
  trainCount=0;
}

static void clearPayloadStats() {
  payloadIndex=0;
  payloadStage=0;
  payloadMeasurementActive=false;
  decisions=0;
  correct=0;

  sampleSum=0;
  sampleMin=65535u;
  sampleMax=0;

  for (uint8_t f=0;f<32u;++f) {
    freqSent[f]=0;
    freqCorrect[f]=0;
  }
  for (uint8_t a=0;a<4u;++a) {
    askSent[a]=0;
    askCorrect[a]=0;
  }
  pskSent[0]=pskSent[1]=0;
  pskCorrect[0]=pskCorrect[1]=0;
}

static void restartAcquisition() {
  rxState=RX_WAIT_LONG_OFF;
  offMetricSum=0;
  offMetricCount=0;
  offMetricAvg=0;
  onThreshold=0;
  candidate=false;
  candidateStartUs=0;
  lowStartUs=0;

  clearCalibration();
  clearTraining();
  clearPayloadStats();
  resetMeasurement(micros());
}

static void reportCalibrationRows() {
  for (uint8_t fi=0;fi<32u;++fi) {
    Serial.printf(
      "CAL4322_ROW fi=%u hz=%u c0=%lu c1=%lu c2=%lu c3=%lu\r\n",
      (unsigned)fi, (unsigned)FSK_HZ_DISPLAY[fi],
      (unsigned long)askCenter[fi][0],
      (unsigned long)askCenter[fi][1],
      (unsigned long)askCenter[fi][2],
      (unsigned long)askCenter[fi][3]);
  }
}

static bool finishCalibration() {
  bool ok=true;

  for (uint8_t f=0;f<32u;++f) {
    for (uint8_t a=0;a<4u;++a) {
      if (calCount[f][a] < 4u) {
        ok=false;
        continue;
      }
      askCenter[f][a]=(uint32_t)(calSum[f][a]/calCount[f][a]);
    }
    freqRef[f]=askCenter[f][0] ? askCenter[f][0] : 1u;
  }

  calibrationValid=ok;
  if (ok) reportCalibrationRows();
  return ok;
}

static uint8_t classifyFreqPair(
    const struct MultiWindow &pilot,
    const struct MultiWindow &data) {

  uint8_t best=0;
  uint32_t bestScore=0;

  for (uint8_t f=0;f<32u;++f) {
    const uint32_t ref=freqRef[f] ? freqRef[f] : 1u;
    const uint64_t sumAmp=(uint64_t)pilot.amp[f]+(uint64_t)data.amp[f];
    const uint32_t score=
        (uint32_t)((sumAmp * 500000ULL) / ref);

    if (score>bestScore) {
      bestScore=score;
      best=f;
    }
  }

  return best;
}

static uint8_t classifyAsk(uint8_t fi, uint32_t amp) {
  uint8_t best=0;
  uint32_t bestDist=0xFFFFFFFFu;

  for (uint8_t a=0;a<4u;++a) {
    const uint32_t c=askCenter[fi][a];
    const uint32_t d=(amp>c)?(amp-c):(c-amp);

    if (d<bestDist) {
      bestDist=d;
      best=a;
    }
  }

  return best;
}

static uint8_t classifyPsk(
    uint8_t fi,
    const struct MultiWindow &pilot,
    const struct MultiWindow &data) {

  const int64_t dot =
      (int64_t)pilot.iv[fi]*(int64_t)data.iv[fi] +
      (int64_t)pilot.qv[fi]*(int64_t)data.qv[fi];

  return (dot < 0) ? 1u : 0u;
}

static inline bool metricOn(const struct MultiWindow &w) {
  return onThreshold && maxAmp32(w)>=onThreshold;
}

static void storeTraining(const struct MultiWindow &w) {
  if (trainCount>=TRAIN_MAX_WINDOWS) return;
  trainStartUs[trainCount]=w.startUs;
  trainEndUs[trainCount]=w.endUs;
  trainPresent[trainCount]=metricOn(w)?1u:0u;
  ++trainCount;
}

static void sortInt32(int32_t *v, uint8_t n) {
  for (uint8_t i=1;i<n;++i) {
    int32_t x=v[i];
    int8_t j=(int8_t)i-1;

    while (j>=0 && v[(uint8_t)j]>x) {
      v[(uint8_t)j+1u]=v[(uint8_t)j];
      --j;
    }

    v[(uint8_t)j+1u]=x;
  }
}

static bool refineSync() {
  int32_t offsets[SYNC_SYMBOLS-1u];
  uint8_t nOffsets=0;
  uint8_t transitionNumber=1;

  for (uint16_t i=1;i<trainCount && transitionNumber<SYNC_SYMBOLS;++i) {
    if (trainPresent[i]==trainPresent[i-1u]) continue;

    const uint32_t c0=trainStartUs[i-1u]+
        ((trainEndUs[i-1u]-trainStartUs[i-1u])/2u);
    const uint32_t c1=trainStartUs[i]+
        ((trainEndUs[i]-trainStartUs[i])/2u);

    const uint32_t edge=c0+((uint32_t)(c1-c0)/2u);
    const uint32_t expected=
        syncCoarseStartUs+(uint32_t)transitionNumber*SYMBOL_US;

    offsets[nOffsets++]=(int32_t)(edge-expected);
    ++transitionNumber;
  }

  if (nOffsets<20u) return false;

  sortInt32(offsets,nOffsets);
  const int32_t median=offsets[nOffsets/2u];

  int64_t sum=0;
  uint8_t n=0;

  for (uint8_t i=0;i<nOffsets;++i) {
    int32_t d=offsets[i]-median;
    if (d<0) d=-d;

    if (d<=4500) {
      sum+=offsets[i];
      ++n;
    }
  }

  const int32_t corr=n?(int32_t)(sum/n):median;
  syncStartUs=syncCoarseStartUs+(uint32_t)corr;

  uint16_t scored=0,good=0;

  for (uint16_t i=0;i<trainCount;++i) {
    const uint32_t centerUs=trainStartUs[i]+
        ((trainEndUs[i]-trainStartUs[i])/2u);

    const int32_t rel=(int32_t)(centerUs-syncStartUs);
    if (rel<0) continue;

    const uint32_t sym=(uint32_t)rel/SYMBOL_US;
    if (sym>=SYNC_SYMBOLS) continue;

    const bool expectedOn=(sym&1u)==0u;

    ++scored;
    if ((trainPresent[i]!=0u)==expectedOn) ++good;
  }

  if (scored<20u) return false;

  syncScoreX1000=(uint16_t)(((uint32_t)good*1000u)/scored);
  return syncScoreX1000>=850u;
}

static void updateSampleStats(uint16_t n) {
  sampleSum += n;
  if (n<sampleMin) sampleMin=n;
  if (n>sampleMax) sampleMax=n;
}

static void reportResult() {
  const uint16_t coverageX100=
      (uint16_t)(((uint32_t)decisions*10000u)/PAYLOAD_SYMBOLS);

  const uint16_t accuracyX100=
      (uint16_t)(((uint32_t)correct*10000u)/PAYLOAD_SYMBOLS);

  const uint16_t avgSamples=
      decisions ? (uint16_t)(sampleSum/(2u*decisions)) : 0u;

  for (uint8_t f=0;f<32u;++f) {
    Serial.printf(
      "COMBO4322_FREQ fi=%u hz=%u sent=%u correct=%u\r\n",
      (unsigned)f,(unsigned)FSK_HZ_DISPLAY[f],
      (unsigned)freqSent[f],(unsigned)freqCorrect[f]);
  }

  for (uint8_t a=0;a<4u;++a) {
    Serial.printf(
      "COMBO4322_ASK ai=%u ask=%u sent=%u correct=%u\r\n",
      (unsigned)a,(unsigned)ASK_LEVELS[a],
      (unsigned)askSent[a],(unsigned)askCorrect[a]);
  }

  for (uint8_t p=0;p<2u;++p) {
    Serial.printf(
      "COMBO4322_PSK pi=%u sent=%u correct=%u\r\n",
      (unsigned)p,(unsigned)pskSent[p],(unsigned)pskCorrect[p]);
  }

  Serial.printf(
    "COMBO4322_RESULT states=256 bits_per_symbol=8 symbol_us=%lu "
    "raw_bps_x10=2667 sent=%u decisions=%u correct=%u coverage_x100=%u "
    "accuracy_x100=%u sync_x1000=%u samples_avg=%u samples_min=%u samples_max=%u\r\n",
    (unsigned long)SYMBOL_US,
    (unsigned)PAYLOAD_SYMBOLS,
    (unsigned)decisions,
    (unsigned)correct,
    (unsigned)coverageX100,
    (unsigned)accuracyX100,
    (unsigned)syncScoreX1000,
    (unsigned)avgSamples,
    (unsigned)(sampleMin==65535u?0u:sampleMin),
    (unsigned)sampleMax);

  reportCalibrationRows();
}

static void decodePayloadSymbol(
    const struct MultiWindow &pilot,
    const struct MultiWindow &data) {

  const uint8_t expected=TEST_PATTERN[payloadIndex&255u];

  const uint8_t efi=payloadFreqIndex(expected);
  const uint8_t eai=payloadAskIndex(expected);
  const uint8_t epi=payloadPskBit(expected);

  const uint8_t dfi=classifyFreqPair(pilot,data);

  const uint32_t avgAmp=
      (uint32_t)(((uint64_t)pilot.amp[dfi]+(uint64_t)data.amp[dfi])/2ULL);

  const uint8_t dai=classifyAsk(dfi,avgAmp);
  const uint8_t dpi=classifyPsk(dfi,pilot,data);

  const uint8_t decoded=
      (uint8_t)((dfi<<3)|(dai<<1)|dpi);

  ++decisions;

  ++freqSent[efi];
  ++askSent[eai];
  ++pskSent[epi];

  if (dfi==efi) ++freqCorrect[efi];
  if (dai==eai) ++askCorrect[eai];
  if (dpi==epi) ++pskCorrect[epi];

  if (decoded==expected) ++correct;

  updateSampleStats(pilot.samples);
  updateSampleStats(data.samples);
}

static void servicePayloadTargeted() {
  if (payloadIndex>=PAYLOAD_SYMBOLS) {
    reportResult();
    Serial.println("COMBO4322_BENCH_COMPLETE");
    restartAcquisition();
    return;
  }

  const uint32_t symbolStartUs=
      payloadStartUs+(uint32_t)payloadIndex*SYMBOL_US;

  const uint32_t pilotStartUs=
      symbolStartUs+PILOT_CENTER_US-(WINDOW_US/2u);

  const uint32_t dataStartUs=
      symbolStartUs+DATA_CENTER_US-(WINDOW_US/2u);

  const uint32_t targetStartUs=
      (payloadStage==0u)?pilotStartUs:dataStartUs;

  if (!payloadMeasurementActive) {
    const uint32_t now=micros();

    if ((int32_t)(now-targetStartUs)<0) return;

    const uint32_t symbolEndUs=symbolStartUs+SYMBOL_US;

    if ((int32_t)((now+WINDOW_US)-symbolEndUs)>=0) {
      // Lost timing for this symbol: skip it cleanly.
      payloadStage=0;
      ++payloadIndex;
      return;
    }

    // Critical PSK detail: both windows use the SAME local symbol reference.
    resetMeasurementWithRef(now,symbolStartUs);
    payloadMeasurementActive=true;
  }

  struct MultiWindow w;
  if (!serviceMultiMeasurement(w)) return;

  payloadMeasurementActive=false;

  if (payloadStage==0u) {
    pilotWindow=w;
    payloadStage=1u;
    return;
  }

  decodePayloadSymbol(pilotWindow,w);

  payloadStage=0u;
  ++payloadIndex;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();

  system_update_cpu_freq(160);
  Serial.println("CPU=160 MHz");

  if (!Phy::begin()) {
    Serial.println("PHY init FAILED");
    while (true) delay(1000);
  }

  Serial.println("ROLE=RX : 4-ASK x 32-FSK x 2-PSK = 256-state / 8-bit-symbol");
  Serial.printf(
    "RF channel=%u fixedGain=0x%04X window_us=%lu "
    "pilot_center_us=%lu psk_flip_us=%lu data_center_us=%lu "
    "raw_bps_x10=2667\r\n",
    (unsigned)RF_CHANNEL,(unsigned)RX_GAIN_CODE,
    (unsigned long)WINDOW_US,
    (unsigned long)PILOT_CENTER_US,
    (unsigned long)PSK_FLIP_US,
    (unsigned long)DATA_CENTER_US);

  Phy::enterRx();
  restartAcquisition();
}

void loop() {
  if (rxState==RX_PAYLOAD) {
    servicePayloadTargeted();
    ESP.wdtFeed();
    return;
  }

  struct MultiWindow w;

  if (!serviceMultiMeasurement(w)) {
    ESP.wdtFeed();
    return;
  }

  static uint32_t statusMs=0;
  const uint32_t nowMs=millis();

  if ((uint32_t)(nowMs-statusMs)>=1000u) {
    statusMs=nowMs;

    Serial.printf(
      "RX_STATUS state=%u max=%lu thr=%lu cal=%u samples=%u\r\n",
      (unsigned)rxState,(unsigned long)maxAmp32(w),
      (unsigned long)onThreshold,calibrationValid?1u:0u,
      (unsigned)w.samples);
  }

  switch (rxState) {
    case RX_WAIT_LONG_OFF:
      offMetricSum+=maxAmp32(w);
      ++offMetricCount;

      if (offMetricCount>=70u) {
        offMetricAvg=offMetricSum/offMetricCount;
        candidate=false;
        rxState=RX_WAIT_MARK;

        Serial.printf(
          "COMBO4322_BASE off=%lu\r\n",
          (unsigned long)offMetricAvg);
      }
      break;

    case RX_WAIT_MARK: {
      const uint32_t margin=
          (offMetricAvg/8u>20000u)?offMetricAvg/8u:20000u;

      const bool high=maxAmp32(w)>=offMetricAvg+margin;

      if (high) {
        if (!candidate) {
          candidate=true;
          candidateStartUs=w.startUs;
        }

        if ((uint32_t)(w.endUs-candidateStartUs)>=100000u) {
          const uint32_t marker=maxAmp32(w);

          onThreshold=offMetricAvg+((marker-offMetricAvg)/2u);
          candidate=false;
          lowStartUs=0;
          rxState=RX_WAIT_MARK_END;

          Serial.printf(
            "COMBO4322_MARK marker=%lu thr=%lu\r\n",
            (unsigned long)marker,(unsigned long)onThreshold);
        }
      } else {
        candidate=false;
      }
      break;
    }

    case RX_WAIT_MARK_END:
      if (maxAmp32(w)<onThreshold) {
        if (!lowStartUs) lowStartUs=w.startUs;

        if ((uint32_t)(w.endUs-lowStartUs)>=150000u) {
          candidate=false;
          rxState=RX_WAIT_CAL;
        }
      } else {
        lowStartUs=0;
      }
      break;

    case RX_WAIT_CAL:
      if (maxAmp32(w)>=onThreshold) {
        if (!candidate) {
          candidate=true;
          candidateStartUs=w.startUs;
        }

        if ((uint32_t)(w.endUs-candidateStartUs)>=20000u) {
          calStartUs=candidateStartUs;
          clearCalibration();
          rxState=RX_CALIBRATE;
          Serial.println("COMBO4322_CAL_RX_START");
        }
      } else {
        candidate=false;
      }
      break;

    case RX_CALIBRATE: {
      const int32_t rs=(int32_t)(w.startUs-calStartUs);
      const int32_t re=(int32_t)(w.endUs-calStartUs);
      const uint32_t total=128u*CAL_STATE_US;

      if (rs>=0 && (uint32_t)rs<total && re>0) {
        const uint32_t u0=(uint32_t)rs;
        const uint32_t u1=(uint32_t)(re-1);

        const uint16_t state=(uint16_t)(u0/CAL_STATE_US);
        const bool same=(u0/CAL_STATE_US)==(u1/CAL_STATE_US);

        const uint32_t p0=u0%CAL_STATE_US;
        const uint32_t p1=u1%CAL_STATE_US;

        if (same && state<128u &&
            p0>=CAL_EDGE_GUARD_US &&
            p1<=CAL_STATE_US-CAL_EDGE_GUARD_US) {

          const uint8_t fi=(uint8_t)((state>>2)&31u);
          const uint8_t ai=(uint8_t)(state&3u);

          calSum[fi][ai]+=w.amp[fi];

          if (calCount[fi][ai]!=65535u)
            ++calCount[fi][ai];
        }
      }

      if (re>=(int32_t)total) {
        if (!finishCalibration()) {
          Serial.println("COMBO4322_CAL_FAIL");
          restartAcquisition();
        } else {
          Serial.println("COMBO4322_CAL_OK");
          candidate=false;
          rxState=RX_WAIT_SYNC;
        }
      }
      break;
    }

    case RX_WAIT_SYNC:
      if (metricOn(w)) {
        if (!candidate) {
          candidate=true;
          candidateStartUs=w.startUs;
        }

        if ((uint32_t)(w.endUs-candidateStartUs)>=15000u) {
          syncCoarseStartUs=candidateStartUs;
          clearTraining();
          storeTraining(w);
          candidate=false;
          rxState=RX_TRAINING;
        }
      } else {
        candidate=false;
      }
      break;

    case RX_TRAINING:
      storeTraining(w);

      if ((uint32_t)(w.endUs-syncCoarseStartUs)>=
          (uint32_t)SYNC_SYMBOLS*SYMBOL_US+18000u) {

        if (!refineSync()) {
          Serial.printf(
            "COMBO4322_SYNC_FAIL windows=%u\r\n",
            (unsigned)trainCount);

          restartAcquisition();
        } else {
          payloadStartUs=
              syncStartUs+
              (uint32_t)SYNC_SYMBOLS*SYMBOL_US+
              INTER_GAP_US;

          clearPayloadStats();
          rxState=RX_WAIT_PAYLOAD;

          Serial.printf(
            "COMBO4322_SYNC_OK score_x1000=%u\r\n",
            (unsigned)syncScoreX1000);
        }
      }
      break;

    case RX_WAIT_PAYLOAD:
      if ((int32_t)(micros()-payloadStartUs)>=0) {
        clearPayloadStats();
        rxState=RX_PAYLOAD;
        Serial.println("COMBO4322_RX_PAYLOAD_START");
      }
      break;

    case RX_PAYLOAD:
    default:
      break;
  }

  ESP.wdtFeed();
}

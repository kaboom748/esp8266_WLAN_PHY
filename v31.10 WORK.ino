#include <Arduino.h>
extern "C" {
  #include <user_interface.h>
}
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// ============================================================
// ESP8266 OOK v31.10 - RESTORE WORKING TX + PREAMBLE QUALIFIER + fixed RX gain 0x7C03
//
// Goals:
//   * no session, no peer, no ACK, no CSMA
//   * APWR deliberately left at 255 as requested
//   * 1000 bit/s OOK raw line rate
//   * 36-bit alternating training preamble + 12-bit start symbol 0xB38
//   * 4-to-6 bit balanced symbols
//   * length + arbitrary payload + CRC16-CCITT/FCS
//   * analog E4 receiver with RH_ASK-style fixed 8x digital PLL
//   * diagnostics kept visible
//
// This is architecture-compatible in spirit with RH_ASK/VirtualWire, but the
// application frame intentionally omits RadioHead addressing headers.
// ============================================================

// ---------------- User tuning ----------------
static constexpr uint8_t  RF_CHANNEL       = 6;
static constexpr uint16_t TONE_K           = 8;
static constexpr uint8_t  TX_ASK           = 0;
static constexpr uint8_t  TX_APWR          = 255; // deliberately NOT clamped

static constexpr uint16_t RX_GAIN_CODE     = 0x7C03u; // fixed: field logs show this is the good gain
static constexpr uint16_t IQ_N             = 128;
static constexpr uint32_t TX_SETTLE_US     = 400u;
static constexpr uint32_t RX_SETTLE_US     = 1000u;
static constexpr uint32_t POST_TX_HOLDOFF_US = 2500u;

// First target: 1000 raw OOK bits/s.
// RH_ASK's receiver PLL is intentionally clocked at 8 samples per bit.
// The E4 path on this hardware is fast enough (~30 us observed), so we
// deliberately decimate it to a fixed 125 us decision cadence at 1000 bit/s.
static constexpr uint16_t BIT_RATE          = 1000u;
static constexpr uint32_t BIT_US            = 1000000UL / BIT_RATE;
static constexpr uint8_t  RX_SAMPLES_PER_BIT = 8u;
static constexpr uint32_t RX_SAMPLE_REQUEST_US = BIT_US / RX_SAMPLES_PER_BIT;

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
static constexpr uint8_t PREAMBLE_START_WINDOW_BITS = 18u;

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
    uint32_t preambleExpired = 0;
    uint8_t preambleAltMax = 0;
    uint32_t symbolErrors = 0;
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

  AskDecoder() { resetAll(); }

  void resetAll() {
    diag_ = Diagnostics{};
    resetSearch();
    envValid_ = false;
  }

  void resetSearch() {
    active_ = false;
    bitCount_ = 0;
    rxBits_ = 0;
    resetPreambleQualifier();
    rxBufLen_ = 0;
    rxCount_ = 0;
    packetReady_ = false;
    pllRamp_ = 0;
    highSamples_ = 0;
    totalSamples_ = 0;
    prevSampleUs_ = 0;
    activeStartUs_ = 0;
  }

  void resetTimingOnly() {
    active_ = false;
    bitCount_ = 0;
    rxBits_ = 0;
    resetPreambleQualifier();
    rxBufLen_ = 0;
    rxCount_ = 0;
    packetReady_ = false;
    pllRamp_ = 0;
    highSamples_ = 0;
    totalSamples_ = 0;
    prevSampleUs_ = 0;
    activeStartUs_ = 0;
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

  void addSample(uint32_t tUs, uint32_t e) {
    ++diag_.samples;
    updateEnvelope(e);

    sampleHigh_ = classify(e, prevSampleUs_ != 0u);

    if (!prevSampleUs_) {
      prevSampleUs_ = tUs;
      lastHigh_ = sampleHigh_;
      highSamples_ = sampleHigh_ ? 1u : 0u;
      totalSamples_ = 1u;
      return;
    }
    prevSampleUs_ = tUs;

    // Integrate one fixed-cadence decision sample.
    if (sampleHigh_) ++highSamples_;
    if (totalSamples_ < 255u) ++totalSamples_;

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
      consumeBit(bit, tUs);

      highSamples_ = 0;
      totalSamples_ = 0;
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
  uint8_t preambleAltRun_ = 0;
  uint8_t preambleAge_ = 0;
  uint8_t rxBuf_[AskProto::MAX_COUNT]{};
  uint8_t rxBufLen_ = 0;
  uint8_t rxCount_ = 0;
  uint32_t activeStartUs_ = 0;
  uint8_t readyPayload_[MAX_PAYLOAD_LEN]{};
  uint8_t readyLen_ = 0;
  Diagnostics diag_{};

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

  bool classify(uint32_t e, bool useHysteresis) const {
    const uint32_t c = diag_.contrast;
    uint32_t h = c / 12u;
    if (h < 20u) h = 20u;
    if (!useHysteresis) return e >= diag_.threshold;
    if (lastHigh_) {
      const uint32_t lo = (diag_.threshold > h) ? diag_.threshold - h : 0u;
      return e >= lo;
    }
    return e > diag_.threshold + h;
  }

  void resetPreambleQualifier() {
    preambleLastBitValid_ = false;
    preambleLastBit_ = false;
    preambleArmed_ = false;
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
      preambleAge_ = 0u;
      ++diag_.preambleExpired;
    }
  }

  void dropPacket() {
    active_ = false;
    bitCount_ = 0;
    rxBufLen_ = 0;
    rxCount_ = 0;
    activeStartUs_ = 0;
    resetPreambleQualifier();
  }

  void consumeBit(bool bit, uint32_t nowUs) {
    ++diag_.bits;
    rxBits_ >>= 1;
    if (bit) rxBits_ |= 0x0800u;

    if (!active_) {
      updatePreambleQualifier(bit);

      if (rxBits_ == AskProto::START_SYMBOL) {
        ++diag_.rawStartHits;
        if (preambleArmed_) {
          ++diag_.startHits;
          active_ = true;
          bitCount_ = 0;
          rxBufLen_ = 0;
          rxCount_ = 0;
          activeStartUs_ = nowUs;
          diag_.contrastMinActive = 0xffffffffu;
          // The qualifier has served its purpose; DATA must not re-arm it.
          preambleArmed_ = false;
          preambleAltRun_ = 0u;
          preambleAge_ = 0u;
        } else {
          ++diag_.preambleRejects;
        }
      }
      return;
    }

    if (++bitCount_ < 12u) return;
    bitCount_ = 0;

    uint8_t hiNib=0, loNib=0;
    const uint8_t symHi = (uint8_t)(rxBits_ & 0x3fu);
    const uint8_t symLo = (uint8_t)((rxBits_ >> 6) & 0x3fu);
    if (!AskProto::decode6(symHi, hiNib) || !AskProto::decode6(symLo, loNib)) {
      diag_.lastBadSymHi = symHi;
      diag_.lastBadSymLo = symLo;
      ++diag_.symbolErrors;
      dropPacket();
      return;
    }
    const uint8_t b = (uint8_t)((hiNib << 4) | loNib);

    if (rxBufLen_ == 0u) {
      rxCount_ = b;
      if (rxCount_ < 3u || rxCount_ > AskProto::MAX_COUNT) {
        diag_.lastBadLen = rxCount_;
        ++diag_.lengthErrors;
        dropPacket();
        return;
      }
      diag_.lastCount = rxCount_;
    }

    if (rxBufLen_ >= AskProto::MAX_COUNT) {
      ++diag_.lengthErrors;
      dropPacket();
      return;
    }
    rxBuf_[rxBufLen_++] = b;

    if (rxBufLen_ < rxCount_) return;

    uint16_t crc = 0xffffu;
    for (uint8_t i=0; i<rxCount_; ++i)
      crc = AskProto::crcCcittUpdate(crc, rxBuf_[i]);

    if (crc != 0xf0b8u) {
      ++diag_.crcErrors;
      dropPacket();
      return;
    }

    const uint8_t payloadLen = (uint8_t)(rxCount_ - 3u);
    if (payloadLen) memcpy(readyPayload_, &rxBuf_[1], payloadLen);
    readyLen_ = payloadLen;
    packetReady_ = true;
    ++diag_.packetsOk;
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
    // Keep the hardware-proven N=128 acquisition path.  On this ESP8266,
    // N=1024 multiplied the E4 scale by about 8x but did not improve the
    // relative ON/OFF contrast, so it hurt the synchronizer in practice.
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
// TX: RH_ASK style symbol stream at 1000 raw bit/s
// ============================================================
static uint32_t txDeadlineUs = 0;

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
  txDeadlineUs += BIT_US;
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
static uint32_t lastSampleRequestUs=0;
static uint32_t sampleCount=0, sampleDtMin=0xffffffffu, sampleDtMax=0, prevSampleUs=0;
static uint64_t sampleDtSum=0;
static uint32_t minE4=0xffffffffu,maxE4=0;
static uint32_t rxGainSamples=0;

static void resetReceiverAfterOwnTx() {
  const uint32_t now=micros();
  rxHoldoffUntilUs=now+POST_TX_HOLDOFF_US;
  lastSampleRequestUs=now;
  decoder.resetTimingOnly();
  prevSampleUs=0;
  // Gain is deliberately never changed: 0x7C03 stays applied at all times.
}

static void serviceRx() {
  if (Phy::mode != Phy::MODE_RX) return;
  const uint32_t now=micros();
  if ((int32_t)(now-rxHoldoffUntilUs)<0) return;

  // Fixed-rate scheduler, like RH_ASK's hardware timer.
  // Preserve an absolute 125 us cadence rather than restarting from now.
  if (!lastSampleRequestUs) lastSampleRequestUs=now;
  const uint32_t nextDue = lastSampleRequestUs + RX_SAMPLE_REQUEST_US;
  if ((int32_t)(now-nextDue)<0) return;

  const uint32_t late = (uint32_t)(now-nextDue);
  if (late >= RX_SAMPLE_REQUEST_US * 3u) {
    lastSampleRequestUs=now;
    decoder.resetTimingOnly();
    prevSampleUs=0;
  } else {
    lastSampleRequestUs=nextDue;
  }

  const uint32_t t0=micros();
  const uint32_t e=Phy::measureE4();
  ++rxGainSamples;
  const uint32_t t1=micros();
  const uint32_t ts=t0+(uint32_t)(t1-t0)/2u;

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
    Serial.printf(" APWR=%u DS=0 TXMODE=ANA-BIT airtime=%lums\n",(unsigned)TX_APWR,
                  (unsigned long)(2u + 6u*(AskProto::PREAMBLE_SYMBOLS + 2u*(TEST_PAYLOAD_LEN+3u))));
  } else {
    Serial.println("TX ERROR");
  }
  resetReceiverAfterOwnTx();
  scheduleNextTx();
}

static void serviceApplication() {
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

  Serial.printf(
    "STAT tx=%lu/%luB rx=%lu/%luB "
    "ASK[samp=%lu bit=%lu edge=%lu start=%lu ok=%lu symBad=%lu lenBad=%lu crc=%lu to=%lu] "
    "Q[ok/start=%lu/%lu=%lu.%lu%%] "
    "PRE[arm=%lu raw=%lu accept=%lu reject=%lu exp=%lu altMax=%u] "
    "PLL[ret=%lu adv=%lu bs=%u..%u] BAD[s=%02X/%02X len=%u] "
    "E4[lo=%lu hi=%lu th=%lu sep=%lu minPkt=%lu maxSep=%lu weak=%lu raw=%lu..%lu] "
    "GAIN[fixed=%04X samples=%lu] "
    "sampdt=%lu[%lu..%lu]us WDTmax=%luus\n",
    (unsigned long)txPackets,(unsigned long)txBytes,
    (unsigned long)rxPackets,(unsigned long)rxBytes,
    (unsigned long)d.samples,(unsigned long)d.bits,(unsigned long)d.transitions,
    (unsigned long)d.startHits,(unsigned long)d.packetsOk,
    (unsigned long)d.symbolErrors,(unsigned long)d.lengthErrors,
    (unsigned long)d.crcErrors,(unsigned long)d.timeouts,
    (unsigned long)d.packetsOk,(unsigned long)d.startHits,
    (unsigned long)(qPermille/10u),(unsigned long)(qPermille%10u),
    (unsigned long)d.preambleArms,(unsigned long)d.rawStartHits,
    (unsigned long)d.startHits,(unsigned long)d.preambleRejects,
    (unsigned long)d.preambleExpired,(unsigned)d.preambleAltMax,
    (unsigned long)d.pllRetard,(unsigned long)d.pllAdvance,
    (unsigned)(d.bitSamplesMin==0xffu?0u:d.bitSamplesMin),(unsigned)d.bitSamplesMax,
    (unsigned)d.lastBadSymHi,(unsigned)d.lastBadSymLo,(unsigned)d.lastBadLen,
    (unsigned long)d.envLow,(unsigned long)d.envHigh,(unsigned long)d.threshold,
    (unsigned long)d.contrast,(unsigned long)cMin,(unsigned long)d.contrastMax,
    (unsigned long)d.weakSamples,(unsigned long)eMin,(unsigned long)maxE4,
    (unsigned)RX_GAIN_CODE,(unsigned long)rxGainSamples,
    (unsigned long)dtAvg,(unsigned long)dtMin,(unsigned long)sampleDtMax,
    (unsigned long)maxSoftWdtGapUs);
}

// ============================================================
// Arduino
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println("ESP8266 OOK v31.10 RESTORED TX + PREAMBLE QUALIFIER + FIXED GAIN 0x7C03 + 8X PLL + 4b6b + CRC16");
  Serial.printf("CH=%u APWR=%u bitrate=%u bit=%luus sample=%luus payload=%uB send=%lu..%lums\n",
                (unsigned)RF_CHANNEL,(unsigned)TX_APWR,(unsigned)BIT_RATE,
                (unsigned long)BIT_US,(unsigned long)RX_SAMPLE_REQUEST_US,(unsigned)TEST_PAYLOAD_LEN,
                (unsigned long)SEND_MIN_MS,(unsigned long)SEND_MAX_MS);
  Serial.println("NO SESSION / NO PEER / NO ACK / NO CSMA");
  Serial.println("TX RESTORED: ASK=0; ON=setAnaScale(255)+bit18; OFF=bit18 off+setAnaScale(0)");
  Serial.println("WIRE: 36-bit training + start 0xB38 + 4b6b(length+data+CRC16)");
  Serial.printf("RX START qualifier: >=%u alternating transitions, window=%u bits\n",
                (unsigned)PREAMBLE_MIN_TRANSITIONS,(unsigned)PREAMBLE_START_WINDOW_BITS);

  randomSeed(system_get_chip_id() ^ system_get_rtc_time() ^ micros());
  if(!Phy::begin()) {
    Serial.println("PHY INIT FAILED");
    while(true) delay(1000);
  }

  decoder.resetAll();
  feedSoftWdtNow();
  Phy::applyRxGain(RX_GAIN_CODE);
  rxHoldoffUntilUs=micros()+RX_SETTLE_US;
  decoder.resetEnvelope();
  scheduleNextTx();
  Serial.println("READY fixed 125us tick; RX gain fixed permanently at 0x7C03");
  printDiagnostics();
}

void loop() {
  serviceSoftWdt();
  serviceRx();
  serviceApplication();
  serviceSoftWdt();

  static uint32_t lastStatMs=0;
  const uint32_t nowMs=millis();
  if((uint32_t)(nowMs-lastStatMs)>=5000u && decoder.searching()) {
    lastStatMs=nowMs;
    printDiagnostics();
    // Do not let a long Serial print appear as a giant RX sample interval.
    prevSampleUs=0;
  }

  static uint32_t lastYieldUs=0;
  const uint32_t nowUs=micros();
  if(decoder.searching() && (uint32_t)(nowUs-lastYieldUs)>=IDLE_YIELD_US) {
    lastYieldUs=nowUs;
    yield();
  }
}

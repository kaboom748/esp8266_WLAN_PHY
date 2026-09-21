/*
  ESP8266_QAM16_LAB_DYNAMIC.ino

  Universal runtime-configurable backend for a 16-state envelope/subcarrier modem.

  Fully runtime-configurable LAB revision. Sync block length, frame training/data
  repeats, alignment padding, scheduler-gap recovery and output verbosity are
  all controlled over Serial/WebSerial.

  Modem:
      2 amplitude codes x 4 envelope phases x 2 envelope frequencies
      = 16 states = 4 bits/symbol.

  This is intentionally a LAB/DIAGNOSTIC sketch, not a production modem.
  It uses the low-level ESP8266 tone gate and IQ_EST/E4 path documented by
  the esp8266_WLAN_PHY reverse-engineering project, but the framing,
  synchronizer, correlator and diagnostics below are written specifically
  for this experiment.

  IMPORTANT:
    - Use short bench tests in a shielded/attenuated setup.
    - Do not use this to interfere with normal 2.4 GHz communications.
    - The raw analog power control is not calibrated in dBm.
    - Default TX bursts are deliberately short and APWR is conservative.

  Serial: 115200 baud

  Useful commands:
    help
    status
    rx
    txframe
    auto 500          // repeat diagnostic frame every 500 ms; 0 disables
    txstate 5 64      // transmit state 5 for 64 symbols, then return to RX
    hold 1 3000       // hold state 1 for 3000 ms, then return to RX
    phasesweep1750 3000  // repeat long 0/90/180/270 blocks at 1.75 kHz with OFF gaps
    phasesweep875 3000   // slower phase-validation sweep at 875 Hz (8 samples / 90 deg)
    amp 0 24          // two digital-scale codes, range 0..63
    iq 192            // IQ_EST integration count
    gain 7C03         // fixed RX gain code, hex
    raw 256           // capture E4 samples immediately
    rawdelay 256 2000 // wait 2000 ms, then capture 256 E4 samples
    verbose 0|1       // per-symbol diagnostic output
    set decode 1      // amplitude decoder; set ampmode 0..5 selects its metric
    syncq 3500 10     // sync quality x1000 and relative contrast permille
    stats
    clear

  State mapping:
    bit3      : amplitude code index A0/A1
    bits2..1  : envelope phase 0/90/180/270 deg
    bit0      : envelope frequency 1.75/3.5 kHz

  Subcarrier mapping at CHIP_RATE=28 kHz:
    F0 = 28 kHz / 16 = 1.75 kHz
    F1 = 28 kHz / 8  = 3.5 kHz

  At gChipsPerSymbol=16:
    symbol rate = 1750 sym/s
    raw payload rate = 7000 bit/s
*/

#ifndef HOST_COMPILE
  #include <Arduino.h>
  extern "C" {
    #include <user_interface.h>
  }
#else
  // Minimal host stubs used only for syntax checking.
  #include <stdint.h>
  #include <stddef.h>
  #include <stdio.h>
  #include <stdlib.h>
  #include <string.h>
  #include <stdarg.h>
  static uint64_t host_us = 0;
  static uint32_t micros(){ host_us += 4; return (uint32_t)host_us; }
  static uint32_t millis(){ return (uint32_t)(host_us/1000); }
  static void delay(unsigned long ms){ host_us += (uint64_t)ms*1000; }
  static void delayMicroseconds(unsigned int us){ host_us += us; }
  static void yield(){}
  static long random(long a,long b){ return a + (b>a ? rand()%(b-a) : 0); }
  enum { STATION_MODE=1, NONE_SLEEP_T=0 };
  static int wifi_station_set_auto_connect(int){ return 1; }
  static int wifi_set_opmode_current(int){ return 1; }
  static void wifi_station_disconnect(){}
  static int wifi_set_sleep_type(int){ return 1; }
  static int wifi_set_channel(int){ return 1; }
  struct ESPStub {
    void wdtFeed(){}
    const char* getResetReason(){ return "HOST"; }
  } ESP;
  struct SerialStub {
    void begin(unsigned long){}
    void println(){ puts(""); }
    void println(const char*s){ puts(s); }
    void print(const char*s){ fputs(s,stdout); }
    void print(unsigned long v){ printf("%lu",v); }
    int available(){ return 0; }
    int read(){ return -1; }
    void printf(const char*fmt,...){ va_list ap; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap); }
  } Serial;
#endif

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

// -----------------------------------------------------------------------------
// Experiment configuration
// -----------------------------------------------------------------------------
static constexpr uint8_t  RF_CHANNEL        = 6;
static constexpr uint16_t TONE_K            = 8;
static constexpr uint16_t DEFAULT_RX_GAIN   = 0x7C03u;
static constexpr uint16_t DEFAULT_IQ_N      = 32u; // faster E4 response for 1.75/3.5 kHz tests

// Raw/legacy analog setup value. It is NOT a calibrated output power.
// Keep this conservative; amplitude symbols use the digital-scale field below.
static constexpr uint8_t  TX_APWR           = 64u;
static constexpr uint32_t TX_SETTLE_US      = 400u;
static constexpr uint32_t RX_SETTLE_US      = 1000u;

static constexpr uint32_t DEFAULT_CHIP_RATE        = 28000u;
static constexpr uint8_t  DEFAULT_CHIPS_PER_SYMBOL = 16u;
static constexpr uint8_t  MAX_CHIPS_PER_SYMBOL     = 64u;
static constexpr uint8_t  SYNC_BLOCKS              = 13u;
static constexpr uint16_t MAX_SYNC_BLOCK_CHIPS     = 64u;
static constexpr uint16_t MAX_SYNC_CHIPS           = SYNC_BLOCKS * MAX_SYNC_BLOCK_CHIPS;
static constexpr uint8_t  STATE_COUNT              = 16u;
static constexpr uint8_t  MAX_TRAIN_REPEATS        = 4u;
static constexpr uint8_t  MAX_DATA_REPEATS         = 8u;
static constexpr uint16_t MAX_FRAME_ALIGN_PAD      = 256u;
static constexpr uint16_t MAX_FRAME_SAMPLES        = 8192u;

static constexpr uint16_t RAW_MAX_SAMPLES   = 1024u;
static constexpr uint32_t AUTO_MIN_MS       = 250u;

// Runtime experiment configuration. Buffer maxima / hardware addresses remain
// compile-time constants; the timing and modem knobs below can be changed from
// WebSerial without reflashing.
static uint32_t gChipRate = DEFAULT_CHIP_RATE;
static uint8_t  gChipsPerSymbol = DEFAULT_CHIPS_PER_SYMBOL;
static uint16_t gPeriodChips[2] = {16u, 8u};
static uint16_t gSweepBlockChips = 128u;
static uint16_t gSweepGapChips = 32u;
static bool     gTelemetry = true;
static uint8_t  gDecodeMode = 2u;

// Frame/synchronizer structure -- runtime controlled from WebSerial.
static uint16_t gSyncBlockChips = 32u;
static uint16_t gSyncPostGuardChips = 32u;
static uint16_t gPreGuardChips = 16u;
static uint16_t gFrameAlignPad = 32u;
static uint8_t  gTrainRepeats = 1u;
static uint8_t  gDataRepeats = 1u;
static uint8_t  gSyncTestStride = 8u;
static bool     gRecoverGaps = true;
static uint16_t gRecoverMaxPad = 128u;
static bool     gPrintErrors = false;

// Dynamic amplitude-decoder / frame-acceptance controls.
// ampmode: 0=current 3-feature pair distance, 1=pair contrast,
//          2=pair normalized contrast, 3=global contrast,
//          4=global normalized contrast, 5=hybrid pair/global contrast.
static uint8_t  gAmpMode = 0u;
static uint16_t gAmpMixPermille = 500u; // mode 5: pair weight, 0..1000
static uint8_t  gFrameMinMatches = 0u;  // expected phase/freq matches in training sweep
static uint32_t gFrameMinTrainQ = 0u;   // average training shapeQ gate
static uint16_t gAlignAcceptMin = 0u;
static uint16_t gAlignAcceptMax = MAX_FRAME_ALIGN_PAD;

static inline uint32_t chipUsFloor() { return 1000000UL / gChipRate; }
static inline uint32_t chipUsRem()   { return 1000000UL % gChipRate; }
static inline uint32_t chipUsCeil()  { const uint32_t f=chipUsFloor(); return f + (chipUsRem()?1u:0u); }
static inline uint16_t syncChipsWanted() { return (uint16_t)(SYNC_BLOCKS * gSyncBlockChips); }
static inline uint16_t trainSymbolsWanted() { return (uint16_t)(STATE_COUNT * gTrainRepeats); }
static inline uint16_t dataSymbolsWanted() { return (uint16_t)(STATE_COUNT * gDataRepeats); }
static inline uint32_t payloadSamplesWanted32() { return (uint32_t)(trainSymbolsWanted()+dataSymbolsWanted()) * gChipsPerSymbol; }
static inline uint16_t frameSamplesWanted() { return (uint16_t)(payloadSamplesWanted32() + gFrameAlignPad); }
static inline bool frameConfigFits(uint8_t symbolChips,uint8_t trainReps,uint8_t dataReps,uint16_t alignPad) {
  return (uint32_t)STATE_COUNT*(trainReps+dataReps)*symbolChips + alignPad <= MAX_FRAME_SAMPLES;
}
static inline uint32_t diagnosticFrameChips() {
  return (uint32_t)gPreGuardChips + syncChipsWanted() + gSyncPostGuardChips +
         (uint32_t)(trainSymbolsWanted()+dataSymbolsWanted())*gChipsPerSymbol + gFrameAlignPad;
}
static inline uint32_t diagnosticFrameMs() {
  return (uint32_t)(((uint64_t)diagnosticFrameChips()*1000u + gChipRate-1u)/gChipRate);
}

static constexpr char SYNC_PATTERN[SYNC_BLOCKS + 1] = "1111100110101";
static inline bool syncExpectedOn(uint16_t chip) { return SYNC_PATTERN[chip / gSyncBlockChips] == '1'; }

// Runtime tuning knobs.
static uint8_t  gAskCode[2] = {0u, 24u};
static uint16_t gIqN = DEFAULT_IQ_N;
static uint16_t gRxGain = DEFAULT_RX_GAIN;
static uint32_t gSyncQualityMin = 800u;       // contrast/residual * 1000
static uint32_t gSyncRelMinPermille = 100u;   // contrast/mean * 1000
static uint32_t gSyncContrastMin = 10000u;     // absolute E4 ON-OFF contrast
static bool     gVerbose = true;

// -----------------------------------------------------------------------------
// Hardware layer
// -----------------------------------------------------------------------------
namespace Radio {
  static constexpr uint32_t TONE1       = 0x600005B8u;
  static constexpr uint32_t TONE2       = 0x600005BCu;
  static constexpr uint32_t TONE3       = 0x600005C4u;
  static constexpr uint32_t GATE_MASK   = 0x00040000u;
  static constexpr uint32_t K_MASK      = 0x000003FFu;
  static constexpr uint32_t SCALE_MASK  = 0x0003FC00u;
  static constexpr uint8_t  SCALE_SHIFT = 10u;
  static constexpr uint32_t PBUS_CMD    = 0x60000594u;
  static constexpr uint32_t PBUS_STATUS = 0x600005A0u;
  static constexpr uint32_t RX_CTRL     = 0x60009B08u;
  static constexpr uint32_t RX_STOP     = 0x08000000u;
  static constexpr uint32_t E4_ADDR     = 0x600005E4u;

  static constexpr uint32_t ROM_IQ_DISABLE   = 0x40006400u;
  static constexpr uint32_t ROM_IQ_ENABLE    = 0x40006430u;
  static constexpr uint32_t ROM_TXCLK        = 0x4000650Cu;
  static constexpr uint32_t ROM_RXCLK        = 0x40006550u;
  static constexpr uint32_t ROM_ANA_SCALE    = 0x4000678Cu;
  static constexpr uint32_t ROM_SET_RX_GAIN  = 0x4000754Cu;
  static constexpr uint32_t ROM_RX_OFF       = 0x40007688u;
  static constexpr uint32_t ROM_RX_ON        = 0x400076CCu;

  using IqEnableFn  = void    (*)(uint32_t, uint32_t);
  using IqDisableFn = void    (*)(void);
  using ClockFn     = void    (*)(int);
  using AnaScaleFn  = uint8_t (*)(uint8_t);
  using RxGainFn    = void    (*)(uint32_t);
  using RxOffFn     = void    (*)(uint32_t);
  using RxOnFn      = void    (*)(void);

#ifndef HOST_COMPILE
  static IqEnableFn  iqEnable = reinterpret_cast<IqEnableFn>(ROM_IQ_ENABLE);
  static IqDisableFn iqDisable = reinterpret_cast<IqDisableFn>(ROM_IQ_DISABLE);
  static ClockFn     txClock = reinterpret_cast<ClockFn>(ROM_TXCLK);
  static ClockFn     rxClock = reinterpret_cast<ClockFn>(ROM_RXCLK);
  static AnaScaleFn  anaScale = reinterpret_cast<AnaScaleFn>(ROM_ANA_SCALE);
  static RxGainFn    setRxGainRom = reinterpret_cast<RxGainFn>(ROM_SET_RX_GAIN);
  static RxOffFn     rxOff = reinterpret_cast<RxOffFn>(ROM_RX_OFF);
  static RxOnFn      rxOn = reinterpret_cast<RxOnFn>(ROM_RX_ON);
#endif

  enum Mode : uint8_t { RX, TX };
  static Mode mode = RX;
  static uint32_t toneShadow = 0u;
  static uint8_t lastAnaReturn = 0u;

  static inline void memw() {
#ifndef HOST_COMPILE
    __asm__ volatile("memw" ::: "memory");
#else
    asm volatile("" ::: "memory");
#endif
  }

  static inline uint32_t rd32(uint32_t a) {
#ifndef HOST_COMPILE
    return *(volatile uint32_t*)a;
#else
    (void)a; return toneShadow;
#endif
  }

  static inline void wr32(uint32_t a, uint32_t v) {
#ifndef HOST_COMPILE
    *(volatile uint32_t*)a = v;
#else
    (void)a; toneShadow = v;
#endif
  }

  static bool pbusWrite(uint8_t sel, uint8_t bank, uint16_t value) {
#ifdef HOST_COMPILE
    (void)sel; (void)bank; (void)value; return true;
#else
    uint32_t cmd = rd32(PBUS_CMD);
    cmd &= 0xFFFF0001u;
    cmd |= ((uint32_t)(bank & 3u) << 14);
    cmd |= ((uint32_t)(value & 0x1FFu) << 5);
    cmd |= ((uint32_t)(sel & 7u) << 2);
    cmd |= 2u;
    memw(); wr32(PBUS_CMD, cmd); memw();

    const uint32_t started = micros();
    while (rd32(PBUS_STATUS) & 0x80000000u) {
      if ((uint32_t)(micros() - started) > 2000u) {
        wr32(PBUS_CMD, rd32(PBUS_CMD) & ~2u); memw();
        return false;
      }
    }
    wr32(PBUS_CMD, rd32(PBUS_CMD) & ~2u); memw();
    return true;
#endif
  }

  static inline void isolateNormalRxDigital() {
    uint32_t v = rd32(RX_CTRL);
    wr32(RX_CTRL, v | RX_STOP); memw();
    v = rd32(PBUS_CMD);
    wr32(PBUS_CMD, v | 1u); memw();
  }

  static bool txAnalogPathOn() {
    // Raw PBUS recipe experimentally used by the referenced project.
    return pbusWrite(2,1,1) && pbusWrite(7,1,95) &&
           pbusWrite(1,1,127) && pbusWrite(6,1,127);
  }

  static void txAnalogPathOff() {
    pbusWrite(6,1,0);
    pbusWrite(1,1,12);
    pbusWrite(2,1,0);
  }

  static void setRxGain(uint16_t code) {
#ifndef HOST_COMPILE
    memw(); setRxGainRom((uint32_t)code); memw();
#else
    (void)code;
#endif
  }

  static void programTone(uint8_t digitalScale, bool gateOn=false) {
    const uint8_t encoded = (uint8_t)(0u - digitalScale);
    uint32_t v = rd32(TONE1) & 0xF0000000u;
    v |= ((uint32_t)TONE_K & K_MASK);
    v |= (((uint32_t)encoded << SCALE_SHIFT) & SCALE_MASK);
    if (gateOn) v |= GATE_MASK;
    else        v &= ~GATE_MASK;
    toneShadow = v;
    memw(); wr32(TONE1, v); memw();

    // Ensure the other test-tone slots are not gated.
    wr32(TONE2, rd32(TONE2) & ~GATE_MASK); memw();
    wr32(TONE3, rd32(TONE3) & ~GATE_MASK); memw();
  }

  static inline void gate(bool on) {
    uint32_t v = toneShadow;
    if (on) v |= GATE_MASK;
    else    v &= ~GATE_MASK;
    toneShadow = v;
    wr32(TONE1, v); memw();
  }

  static inline void setDigitalScale(uint8_t ds) {
    if (ds > 63u) ds = 63u;
    const uint8_t encoded = (uint8_t)(0u - ds);
    uint32_t v = toneShadow;
    v &= ~SCALE_MASK;
    v |= (((uint32_t)encoded << SCALE_SHIFT) & SCALE_MASK);
    toneShadow = v;
    wr32(TONE1, v); memw();
  }

  static bool begin() {
#ifdef HOST_COMPILE
    mode = RX;
    return true;
#else
    wifi_station_set_auto_connect(0);
    if (!wifi_set_opmode_current(STATION_MODE)) return false;
    delay(250);
    wifi_station_disconnect();
    if (!wifi_set_sleep_type(NONE_SLEEP_T)) return false;
    if (!wifi_set_channel(RF_CHANNEL)) return false;
    delay(80);

    memw(); rxOn(); memw();
    memw(); rxClock(1); memw();
    delay(10);
    isolateNormalRxDigital();
    setRxGain(gRxGain);
    delayMicroseconds(RX_SETTLE_US);
    mode = RX;
    return true;
#endif
  }

  static bool enterTx() {
    if (mode == TX) return true;
#ifndef HOST_COMPILE
    memw(); iqDisable(); memw();
    memw(); rxClock(0); memw();
    memw(); rxOff(1u); memw();
#endif
    isolateNormalRxDigital();
    if (!txAnalogPathOn()) return false;
#ifndef HOST_COMPILE
    memw(); lastAnaReturn = anaScale(TX_APWR); memw();
    memw(); txClock(1); memw();
#else
    lastAnaReturn = 0;
#endif
    programTone(gAskCode[0], false);
    delayMicroseconds(TX_SETTLE_US);
    mode = TX;
    return true;
  }

  static void enterRx() {
    if (mode == RX) {
      setRxGain(gRxGain);
      return;
    }
    gate(false);
#ifndef HOST_COMPILE
    memw(); txClock(0); memw();
#endif
    txAnalogPathOff();
#ifndef HOST_COMPILE
    memw(); rxOn(); memw();
    memw(); rxClock(1); memw();
#endif
    isolateNormalRxDigital();
    setRxGain(gRxGain);
    delayMicroseconds(RX_SETTLE_US);
    mode = RX;
  }

  static uint32_t measureE4() {
#ifdef HOST_COMPILE
    return 100000u;
#else
    memw(); iqEnable(1u, gIqN); memw();
    const uint32_t e = rd32(E4_ADDR);
    memw(); iqDisable(); memw();
    return e;
#endif
  }

  static uint32_t toneReadback() { return rd32(TONE1); }
}

// -----------------------------------------------------------------------------
// General statistics
// -----------------------------------------------------------------------------
struct Stats {
  uint32_t rxSamples = 0;
  uint32_t rxLateResets = 0;
  uint32_t rxLateMaxUs = 0;
  uint32_t dtMinUs = 0xffffffffu;
  uint32_t dtMaxUs = 0u;
  uint64_t dtSumUs = 0u;
  uint32_t dtCount = 0u;
  uint32_t e4Min = 0xffffffffu;
  uint32_t e4Max = 0u;
  uint32_t e4MeasureMinUs = 0xffffffffu;
  uint32_t e4MeasureMaxUs = 0u;
  uint64_t e4MeasureSumUs = 0u;
  uint32_t e4MeasureCount = 0u;

  uint32_t syncTests = 0;
  uint32_t syncHits = 0;
  uint32_t frames = 0;
  uint32_t frameDrops = 0;
  uint32_t symbols = 0;
  uint32_t symbolErrors = 0;
  uint32_t ampErrors = 0;
  uint32_t phaseErrors = 0;
  uint32_t freqErrors = 0;
  uint32_t weakMarginSymbols = 0;
  uint32_t frameGapEvents = 0;
  uint32_t framePadChips = 0;
  uint32_t framePadMax = 0;
  uint32_t frameGapAborts = 0;

  uint32_t txChips = 0;
  uint32_t txLateMaxUs = 0;
  uint64_t txLateSumUs = 0;
  uint32_t txLateCount = 0;
};
static Stats gStats;

static void clearStats() { gStats = Stats{}; }

// -----------------------------------------------------------------------------
// WDT helper
// -----------------------------------------------------------------------------
static uint32_t gLastWdtFeedUs = 0u;
static inline void feedWdt() {
  const uint32_t now = micros();
  if (gLastWdtFeedUs == 0u || (uint32_t)(now - gLastWdtFeedUs) >= 15000u) {
#ifndef HOST_COMPILE
    ESP.wdtFeed();
#endif
    gLastWdtFeedUs = now;
  }
}

// -----------------------------------------------------------------------------
// TX scheduler and symbol generator
// -----------------------------------------------------------------------------
static uint32_t gTxDeadlineUs = 0u;
static uint32_t gTxFrac = 0u;

static inline uint32_t nextChipIntervalUsTx() {
  uint32_t dt = chipUsFloor();
  gTxFrac += chipUsRem();
  if (gTxFrac >= gChipRate) {
    gTxFrac -= gChipRate;
    ++dt;
  }
  return dt;
}

static inline void txWaitUntil(uint32_t deadline) {
  while ((int32_t)(micros() - deadline) < 0) feedWdt();
}

static inline void txChip(bool on) {
  // Edge at the absolute chip deadline, then remain in this function until
  // the chip is complete.  This matters because amplitude changes are made
  // only at symbol boundaries; returning at the start of the last chip would
  // otherwise change digital_scale one chip too early.
  txWaitUntil(gTxDeadlineUs);
  const uint32_t now = micros();
  const uint32_t late = (uint32_t)(now - gTxDeadlineUs);
  if (late > gStats.txLateMaxUs) gStats.txLateMaxUs = late;
  gStats.txLateSumUs += late;
  ++gStats.txLateCount;

  Radio::gate(on);
  ++gStats.txChips;
  gTxDeadlineUs += nextChipIntervalUsTx();
  txWaitUntil(gTxDeadlineUs);
}

static inline bool symbolChip(uint8_t freq, uint8_t phase, uint16_t chip) {
  const uint16_t period = gPeriodChips[freq & 1u];
  const uint16_t quarter = period / 4u;
  const uint16_t shift = (uint16_t)(phase & 3u) * quarter;
  const uint16_t p = (uint16_t)((chip + shift) % period);
  return p < (period / 2u);
}

static void txStateSymbol(uint8_t state) {
  const uint8_t amp   = (state >> 3) & 1u;
  const uint8_t phase = (state >> 1) & 3u;
  const uint8_t freq  = state & 1u;
  Radio::setDigitalScale(gAskCode[amp]);
  for (uint16_t c=0; c<gChipsPerSymbol; ++c)
    txChip(symbolChip(freq, phase, c));
}

static void txSync() {
  Radio::setDigitalScale(gAskCode[1]);
  for (uint16_t i=0; i<syncChipsWanted(); ++i)
    txChip(syncExpectedOn(i));
}

static void txGuard(uint16_t chips) {
  Radio::gate(false);
  for (uint16_t i=0; i<chips; ++i) txChip(false);
}

static bool beginTimedTx() {
  feedWdt();
  if (!Radio::enterTx()) return false;
  gTxFrac = 0u;
  gTxDeadlineUs = micros() + 100u;
  return true;
}

static void endTimedTx() {
  txWaitUntil(gTxDeadlineUs);
  Radio::gate(false);
  Radio::enterRx();
  feedWdt();
}

static bool transmitDiagnosticFrame() {
  if (!beginTimedTx()) return false;

  txGuard(gPreGuardChips);
  txSync();
  // Leave a quiet alignment cushion after the slow preamble. The receiver
  // captures this lead-in and searches the exact training-symbol boundary.
  txGuard(gSyncPostGuardChips);

  // CAL16 training: two complete 0..15 sweeps.  This captures the real
  // E4 response for every amplitude/phase/frequency combination instead of
  // assuming that amplitude behaves identically at every phase.
  for (uint8_t r=0; r<gTrainRepeats; ++r)
    for (uint8_t s=0; s<STATE_COUNT; ++s)
      txStateSymbol(s);

  for (uint8_t r=0; r<gDataRepeats; ++r)
    for (uint8_t s=0; s<STATE_COUNT; ++s)
      txStateSymbol(s);

  // Extra tail gives the aligned receiver enough samples even when the sync
  // detector fires a few chips before/after the ideal preamble boundary.
  txGuard(gFrameAlignPad);
  endTimedTx();
  return true;
}

static bool transmitRepeatedState(uint8_t state, uint16_t symbols) {
  if (state >= 16u) return false;
  if (symbols == 0u) return true;
  if (symbols > 256u) symbols = 256u; // keep bursts short
  if (!beginTimedTx()) return false;
  txGuard(16u);
  for (uint16_t i=0; i<symbols; ++i) txStateSymbol(state);
  txGuard(16u);
  endTimedTx();
  return true;
}

// Hold one known 16-state symbol for a human-friendly time window so a second
// ESP can arm a RAW E4 capture without racing a ~100 ms burst.  The limit is
// intentionally short to keep this a bench diagnostic, not a continuous TX.
static bool transmitHeldState(uint8_t state, uint32_t durationMs) {
  if (state >= 16u) return false;
  if (durationMs < 100u) durationMs = 100u;
  if (durationMs > 10000u) durationMs = 10000u;

  const uint32_t symbolRate = gChipRate / gChipsPerSymbol;
  uint32_t symbols = (uint32_t)(((uint64_t)durationMs * symbolRate + 999u) / 1000u);
  if (symbols == 0u) symbols = 1u;

  if (!beginTimedTx()) return false;
  txGuard(16u);
  for (uint32_t i=0; i<symbols; ++i) txStateSymbol(state);
  txGuard(16u);
  endTimedTx();
  return true;
}

// Generic phase sweep used by the WebSerial frontend.  The period is supplied
// in chips and must be divisible by 4 so 0/90/180/270 degrees are exact.
static bool transmitPhaseSweep(uint16_t periodChips, uint32_t durationMs, uint8_t ampIndex) {
  if (periodChips < 4u || periodChips > 128u || (periodChips & 3u)) return false;
  if (durationMs < 100u) durationMs = 100u;
  if (durationMs > 10000u) durationMs = 10000u;
  ampIndex &= 1u;

  const uint32_t targetChips = (uint32_t)(((uint64_t)durationMs * gChipRate) / 1000u);
  const uint16_t half = periodChips / 2u;
  const uint16_t quarter = periodChips / 4u;
  uint32_t sentChips = 0u;

  if (!beginTimedTx()) return false;
  Radio::setDigitalScale(gAskCode[ampIndex]);

  while (sentChips < targetChips) {
    for (uint8_t phase=0u; phase<4u && sentChips < targetChips; ++phase) {
      for (uint16_t i=0u; i<gSweepGapChips && sentChips < targetChips; ++i) {
        txChip(false); ++sentChips;
      }
      const uint16_t shift=(uint16_t)phase*quarter;
      for (uint16_t c=0u; c<gSweepBlockChips && sentChips < targetChips; ++c) {
        const uint16_t pos=(uint16_t)((c+shift)%periodChips);
        txChip(pos<half); ++sentChips;
      }
    }
    feedWdt();
  }
  txGuard(gSweepGapChips);
  endTimedTx();
  return true;
}

static bool transmitPhaseSweep1750(uint32_t durationMs) { return transmitPhaseSweep(16u,durationMs,0u); }
static bool transmitPhaseSweep875(uint32_t durationMs)  { return transmitPhaseSweep(32u,durationMs,0u); }

// -----------------------------------------------------------------------------
// RX sampler, sync detector and diagnostic frame decoder
// -----------------------------------------------------------------------------
static uint32_t gRxDeadlineUs = 0u;
static uint32_t gRxFrac = 0u;
static uint32_t gPrevSampleTs = 0u;

static inline uint32_t nextChipIntervalUsRx() {
  uint32_t dt = chipUsFloor();
  gRxFrac += chipUsRem();
  if (gRxFrac >= gChipRate) {
    gRxFrac -= gChipRate;
    ++dt;
  }
  return dt;
}

static uint32_t gSyncRing[MAX_SYNC_CHIPS]{};
static uint16_t gSyncPos = 0u;
static uint16_t gSyncCount = 0u;
static uint8_t  gSyncTestDecim = 0u;

static bool gCollectingFrame = false;
static bool gFrameReady = false;
static uint16_t gFrameIndex = 0u;
static uint32_t gFrame[MAX_FRAME_SAMPLES]{};
static uint16_t gFramePendingPad = 0u;
static uint32_t gFrameLastE4 = 0u;
static bool gFrameHaveLastE4 = false;
static uint32_t gLastSyncContrast = 0u;
static uint32_t gLastSyncQuality = 0u;
static uint32_t gLastSyncRelPermille = 0u;

static uint16_t gRawWanted = 0u;
static uint16_t gRawCount = 0u;
static bool gRawReady = false;
static bool gRawTriggered = true;       // normal raw starts immediately
static uint32_t gRawTriggerThreshold = 0u;
static uint16_t gRawDelayWanted = 0u;
static uint32_t gRawDelayDeadlineMs = 0u;
static uint32_t gRawE4[RAW_MAX_SAMPLES]{};
static uint16_t gRawDt[RAW_MAX_SAMPLES]{};

static void resetRxSearch() {
  gSyncPos = 0u;
  gSyncCount = 0u;
  gSyncTestDecim = 0u;
  gCollectingFrame = false;
  gFrameReady = false;
  gFrameIndex = 0u;
  gFramePendingPad = 0u;
  gFrameLastE4 = 0u;
  gFrameHaveLastE4 = false;
}

static uint32_t uabsdiff(uint32_t a, uint32_t b) {
  return a > b ? a-b : b-a;
}

static bool testSyncWindow() {
  const uint16_t syncChips=syncChipsWanted();
  if (gSyncCount < syncChips) return false;
  ++gStats.syncTests;
  uint64_t onSum=0, offSum=0; uint16_t onN=0, offN=0;
  for (uint16_t i=0;i<syncChips;++i) {
    const uint16_t idx=(uint16_t)((gSyncPos+i)%syncChips);
    if(syncExpectedOn(i)){onSum+=gSyncRing[idx];++onN;} else {offSum+=gSyncRing[idx];++offN;}
  }
  if(!onN||!offN)return false;
  const uint32_t onMean=(uint32_t)(onSum/onN), offMean=(uint32_t)(offSum/offN);
  if(onMean<=offMean)return false;
  const uint32_t contrast=onMean-offMean;
  const uint32_t grandMean=(onMean+offMean)/2u;
  if(contrast<gSyncContrastMin)return false;
  uint64_t residualSum=0;
  for(uint16_t i=0;i<syncChips;++i){
    const uint16_t idx=(uint16_t)((gSyncPos+i)%syncChips);
    residualSum+=uabsdiff(gSyncRing[idx],syncExpectedOn(i)?onMean:offMean);
  }
  const uint32_t residual=(uint32_t)(residualSum/syncChips);
  const uint32_t quality=(uint32_t)(((uint64_t)contrast*1000u)/(residual+1u));
  const uint32_t rel=(uint32_t)(((uint64_t)contrast*1000u)/(grandMean+1u));
  if(quality<gSyncQualityMin||rel<gSyncRelMinPermille)return false;
  gLastSyncContrast=contrast; gLastSyncQuality=quality; gLastSyncRelPermille=rel; ++gStats.syncHits; return true;
}

struct ShapeResult {
  uint8_t freq = 0;
  uint8_t phase = 0;
  int64_t best = 0;
  int64_t second = 0;
  uint32_t contrast = 0;
  uint32_t residual = 0;
  uint32_t shapeQ = 0;
  uint32_t marginPermille = 0;
  uint32_t meanOn = 0;
  uint32_t meanOff = 0;
};

// Manual prototype: Arduino's .ino preprocessor can otherwise auto-generate
// this prototype before ShapeResult is declared, causing:
//   'ShapeResult' does not name a type
static ShapeResult analyzeShape(const uint32_t *x);
static inline uint32_t normalizedContrast(const ShapeResult &r);
static uint8_t decodeLegacyAmplitude(const ShapeResult &r,
                                     const uint32_t muContrast[STATE_COUNT]);
static uint8_t decodePairAmplitude(const ShapeResult &r,
                                   const uint32_t muContrast[STATE_COUNT],
                                   const uint32_t muOn[STATE_COUNT],
                                   const uint32_t muOff[STATE_COUNT]);
static uint8_t decodeDynamicAmplitude(const ShapeResult &r,
                                      const uint32_t muContrast[STATE_COUNT],
                                      const uint32_t muOn[STATE_COUNT],
                                      const uint32_t muOff[STATE_COUNT],
                                      const uint32_t muNorm[STATE_COUNT]);

static ShapeResult analyzeShape(const uint32_t *x) {
  ShapeResult out{};
  bool first=true;
  int64_t best = -(1LL<<60), second = -(1LL<<60);
  uint8_t bestF=0, bestP=0;

  for (uint8_t f=0; f<2u; ++f) {
    for (uint8_t p=0; p<4u; ++p) {
      int64_t score=0;
      for (uint16_t c=0; c<gChipsPerSymbol; ++c)
        score += symbolChip(f,p,c) ? (int64_t)x[c] : -(int64_t)x[c];

      if (first || score > best) {
        second = best;
        best = score;
        bestF=f; bestP=p;
        first=false;
      } else if (score > second) {
        second=score;
      }
    }
  }

  uint64_t onSum=0, offSum=0;
  uint16_t onN=0, offN=0;
  for (uint16_t c=0; c<gChipsPerSymbol; ++c) {
    if (symbolChip(bestF,bestP,c)) { onSum += x[c]; ++onN; }
    else                           { offSum += x[c]; ++offN; }
  }
  const uint32_t onMean = onN ? (uint32_t)(onSum/onN) : 0u;
  const uint32_t offMean = offN ? (uint32_t)(offSum/offN) : 0u;
  const uint32_t contrast = onMean > offMean ? onMean-offMean : 0u;

  uint64_t residualSum=0;
  for (uint16_t c=0; c<gChipsPerSymbol; ++c)
    residualSum += uabsdiff(x[c], symbolChip(bestF,bestP,c) ? onMean : offMean);
  const uint32_t residual = (uint32_t)(residualSum/gChipsPerSymbol);

  out.freq=bestF;
  out.phase=bestP;
  out.best=best;
  out.second=second;
  out.meanOn=onMean;
  out.meanOff=offMean;
  out.contrast=contrast;
  out.residual=residual;
  out.shapeQ=(uint32_t)(((uint64_t)contrast*1000u)/(residual+1u));
  const uint64_t denom=(uint64_t)(best>=0?best:-best)+1u;
  out.marginPermille=(uint32_t)(((uint64_t)(best-second)*1000u)/denom);
  return out;
}

static void correlationIQ(const uint32_t *x, uint8_t freq, int32_t &iOut, int32_t &qOut) {
  uint64_t sum=0u;
  for (uint16_t c=0;c<gChipsPerSymbol;++c) sum += x[c];
  const int64_t mean=(int64_t)(sum/gChipsPerSymbol);
  int64_t si=0, sq=0;
  for (uint16_t c=0;c<gChipsPerSymbol;++c) {
    const int64_t d=(int64_t)x[c]-mean;
    si += symbolChip(freq,0u,c) ? d : -d;
    sq += symbolChip(freq,1u,c) ? d : -d;
  }
  si /= gChipsPerSymbol; sq /= gChipsPerSymbol;
  if(si>2147483647LL)si=2147483647LL; if(si<-2147483647LL)si=-2147483647LL;
  if(sq>2147483647LL)sq=2147483647LL; if(sq<-2147483647LL)sq=-2147483647LL;
  iOut=(int32_t)si; qOut=(int32_t)sq;
}

static inline int64_t sabs64(int64_t v) { return v < 0 ? -v : v; }

static uint16_t trainingSymbolIndex(uint8_t rep, uint8_t state) {
  return (uint16_t)rep * STATE_COUNT + state;
}

static inline uint32_t normalizedContrast(const ShapeResult &r) {
  return (uint32_t)(((uint64_t)r.contrast * 2000u) / ((uint64_t)r.meanOn + r.meanOff + 1u));
}

static void buildCal16(uint16_t frameBase,
                       uint32_t muContrast[STATE_COUNT],
                       uint32_t muOn[STATE_COUNT],
                       uint32_t muOff[STATE_COUNT],
                       uint32_t muNorm[STATE_COUNT]) {
  for (uint8_t s=0; s<STATE_COUNT; ++s) {
    uint64_t sc=0, so=0, sf=0, sn=0;
    for (uint8_t rep=0; rep<gTrainRepeats; ++rep) {
      const uint16_t ti=trainingSymbolIndex(rep,s);
      const ShapeResult tr=analyzeShape(&gFrame[frameBase + ti*gChipsPerSymbol]);
      sc += tr.contrast;
      so += tr.meanOn;
      sf += tr.meanOff;
      sn += normalizedContrast(tr);
    }
    muContrast[s]=(uint32_t)(sc/gTrainRepeats);
    muOn[s]=(uint32_t)(so/gTrainRepeats);
    muOff[s]=(uint32_t)(sf/gTrainRepeats);
    muNorm[s]=(uint32_t)(sn/gTrainRepeats);
  }
}

static uint8_t decodeLegacyAmplitude(const ShapeResult &r,
                                     const uint32_t muContrast[STATE_COUNT]) {
  const uint8_t s0=(uint8_t)((0u<<3)|(0u<<1)|(r.freq&1u));
  const uint8_t s1=(uint8_t)((1u<<3)|(0u<<1)|(r.freq&1u));
  return uabsdiff(r.contrast,muContrast[s1]) < uabsdiff(r.contrast,muContrast[s0]) ? 1u : 0u;
}

static uint8_t decodePairAmplitude(const ShapeResult &r,
                                   const uint32_t muContrast[STATE_COUNT],
                                   const uint32_t muOn[STATE_COUNT],
                                   const uint32_t muOff[STATE_COUNT]) {
  const uint8_t base=(uint8_t)(((r.phase&3u)<<1)|(r.freq&1u));
  const uint8_t s0=base;
  const uint8_t s1=(uint8_t)(0x8u|base);

  // State-specific three-feature distance.  Contrast gets double weight,
  // while ON/OFF centroids help when E4 has a phase-dependent baseline.
  const uint64_t d0=(uint64_t)uabsdiff(r.contrast,muContrast[s0])*2u
                   +uabsdiff(r.meanOn,muOn[s0])+uabsdiff(r.meanOff,muOff[s0]);
  const uint64_t d1=(uint64_t)uabsdiff(r.contrast,muContrast[s1])*2u
                   +uabsdiff(r.meanOn,muOn[s1])+uabsdiff(r.meanOff,muOff[s1]);
  return d1 < d0 ? 1u : 0u;
}

static uint8_t decodeDynamicAmplitude(const ShapeResult &r,
                                      const uint32_t muContrast[STATE_COUNT],
                                      const uint32_t muOn[STATE_COUNT],
                                      const uint32_t muOff[STATE_COUNT],
                                      const uint32_t muNorm[STATE_COUNT]) {
  if (gAmpMode==0u) return decodePairAmplitude(r,muContrast,muOn,muOff);

  const uint8_t base=(uint8_t)(((r.phase&3u)<<1)|(r.freq&1u));
  const uint8_t s0=base, s1=(uint8_t)(0x8u|base);
  const uint32_t rn=normalizedContrast(r);

  if (gAmpMode==1u)
    return uabsdiff(r.contrast,muContrast[s1]) < uabsdiff(r.contrast,muContrast[s0]) ? 1u : 0u;
  if (gAmpMode==2u)
    return uabsdiff(rn,muNorm[s1]) < uabsdiff(rn,muNorm[s0]) ? 1u : 0u;

  uint64_t gc0=0,gc1=0,gn0=0,gn1=0;
  for (uint8_t b=0;b<8u;++b) {
    gc0 += muContrast[b]; gc1 += muContrast[8u+b];
    gn0 += muNorm[b];     gn1 += muNorm[8u+b];
  }
  const uint32_t gC0=(uint32_t)(gc0/8u), gC1=(uint32_t)(gc1/8u);
  const uint32_t gN0=(uint32_t)(gn0/8u), gN1=(uint32_t)(gn1/8u);
  if (gAmpMode==3u)
    return uabsdiff(r.contrast,gC1) < uabsdiff(r.contrast,gC0) ? 1u : 0u;
  if (gAmpMode==4u)
    return uabsdiff(rn,gN1) < uabsdiff(rn,gN0) ? 1u : 0u;

  // Hybrid contrast classifier.  Pair-specific calibration follows the
  // phase/frequency response; the global centroid suppresses noisy pair
  // calibration. ampmix is the pair weight in permille.
  const uint64_t w=gAmpMixPermille, gw=1000u-w;
  const uint64_t d0=(uint64_t)uabsdiff(r.contrast,muContrast[s0])*w
                   +(uint64_t)uabsdiff(r.contrast,gC0)*gw;
  const uint64_t d1=(uint64_t)uabsdiff(r.contrast,muContrast[s1])*w
                   +(uint64_t)uabsdiff(r.contrast,gC1)*gw;
  return d1 < d0 ? 1u : 0u;
}

// Full 16-state template distance.  Both the received symbol and the training
// template are centered by their own mean.  That removes slow ambient/DC E4
// drift while preserving modulation depth, phase and subcarrier shape.
static uint64_t centeredTemplateDistance(const uint32_t *x, uint8_t state, uint16_t frameBase) {
  int64_t xMean=0, tMean=0;
  for (uint16_t c=0; c<gChipsPerSymbol; ++c) {
    xMean += x[c];
    uint64_t tv=0;
    for (uint8_t rep=0; rep<gTrainRepeats; ++rep) {
      const uint16_t ti=trainingSymbolIndex(rep,state);
      tv += gFrame[(uint16_t)(frameBase + ti*gChipsPerSymbol+c)];
    }
    tMean += (int64_t)(tv/gTrainRepeats);
  }
  xMean /= gChipsPerSymbol;
  tMean /= gChipsPerSymbol;

  uint64_t dist=0;
  for (uint16_t c=0; c<gChipsPerSymbol; ++c) {
    uint64_t tv=0;
    for (uint8_t rep=0; rep<gTrainRepeats; ++rep) {
      const uint16_t ti=trainingSymbolIndex(rep,state);
      tv += gFrame[(uint16_t)(frameBase + ti*gChipsPerSymbol+c)];
    }
    const int64_t t=(int64_t)(tv/gTrainRepeats)-tMean;
    const int64_t v=(int64_t)x[c]-xMean;
    dist += (uint64_t)sabs64(v-t);
  }
  return dist;
}

static uint8_t decodeTemplate16(const uint32_t *x, uint32_t &marginPermille, uint16_t frameBase) {
  uint64_t best=~0ULL, second=~0ULL;
  uint8_t bestState=0u;
  for (uint8_t s=0; s<STATE_COUNT; ++s) {
    const uint64_t d=centeredTemplateDistance(x,s,frameBase);
    if (d < best) {
      second=best;
      best=d;
      bestState=s;
    } else if (d < second) {
      second=d;
    }
  }
  if (second==~0ULL) second=best;
  marginPermille=(uint32_t)(((second-best)*1000ULL)/(second+1ULL));
  return bestState;
}

static uint16_t findTrainingAlignment(uint8_t &bestMatchesOut, uint32_t &bestTrainQOut) {
  const uint16_t payloadSamples =
      (uint16_t)payloadSamplesWanted32();
  uint16_t maxOff = gFrameAlignPad;
  if ((uint32_t)payloadSamples + maxOff > gFrameIndex)
    maxOff = (gFrameIndex > payloadSamples) ? (uint16_t)(gFrameIndex - payloadSamples) : 0u;

  uint64_t bestScore = 0u;
  uint16_t bestOff = 0u;
  uint8_t bestMatches = 0u;
  uint32_t bestTrainQ = 0u;

  // Score the first complete 0..15 training sweep.  Amplitude is ignored for
  // alignment; the known phase/frequency shape is enough and is much more
  // stable than E4 absolute level.
  for (uint16_t off=0; off<=maxOff; ++off) {
    uint64_t score=0u, qsum=0u;
    uint32_t matches=0u;
    for (uint8_t s=0; s<STATE_COUNT; ++s) {
      const ShapeResult r=analyzeShape(&gFrame[off + (uint16_t)s*gChipsPerSymbol]);
      qsum += r.shapeQ;
      const uint8_t expPhase=(s>>1)&3u;
      const uint8_t expFreq=s&1u;
      if (r.phase==expPhase && r.freq==expFreq) {
        score += (uint64_t)r.shapeQ + (uint64_t)r.marginPermille*8u;
        ++matches;
      }
    }
    // Matching the expected state order dominates raw quality.
    score += (uint64_t)matches * 1000000ULL;
    if (score > bestScore) {
      bestScore=score;
      bestOff=off;
      bestMatches=(uint8_t)matches;
      bestTrainQ=(uint32_t)(qsum/STATE_COUNT);
    }
  }
  bestMatchesOut=bestMatches;
  bestTrainQOut=bestTrainQ;
  return bestOff;
}

static void analyzeFrame() {
  if (!gFrameReady) return;

  uint8_t alignMatches=0u;
  uint32_t alignTrainQ=0u;
  const uint16_t frameBase=findTrainingAlignment(alignMatches,alignTrainQ);

  if (frameBase < gAlignAcceptMin || frameBase > gAlignAcceptMax ||
      alignMatches < gFrameMinMatches || alignTrainQ < gFrameMinTrainQ) {
    ++gStats.frameDrops;
    Serial.printf("FRAME_DROP align=%u matches=%u trainQ=%lu gateAlign=[%u..%u] minMatch=%u minQ=%lu\n",
      (unsigned)frameBase,(unsigned)alignMatches,(unsigned long)alignTrainQ,
      (unsigned)gAlignAcceptMin,(unsigned)gAlignAcceptMax,(unsigned)gFrameMinMatches,(unsigned long)gFrameMinTrainQ);
    gFrameReady=false; gFrameIndex=0u; gSyncCount=0u; gSyncPos=0u;
    gPrevSampleTs=0u; gRxDeadlineUs=0u;
    return;
  }

  uint32_t muContrast[STATE_COUNT]{};
  uint32_t muOn[STATE_COUNT]{};
  uint32_t muOff[STATE_COUNT]{};
  uint32_t muNorm[STATE_COUNT]{};
  buildCal16(frameBase,muContrast,muOn,muOff,muNorm);

  Serial.printf("FRAME syncC=%lu syncQ=%lu rel=%lu/1000 syncBlock=%u align=%u match=%u trainQ=%lu CAL16 decode=%u ampMode=%u "
                "P0F0[A0=%lu A1=%lu] P0F1[A0=%lu A1=%lu]\n",
      (unsigned long)gLastSyncContrast,
      (unsigned long)gLastSyncQuality,
      (unsigned long)gLastSyncRelPermille,
      (unsigned)gSyncBlockChips,
      (unsigned)frameBase,
      (unsigned)alignMatches,
      (unsigned long)alignTrainQ,
      (unsigned)gDecodeMode,
      (unsigned)gAmpMode,
      (unsigned long)muContrast[0x0u],(unsigned long)muContrast[0x8u],
      (unsigned long)muContrast[0x1u],(unsigned long)muContrast[0x9u]);

  uint32_t frameErr=0, ampErr=0, phaseErr=0, freqErr=0;
  uint32_t marginMin=0xffffffffu, shapeQMin=0xffffffffu;
  uint64_t marginSum=0, shapeQSum=0;

  const uint16_t trainSymbols=trainSymbolsWanted();
  const uint16_t dataSymbols=dataSymbolsWanted();
  const uint16_t dataBase=(uint16_t)(frameBase + trainSymbols*gChipsPerSymbol);
  for (uint16_t i=0;i<dataSymbols;++i) {
    const uint32_t *x=&gFrame[dataBase+i*gChipsPerSymbol];
    const ShapeResult r=analyzeShape(x);

    uint8_t got=0u;
    uint32_t decodeMargin=r.marginPermille;
    if (gDecodeMode==2u) {
      got=decodeTemplate16(x,decodeMargin,frameBase);
    } else {
      const uint8_t amp=(gDecodeMode==1u)
        ? decodeDynamicAmplitude(r,muContrast,muOn,muOff,muNorm)
        : decodeLegacyAmplitude(r,muContrast);
      got=(uint8_t)((amp<<3)|(r.phase<<1)|r.freq);
    }

    const uint8_t amp=(got>>3)&1u;
    const uint8_t phase=(got>>1)&3u;
    const uint8_t freq=got&1u;
    const uint8_t exp=(uint8_t)(i%STATE_COUNT);

    if (gTelemetry) {
      int32_t ci=0,cq=0; correlationIQ(x,freq,ci,cq);
      Serial.printf("EVT,POINT,%u,%u,%u,%u,%u,%ld,%ld,%lu,%lu,%lu\n",
        (unsigned)exp,(unsigned)got,(unsigned)amp,(unsigned)(phase*90u),(unsigned)freq,
        (long)ci,(long)cq,(unsigned long)r.contrast,(unsigned long)decodeMargin,(unsigned long)r.shapeQ);
    }

    const bool ae=((got^exp)&0x8u)!=0;
    const bool pe=((got^exp)&0x6u)!=0;
    const bool fe=((got^exp)&0x1u)!=0;
    const bool bad=got!=exp;
    frameErr += bad;
    ampErr += ae;
    phaseErr += pe;
    freqErr += fe;

    if (decodeMargin < marginMin) marginMin=decodeMargin;
    if (r.shapeQ < shapeQMin) shapeQMin=r.shapeQ;
    marginSum += decodeMargin;
    shapeQSum += r.shapeQ;
    if (decodeMargin < 250u) ++gStats.weakMarginSymbols;

    if (gVerbose || (gPrintErrors && bad)) {
      Serial.printf("  SYM %02u exp=%X got=%X A=%u P=%u F=%u C=%lu Q=%lu M=%lu on=%lu off=%lu %s%s%s\n",
        (unsigned)i,(unsigned)exp,(unsigned)got,
        (unsigned)amp,(unsigned)(phase*90u),(unsigned)freq,
        (unsigned long)r.contrast,(unsigned long)r.shapeQ,(unsigned long)decodeMargin,
        (unsigned long)r.meanOn,(unsigned long)r.meanOff,
        ae?" AMPERR":"",pe?" PHERR":"",fe?" FREQERR":"");
    }
  }

  ++gStats.frames;
  gStats.symbols += dataSymbols;
  gStats.symbolErrors += frameErr;
  gStats.ampErrors += ampErr;
  gStats.phaseErrors += phaseErr;
  gStats.freqErrors += freqErr;

  Serial.printf("FRAME_SUM err=%lu/%u amp=%lu phase=%lu freq=%lu marginAvg=%lu min=%lu shapeQAvg=%lu min=%lu\n",
    (unsigned long)frameErr,(unsigned)dataSymbols,
    (unsigned long)ampErr,(unsigned long)phaseErr,(unsigned long)freqErr,
    (unsigned long)(marginSum/dataSymbols),(unsigned long)marginMin,
    (unsigned long)(shapeQSum/dataSymbols),(unsigned long)shapeQMin);

  gFrameReady=false;
  gFrameIndex=0u;
  gSyncCount=0u;
  gSyncPos=0u;
  gPrevSampleTs=0u; // printing created a deliberate gap
  gRxDeadlineUs=0u;
}

static void serviceRx() {
  if (Radio::mode != Radio::RX || gFrameReady || gRawReady) return;

  const uint32_t now=micros();
  if (!gRxDeadlineUs) {
    gRxFrac=0u;
    gRxDeadlineUs=now+nextChipIntervalUsRx();
    return;
  }
  if ((int32_t)(now-gRxDeadlineUs)<0) return;

  const uint32_t late=(uint32_t)(now-gRxDeadlineUs);
  if (late > gStats.rxLateMaxUs) gStats.rxLateMaxUs=late;
  if (late >= chipUsCeil()*3u) {
    ++gStats.rxLateResets;
    if (gCollectingFrame) {
      // Do not throw away a whole frame because the ESP8266 scheduler stole
      // a few chip periods. Preserve the time axis by padding the missed
      // sample slots, then place the current E4 sample after that gap.
      uint32_t miss = late / chipUsFloor();
      if (miss > 0xffffu) miss = 0xffffu;
      ++gStats.frameGapEvents;
      if (!gRecoverGaps || miss > gRecoverMaxPad) {
        ++gStats.frameGapAborts;
        resetRxSearch();
        gPrevSampleTs=0u;
      } else {
        gFramePendingPad=(uint16_t)miss;
        gStats.framePadChips+=miss;
        if(miss>gStats.framePadMax)gStats.framePadMax=miss;
      }
      gRxFrac=0u;
      gRxDeadlineUs=now+nextChipIntervalUsRx();
    } else {
      resetRxSearch();
      gPrevSampleTs=0u;
      gRxFrac=0u;
      gRxDeadlineUs=now+nextChipIntervalUsRx();
    }
  } else {
    gRxDeadlineUs += nextChipIntervalUsRx();
  }

  const uint32_t t0=micros();
  const uint32_t e=Radio::measureE4();
  const uint32_t t1=micros();
  const uint32_t cost=t1-t0;
  const uint32_t ts=t0+cost/2u;

  ++gStats.rxSamples;
  if (e<gStats.e4Min) gStats.e4Min=e;
  if (e>gStats.e4Max) gStats.e4Max=e;
  if (cost<gStats.e4MeasureMinUs) gStats.e4MeasureMinUs=cost;
  if (cost>gStats.e4MeasureMaxUs) gStats.e4MeasureMaxUs=cost;
  gStats.e4MeasureSumUs+=cost;
  ++gStats.e4MeasureCount;

  uint16_t thisDt=0;
  if (gPrevSampleTs) {
    const uint32_t dt=ts-gPrevSampleTs;
    thisDt=(uint16_t)(dt>0xffffu?0xffffu:dt);
    if(dt<gStats.dtMinUs)gStats.dtMinUs=dt;
    if(dt>gStats.dtMaxUs)gStats.dtMaxUs=dt;
    gStats.dtSumUs+=dt;
    ++gStats.dtCount;
  }
  gPrevSampleTs=ts;

  // Delayed raw capture: arm now, start later. This avoids false triggers
  // from ambient 2.4 GHz traffic.
  if (gRawDelayWanted && (int32_t)(millis() - gRawDelayDeadlineMs) >= 0) {
    gRawWanted = gRawDelayWanted;
    gRawDelayWanted = 0u;
    gRawCount = 0u;
    gRawReady = false;
    gRawTriggered = true;
    gRawTriggerThreshold = 0u;
    gPrevSampleTs = ts;
    thisDt = 0u;
  }

  // Raw capture. In triggered mode, wait until E4 crosses the requested
  // threshold, then capture the current sample and the following samples.
  if (gRawWanted && gRawCount<gRawWanted) {
    if (!gRawTriggered) {
      if (e >= gRawTriggerThreshold) {
        gRawTriggered = true;
        gPrevSampleTs = ts; // first captured dt is intentionally 0
        thisDt = 0u;
      }
    }
    if (gRawTriggered) {
      gRawE4[gRawCount]=e;
      gRawDt[gRawCount]=thisDt;
      ++gRawCount;
      if(gRawCount>=gRawWanted){ gRawWanted=0u; gRawReady=true; }
    }
  }

  // Do not double-store raw data inside acceptRxSample().
  if (!gRawReady) {
    if (gFrameReady) return;
    if (gCollectingFrame) {
      const uint16_t wanted=frameSamplesWanted();
      if (gFramePendingPad) {
        const uint32_t fill = gFrameHaveLastE4 ? gFrameLastE4 : e;
        while (gFramePendingPad && gFrameIndex<wanted) {
          gFrame[gFrameIndex++] = fill;
          --gFramePendingPad;
        }
      }
      if(gFrameIndex<wanted)gFrame[gFrameIndex++]=e;
      gFrameLastE4=e;
      gFrameHaveLastE4=true;
      if(gFrameIndex>=wanted){gCollectingFrame=false;gFrameReady=true;}
    } else {
      const uint16_t syncChips=syncChipsWanted();
      gSyncRing[gSyncPos]=e;
      gSyncPos=(uint16_t)((gSyncPos+1u)%syncChips);
      if(gSyncCount<syncChips)++gSyncCount;
      if(++gSyncTestDecim>=gSyncTestStride)gSyncTestDecim=0u;
      if(gSyncTestDecim==0u && testSyncWindow()){
        gCollectingFrame=true;
        gFrameIndex=0u;
        gFramePendingPad=0u;
        gFrameLastE4=0u;
        gFrameHaveLastE4=false;
        gSyncCount=0u;
        gSyncPos=0u;
        gSyncTestDecim=0u;
      }
    }
  }
  feedWdt();
}

// -----------------------------------------------------------------------------
// Raw capture dump
// -----------------------------------------------------------------------------
static void dumpRawIfReady() {
  if(!gRawReady) return;
  Serial.println("RAW_BEGIN index,dt_us,e4");
  for(uint16_t i=0;i<gRawCount;++i)
    Serial.printf("RAW,%u,%u,%lu\n",(unsigned)i,(unsigned)gRawDt[i],(unsigned long)gRawE4[i]);
  Serial.println("RAW_END");
  gRawReady=false;
  gRawCount=0u;
  gRawTriggered=true;
  gRawTriggerThreshold=0u;
  gRawDelayWanted=0u;
  gRawDelayDeadlineMs=0u;
  gPrevSampleTs=0u;
  gRxDeadlineUs=0u;
  resetRxSearch();
}

// -----------------------------------------------------------------------------
// Diagnostics / serial console
// -----------------------------------------------------------------------------
static uint32_t gAutoPeriodMs=0u;
static uint32_t gNextAutoMs=0u;

static void printConfigEvent() {
  Serial.printf("EVT,CFG,chiprate,%lu,symbolchips,%u,period0,%u,period1,%u,ask0,%u,ask1,%u,iq,%u,gain,%04X,telemetry,%u,decode,%u,ampmode,%u,ampmix,%u,framematch,%u,frameq,%lu,alignmin,%u,alignmax,%u,syncq,%lu,syncrel,%lu,syncc,%lu,syncblock,%u,syncguard,%u,preguard,%u,trainreps,%u,datareps,%u,alignpad,%u,syncstride,%u,recover,%u,maxpad,%u,errprint,%u\n",
    (unsigned long)gChipRate,(unsigned)gChipsPerSymbol,(unsigned)gPeriodChips[0],(unsigned)gPeriodChips[1],
    (unsigned)gAskCode[0],(unsigned)gAskCode[1],(unsigned)gIqN,(unsigned)gRxGain,(unsigned)gTelemetry,(unsigned)gDecodeMode,
    (unsigned)gAmpMode,(unsigned)gAmpMixPermille,(unsigned)gFrameMinMatches,(unsigned long)gFrameMinTrainQ,(unsigned)gAlignAcceptMin,(unsigned)gAlignAcceptMax,
    (unsigned long)gSyncQualityMin,(unsigned long)gSyncRelMinPermille,(unsigned long)gSyncContrastMin,
    (unsigned)gSyncBlockChips,(unsigned)gSyncPostGuardChips,(unsigned)gPreGuardChips,(unsigned)gTrainRepeats,(unsigned)gDataRepeats,
    (unsigned)gFrameAlignPad,(unsigned)gSyncTestStride,(unsigned)gRecoverGaps,(unsigned)gRecoverMaxPad,(unsigned)gPrintErrors);
}

static void printStatus() {
  const uint32_t dtAvg=gStats.dtCount?(uint32_t)(gStats.dtSumUs/gStats.dtCount):0u;
  const uint32_t e4AvgCost=gStats.e4MeasureCount?(uint32_t)(gStats.e4MeasureSumUs/gStats.e4MeasureCount):0u;
  const uint32_t txLateAvg=gStats.txLateCount?(uint32_t)(gStats.txLateSumUs/gStats.txLateCount):0u;
  const uint32_t f0=gPeriodChips[0]?(gChipRate/gPeriodChips[0]):0u;
  const uint32_t f1=gPeriodChips[1]?(gChipRate/gPeriodChips[1]):0u;
  const uint32_t sym=gChipsPerSymbol?(gChipRate/gChipsPerSymbol):0u;
  Serial.printf("STATUS mode=%s chip=%luHz F0=%luHz F1=%luHz symbolchips=%u sym=%lu/s raw=%lubps IQ_N=%u gain=%04X ASK0=%u ASK1=%u APWRraw=%u\n",
    Radio::mode==Radio::RX?"RX":"TX",(unsigned long)gChipRate,(unsigned long)f0,(unsigned long)f1,
    (unsigned)gChipsPerSymbol,(unsigned long)sym,(unsigned long)(sym*4u),(unsigned)gIqN,(unsigned)gRxGain,
    (unsigned)gAskCode[0],(unsigned)gAskCode[1],(unsigned)TX_APWR);
  Serial.printf("  periods=[%u,%u] telemetry=%u decode=%u ampMode=%u mix=%u frameGate match>=%u Q>=%lu align=[%u..%u] syncQ>=%lu rel>=%lu/1000 syncC>=%lu syncBlock=%u stride=%u verbose=%u errprint=%u auto=%lums\n",
    (unsigned)gPeriodChips[0],(unsigned)gPeriodChips[1],(unsigned)gTelemetry,(unsigned)gDecodeMode,
    (unsigned)gAmpMode,(unsigned)gAmpMixPermille,(unsigned)gFrameMinMatches,(unsigned long)gFrameMinTrainQ,(unsigned)gAlignAcceptMin,(unsigned)gAlignAcceptMax,
    (unsigned long)gSyncQualityMin,(unsigned long)gSyncRelMinPermille,(unsigned long)gSyncContrastMin,(unsigned)gSyncBlockChips,(unsigned)gSyncTestStride,
    (unsigned)gVerbose,(unsigned)gPrintErrors,(unsigned long)gAutoPeriodMs);
  Serial.printf("  frame pre=%u sync=%u post=%u train=%ux16 data=%ux16 align=%u samples=%u/%u frame~%lums recover=%u maxpad=%u\n",
    (unsigned)gPreGuardChips,(unsigned)syncChipsWanted(),(unsigned)gSyncPostGuardChips,(unsigned)gTrainRepeats,(unsigned)gDataRepeats,(unsigned)gFrameAlignPad,
    (unsigned)frameSamplesWanted(),(unsigned)MAX_FRAME_SAMPLES,(unsigned long)diagnosticFrameMs(),(unsigned)gRecoverGaps,(unsigned)gRecoverMaxPad);
  Serial.printf("  RX samples=%lu dtAvg=%lu [%lu..%lu]us lateReset=%lu lateMax=%luus E4=[%lu..%lu] measureAvg=%lu [%lu..%lu]us\n",
    (unsigned long)gStats.rxSamples,(unsigned long)dtAvg,(unsigned long)(gStats.dtMinUs==0xffffffffu?0:gStats.dtMinUs),(unsigned long)gStats.dtMaxUs,
    (unsigned long)gStats.rxLateResets,(unsigned long)gStats.rxLateMaxUs,(unsigned long)(gStats.e4Min==0xffffffffu?0:gStats.e4Min),(unsigned long)gStats.e4Max,
    (unsigned long)e4AvgCost,(unsigned long)(gStats.e4MeasureMinUs==0xffffffffu?0:gStats.e4MeasureMinUs),(unsigned long)gStats.e4MeasureMaxUs);
  Serial.printf("  SYNC tests=%lu hits=%lu FRAME=%lu drop=%lu SYM=%lu err=%lu amp=%lu phase=%lu freq=%lu weakMargin=%lu gapEvt=%lu pad=%lu padMax=%lu abort=%lu TXchips=%lu TXlateAvg=%luus max=%luus\n",
    (unsigned long)gStats.syncTests,(unsigned long)gStats.syncHits,(unsigned long)gStats.frames,(unsigned long)gStats.frameDrops,(unsigned long)gStats.symbols,
    (unsigned long)gStats.symbolErrors,(unsigned long)gStats.ampErrors,(unsigned long)gStats.phaseErrors,(unsigned long)gStats.freqErrors,
    (unsigned long)gStats.weakMarginSymbols,(unsigned long)gStats.frameGapEvents,(unsigned long)gStats.framePadChips,(unsigned long)gStats.framePadMax,(unsigned long)gStats.frameGapAborts,
    (unsigned long)gStats.txChips,(unsigned long)txLateAvg,(unsigned long)gStats.txLateMaxUs);
  printConfigEvent();
}

static void printHelp() {
  Serial.println("Commands:");
  Serial.println("  help | status | config | stats | clear | rx");
  Serial.println("  txframe | auto <ms> | txstate <0..15> <count> | hold <state> <ms>");
  Serial.println("  phasesweep <period_chips> <duration_ms> [amp0or1]");
  Serial.println("  phasesweep875 <ms> | phasesweep1750 <ms>   (compatibility aliases)");
  Serial.println("  raw <16..1024> | rawdelay <16..1024> <delay_ms> | rawtrig <n> <threshold>");
  Serial.println("  set chiprate <1000..40000>");
  Serial.println("  set symbolchips <8..64>");
  Serial.println("  set period0|period1 <4..128, divisible by 4>");
  Serial.println("  set ask0|ask1 <0..63> | set iq <16..1024> | set gain <hex>");
  Serial.println("  set sweepblock <16..2048> | set sweepgap <0..512>");
  Serial.println("  set telemetry <0|1> | set verbose <0|1> | set errprint <0|1>");
  Serial.println("  set ampmode <0..5> | set ampmix <0..1000>");
  Serial.println("  set framematch <0..16> | set frameq <0..1000000> | set alignmin|alignmax <0..256>");
  Serial.println("  set syncq <value> | set syncrel <permille> | set syncc <E4_contrast>");
  Serial.println("  set syncblock <4..64> | set syncguard <0..256> | set preguard <0..256>");
  Serial.println("  set trainreps <1..4> | set datareps <1..8> | set alignpad <0..256>");
  Serial.println("  set syncstride <1..32> | set recover <0|1> | set maxpad <0..2048>");
  Serial.println("Legacy aliases: amp <a0> <a1>, iq <n>, gain <hex>, verbose <0|1>, syncq <q> <rel>");
}

static char gCmd[160]{};
static uint8_t gCmdLen=0u;

static void handleCommand(char *line) {
  char *cmd=strtok(line," \t");
  if(!cmd)return;

  if(!strcmp(cmd,"help")){printHelp();return;}

  if(!strcmp(cmd,"config")){printConfigEvent();return;}
  if(!strcmp(cmd,"set")){
    char *key=strtok(nullptr," \t"), *val=strtok(nullptr," \t");
    if(!key||!val){Serial.println("ERR set <key> <value>");return;}
    if(!strcmp(key,"chiprate")){
      uint32_t v=strtoul(val,nullptr,0); if(v<1000u||v>40000u){Serial.println("ERR chiprate 1000..40000");return;}
      gChipRate=v; resetRxSearch(); gRxDeadlineUs=0u; gTxFrac=gRxFrac=0u; Serial.printf("OK chiprate=%lu\n",(unsigned long)v); printConfigEvent(); return;
    }
    if(!strcmp(key,"symbolchips")){
      uint32_t v=strtoul(val,nullptr,0); if(v<8u||v>MAX_CHIPS_PER_SYMBOL){Serial.println("ERR symbolchips 8..64");return;}
      if(!frameConfigFits((uint8_t)v,gTrainRepeats,gDataRepeats,gFrameAlignPad)){Serial.println("ERR frame buffer: reduce trainreps/datareps/alignpad");return;}
      gChipsPerSymbol=(uint8_t)v; resetRxSearch(); Serial.printf("OK symbolchips=%u\n",(unsigned)gChipsPerSymbol); printConfigEvent(); return;
    }
    if(!strcmp(key,"period0")||!strcmp(key,"period1")){
      uint32_t v=strtoul(val,nullptr,0); if(v<4u||v>128u||(v&3u)){Serial.println("ERR period 4..128 and divisible by 4");return;}
      const uint8_t fi=key[6]-'0'; gPeriodChips[fi]=(uint16_t)v; Serial.printf("OK period%u=%u F=%luHz\n",(unsigned)fi,(unsigned)v,(unsigned long)(gChipRate/v)); printConfigEvent(); return;
    }
    if(!strcmp(key,"ask0")||!strcmp(key,"ask1")){
      uint32_t v=strtoul(val,nullptr,0); if(v>63u){Serial.println("ERR ASK 0..63");return;}
      const uint8_t ai=key[3]-'0'; gAskCode[ai]=(uint8_t)v; Serial.printf("OK ask%u=%u\n",(unsigned)ai,(unsigned)v); printConfigEvent(); return;
    }
    if(!strcmp(key,"iq")){uint32_t v=strtoul(val,nullptr,0);if(v<16u||v>1024u){Serial.println("ERR IQ 16..1024");return;}gIqN=(uint16_t)v;Serial.printf("OK IQ_N=%u\n",(unsigned)gIqN);printConfigEvent();return;}
    if(!strcmp(key,"gain")){gRxGain=(uint16_t)strtoul(val,nullptr,16);Radio::setRxGain(gRxGain);Serial.printf("OK gain=%04X\n",(unsigned)gRxGain);printConfigEvent();return;}
    if(!strcmp(key,"sweepblock")){uint32_t v=strtoul(val,nullptr,0);if(v<16u||v>2048u){Serial.println("ERR sweepblock 16..2048");return;}gSweepBlockChips=(uint16_t)v;Serial.printf("OK sweepblock=%u\n",(unsigned)v);printConfigEvent();return;}
    if(!strcmp(key,"sweepgap")){uint32_t v=strtoul(val,nullptr,0);if(v>512u){Serial.println("ERR sweepgap 0..512");return;}gSweepGapChips=(uint16_t)v;Serial.printf("OK sweepgap=%u\n",(unsigned)v);printConfigEvent();return;}
    if(!strcmp(key,"telemetry")){gTelemetry=strtoul(val,nullptr,0)!=0;Serial.printf("OK telemetry=%u\n",(unsigned)gTelemetry);printConfigEvent();return;}
    if(!strcmp(key,"decode")){uint32_t v=strtoul(val,nullptr,0);if(v>2u){Serial.println("ERR decode 0..2");return;}gDecodeMode=(uint8_t)v;Serial.printf("OK decode=%u\n",(unsigned)gDecodeMode);printConfigEvent();return;}
    if(!strcmp(key,"ampmode")){uint32_t v=strtoul(val,nullptr,0);if(v>5u){Serial.println("ERR ampmode 0..5");return;}gAmpMode=(uint8_t)v;Serial.printf("OK ampmode=%u\n",(unsigned)gAmpMode);printConfigEvent();return;}
    if(!strcmp(key,"ampmix")){uint32_t v=strtoul(val,nullptr,0);if(v>1000u){Serial.println("ERR ampmix 0..1000");return;}gAmpMixPermille=(uint16_t)v;Serial.printf("OK ampmix=%u\n",(unsigned)gAmpMixPermille);printConfigEvent();return;}
    if(!strcmp(key,"framematch")){uint32_t v=strtoul(val,nullptr,0);if(v>16u){Serial.println("ERR framematch 0..16");return;}gFrameMinMatches=(uint8_t)v;Serial.printf("OK framematch=%u\n",(unsigned)gFrameMinMatches);printConfigEvent();return;}
    if(!strcmp(key,"frameq")){uint32_t v=strtoul(val,nullptr,0);gFrameMinTrainQ=v;Serial.printf("OK frameq=%lu\n",(unsigned long)gFrameMinTrainQ);printConfigEvent();return;}
    if(!strcmp(key,"alignmin")){uint32_t v=strtoul(val,nullptr,0);if(v>MAX_FRAME_ALIGN_PAD||v>gAlignAcceptMax){Serial.println("ERR alignmin 0..alignmax");return;}gAlignAcceptMin=(uint16_t)v;Serial.printf("OK alignmin=%u\n",(unsigned)gAlignAcceptMin);printConfigEvent();return;}
    if(!strcmp(key,"alignmax")){uint32_t v=strtoul(val,nullptr,0);if(v>MAX_FRAME_ALIGN_PAD||v<gAlignAcceptMin){Serial.println("ERR alignmax alignmin..256");return;}gAlignAcceptMax=(uint16_t)v;Serial.printf("OK alignmax=%u\n",(unsigned)gAlignAcceptMax);printConfigEvent();return;}
    if(!strcmp(key,"verbose")){gVerbose=strtoul(val,nullptr,0)!=0;Serial.printf("OK verbose=%u\n",(unsigned)gVerbose);return;}
    if(!strcmp(key,"syncq")){gSyncQualityMin=strtoul(val,nullptr,0);Serial.printf("OK syncQ=%lu\n",(unsigned long)gSyncQualityMin);return;}
    if(!strcmp(key,"syncrel")){gSyncRelMinPermille=strtoul(val,nullptr,0);Serial.printf("OK syncrel=%lu\n",(unsigned long)gSyncRelMinPermille);return;}
    if(!strcmp(key,"syncc")){gSyncContrastMin=strtoul(val,nullptr,0);Serial.printf("OK syncC=%lu\n",(unsigned long)gSyncContrastMin);return;}
    if(!strcmp(key,"syncblock")){uint32_t v=strtoul(val,nullptr,0);if(v<4u||v>MAX_SYNC_BLOCK_CHIPS){Serial.println("ERR syncblock 4..64");return;}gSyncBlockChips=(uint16_t)v;resetRxSearch();Serial.printf("OK syncblock=%u syncchips=%u\n",(unsigned)gSyncBlockChips,(unsigned)syncChipsWanted());printConfigEvent();return;}
    if(!strcmp(key,"syncguard")){uint32_t v=strtoul(val,nullptr,0);if(v>256u){Serial.println("ERR syncguard 0..256");return;}gSyncPostGuardChips=(uint16_t)v;Serial.printf("OK syncguard=%u\n",(unsigned)v);printConfigEvent();return;}
    if(!strcmp(key,"preguard")){uint32_t v=strtoul(val,nullptr,0);if(v>256u){Serial.println("ERR preguard 0..256");return;}gPreGuardChips=(uint16_t)v;Serial.printf("OK preguard=%u\n",(unsigned)v);printConfigEvent();return;}
    if(!strcmp(key,"trainreps")){uint32_t v=strtoul(val,nullptr,0);if(v<1u||v>MAX_TRAIN_REPEATS){Serial.println("ERR trainreps 1..4");return;}if(!frameConfigFits(gChipsPerSymbol,(uint8_t)v,gDataRepeats,gFrameAlignPad)){Serial.println("ERR frame buffer");return;}gTrainRepeats=(uint8_t)v;resetRxSearch();Serial.printf("OK trainreps=%u\n",(unsigned)v);printConfigEvent();return;}
    if(!strcmp(key,"datareps")){uint32_t v=strtoul(val,nullptr,0);if(v<1u||v>MAX_DATA_REPEATS){Serial.println("ERR datareps 1..8");return;}if(!frameConfigFits(gChipsPerSymbol,gTrainRepeats,(uint8_t)v,gFrameAlignPad)){Serial.println("ERR frame buffer");return;}gDataRepeats=(uint8_t)v;resetRxSearch();Serial.printf("OK datareps=%u\n",(unsigned)v);printConfigEvent();return;}
    if(!strcmp(key,"alignpad")){uint32_t v=strtoul(val,nullptr,0);if(v>MAX_FRAME_ALIGN_PAD){Serial.println("ERR alignpad 0..256");return;}if(!frameConfigFits(gChipsPerSymbol,gTrainRepeats,gDataRepeats,(uint16_t)v)){Serial.println("ERR frame buffer");return;}gFrameAlignPad=(uint16_t)v;resetRxSearch();Serial.printf("OK alignpad=%u\n",(unsigned)v);printConfigEvent();return;}
    if(!strcmp(key,"syncstride")){uint32_t v=strtoul(val,nullptr,0);if(v<1u||v>32u){Serial.println("ERR syncstride 1..32");return;}gSyncTestStride=(uint8_t)v;resetRxSearch();Serial.printf("OK syncstride=%u\n",(unsigned)v);printConfigEvent();return;}
    if(!strcmp(key,"recover")){gRecoverGaps=strtoul(val,nullptr,0)!=0;Serial.printf("OK recover=%u\n",(unsigned)gRecoverGaps);printConfigEvent();return;}
    if(!strcmp(key,"maxpad")){uint32_t v=strtoul(val,nullptr,0);if(v>2048u){Serial.println("ERR maxpad 0..2048");return;}gRecoverMaxPad=(uint16_t)v;Serial.printf("OK maxpad=%u\n",(unsigned)v);printConfigEvent();return;}
    if(!strcmp(key,"errprint")){gPrintErrors=strtoul(val,nullptr,0)!=0;Serial.printf("OK errprint=%u\n",(unsigned)gPrintErrors);printConfigEvent();return;}
    Serial.println("ERR unknown set key"); return;
  }
  if(!strcmp(cmd,"status")||!strcmp(cmd,"stats")){printStatus();return;}
  if(!strcmp(cmd,"clear")){clearStats();Serial.println("OK stats cleared");return;}
  if(!strcmp(cmd,"rx")){
    gAutoPeriodMs=0u; Radio::enterRx(); resetRxSearch(); gRxDeadlineUs=0u; gPrevSampleTs=0u;
    Serial.println("OK RX");return;
  }
  if(!strcmp(cmd,"txframe")){
    gAutoPeriodMs=0u;
    const bool ok=transmitDiagnosticFrame();
    resetRxSearch();gRxDeadlineUs=0u;gPrevSampleTs=0u;
    Serial.printf("TXFRAME %s\n",ok?"OK":"FAIL");return;
  }
  if(!strcmp(cmd,"auto")){
    char *a=strtok(nullptr," \t"); if(!a){Serial.println("ERR auto <ms>");return;}
    uint32_t ms=(uint32_t)strtoul(a,nullptr,0);
    if(ms){uint32_t minMs=diagnosticFrameMs()+50u;if(minMs<AUTO_MIN_MS)minMs=AUTO_MIN_MS;if(ms<minMs)ms=minMs;}
    gAutoPeriodMs=ms;gNextAutoMs=millis()+ms;
    Serial.printf("OK auto=%lums\n",(unsigned long)ms);return;
  }
  if(!strcmp(cmd,"txstate")){
    char *a=strtok(nullptr," \t"),*b=strtok(nullptr," \t");
    if(!a||!b){Serial.println("ERR txstate <0..15> <count>");return;}
    uint8_t s=(uint8_t)strtoul(a,nullptr,0);
    uint16_t n=(uint16_t)strtoul(b,nullptr,0);
    const bool ok=transmitRepeatedState(s,n);
    resetRxSearch();gRxDeadlineUs=0u;gPrevSampleTs=0u;
    Serial.printf("TXSTATE state=%u n=%u %s\n",(unsigned)s,(unsigned)n,ok?"OK":"FAIL");return;
  }
  if(!strcmp(cmd,"hold")){
    char *a=strtok(nullptr," \t"),*b=strtok(nullptr," \t");
    if(!a||!b){Serial.println("ERR hold <0..15> <100..10000_ms>");return;}
    uint32_t st=strtoul(a,nullptr,0);
    uint32_t ms=strtoul(b,nullptr,0);
    if(st>15u){Serial.println("ERR state range 0..15");return;}
    if(ms<100u) ms=100u;
    if(ms>10000u) ms=10000u;
    Serial.printf("HOLD start state=%lu duration=%lums\n",(unsigned long)st,(unsigned long)ms);
    const bool ok=transmitHeldState((uint8_t)st,ms);
    resetRxSearch();gRxDeadlineUs=0u;gPrevSampleTs=0u;
    Serial.printf("HOLD done state=%lu duration=%lums %s\n",(unsigned long)st,(unsigned long)ms,ok?"OK":"FAIL");return;
  }
  if(!strcmp(cmd,"phasesweep")){
    char *a=strtok(nullptr," \t"),*b=strtok(nullptr," \t"),*c=strtok(nullptr," \t");
    if(!a||!b){Serial.println("ERR phasesweep <period_chips> <duration_ms> [amp]");return;}
    uint32_t p=strtoul(a,nullptr,0),ms=strtoul(b,nullptr,0),amp=c?strtoul(c,nullptr,0):0u;
    if(p<4u||p>128u||(p&3u)){Serial.println("ERR period 4..128 divisible by 4");return;}
    if(ms<100u)ms=100u;if(ms>10000u)ms=10000u;if(amp>1u)amp=1u;
    Serial.printf("PHASESWEEP start period=%lu F=%luHz block=%u gap=%u amp=%lu duration=%lums\n",(unsigned long)p,(unsigned long)(gChipRate/p),(unsigned)gSweepBlockChips,(unsigned)gSweepGapChips,(unsigned long)amp,(unsigned long)ms);
    const bool ok=transmitPhaseSweep((uint16_t)p,ms,(uint8_t)amp);
    resetRxSearch();gRxDeadlineUs=0u;gPrevSampleTs=0u;
    Serial.printf("PHASESWEEP done %s\n",ok?"OK":"FAIL");return;
  }
  if(!strcmp(cmd,"phasesweep1750")){
    char *a=strtok(nullptr," \t");
    if(!a){Serial.println("ERR phasesweep1750 <100..10000_ms>");return;}
    uint32_t ms=strtoul(a,nullptr,0);
    if(ms<100u) ms=100u;
    if(ms>10000u) ms=10000u;
    Serial.printf("PHASESWEEP1750 start F=1.75kHz A=A0 block=128chips gap=32chips phases=0/90/180/270 duration=%lums\n",(unsigned long)ms);
    const bool ok=transmitPhaseSweep1750(ms);
    resetRxSearch();gRxDeadlineUs=0u;gPrevSampleTs=0u;
    Serial.printf("PHASESWEEP1750 done duration=%lums %s\n",(unsigned long)ms,ok?"OK":"FAIL");return;
  }
  if(!strcmp(cmd,"phasesweep875")){
    char *a=strtok(nullptr," \t");
    if(!a){Serial.println("ERR phasesweep875 <100..10000_ms>");return;}
    uint32_t ms=strtoul(a,nullptr,0);
    if(ms<100u) ms=100u;
    if(ms>10000u) ms=10000u;
    Serial.printf("PHASESWEEP875 start F=875Hz A=A0 block=128chips gap=32chips phases=0/90/180/270 step=8chips duration=%lums\n",(unsigned long)ms);
    const bool ok=transmitPhaseSweep875(ms);
    resetRxSearch();gRxDeadlineUs=0u;gPrevSampleTs=0u;
    Serial.printf("PHASESWEEP875 done duration=%lums %s\n",(unsigned long)ms,ok?"OK":"FAIL");return;
  }
  if(!strcmp(cmd,"amp")){
    char *a=strtok(nullptr," \t"),*b=strtok(nullptr," \t");
    if(!a||!b){Serial.println("ERR amp <0..63> <0..63>");return;}
    uint32_t x=strtoul(a,nullptr,0),y=strtoul(b,nullptr,0);
    if(x>63u||y>63u){Serial.println("ERR ASK range 0..63");return;}
    gAskCode[0]=(uint8_t)x;gAskCode[1]=(uint8_t)y;
    Serial.printf("OK ASK A0=%u A1=%u\n",(unsigned)gAskCode[0],(unsigned)gAskCode[1]);return;
  }
  if(!strcmp(cmd,"iq")){
    char *a=strtok(nullptr," \t");if(!a){Serial.println("ERR iq <16..1024>");return;}
    uint32_t v=strtoul(a,nullptr,0);if(v<16u||v>1024u){Serial.println("ERR IQ range");return;}
    gIqN=(uint16_t)v;Serial.printf("OK IQ_N=%u\n",(unsigned)gIqN);return;
  }
  if(!strcmp(cmd,"gain")){
    char *a=strtok(nullptr," \t");if(!a){Serial.println("ERR gain <hex>");return;}
    gRxGain=(uint16_t)strtoul(a,nullptr,16);Radio::setRxGain(gRxGain);
    Serial.printf("OK gain=%04X\n",(unsigned)gRxGain);return;
  }
  if(!strcmp(cmd,"raw")){
    char *a=strtok(nullptr," \t");if(!a){Serial.println("ERR raw <16..1024>");return;}
    uint32_t n=strtoul(a,nullptr,0);if(n<16u||n>RAW_MAX_SAMPLES){Serial.println("ERR raw range 16..1024");return;}
    gRawCount=0u;gRawReady=false;gRawWanted=(uint16_t)n;
    gRawDelayWanted=0u;gRawDelayDeadlineMs=0u;
    gRawTriggered=true;gRawTriggerThreshold=0u;resetRxSearch();
    Serial.printf("OK raw capture armed n=%u\n",(unsigned)n);return;
  }
  if(!strcmp(cmd,"rawdelay")){
    char *a=strtok(nullptr," \t"),*b=strtok(nullptr," \t");
    if(!a||!b){Serial.println("ERR rawdelay <16..1024> <delay_ms>");return;}
    uint32_t n=strtoul(a,nullptr,0), d=strtoul(b,nullptr,0);
    if(n<16u||n>RAW_MAX_SAMPLES){Serial.println("ERR rawdelay sample range");return;}
    if(d<100u||d>10000u){Serial.println("ERR rawdelay delay range 100..10000 ms");return;}
    gRawCount=0u;gRawReady=false;gRawWanted=0u;
    gRawDelayWanted=(uint16_t)n;gRawDelayDeadlineMs=millis()+d;
    gRawTriggered=true;gRawTriggerThreshold=0u;resetRxSearch();
    Serial.printf("OK raw delayed capture armed n=%u delay=%lums\n",(unsigned)n,(unsigned long)d);return;
  }
  if(!strcmp(cmd,"rawtrig")){
    char *a=strtok(nullptr," \t"),*b=strtok(nullptr," \t");
    if(!a||!b){Serial.println("ERR rawtrig <16..1024> <E4_threshold>");return;}
    uint32_t n=strtoul(a,nullptr,0), th=strtoul(b,nullptr,0);
    if(n<16u||n>RAW_MAX_SAMPLES){Serial.println("ERR rawtrig sample range");return;}
    if(th==0u){Serial.println("ERR rawtrig threshold must be >0");return;}
    gRawCount=0u;gRawReady=false;gRawWanted=(uint16_t)n;
    gRawDelayWanted=0u;gRawDelayDeadlineMs=0u;
    gRawTriggered=false;gRawTriggerThreshold=th;resetRxSearch();
    Serial.printf("OK raw trigger armed n=%u threshold=%lu\n",(unsigned)n,(unsigned long)th);return;
  }
  if(!strcmp(cmd,"verbose")){
    char *a=strtok(nullptr," \t");if(!a){Serial.println("ERR verbose 0|1");return;}
    gVerbose=strtoul(a,nullptr,0)!=0;Serial.printf("OK verbose=%u\n",(unsigned)gVerbose);return;
  }
  if(!strcmp(cmd,"syncq")){
    char *a=strtok(nullptr," \t"),*b=strtok(nullptr," \t");
    if(!a||!b){Serial.println("ERR syncq <quality_x1000> <relative_permille>");return;}
    gSyncQualityMin=strtoul(a,nullptr,0);gSyncRelMinPermille=strtoul(b,nullptr,0);
    Serial.printf("OK syncQ=%lu rel=%lu/1000\n",(unsigned long)gSyncQualityMin,(unsigned long)gSyncRelMinPermille);return;
  }

  Serial.println("ERR unknown command; type help");
}

static void executeConsoleLine() {
  // Trim trailing spaces/tabs.
  while(gCmdLen && (gCmd[gCmdLen-1u]==' ' || gCmd[gCmdLen-1u]=='\t')) --gCmdLen;
  gCmd[gCmdLen]='\0';

  // Ignore an empty line. This also makes CR+LF work naturally: CR executes
  // the command, then the following LF sees an empty buffer and does nothing.
  if(!gCmdLen) return;

  Serial.print("> ");
  Serial.println(gCmd);
  handleCommand(gCmd);
  gCmdLen=0u;
}

static void serviceConsole() {
  // Avoid command parsing during a captured diagnostic frame.
  if(gCollectingFrame || gFrameReady || gRawReady)return;

  while(Serial.available()) {
    int c=Serial.read();
    if(c<0)break;

    // NORMAL KEYBOARD SUPPORT:
    // Enter may arrive as CR, LF, or CR+LF depending on the terminal.
    // Any one of them terminates and executes the command.
    if(c=='\r' || c=='\n') {
      executeConsoleLine();
      continue;
    }

    // Backspace/Delete editing for ordinary serial terminals.
    if(c==8 || c==127) {
      if(gCmdLen) --gCmdLen;
      continue;
    }

    // Ignore other ASCII control characters.
    if(c<32 || c>126) continue;

    // Make commands case-insensitive while preserving numbers/punctuation.
    if(c>='A' && c<='Z') c += ('a'-'A');

    if(gCmdLen < sizeof(gCmd)-1u) {
      gCmd[gCmdLen++] = (char)c;
    } else {
      gCmdLen=0u;
      Serial.println("ERR command too long; buffer cleared");
    }
  }
}

static void serviceAutoTx() {
  if(!gAutoPeriodMs || gCollectingFrame || gFrameReady || gRawWanted || gRawReady)return;
  const uint32_t now=millis();
  if((int32_t)(now-gNextAutoMs)<0)return;
  const bool ok=transmitDiagnosticFrame();
  resetRxSearch();gRxDeadlineUs=0u;gPrevSampleTs=0u;
  Serial.printf("AUTO_TXFRAME %s\n",ok?"OK":"FAIL");
  gNextAutoMs=millis()+gAutoPeriodMs;
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println();
  Serial.println("ESP8266 QAM16 dynamic WebSerial backend");
#ifndef HOST_COMPILE
  Serial.printf("Reset: %s\n",ESP.getResetReason().c_str());
#else
  Serial.printf("Reset: %s\n",ESP.getResetReason());
#endif
  Serial.printf("Runtime defaults: chip=%lu Hz, period0=%u, period1=%u, symbolchips=%u\n",(unsigned long)gChipRate,(unsigned)gPeriodChips[0],(unsigned)gPeriodChips[1],(unsigned)gChipsPerSymbol);
  Serial.println("WebSerial protocol: CAL16 LAB-DYNAMIC backend; runtime frame/sync/recovery/amplitude-decoder controls enabled; EVT,POINT feeds the live constellation.");
  Serial.println("Mapping: A(1 bit) x phase(2 bits) x frequency(1 bit)");
  Serial.println("Bench use only; keep bursts short and use shielding/attenuation.");
  Serial.println("Console: type rx then Enter. CR, LF and CR+LF are all accepted.");

  if(!Radio::begin()) {
    Serial.println("FATAL: radio init failed");
    return;
  }
  Radio::setRxGain(gRxGain);
  resetRxSearch();
  clearStats();
  printStatus();
  printHelp();
}

void loop() {
  feedWdt();
  serviceRx();

  if(gFrameReady) analyzeFrame();
  if(gRawReady) dumpRawIfReady();

  serviceConsole();
  serviceAutoTx();
  feedWdt();

  static uint32_t lastIdleYield=0u;
  if(!gCollectingFrame && !gFrameReady && (uint32_t)(micros()-lastIdleYield)>200000u) {
    lastIdleYield=micros();
    yield();
  }
}

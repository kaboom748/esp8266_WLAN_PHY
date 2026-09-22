# ESP8266 — Standalone RF PHY and OOK / ASK / FSK / M-FSK / QPSK / QAM Modulations
## Consolidated Reverse-Engineering Project Reference — v2.2

**Initial consolidation date: 2026-09-13 — v2.1 revision: 2026-09-18 — v2.2 silicon-validation revision: 2026-09-22**  
**Status: CONSOLIDATED REFERENCE — software/ROM + hardware boundaries + measured silicon validation**  
**Scope: ESP8266 / LX106 / mask-ROM / PHY v6 / libphy / libpp / libnet80211 / librftest.a from the analyzed corpus**

**v2.1 revision — additional audited corpus:** local 64 KiB mask ROM plus the supplied `libphy.a`, `libpp.a`, and `libnet80211.a`. v2.1 integrates targeted decompilation of ASK/M-ASK, RX gain, noise-floor, IQ_EST, DC/IQ calibration and PWDET/SAR paths.

**v2.2 revision — bench/silicon validation:** controlled ESP8266 measurements add direct evidence for IQ_EST/E4 freshness, the START/DONE rearm handshake, OOK envelope tracking at 10 kHz with `IQ_N=16`, rejection of `0x60009824` as a fast OOK substitute in the tested configuration, and measured ROM/EDGE/SOFT acquisition costs. These are project bench results, not official Espressif specifications.

> **Important — meaning of the word “reference.”** This document is the consolidated reference **for the reverse-engineering project**. It is not an official Espressif document and must not be presented as one.

> **Objective.** Merge the four project OOK/ASK/FSK references without loss, correct ambiguities or errors identified during later passes, then add the QPSK/QAM body of work and the direct analysis of `librftest.a`.

> **Normative rule.** Parts I through XII of this document form the normative v2.2 synthesis. The appendices preserve the four complete v1.0 references and the QAM v0.60→v0.83 history so that no source information is lost. Where historical wording differs from the v2.2 synthesis, the v2.2 synthesis takes precedence.

---

# TABLE OF CONTENTS

1. Evidence convention and definition of percentages
2. Executive summary and status of the four modulation families
3. Common PHY architecture and block map
4. TX OOK / ASK / M-ASK
5. RX OOK / ASK / M-ASK
6. TX FSK / M-FSK
7. RX FSK / M-FSK
8. TX QPSK / 16-QAM / 64-QAM
9. RX QPSK / 16-QAM / 64-QAM
10. Direct analysis of `librftest.a`
11. Consolidated map of registers, functions, and boundaries
12. Corrections, supersessions, remaining unknowns, and experimental plan
13. Appendices — four complete v1.0 references + detailed QAM history

---

# PART I — EVIDENCE CONVENTION

## 1. Three levels that must no longer be conflated

The project now explicitly distinguishes three levels.

### Level A — demonstrated software / ROM / ABI / MMIO

This level covers what can be statically closed from the actual code:

```text
mask-ROM
libphy.a
libpp.a
libnet80211.a
librftest.a
DWARF
relocations
symbols
strings
MMIO / PBUS / I²C accesses
structures and ABI
```

A “100% software/static” status means that the **CPU contract and command sequence** are closed for the stated scope. It does not mean that the silicon's analog response is known.

### Level B — strongly inferred hardware architecture

This level groups conclusions imposed by software flow but whose internal electrical name is not exposed: the QAM mapper behind `RATE`, the IQ_EST correlator, the NCO/phase accumulator behind `tone_control`, the exact role of certain handshake bits, and so on.

### Level C — silicon / RF validation

This level requires actual measurement: dBm, MMIO→RF delay, settling, phase, jitter, sensitivity, BER, gain in dB, the `tone_control→Hz` law, freshness of CFO on a non-802.11 tone, absolute `E4→dBm` transfer, upper OOK bandwidth limit, etc.

## 2. Status convention

| Mark | Meaning |
|---|---|
| **100% software** | CPU/ROM/ABI sequence closed in the corpus |
| **~99% structural** | architecture imposed with very high confidence; exact electrical name may remain open |
| **candidate** | technically plausible path, not demonstrated |
| **ruled out** | hypothesis incompatible with the analyzed corpus |
| **silicon validation** | physical question that cannot be resolved statically |

## 3. Overall understanding summary

The percentages below describe **software/ROM understanding**, not a guarantee that an RF modem has already been validated on a bench.

| Family | TX software/ROM | RX software/ROM | Comment |
|---|---:|---:|---|
| OOK | **100%** | **100% software for the canonical fixed-gain path** | generation + IQ_EST + gain/PBUS/noise-floor closed; E4 OOK detection and fresh START/DONE handshake validated on silicon to at least 10 kHz in the tested setup; absolute RF calibration remains open |
| ASK / M-ASK | **100%** | **100% software for the canonical fixed-gain path** | multilevel generation + RX gain + E/Corr²/DC² metrics closed; actual dynamic range remains to be measured |
| FSK / M-FSK | **100%** | **100%** | TX command path and RX CFO contract closed; non-802.11 response remains to be validated |
| Standard Wi-Fi QPSK / QAM | **~95–99% at the CPU interface** | **~90–95% at the CPU interface** | native mapper/demapper behind a hardware boundary |
| Arbitrary proprietary QPSK/QAM | **not closed** | **not closed** | no direct symbol/IQ/LLR port found |

The distinction between “standard QAM understood” and “proprietary QAM feasible” is fundamental. The CPU can select the native modulation, but the corpus does not expose an arbitrary constellation-point port.

---

# PART II — COMMON PHY ARCHITECTURE

## 4. Global model

The project's best consolidated model is:

```text
LX106 / firmware
   │
   ├── WDEV / PP / LMAC / DMA
   │
   ├── PHY registers 0x6000xxxx
   │
   ├── PBUS / internal RF I²C
   │
   ▼
hardware digital baseband
   │
   ├── tone / test generator
   ├── OFDM TX: scrambler → FEC → interleaver → mapper
   ├── OFDM RX: FFT/equalization → demapper → deinterleave/FEC
   ├── IQ_EST / correlations / energy
   ├── CFO / EVM / noise-floor
   ▼
analog chain / PLL / mixer / PA / LNA / ADC-DAC
```

From the CPU's point of view, the ESP8266 is therefore not a general-purpose SDR: several vector-oriented blocks exist, but their normal interface mainly exposes **commands, statistics, and packets**, not a continuous `I[n],Q[n]` stream.

## 5. Consolidated map of the tone / IQ_EST region

```text
0x60000504..0x60000560  TX BB attenuation / power table
0x6000057C              IQ_EST_CTRL
0x60000580              correlation R0
0x60000584              correlation R1
0x60000588              correlation R2
0x6000058C              correlation R3
0x60000590 bit4         RXMAX_EXT_DIG (digital RX extension control, not the primary gain code)
0x60000594              PBUS / test command
0x60000598              continuous/test control
0x6000059C              continuous/test control
0x600005A0              PBUS status
0x600005B8              tone slot 1
0x600005BC              tone slot 2
0x600005C4              tone slot 3
0x600005DC              DC / mean I
0x600005E0              DC / mean Q
0x600005E4              latched energy / power from the last completed IQ_EST measurement
0x600005E8              used by rftest/PBUS TX; exact sub-role still to be classified
```

## 6. Consolidated map of RX results / TXIQ

```text
0x60009800              shared CFO/EVM result
  bit0                  validity required by the CFO getter
  bits15:8              raw signed8 CFO
  bits28:16             metric read by phy_get_bb_evm()

0x60009804              observed in librftest.a do_rx_poll(); exact role open
0x60009824              hardware noise-floor readout; direct-read OOK probe was invariant in the tested OFF/ON experiment
0x60009860              digital TXIQ configuration/correction
0x600098DC              CFO-cycle handshake/finalization
0x60009B4C              observed in librftest.a do_rx_poll(); exact role open
0x60009B00 bit28        CCA control used behind phy_enable_agc()/phy_disable_agc()
0x60009B60              noise-floor subsystem control/start
0x60009B64[31:20]       processed noise-floor representation read by ram_get_noisefloor()
0x60009B64[11:0]        noise-floor configuration/start fields
```

### 6A. PWDET / SAR / TX power-control map added in v2.1

```text
0x60000D50 bit0         PWDET enable
0x60000D50 bit1         SAR/FM acquisition trigger
0x60000D5C bits21/23    PWDET configuration cleared by rom_en_pwdet()/tx_pwctrl_bg_init()
0x60000D60              background TX power-control control
0x60000D80..0x60000D9C  bank of eight SAR results
```

The software path is closed as **TX power measurement/calibration**. It must no longer be presented as a demonstrated OOK RX detector. Absolute conversion of these codes to dBm remains a silicon/RF question.

## 7. Useful WDEV map

```text
0x3FF20004 bit31        high-level WDEV RX gate
0x3FF20038             observed in factory RX; exact role open
0x3FF2003C[19:16]      context gate used by phy_get_bb_freqoffset()
0x3FF20040             observed in factory RX; exact role open

0x3FF20C18             WDEV interrupt enable
0x3FF20C20             latched WDEV events
0x3FF20C24             event clear/acknowledge
0x3FF20C20.bit8        entry into the main RX path before discard/success

0x3FF20CDC             TX DMA descriptor/control
0x3FF20CE0             TX PPDU/PLCP control: LENGTH/RATE/KID/HT
0x3FF20CE4             HT-SIG low 32 bits
0x3FF20CE8             Duration/ID control
```

---

# PART III — TX OOK / ASK / M-ASK

## 8. Common hardware primitive

The standalone generator is based on tone slot 1:

```text
TONE1 = 0x600005B8
```

The reconstructed canonical packing of `rom_start_tx_tone()` is:

```text
31        28 27                  18 17          10 9                    0
┌───────────┬──────────────────────┬──────────────┬──────────────────────┐
│ preserved │ mode/test region     │ scale code   │ raw tone_control     │
│           │ bit18 = normal gate  │ 8 bits       │ stimulus / step      │
└───────────┴──────────────────────┴──────────────┴──────────────────────┘
```

The ROM does not explicitly mask `tone_control` to 10 bits. The `0x3FF` mask used by standalone firmware is a **project safety measure**, not an instruction observed in the ROM.

## 9. OOK

Canonical OOK:

```text
bit18 = 1 → tone active
bit18 = 0 → tone disabled in the slot
```

The TX clock remains active between symbols. `rom_stop_tx_tone()` must not be called for every symbol because that routine also disables the global TX clock.

Mask:

```c
#define TONE_GATE_MASK 0x00040000u
```

Modulation must use a targeted read-modify-write (RMW) of the slot so that `tone_control`, scale, and upper modes are preserved.

## 10. ASK / M-ASK

The digital field is:

```text
bits17:10
```

with the exact encoding:

```text
digital_field = (-digital_scale) mod 256
```

and the canonical path uses `digital_scale = 0..63`.

TXIQ demonstrates that this field can be rewritten while the TX clock remains active. This closes the software amplitude-modulation mechanism, but not the RF `code→dBm` transfer law.

## 11. What is closed / what remains physical

Closed in software: address, gate, scale, RMW, slot initialization, `MEMW` discipline, separation of digital/analog scale, and keeping the TX clock active.

To be measured: actual extinction, amplitude/power slope, phase during hot update, latency, jitter, number of distinguishable M-ASK levels, and absolute power.

### 11A. PWDET/SAR: TX power calibration, not the modulator

ROM and PHY v6 decompilation closes the path:

```text
rom_en_pwdet()
  ↓
SAR init
  ↓
0x60000D5C configuration
  ↓
0x60000D50[0] enable
```

`ram_get_fm_sar_dout()` triggers acquisition through `0x60000D50[1]`, then `read_sar_dout()` reads eight results from `0x60000D80..0x60000D9C`.

`rom_get_power_db()` then builds a relative logarithmic power measurement from the SAR results, while `meas_tone_pwr_db()` measures a known TX tone using two consecutive acquisitions. `tx_pwctrl_cal()`, `tx_pwctrl_background()` and `ram_rfcal_pwrctrl()` use this chain to correct TX gain/attenuation.

**v2.1 normative conclusion:** PWDET/SAR is an excellent **TX baseline-power calibration** path and a useful way to characterize an M-ASK LUT, but it is neither the symbol-by-symbol modulator nor a demonstrated OOK RX detector.

---

# PART IV — RX OOK / ASK / M-ASK

## 12. Canonical architecture

```text
RF/PHY initialization
   ↓
rom_pbus_xpd_rx_on()
   ↓
RX clock ON
   ↓
PBUS debug: packet RX removed, RF RX preserved
   ↓
deterministic RX gain
   ↓
IQ_EST(mode=1,N)
   ↓
DONE
   ↓
0x600005E4 = latched window energy/power from the completed measurement
   ↓
1 threshold: OOK
multiple thresholds: ASK / M-ASK
```

## 13. IQ_EST_CTRL

```text
0x6000057C
bit0       ENABLE
bit1       START
bits16:2   N[14:0]
bit18      MODE
bit31      DONE
```

One demonstrated calibration setting is `MODE=1, N=1024`.

The same block also exposes:

```text
R0..R3              statistics/correlations
DC_I / DC_Q          signed means
E4                   energy / power
```

### 13A. v2.2 silicon validation — E4 freshness and direct IQ_EST rearm

Bench measurements on 2026-09-22 refine the CPU-visible behavior of this block. In the tested configuration, `0x600005E4` behaves as a **latched result of the most recently completed IQ_EST measurement**, not as a continuously refreshed RSSI register and not as a calibrated dBm value. A no-trigger/free-read experiment returns the same captured E4 value repeatedly until another measurement is launched.

The project tested four acquisition strategies:

```text
ROM    : canonical enable/start/wait/read path
PULSE  : legacy direct START pulse; does not prove DONE went low
EDGE   : START=0 → wait DONE=0 → START=1 → wait DONE=1 → read E4
SOFT   : software EN/START reset plus the same DONE-low/DONE-high handshake
```

The legacy `PULSE`/old-`rearm` method is **not a freshness guarantee**: DONE may already be high from the previous acquisition, so a poll for DONE=1 can terminate immediately. The `EDGE` method explicitly observes a complete DONE transition and is therefore the preferred direct-rearm experiment.

On a 256-sample run, both `EDGE` and `SOFT` observed `DONE` high on entry, then low, then high again for **256/256 samples**, with `low_to=0`, `high_to=0`, and no acquisition timeout. This validates the direct handshake on the tested silicon:

```text
START = 0
wait until DONE = 0
START = 1
wait until DONE = 1
read E4
```

The same bench series demonstrates OOK envelope tracking with `IQ_N=16` at **10 kHz**. With 256 samples nominally spaced by 10 µs (2.56 ms total) and the recorded E4 values reclassified at a 60,000-unit threshold, the measured transition counts were:

| acquisition path | transitions | expected for 10 kHz over 2.56 ms |
|---|---:|---:|
| ROM | 51 | ~51.2 |
| EDGE | 49 | ~51.2 |
| SOFT | 50 | ~51.2 |

This establishes **at least 10 kHz OOK tracking in the tested bench configuration**; it does not establish a 10 kHz upper bandwidth limit. A practical detector should use calibrated thresholds or hysteresis rather than treating the raw E4 code as an absolute RF unit.

Measured acquisition overhead in the same normalized run (`F_CPU=80 MHz`, `IQ_N=16`) was approximately:

```text
ROM   cost_avg ≈ 353 cycles
EDGE  cost_avg ≈ 396 cycles
SOFT  cost_avg ≈ 390 cycles
```

At zero requested inter-sample delay, the observed average sample spacing was approximately 382 cycles (ROM), 423 cycles (EDGE), and 419 cycles (SOFT), i.e. about 209 kS/s, 189 kS/s, and 191 kS/s at 80 MHz. These figures are implementation/bench measurements, not guaranteed hardware limits.

## 14. Important AGC/CCA correction

The wrappers historically named `phy_enable_agc()` / `phy_disable_agc()` must not be interpreted as proof of general AGC control in this corpus.

Exact tracing of the chip-v6 `phy_ops` table gives:

```text
phy_enable_agc()  → rom_chip_v5_enable_cca()
phy_disable_agc() → rom_chip_v5_disable_cca()
```

and the two ROM routines manipulate:

```text
0x60009B00 bit28

enable  : clear bit28
disable : set bit28
```

**v2.1 correction:** this closes the corresponding CCA software contract, but it does **not demonstrate** that `0x60009B00[28]` by itself freezes analog gain. For ASK/M-ASK, the canonical path therefore remains an **explicit deterministic PBUS gain**, without relying on an “AGC freeze” assumption.

### 14A. 15-bit composite RX gain — PBUS mapping closed

For a gain word `g`:

```text
g[2:0]   → PBUS(3,2)[5:3]

g[3]     → PBUS(3,1)[6]
g[4]     → PBUS(3,1)[5]
g[5]     → PBUS(3,1)[4]
g[6]     → PBUS(3,1)[3]
g[7]     → PBUS(3,1)[2]
g[8]     → PBUS(3,1)[1]
g[9]     → PBUS(3,1)[0]

g[10]    → PBUS(2,1)[1]
g[14:11] → PBUS(2,1)[6:3]
```

`PBUS(2,1)` preserves bits `0,2,7,8` through `old & 0x185`. The 15-bit code is therefore **composite** and must not be interpreted as a linear dB scale.

### 14B. Baseband gain — exact coarse/fine formula

`pbus_set_rxbbgain(gain)` programs:

```text
gain 0..5    → coarse = 0x00
gain 6..11   → coarse = 0x40
gain 12..17  → coarse = 0x60
gain 18..23  → coarse = 0x70
gain >=24    → coarse = 0x78

fine = ((gain % 6) << 3) | 0x06
```

then:

```text
PBUS force-test(3,1,coarse)
PBUS force-test(3,2,fine)
```

The `rom_pbus_force_test()` protocol uses `0x60000594` as command and `0x600005A0` as status/handshake.

### 14C. v6 RX gain table

`register_chipv6_phy_init_param()` sets:

```text
rx_gain_swp_step = phy_init_data + 2
```

`gen_rx_gain_table()` uses **16 step lengths** and **16 base codes** to generate **127 15-bit gain codes**.

Inside one segment, the pattern is structured in groups of six:

```c
if (r < 24) {
    q = r / 6;
    coarse = (1 << q) - 1;   // ladder/thermometer
    fine   = r % 6;
} else {
    coarse = 15;
    fine   = min(r - 24, 5);
}

code15 = base_code[stage] + (coarse << 3) + fine;
```

The AGC ramp is therefore not entirely fixed in ROM: it is partly parameterized by `phy_init_data`.

### 14D. `code15 → bb_gain` conversion

`set_rx_gain_testchip_50()` reconstructs:

```c
coarse_count = min(popcount((code15 >> 3) & 0x7F), 4);
fine         = code15 & 0x7;
bb_gain      = min(6 * coarse_count + fine, 29);
```

The PHY therefore works with **30 BB indices**, `0..29`.

### 14E. `0x60000590[4]`: RXMAX_EXT_DIG, not the primary gain register

The supplied build reaches `0x60000590` through base `0x60000200 + 0x390`.

`chip_v6_rxmax_ext_dig()` controls **bit 4** exactly. `set_rx_gain_cal_iq()` temporarily disables it during calibration and re-enables it afterward.

**Normative correction:** `0x60000590[4]` belongs to the **RXMAX_EXT_DIG** subsystem. Primary gain programming remains on PBUS.

### 14F. Noise-floor: two different representations

Both previously observed addresses are valid:

```text
0x60009824[11:0]   raw hardware measurement
0x60009B64[31:20]  processed representation read by ram_get_noisefloor()
```

Reconstructed formulas:

```c
raw = REG32(0x60009824) & 0xFFF;
nf_hw = (int16_t)((raw - 4095) >> 1);
```

and:

```c
x = (REG32(0x60009B64) >> 20) & 0xFFF;
nf = (int16_t)(((x + 1) >> 1) - 2048);
```

`0x60009B60` and the low bits of `0x60009B64` control/start the subsystem. `get_noisefloor_sat()` clamps the value to `[-392,-340]`; quarter-dB interpretation is consistent with Espressif naming but remains distinct from absolute RF calibration.

**v2.2 bench qualification of `0x60009824`:** a fast direct-read probe of `0x60009824[11:0]` stayed invariant across the tested OOK TX OFF/ON condition (decoded value `-684` in that run), while triggered E4 changed by orders of magnitude. Therefore `0x60009824` remains a valid noise-floor-related readout from the static analysis, but it is **not a demonstrated fast OOK envelope detector** and must not be substituted for fresh IQ_EST/E4 measurements on the basis of read speed alone.

### 14G. `rom_get_corr_power()` — three metrics, not one

`rom_get_corr_power()` reads:

```text
0x60000580 / 584 / 588 / 58C
0x600005DC / 5E0 / 5E4
```

and produces three outputs:

```text
out[0] = normalized E4 energy

corr_re = R0 + R3
corr_im = R1 - R2
out[1]  = normalized |corr|²

out[2]  = normalized |DC|²
```

An M-ASK RX can therefore use `E`, `Corr²`, and `DC²`, even though **E4 alone remains the simplest canonical metric**.

### 14H. Per-gain DC/IQ calibration

The RX table first stores the **127 gain codes**, then from about `+0x100` onward contains **30 eight-byte records** associated with BB indices `0..29`.

Each record holds four 9-bit DC-calibration values. `set_cal_rxdc()` applies them to:

```text
PBUS(4,1)
PBUS(5,1)
PBUS(4,2)
PBUS(5,2)
```

`set_rx_gain_cal_iq()` then orchestrates IQ calibration: temporary RXMAX_EXT_DIG disable, I²C/loopback configuration, **4 acquisitions**, averaging, clamping the two coefficients to `[-15,+15]` and `[-31,+31]`, then compact storage.

`ram_rxiq_get_mis()` confirms that `0x60000580..0x6000058C` are **complex mismatch/correlation statistics**, not raw I/Q samples.

## 15. Physical limit

The external-input→IQ_EST path is now both strongly reconstructed and partially validated on silicon. Fresh E4 acquisition via the canonical ROM path and direct START/DONE edge handshake is demonstrated, and OOK tracking is demonstrated to at least 10 kHz with `IQ_N=16` in the tested setup. Remaining physical questions include absolute `E4→dBm`, full `N→time` characterization, sensitivity, saturation, gain-code→dB, upper OOK bandwidth, temperature/device spread, and the actual number of distinguishable ASK levels.

---

# PART V — TX FSK / M-FSK

## 16. Principle

The project's fast-FSK keeps:

```text
RFPLL fixed
channel fixed
TX RF active
TX clock active
bit18 gate active
scale constant
```

and modifies only:

```text
tone_control = K0, K1, ... K(M-1)
```

through a targeted RMW of the low field of `0x600005B8`.

## 17. Software / RF boundary

The software path is 100% closed: code can modify the field, preserve it via RMW, keep the TX clock active, and avoid the slow RFPLL path.

However, only the silicon defines:

```text
Ki → frequency fi
settling time
phase continuity
jitter
spectral transients
```

## 18. Why `set_rf_freq_offset()` is not the fast modulator

This function follows an RFPLL sequence:

```text
set_rf_freq_offset()
  ↓
ram_rfpll_set_freq()
  ↓
write RFPLL SDM
  ↓
restart calibration
  ↓
wait_rfpll_cal_end()
```

It is therefore a retuning/correction mechanism, not the canonical symbol-by-symbol path.

---

# PART VI — RX FSK / M-FSK

## 19. Exact CFO result

```text
BB_CFO_RESULT = 0x60009800
bit0          = CFO validity condition
bits15:8      = raw signed8 CFO
```

The getter applies:

```text
WDEV gate: ((0x3FF2003C >> 16) & 0xF) < 8
```

then:

```text
CFO = (raw_signed8 * 107) >> 6
```

Otherwise the sentinel is:

```text
0x7FFF
```

## 20. Exact handshake

After every attempt, valid or invalid:

```c
r = REG32(0x600098DC);
r |= 0x0F;
REG32(0x600098DC) = r;
```

The software role is closed as **CFO-attempt finalization/handshake**. The detailed electrical name of each bit (`ACK`, `CLEAR`, `REARM`, `W1C`, strobe, etc.) is not required to reproduce the CPU contract.

## 21. Measurement / correction separation

```text
phy_meas_freq_offset  = latest CFO measurement
phy_freq_offset       = applied state/correction
```

`phy_get_freq_param(corr_out,meas_out)` explicitly exposes these two states in that order.

## 22. Physical limit

RX FSK is 100% closed **at the CPU/static scope**, but silicon testing is still required to demonstrate that the baseband produces fresh CFO on an arbitrary non-802.11 tone/FSK signal and to determine the update rate.

---

# PART VII — TX QPSK / QAM

## 23. First conclusion: the native QAM mapper exists and is selected by RATE

The field:

```text
esf_tx_desc_s.rate  @ offset +8, 8 bits
```

is loaded by `lmacSetTxFrame()`. For legacy OFDM, the low nibble is written to:

```text
0x3FF20CE0[15:12]
```

and length is written to:

```text
0x3FF20CE0[11:0]
```

The consolidated word layout is:

```text
31                             25 24 23               16 15         12 11             0
┌────────────────────────────────┬──┬───────────────────┬─────────────┬────────────────┐
│             0                  │HT│       KID         │    RATE     │     LENGTH     │
│                                │  │      8 bits       │    4 bits   │     12 bits    │
└────────────────────────────────┴──┴───────────────────┴─────────────┴────────────────┘
```

Pseudo-code:

```c
plcp1 =
      (length & 0x0FFF)
    | ((rate & 0x0F) << 12)
    | ((uint32_t)kid << 16)
    | (is_ht ? 0x01000000u : 0);
```

## 24. Legacy rate → modulation/coding table

| CPU/PHY code | Data rate | Modulation | Code rate |
|---:|---:|---|---:|
| `0x0B` | 6 Mb/s | BPSK | 1/2 |
| `0x0F` | 9 Mb/s | BPSK | 3/4 |
| `0x0A` | 12 Mb/s | QPSK | 1/2 |
| `0x0E` | 18 Mb/s | QPSK | 3/4 |
| `0x09` | 24 Mb/s | 16-QAM | 1/2 |
| `0x0D` | 36 Mb/s | 16-QAM | 3/4 |
| `0x08` | 48 Mb/s | 64-QAM | 2/3 |
| `0x0C` | 54 Mb/s | 64-QAM | 3/4 |

The `0x0F / 0x0E / 0x0D / 0x0C` group is especially useful experimentally: **same 3/4 code rate, different constellation**.

## 25. Exact mapper boundary

For legacy rates, no second separate CPU register is programmed for:

```text
MODULATION
FEC_RATE
PUNCTURE_MODE
INTERLEAVER_MODE
CONSTELLATION_INDEX
```

The model is therefore:

```text
CE0.RATE
   ↓
internal PHY decoder
   ├─ modulation family
   ├─ coding rate
   ├─ puncturing
   ├─ interleaver parameters
   └─ constellation mapper mode
```

The TX WDEV bank is functionally classified as:

```text
0x3FF20CDC  DMA/control
0x3FF20CE0  PPDU/PLCP control
0x3FF20CE4  HT-SIG
0x3FF20CE8  Duration/ID
```

None of these words is an `I`, `Q`, constellation-index, or direct coded-bit port.

## 26. Downstream closure toward WDEV

`lmacTxFrame()` calls `lmacSetTxFrame()`, then `wDev_EnableTransmit(index,aifs,backoff)`. The `wDev_EnableTransmit()` ABI carries neither `rate`, nor `I/Q`, nor a constellation. `rate` is not reread after CE0 preparation on the legacy path.

This reinforces that the standard CPU modulation boundary is **CE0.RATE**.

## 27. Raw freedom and fixed rate

`wifi_send_pkt_freedom` / `ieee80211_freedom_output()` let the caller choose a raw MAC frame, but the path converges through `ppTxPkt → ppProcessTxQ → lmacTxFrame`.

Therefore:

```text
raw MAC ≠ raw PHY
```

Fixed-rate control can select the native constellation, but does not provide arbitrary coded bits, symbols, or I/Q.

## 28. `tx_cont_*` and “iqview”

Continuous TX mainly manipulates:

```text
0x60000594
0x60000598
0x6000059C
```

while saving/restoring test state. It receives no payload, rate, I/Q, or constellation index.

The term “iqview” in the factory test refers to the test mode intended for use with an external IQView instrument; it is not an internal CPU I/Q port.

## 29. Audit of named APIs

The symbol audit of `libphy`, `libpp`, `libnet80211`, ROM, and now `librftest.a` reveals no named API for:

```text
scrambler bypass
FEC bypass
interleaver bypass
coded-bit input
QAM mapper input
constellation index
raw IQ TX FIFO
```

This does not prove that no anonymous MMIO exists physically, but it very strongly closes the exposed software surface.

## 30. TXIQ: what the CPU actually controls

The register:

```text
0x60009860
```

contains TXIQ correction fields:

```text
bits28:24  signed/encoded gain_code
bits23:18  signed/encoded phase_code
bits17:16  forced to 11 in the observed path
bit0       enabled/latched by phy_bb_rx_cfg(), exact electrical role unnamed
```

The coefficients are **mismatch corrections**, not an arbitrary rotator. The phase formula is a normalized error metric, which rules out the idea of directly producing 0/90/180/270° by simply writing the trim.

## 31. TXIQ vector stimuli

The observed states:

```text
0x10B ↔ 0x20B   gain pair: two separate orthogonal rails with very high confidence
0x00B ↔ 0x04B   phase pair: A+B / A-B-type cross-combinations with high confidence
```

demonstrate a **miniature calibration vector generator**. However, the four signs required for complete QPSK are not exposed in the observed standard path.

Physical bit 25 of the upper nibble is not a sign candidate on the standard path: the analyzed TXIQ sequences explicitly hold it at zero.

## 32. Verdict on proprietary TX QAM

Current status:

```text
standard Wi-Fi QAM via native mapper          : demonstrated
QPSK/16QAM/64QAM selection by rate            : demonstrated
direct injection of arbitrary QAM symbols     : not found
CPU TX I/Q FIFO                               : not found
factory mapper bypass                         : not found
TXIQ as QPSK/QAM synthesis                    : partial path, not closed
```

Without a new hardware port, the most credible path is now to **mathematically repurpose the 802.11 pipeline**: compute the input data so as to obtain coded-bit / symbol sequences compatible with scrambler+FEC+puncturing+interleaver constraints. This possibility has not yet been demonstrated end-to-end.

---

# PART VIII — RX QPSK / QAM

## 33. IQ_EST is not a raw I/Q stream

`rom_dc_iq_est(mode,N,out)`:

```text
configures IQ_EST
reads 0x600005DC / 0x600005E0
arithmetic shift
signed division by N+1
returns mean I, mean Q
```

`N=0` is accepted by software, but that means **minimum window**, not proof of an instantaneous I/Q ADC sample.

The registers:

```text
0x60000580..0x6000058C
```

are read as correlation/matrix statistics. The code forms, among others:

```text
R0 ± R3
R1 ± R2
```

and performs 64-bit gain/phase mismatch calculations. They do not constitute a demonstrated `I_symbol/Q_symbol` FIFO.

## 34. `phy_adc_read_fast()` is not the RF I/Q ADC

The function uses the SAR/TOUT subsystem. It is therefore ruled out as a raw RF I/Q capture path.

## 35. Standard RX interface after the demapper

The exact `RxControl` is 12 bytes and exposes packet metadata: RSSI, rate/sig_mode, length, MCS, CWB, HT length, smoothing, sounding, aggregation, STBC, FEC, SGI, rxend_state, ampdu count, channel, and noise floor.

It does not expose:

```text
I/Q
LLR
soft bits
per-symbol EVM
constellation index
```

`esf_buf_s` is 40 bytes and contains pointers/buffers, lengths, `chl_freq_offset`, bookkeeping, and descriptor fields. Again, no soft-symbol buffer is present.

The model is:

```text
hardware digital baseband
  FFT / equalization
  derotation
  QPSK/QAM demapper
  deinterleaver
  FEC decode
  CRC / RX state
        ↓
bytes + RxControl + metadata
        ↓
LX106
```

## 36. CFO / EVM

`0x60009800` carries at least:

```text
bit0       validity required by the CFO path
bits15:8   signed raw CFO
bits28:16  metric read by phy_get_bb_evm()
```

`phy_get_bb_evm()` is not a normal packet-RX export: in the standard corpus, its only direct call found is from `fix_cache_bug()`, an initialization/cache sequence. That call must therefore not be interpreted as continuous per-packet EVM measurement.

## 37. Consequence for proprietary RX QAM

The standard path does not allow recovery of a complex symbol before FEC. The scenarios still open are:

```text
A. hidden pre-FEC / pre-demapper tap
B. internal SRAM/FIFO not referenced by the SDK
C. digital-baseband test mode
D. native Wi-Fi pipeline, proprietary protocol above it
E. IQ_EST-assisted statistical detection instead of classical QAM
```

Classical proprietary RX QAM therefore remains more constrained than TX.

---

# PART IX — DIRECT ANALYSIS OF `librftest.a`

## 38. Identity of the analyzed archive

The file actually supplied to the project is:

```text
name      : librftest.a
size      : 83,626 bytes
SHA-256   : 01c9b9712cd5772823b6b647fa2181c8b594162b5b7091663b8b3bd482400b8e
```

**Important correction:** this exact local size supersedes earlier estimates based on GitHub web display (`60.8 kB`, `81.7 kB`, etc.). The normative size of the analyzed file is **83,626 bytes**.

Archive members:

```text
bb_common.o
crc.o
mac_common.o
rftest_func.o
```

The archive is not stripped and retains many function names.

## 39. Important symbols

Among the defined symbols:

```text
ate_txframe_dut
do_rx_poll
set_tone_freq_step
get_rx_tone_pwr
iqmis_set
get_iqmis_cal
settxframe_rate
test_tx_frame
tx_data_frame
FillTxPacket
WifiTxStart
WifiRxStart
esp_tx_func
esp_tx_func_org
esp_rx_func
tx_pocket_test_enable
tx_pocket_test_func
wifitxout_func
set_tx_pbus_on
```

No symbol named `qam`, `qpsk`, `mapper`, `interleaver`, `coded-bit`, `constellation`, `llr`, or I/Q FIFO was found.

## 40. Factory RX: `freq_offset` positively closed

`do_rx_poll()` is 0x2B3 bytes long and contains a direct call relocation to:

```text
phy_get_bb_freqoffset
```

The same function contains the string:

```text
Correct: %d Desired: %d RSSI: %d noise: %d gain: %d err: %d err_fcs: %d freq_offset: %d
```

Thus the factory `freq_offset` field does indeed come from the **same CFO getter** as the RX path analyzed in `libphy/libpp`. This is no longer a numerical hypothesis.

## 41. New RX candidates observed in `do_rx_poll()`

The function's literal pool/accesses notably expose:

```text
0x60009800   known: CFO/EVM
0x60009804   new / exact role open
0x60009824   known: noise-floor
0x60009B4C   new / exact role open
0x3FF20038   new / exact role open
0x3FF2003C   known: CFO context gate
0x3FF20040   new / exact role open
```

These addresses are now the best targets for deeper factory-RX mapping.

## 42. Factory TX: packet graph

The public path can be summarized as:

```text
esp_tx_func()
  ↓
esp_tx_func_org()
  ↓
tx_pocket_test_enable()
  ↓
FillTxPacket()
  ↓
WifiTxStart()
  ↓
test_tx_frame()
```

The lower ATE path contains:

```text
tx_pocket_test_func()
  ↓
ate_txframe_dut()
  ↓
fill_txdataframe()
  ↓
fill_tx_frame()
  ↓
tx_data_frame()
  ↓
PLCP/WDEV
```

`ate_txframe_dut()` is 0x83B bytes long. Despite its size, its graph remains that of a frame/PLCP generator plus test/calibration infrastructure, not a direct constellation port.

## 43. Factory TX registers

`ate_txframe_dut()` notably exposes:

```text
0x60000504
0x60000524
0x3FF2006C
0x3FF20C48
0x3FF20C4C
0x3FF20C94
0x3FF20004
0x3FF20000
0x3FF20C68
0x3FF2001C
0x3FF20C00
0x3FF20E44
```

`0x60000504` and `0x60000524` fall inside the already-classified `0x60000504..0x60000560` TX BB attenuation/power table. They therefore do not constitute an obvious new QAM port.

## 44. Factory IQ calibration

The archive also contains:

```text
iqmis_set()
get_iqmis_cal()
get_rx_tone_pwr()
set_tone_freq_step()
```

The observed IQ paths converge on the already-mapped tone-slot/calibration family, including `0x600005C4` for slot 3 in the relevant sequences. This enriches factory instrumentation without providing a symbol stream.

## 45. `librftest.a` conclusion

For QAM:

```text
named mapper bypass                     : not found
TX I/Q FIFO                             : not found
RX soft-symbol / LLR                    : not found
factory CFO via phy_get_bb_freqoffset   : demonstrated
additional factory metrics              : demonstrated
additional RX MMIO to classify          : yes
```

This archive **weakens the factory-TX-bypass hypothesis** and **strengthens the RX instrumentation/debug path**.

---

# PART X — CONSOLIDATED PRIMITIVE MAP

## 46. TX tone / RF

```text
rom_set_txclk_en
rom_set_ana_inf_tx_scale
rom_start_tx_tone
rom_stop_tx_tone
rom_txtone_linear_pwr
rom_pbus_xpd_tx_on/off
rom_pbus_set_txgain
set_rf_freq_offset
ram_rfpll_set_freq
wait_rfpll_cal_end
```

## 47. RX energy / gain / calibration

```text
rom_iq_est_enable
rom_iq_est_disable
rom_get_corr_power
ram_rxiq_get_mis
ram_rxiq_cover_mg_mp
ram_rfcal_rxiq
set_rx_gain_cal_iq
set_rx_gain_testchip_50
set_cal_rxdc
gen_rx_gain_table
rom_pbus_set_rxgain / ram_pbus_set_rxgain
pbus_set_rxbbgain
rom_pbus_force_test
rom_pbus_xpd_rx_on/off
ram_pbus_debugmode
pbus_workmode
read_hw_noisefloor
ram_get_noisefloor
ram_set_noise_floor
ram_start_noisefloor
get_noisefloor_sat
chip_v6_rxmax_ext_dig
```

### 47A. TX power measurement / calibration

```text
rom_en_pwdet
ram_get_fm_sar_dout
read_sar_dout
rom_get_power_db
meas_tone_pwr_db
tx_pwctrl_bg_init
tx_pwctrl_cal
tx_pwctrl_background
ram_rfcal_pwrctrl
```

## 48. CFO / QAM RX

```text
phy_get_bb_freqoffset
phy_get_bb_evm
phy_get_freq_param
wDev_ProcessFiq
wDev_ProcessRxSucData
HdlChlFreqCal
rom_dc_iq_est
ram_rxiq_get_mis
```

## 49. Standard QAM TX

```text
ieee80211_freedom_output
ppTxPkt
ppProcessTxQ
lmacTxFrame
lmacSetTxFrame
wDev_EnableTransmit
```

---

# PART XI — CORRECTIONS AND SUPERSESSIONS

## 50. Consolidated corrections

### 50.1 “Official”

Every occurrence of “official reference” means **the project's official reference**, never an official Espressif publication.

### 50.2 100% percentages

A “100%” status is always bounded by the **explicitly defined software/static scope**. It does not automatically include the silicon's RF response.

### 50.3 10-bit `tone_control`

The `0x3FF` mask is a recommended protection for standalone firmware. It is not a mask explicitly executed by `rom_start_tx_tone()`.

### 50.4 AGC / CCA

The `phy_enable_agc/phy_disable_agc` wrappers must not be used as proof of full AGC control. In this corpus, the relevant operation table ties them to CCA.

### 50.5 `phy_adc_read_fast`

Ruled out as an RF I/Q access path: it belongs to the SAR/TOUT ADC.

### 50.6 TXIQ phase

`txiq_phase` is a normalized mismatch metric, not an angle in degrees. The trim is not an arbitrary QPSK rotator.

### 50.7 Bit 25 in TXIQ stimuli

Bit 25 is no longer a serious “sign/quadrant” candidate on the standard path; the analyzed sequences force it to zero.

### 50.8 `tx_cont` / iqview

Continuous/test mode provides no payload/IQ/constellation input. “iqview” describes use with an external instrument, not an internal I/Q port.

### 50.9 Raw freedom

`wifi_send_pkt_freedom` supplies a raw MAC frame but rejoins the standard PHY pipeline. Raw MAC is not raw PHY.

### 50.10 IQ_EST in QAM RX

IQ_EST is a window-statistics engine: mean/DC, correlations, and power. It must not be presented as a symbol FIFO.

### 50.11 `phy_get_bb_evm()`

Its call from `fix_cache_bug()` is an init/cache path; it is not a normal per-packet EVM export.

### 50.12 Factory `freq_offset`

Now closed by `librftest.a`: `do_rx_poll()` directly calls `phy_get_bb_freqoffset()`.

### 50.13 Size of `librftest.a`

The analyzed file is exactly **83,626 bytes**; previously cited web-display sizes are no longer normative.

### 50.14 AGC “freeze”

Earlier research wording could suggest that `phy_disable_agc()` was a demonstrated gain hold. **Superseded:** the exact path reaches the ROM CCA primitives and `0x60009B00[28]`. Analog-gain freeze is not demonstrated by that bit alone. Canonical ASK/M-ASK RX uses an explicitly fixed PBUS gain.

### 50.15 `0x60000590`

**Superseded:** `0x60000590` is not the primary gain-code register. Bit 4 is used by `RXMAX_EXT_DIG`. The primary composite gain is programmed through PBUS.

### 50.16 Noise-floor `0x60009824` vs `0x60009B64`

Both addresses are correct but represent different levels of the subsystem: `0x60009824[11:0]` is the raw hardware measurement; `0x60009B64[31:20]` is the processed representation read by `ram_get_noisefloor()`.

### 50.17 PWDET on RX

The hypothesis “PWDET = fast OOK RX detector” is not demonstrated and must not be normative. Decompilation ties it strongly to the **SAR / TX power measurement/calibration** chain.

### 50.18 `ram_get_corr_power`

In the exact supplied 2020 `libphy.a`, no exported `ram_get_corr_power` symbol is present. The normative implementation for this corpus remains `rom_get_corr_power()` (possibly reached through a function table). Older RAM mentions belong to other builds/history.

### 50.19 Scope of “100% software” ASK/M-ASK

v2.1 closes 100% of the **canonical fixed-gain functional software path**: TX modulation, PBUS/RX gain, IQ_EST, `get_corr_power`, noise-floor and the required DC/IQ calibration paths. It does not close analog code→dB transfer, dBm, saturation, sensitivity, BER or the number of truly separable M-ASK levels.

---

# PART XII — REMAINING UNKNOWNS AND RESEARCH PLAN

## 51. OOK/ASK/M-ASK

The canonical software path is closed at **100% within the analyzed corpus**, and v2.2 adds selected silicon validation. Demonstrated on the tested bench:

```text
E4 is latched per completed IQ_EST measurement
ROM IQ_EST path returns fresh E4 measurements
EDGE direct rearm: START↓ / DONE↓ / START↑ / DONE↑ is functional
SOFT rearm also produces fresh measurements
legacy PULSE/old-rearm is not a freshness guarantee
OOK envelope tracking is demonstrated to at least 10 kHz with IQ_N=16
0x60009824 did not respond as a fast OOK detector in the tested OFF/ON experiment
```

Remaining questions are physical/statistical:

```text
digital_scale→RF amplitude / dBm
PWDET/SAR→absolute dBm and drift
E4/Corr²/DC²→absolute RF level
full N→hardware-time law
gain code→actual gain in dB
bit18 extinction
latency/jitter and upper OOK bandwidth
hot-update phase
saturation
sensitivity/dynamic range
temperature/device spread
BER vs SNR/rate
number of truly separable M-ASK levels
```

Priority validation plan:

1. sweep `digital_scale=0..63` and build the RF amplitude/power curve;
2. compare that curve against internal PWDET/SAR metrics;
3. map `code15` / `bb_gain` against a known RF level;
4. measure `(E, Corr², DC²)` clusters for A0/A1/A2/A3 at fixed gain;
5. determine the upper OOK tracking limit and optimize the fresh EDGE path (e.g. CCOUNT-based timeout/polling);
6. compute `Dmin` and BER to objectively choose among OOK, 2-ASK, 4-ASK and possibly 8-ASK.

## 52. FSK

TX: measure `tone_control→Hz`, settling, and phase.  
RX: demonstrate fresh CFO on non-802.11 tone/FSK and measure update rate/latency.

## 53. QAM TX

Priorities:

1. formalize inversion/constraints of the scrambler→FEC→puncturing→interleaver→mapper chain;
2. determine which QPSK/16-QAM/64-QAM point sequences are reachable from input bytes;
3. test the four 3/4 rates `0x0F/0x0E/0x0D/0x0C` to isolate constellation effects;
4. pursue anonymous MMIO/test modes only if new evidence appears;
5. validate with an external I/Q receiver.

## 54. QAM RX

Priorities:

1. disassemble `do_rx_poll()` more deeply;
2. classify `0x60009804`, `0x60009B4C`, `0x3FF20038`, and `0x3FF20040`;
3. passively snapshot `0x60009800` at WDEV `event.bit8`;
4. search for a pre-FEC SRAM/FIFO/test mode;
5. if no tap exists, accept the bytes+metadata boundary and design the protocol around the native Wi-Fi demapper or IQ_EST statistics.

## 55. Consolidated verdict

```text
OOK/ASK : software/ROM layer closed; selected IQ_EST/E4/OOK behavior validated on silicon
FSK     : software/ROM layer closed within the stated scope
QAM     : standard CPU interface very well understood,
          but arbitrary symbol access is still not exposed
```

The main unknown territory is no longer peripheral firmware. It now lies **inside the digital baseband beyond the CPU-visible registers**, especially around the QAM mapper/demapper.

---

# APPENDICES — COMPLETE PRESERVATION OF SOURCE REFERENCES

> The following appendices are included to satisfy the **no information loss** requirement. They preserve the content of the four v1.0 references, with only heading-level demotion for Markdown integration. Their historical content may use wording that is later refined by the normative v2.1 synthesis above.


---
# APPENDIX A — TX OOK / ASK — complete v1.0 reference

### ESP8266 — Standalone OOK / ASK Generator
#### Official Technical Reference of the Reverse-Engineering Project

**Single edition: v1.0**  
**Date: 2026-09-12**  
**Status: FROZEN — single release on request**  
**Scope: ESP8266 / mask-ROM + PHY v6 from the analyzed corpus**

> **Important — status of the word “official.”** This document is the official reference **for the reverse-engineering project carried out on the analyzed corpus**. It is not an official Espressif document and must not be presented as one.

> **Maintenance rule.** This edition is deliberately frozen. Later discoveries will not modify this document. They will continue to be integrated into the living master document `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

#### 1. Purpose

This document describes how to use the ESP8266 as a **standalone OOK and ASK/M-ASK RF generator**, after one-time RF/PHY initialization, **without returning to Wi-Fi operation**.

The target is not:

- to transmit 802.11 frames;
- to make OOK/ASK coexist with normal Wi-Fi;
- to restore PP/LMAC/WDEV after modulation;
- to turn the ESP8266 into a general-purpose I/Q SDR.

The target is:

```text
BOOT
  ↓
one-time RF-PLL-PHY initialization / calibration
  ↓
stop / abandon normal Wi-Fi traffic
  ↓
keep RF + PLL + TX clock active
  ↓
configure tone generator slot 1
  ↓
┌────────────────────────────────────────────┐
│ OOK   : bit18 ON/OFF                       │
│ ASK   : bits17:10 hot-updated              │
│ M-ASK : multiple digital_scale values      │
└────────────────────────────────────────────┘
  ↓
remain in RF-generator mode
```

---

#### 2. Confidence level

##### 2.1 What is considered fully decoded on the software-control side

| Item | Status |
|---|---:|
| tone slot 1 address | **100%** |
| OOK gate bit18 | **100%** |
| ASK mask bits17:10 | **100%** |
| `(-digital_scale)&0xFF` encoding | **100%** |
| hot ASK update | **100% demonstrated** |
| distinction between digital scale / analog scale | **100% software** |
| `start_tx_tone()` / `stop_tx_tone()` behavior relevant to OOK | **100%** |
| requirement to keep the TX clock active during symbols | **100%** |
| requirement to normalize the slot before modulation | **100%** |

##### 2.2 What is not a software gap but hardware characterization

The following points must be measured on silicon/RF:

- actual RF extinction when bit18 = 0;
- time between an MMIO write and the RF change;
- temporal jitter of the RF edge;
- OFF→ON phase continuity;
- `digital_scale → RF amplitude / power` relationship;
- actual number of distinguishable ASK levels;
- spectral transients;
- absolute power in dBm.

LX106 disassembly cannot provide these physical values.

---

#### 3. Corpus and functions used

The model is based on:

- analyzed ESP8266 mask-ROM;
- analyzed `libphy.a` / PHY v6;
- ROM paths `rfcal_pwrctrl`, `rfcal_rxiq`, `rfcal_txcap`, TXIQ;
- direct accesses to the tone block;
- internal IQ_EST and SAR instrumentation.

Identified useful ROM functions:

```text
0x40006B08  phy_get_romfuncs
0x40006C50  rom_set_channel_freq
0x40007268  rom_i2c_readReg
0x400072D8  rom_i2c_writeReg
0x4000754C  rom_pbus_set_rxgain
0x40007610  rom_pbus_set_txgain
0x400076FC  rom_pbus_xpd_tx_off
0x40007740  rom_pbus_xpd_tx_on
0x400077A0  rom_pbus_xpd_tx_on__low_gain
0x40007968  rom_rfpll_set_freq
0x40007EB4  rom_rfcal_pwrctrl
0x4000804C  rom_rfcal_rxiq
0x40008388  rom_rfcal_txcap
0x40008610  rom_rfcal_txiq
```

The tone generator is also exposed through `g_phyFuns`:

```text
+0x03C  rom_set_txclk_en
+0x050  rom_set_ana_inf_tx_scale
+0x068  rom_start_tx_tone
+0x06C  rom_stop_tx_tone
+0x070  rom_txtone_linear_pwr
```

In the analyzed dump:

```text
rom_start_tx_tone ≈ 0x400068B4
rom_stop_tx_tone  ≈ 0x4000698C
```

---

#### 4. Tone-generator registers

Three slots exist:

```text
0x600005B8   TONE SLOT 1
0x600005BC   TONE SLOT 2
0x600005C4   TONE SLOT 3
```

For OOK/ASK, **slot 1 is sufficient**.

The three direct ROM uses found (`rfcal_pwrctrl`, `rfcal_rxiq`, `rfcal_txcap`) use slot 1 and leave slots 2/3 disabled.

---

#### 5. Slot 1 packing

The reconstructed software behavior of `rom_start_tx_tone()` is:

```c
r = REG32(slot);
r &= 0xF0000000;
r |= raw_control;
r |= ((uint32_t)((0x100 - digital_scale) & 0xff) << 10);
r |= ((uint32_t)mode_code << 18);
REG32(slot) = r;
```

Practical representation:

```text
31        28 27                  18 17          10 9                    0
┌───────────┬──────────────────────┬──────────────┬──────────────────────┐
│ preserved │ mode/test region     │ scale code   │ raw tone_control     │
│           │ bit18 = normal gate  │ 8 bits       │ observed values 8/64 │
└───────────┴──────────────────────┴──────────────┴──────────────────────┘
```

##### 5.1 Important: field width

The ROM does **not** explicitly mask:

```c
raw_control &= 0x3ff;
mode_code   &= 0x3ff;
```

Those masks do not exist in `rom_start_tx_tone()`.

For standalone firmware, however, it is prudent to use:

```c
control_safe = tone_control & 0x3ff;
```

to ensure that an incorrect `tone_control` does not overlap the ASK field. This mask is **protection in our firmware**, not ROM behavior.

---

#### 6. Normal tone mode

For a normal tone:

```text
mode_code = 1
```

Therefore:

```text
1 << 18 = 0x00040000
```

Bit18 is therefore the active bit of normal mode.

`rom_stop_tx_tone()` clears exactly:

```text
0x00040000
```

and does not clear the other mode/test bits.

This is the main software proof that **bit18 is the minimal tone gate in normal mode**.

---

### PART I — OOK

#### 7. OOK principle

OOK = **On-Off Keying**.

The correct strategy is not:

```text
symbol 1: start_tx_tone()
symbol 0: stop_tx_tone()
```

because `rom_stop_tx_tone()`:

1. clears bit18;
2. then disables the global TX clock.

That would impose an unnecessary TX-path reinitialization between symbols.

The correct strategy is:

```text
PLL       remains active
RF TX     remains prepared
TX clock  remains active
slot1     remains configured

bit18 = 1 → ON
bit18 = 0 → OFF
```

---

#### 8. Canonical OOK constants

```c
#define TONE1_ADDR       0x600005B8u
#define TONE_GATE_BIT    18u
#define TONE_GATE_MASK   0x00040000u
```

##### ON

```c
slot |= TONE_GATE_MASK;
```

##### OFF

```c
slot &= ~TONE_GATE_MASK;
```

No other field should be modified during an OOK symbol.

---

#### 9. Canonical OOK primitive

Pseudo-C:

```c
static inline void xtensa_memw(void)
{
    __asm__ volatile ("memw" ::: "memory");
}

static inline uint32_t reg32_read(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void reg32_write(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

static inline void ook_set(bool on)
{
    uint32_t r;

    xtensa_memw();
    r = reg32_read(TONE1_ADDR);

    if (on)
        r |= TONE_GATE_MASK;
    else
        r &= ~TONE_GATE_MASK;

    xtensa_memw();
    reg32_write(TONE1_ADDR, r);
}
```

The relevant ROM sequences use `MEMW` barriers around MMIO accesses to the tone block. The standalone primitive should preserve this discipline.

---

#### 10. Why read-modify-write is required

Do not write:

```c
REG32(TONE1_ADDR) = TONE_GATE_MASK;
```

That would destroy:

- `tone_control`;
- the ASK scale;
- upper mode/test bits;
- potentially other preserved bits.

Always use a **targeted RMW**.

---

#### 11. Mandatory slot normalization before OOK

After TXIQ calibration, the slot may retain test/mode bits even though bit18 has been cleared.

It is therefore incorrect to begin with:

```c
REG32(TONE1_ADDR) |= TONE_GATE_MASK;
```

on an unknown state.

Before the first symbol, reconstruct a normal slot word:

```c
static inline uint32_t tone1_build_normal(
    uint32_t old_slot,
    uint16_t tone_control,
    uint8_t digital_scale,
    bool enabled)
{
    uint32_t r;
    uint8_t code = (uint8_t)(0u - digital_scale);

    r  = old_slot & 0xF0000000u;
    r |= ((uint32_t)tone_control & 0x3FFu);
    r |= ((uint32_t)code << 10);
    r |= ((uint32_t)(enabled ? 1u : 0u) << 18);

    return r;
}
```

Here, `&0x3FF` is the recommended standalone protection, not an observed ROM mask.

---

### PART II — ASK / M-ASK

#### 12. ASK principle

ASK = **Amplitude Shift Keying**.

The tone remains active and its digital component is modified through:

```text
bits17:10
```

The TXIQ path demonstrates that this field can be **hot-rewritten while the TX clock remains active**.

It is therefore the canonical mechanism for:

- 2-ASK;
- 4-ASK;
- 8-ASK;
- M-ASK in general, within measured RF limits.

---

#### 13. Exact digital-scale encoding

The field is not a direct positive amplitude integer.

The exact software encoding is:

```text
digital_field = (-digital_scale) mod 256
slot[17:10]   = digital_field
```

For the canonical path:

```text
digital_scale = 0..63
```

Examples:

| `digital_scale` | 8-bit field |
|---:|---:|
| 0 | `0x00` |
| 1 | `0xFF` |
| 2 | `0xFE` |
| 3 | `0xFD` |
| ... | ... |
| 63 | `0xC1` |

The raw field must therefore **never** be interpreted directly as a linear amplitude.

---

#### 14. Canonical ASK mask

```c
#define TONE_SCALE_SHIFT  10u
#define TONE_SCALE_MASK   0x0003FC00u
```

Primitive:

```c
static inline void ask_set_digital_scale(uint8_t digital_scale)
{
    uint32_t r;
    uint8_t code;

    /* decoded canonical path */
    if (digital_scale > 63)
        digital_scale = 63;

    code = (uint8_t)(0u - digital_scale);

    xtensa_memw();
    r = reg32_read(TONE1_ADDR);

    r &= ~TONE_SCALE_MASK;
    r |= ((uint32_t)code << TONE_SCALE_SHIFT);

    xtensa_memw();
    reg32_write(TONE1_ADDR, r);
}
```

This write preserves:

- bit18;
- `tone_control`;
- upper modes;
- bits31:28.

---

#### 15. M-ASK

A software M-ASK constellation can be represented by a table:

```c
static const uint8_t ask_levels_4[4] = {
    LEVEL0,
    LEVEL1,
    LEVEL2,
    LEVEL3
};
```

Then:

```c
ask_set_digital_scale(ask_levels_4[symbol & 3]);
```

**Caution:** software defines the codes, not the absolute RF amplitudes. `LEVEL0...LEVEL3` must be selected after instrumented characterization so that the resulting RF levels are properly spaced.

---

#### 16. ASK versus OOK

##### OOK

```text
bit18 changes
scale remains fixed
```

##### ASK

```text
bit18 remains 1
bits17:10 change
```

##### Possible combination

Firmware can also use:

```text
absolute logical OFF: bit18 = 0
active levels        : bit18 = 1 + multiple digital_scale values
```

This allows one truly gated symbol and multiple active levels, but the actual RF separation between levels must be measured.

---

### PART III — ANALOG SCALE

#### 17. `rom_set_ana_inf_tx_scale()`

This function splits a value `x` between:

- a digital component;
- an internal analog component.

Reconstructed pseudo-code:

```c
uint8_t set_ana_inf_tx_scale(uint8_t x)
{
    uint8_t analog_scale;
    uint8_t digital_scale;

    if (x < 64) {
        analog_scale  = 0;
        digital_scale = x;
    } else {
        analog_scale  = (uint8_t)(63 - x);
        digital_scale = 63;
    }

    /* internal I2C block 0x77, host 0, reg 9, bits7:0 */
    i2c_write_mask(0x77, 0, 9, 7, 0, analog_scale);

    return digital_scale;
}
```

The observed Xtensa branch is an unsigned comparison against `64`.

---

#### 18. Role of analog scale in a standalone modulator

Analog scale should not be used symbol-by-symbol for fast ASK.

Recommended architecture:

```text
analog scale
      ↓
range / baseline-level setting
      ↓
fixed during the frame

digital scale bits17:10
      ↓
fast ASK modulation
```

The analog portion goes through an internal PHY I²C bus; it is more transactional and serves a different purpose from direct slot RMW.

---

### PART IV — RF / PBUS / TX CLOCK

#### 19. `start_tx_tone()` does not enable the entire RF chain

The digital generator and RF chain are separate.

ROM calibrations show:

```text
RF/PBUS preparation
        ↓
RX RF OFF
        ↓
TX XPD ON
        ↓
scale / detector setup
        ↓
start_tx_tone()
```

Therefore, calling only `rom_start_tx_tone()` on a cold RF chain is not a demonstrated complete initialization procedure.

---

#### 20. PBUS

Main registers:

```text
0x60000594   PBUS command
0x600005A0   PBUS status
```

Reconstructed command format:

```text
bits15:14   bank
bits13:5    value (9 bits)
bits4:2     selector
bit1        START
bit0        preserved
```

Pseudo-code:

```c
cmd = REG32(0x60000594);
cmd &= 0xFFFF0001;
cmd |= (bank << 14);
cmd |= (value << 5);
cmd |= (selector << 2);
cmd |= 0x2;
REG32(0x60000594) = cmd;

while (REG32(0x600005A0) & 0x80000000)
    ;

REG32(0x60000594) &= ~0x2;
```

---

#### 21. Observed TX RF activation

##### TX OFF

`rom_pbus_xpd_tx_off()`:

```text
PBUS(6, 1,   0)
PBUS(1, 1,  12)
PBUS(2, 1,   0)
```

##### TX ON

`rom_pbus_xpd_tx_on()`:

```text
PBUS(2, 1,   1)
PBUS(7, 1,  95)
PBUS(0, 1,   x)
PBUS(1, 1, 127)
PBUS(6, 1, 127)
```

The low-gain variant notably uses:

```text
PBUS(7, 1, 0)
```

instead of `95`.

Selector `4 / bank 1` is used by `rom_pbus_set_txgain()`.

---

#### 22. TX clock

`rom_start_tx_tone()` begins with:

```text
rom_set_txclk_en(1)
```

`rom_stop_tx_tone()` performs:

```text
clear bit18
then
rom_set_txclk_en(0)
```

Fundamental consequence:

> **Fast OOK and ASK must keep the TX clock active.**

---

### PART V — STANDALONE STARTUP WITHOUT RETURNING TO WI-FI

#### 23. Correct definition of “disable Wi-Fi”

In this project, “disable Wi-Fi” means:

- stop producing 802.11 traffic;
- no longer allow PP/LMAC/WDEV to retake ownership of TX;
- prevent sleep/wakeup and RF reconfiguration during the session;
- **do not power down the PHY/RF required by the generator**.

A high-level call that completely disables the radio is therefore not equivalent to the desired goal.

---

#### 24. Recommended robust procedure

##### Phase A — boot / calibration

1. boot the silicon normally;
2. allow the PHY to perform the required initialization/calibration;
3. fix the RF channel/frequency;
4. prepare the RF test mode / TX chain;
5. prevent mechanisms that could put the RF back to sleep or reinitialize it;
6. permanently abandon Wi-Fi traffic.

##### Phase B — TX RF preparation

Starting from an initialized RF state:

```text
PBUS debug/test
RX RF off
TX XPD on
choose baseline gain / scale
TX clock on
```

The `rfcal_pwrctrl` and `rfcal_txcap` calibrations demonstrate this skeleton.

##### Phase C — tone normalization

1. select `tone_control`;
2. select the baseline scale;
3. fully rewrite slot 1 in `mode_code=1`;
4. stop using old TXIQ bits left by calibration.

##### Phase D — modulation

```text
OOK   → bit18 only
ASK   → bits17:10 only
M-ASK → bits17:10 only
```

##### Phase E — no Wi-Fi restoration

The firmware remains in generator state.

---

#### 25. Important limitation: fully bare-metal cold start

The corpus allows the **OOK/ASK primitives** to be completely decoded, but does not demonstrate one unique, universal minimal sequence from a completely cold reset that replaces all PHY/RF initialization/calibration.

The robust procedure is therefore:

> **initialize/calibrate RF once using the existing PHY/test path, then permanently take control of the generator.**

Do not present an isolated PBUS sequence as a guaranteed replacement for all analog silicon initialization.

---

### PART VI — FIRMWARE SKELETON

#### 26. Registers and masks

```c
#include <stdint.h>
#include <stdbool.h>

#define TONE1_ADDR        0x600005B8u
#define TONE_GATE_MASK    0x00040000u
#define TONE_SCALE_MASK   0x0003FC00u
#define TONE_SCALE_SHIFT  10u
#define TONE_CONTROL_MASK 0x000003FFu  /* local protection */
```

---

#### 27. MMIO access

```c
static inline void memw(void)
{
    __asm__ volatile ("memw" ::: "memory");
}

static inline uint32_t rd32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void wr32(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}
```

---

#### 28. Clean construction of slot 1

```c
static inline uint32_t build_tone1_normal(
    uint32_t previous,
    uint16_t tone_control,
    uint8_t digital_scale,
    bool gate)
{
    uint8_t scale_code;
    uint32_t r;

    if (digital_scale > 63)
        digital_scale = 63;

    scale_code = (uint8_t)(0u - digital_scale);

    r  = previous & 0xF0000000u;
    r |= ((uint32_t)tone_control & TONE_CONTROL_MASK);
    r |= ((uint32_t)scale_code << TONE_SCALE_SHIFT);
    r |= gate ? TONE_GATE_MASK : 0u;

    return r;
}
```

---

#### 29. Slot initialization

```c
static void tone1_init(uint16_t tone_control,
                       uint8_t digital_scale,
                       bool start_on)
{
    uint32_t old;
    uint32_t fresh;

    memw();
    old = rd32(TONE1_ADDR);

    fresh = build_tone1_normal(
        old,
        tone_control,
        digital_scale,
        start_on
    );

    memw();
    wr32(TONE1_ADDR, fresh);
}
```

Before this call, TX RF and the TX clock must be ready.

---

#### 30. OOK

```c
static inline void tone_ook(bool on)
{
    uint32_t r;

    memw();
    r = rd32(TONE1_ADDR);

    if (on)
        r |= TONE_GATE_MASK;
    else
        r &= ~TONE_GATE_MASK;

    memw();
    wr32(TONE1_ADDR, r);
}
```

Logical example:

```c
for (;;) {
    tone_ook(true);
    symbol_delay();

    tone_ook(false);
    symbol_delay();
}
```

`symbol_delay()` depends on the desired data rate and must be replaced by a deterministic timing mechanism appropriate to the final firmware.

---

#### 31. ASK

```c
static inline void tone_ask(uint8_t digital_scale)
{
    uint32_t r;
    uint8_t code;

    if (digital_scale > 63)
        digital_scale = 63;

    code = (uint8_t)(0u - digital_scale);

    memw();
    r = rd32(TONE1_ADDR);

    r &= ~TONE_SCALE_MASK;
    r |= ((uint32_t)code << TONE_SCALE_SHIFT);

    memw();
    wr32(TONE1_ADDR, r);
}
```

4-ASK example:

```c
static const uint8_t level[4] = {
    L0, L1, L2, L3
};

for (;;) {
    uint8_t sym = next_2_bits();
    tone_ask(level[sym]);
    symbol_delay();
}
```

The `L0...L3` values must be obtained by measurement.

---

#### 32. OOK + ASK levels

A useful extension:

```c
static inline void send_symbol(bool active, uint8_t level)
{
    if (!active) {
        tone_ook(false);
        return;
    }

    tone_ask(level);
    tone_ook(true);
}
```

This allows:

- OFF by gate;
- multiple active levels by digital scale.

---

### PART VII — CONCURRENCY AND TIMING

#### 33. Interrupts

For temporally clean modulation, a symbol transition must not be delayed arbitrarily by:

- Wi-Fi ISRs;
- PHY maintenance timers;
- sleep/wakeup;
- network tasks;
- channel/PLL reconfiguration.

Because the target does not need to return to Wi-Fi, it is preferable to remove/neutralize those sources instead of attempting coexistence.

---

#### 34. Atomicity

The RMW:

```text
read slot
modify field
write slot
```

is not atomic with respect to another writer of the same register.

The final firmware must have **one owner only of the tone slot**.

After Wi-Fi and concurrent calibrations are abandoned, that owner must be the modulation loop.

---

#### 35. `MEMW`

The ROM routines surround tone accesses with Xtensa `MEMW` barriers.

This means actual timing includes:

```text
LX106
 ↓
MEMW
 ↓
MMIO
 ↓
peripheral bus
 ↓
tone block
 ↓
DAC/RF
```

A CPU instruction duration is therefore not equal to the actual RF delay.

---

### PART VIII — INTERNAL INSTRUMENTATION

#### 36. IQ-estimator path

The IQ_EST block notably uses:

```text
0x6000057C  control
0x60000580
0x60000584
0x60000588
0x6000058C  correlations
0x600005E4  power/energy used by RXIQ
```

`M=1` is used by RXIQ correlation/power paths.

This path can compare stable tone states through internal loopback.

---

#### 37. TX detector / SAR path

`rom_txtone_linear_pwr(n,q)` accumulates:

```text
Σ ((out0 << q) / max(out1,1))
```

TXIQ uses:

```text
rom_txtone_linear_pwr(4, 10)
```

which is a Q10 metric across four acquisitions.

SAR outputs are reconstructed as:

```text
out0 = max(2×(s1+s2+s3) - 3×(s6+s7), 0)
out1 = max(3×(s4+s5)    - 3×(s6+s7), 0)
```

Each acquisition contains at least:

```text
ets_delay_us(25)
```

Therefore:

```text
txtone_linear_pwr(4,10) ≥ 100 µs of explicit delays
```

This path is useful for:

- comparing steady-state ON/OFF;
- comparing multiple ASK levels;
- calibrating a relative M-ASK table.

It is not fast enough to measure a very short OOK edge directly.

---

### PART IX — VALIDATION PROCEDURE

#### 38. OOK validation

In a controlled RF environment:

1. prepare RF/PLL/TX;
2. initialize the normal slot;
3. measure the bit18=1 state;
4. set bit18=0 without disabling the TX clock;
5. measure the OFF state;
6. repeat to verify stability;
7. measure the edge with fast external instrumentation.

Extract:

```text
P_ON
P_OFF
extinction = P_ON - P_OFF
t_on
t_off
jitter
OFF→ON phase if the instrument supports it
```

---

#### 39. ASK validation

For each selected `digital_scale`:

1. keep RF, PLL, TX clock, and bit18 constant;
2. write only bits17:10;
3. wait for steady state;
4. measure internal SAR level and external RF level;
5. build a table:

```text
digital_scale → relative level → measured dBm
```

Then choose ASK symbols to obtain genuinely distinguishable levels.

---

#### 40. What not to do

##### Do not implement OOK with `stop_tx_tone()` for every symbol

Wrong:

```text
start / stop / start / stop
```

because `stop_tx_tone()` disables the TX clock.

##### Do not use analog scale as the fast modulator

It goes through internal I²C; digital RMW is the demonstrated fast path.

##### Do not simply re-enable bit18 on an unknown slot

Always normalize slot 1 first.

##### Do not overwrite the entire register for one symbol

Always modify only the relevant field.

##### Do not allow sleep/wakeup to reinitialize RF

The generator assumes a stable RF/PLL state.

##### Do not assume `digital_scale=0` means RF=0

The digital code is known; the RF effect must be measured.

---

### PART X — FINAL MODEL STATE

#### 41. Canonical OOK

```text
register : 0x600005B8
field    : bit18

OFF = clear bit18
ON  = set bit18

PLL       : stable
TX clock  : ON
RF/XPD    : prepared
scale     : stable
control   : stable
```

---

#### 42. Canonical ASK

```text
register : 0x600005B8
field    : bits17:10

code = ((-digital_scale) & 0xff) << 10
```

Update:

```text
slot = (slot & ~0x0003FC00) | code
```

During M-ASK:

```text
bit18 = 1
TX clock = ON
RF/PLL = stable
analog scale = fixed
```

---

#### 43. Recommended complete architecture

```text
                   ESP8266
                      │
                      ▼
             boot + calibration
                      │
                      ▼
               channel / RF PLL
                      │
                      ▼
                abandon Wi-Fi
                      │
                      ▼
        PBUS / RX off / TX XPD on
                      │
                      ▼
                TX clock ON
                      │
                      ▼
      normalized slot1: mode_code=1
                      │
        ┌─────────────┴─────────────┐
        │                           │
        ▼                           ▼
       OOK                         ASK
    bit18 RMW                  bits17:10 RMW
        │                           │
        └─────────────┬─────────────┘
                      ▼
                  DAC / RF
```

---

#### 44. Exact boundary between “decoded” and “to be measured”

##### Decoded

```text
which register to write
which bit for OOK
which mask for ASK
how to encode scale
how to hot-update
why the TX clock must stay active
how not to destroy tone_control
how to normalize the slot
how to separate digital and analog scale
which internal instruments can be used
```

##### To be measured

```text
how many dB each scale produces
how much extinction bit18=0 produces
how many ns/µs an RF edge takes
how much jitter exists
whether phase remains continuous during OFF
how many M-ASK levels are actually usable
```

---

### PART XI — RF / COMPLIANCE NOTE

#### 45. Laboratory use

The generator operates in the ESP8266's 2.4 GHz RF chain. Tests must be carried out so as not to interfere with other radio systems.

Good practices:

- shielded or strongly attenuated environment;
- minimum necessary power;
- appropriate load/attenuation and instrumentation when conducted output is available;
- compliance with local rules applicable to RF emissions;
- do not use this technique to jam or interfere with third-party networks.

---

### PART XII — QUICK REFERENCE

#### 46. Cheat sheet

```text
TONE SLOT 1       = 0x600005B8
TONE SLOT 2       = 0x600005BC
TONE SLOT 3       = 0x600005C4

OOK GATE           = bit18
OOK MASK           = 0x00040000

ASK FIELD          = bits17:10
ASK MASK           = 0x0003FC00
ASK SHIFT          = 10
ASK ENCODING       = (-digital_scale) & 0xff
DIGITAL SCALE      = 0..63 (canonical path)

TONE CONTROL       = observed low field
SAFE LOCAL MASK    = 0x000003ff

NORMAL MODE        = 1

OOK SYMBOL:
    RMW bit18 only

ASK SYMBOL:
    RMW bits17:10 only

DO NOT:
    call stop_tx_tone() between symbols
    perform analog I2C writes between fast ASK symbols
    write the entire slot for every symbol
    reuse a post-TXIQ slot without normalization
```

---

#### 47. Final minimal pseudo-code

```c
rf_phy_boot_and_calibrate_once();
set_channel_and_lock_rf();
disable_wifi_protocol_forever_but_keep_rf_awake();
prepare_pbus_tx_path();
set_base_analog_power_once();
set_tx_clock(true);

tone1_init(TONE_CONTROL, INITIAL_DIGITAL_SCALE, false);

for (;;) {
    switch (next_mode()) {
    case MOD_OOK:
        tone_ook(next_bit());
        break;

    case MOD_ASK:
        tone_ook(true);
        tone_ask(next_ask_level());
        break;
    }

    wait_symbol_boundary();
}
```

The high-level functions `rf_phy_boot_and_calibrate_once()`, `disable_wifi_protocol_forever_but_keep_rf_awake()`, and `prepare_pbus_tx_path()` represent system integration. The OOK/ASK mechanism itself is fully defined by the primitives in this document.

---

### 48. Project's official conclusion for OOK/ASK

For the analyzed ESP8266 corpus, software control of the OOK/ASK generator is considered **closed**:

```text
OOK = gate bit18 of 0x600005B8
ASK = hot-update bits17:10 of 0x600005B8
```

The fast path must not disable the TX clock or RF between symbols.

Analog scale is a range-setting mechanism; digital scale is the fast ASK modulation mechanism.

The slot must be normalized once in normal-tone mode before any modulation.

Absolute RF values, latencies, and phase phenomena are deliberately excluded from the term “software decoding” and must be obtained experimentally.

---

#### 49. Document status

**Document:** `ESP8266_ASK_OOK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`  
**Edition:** 1.0  
**Release:** single  
**Future maintenance:** none — frozen document  
**Project continuation:** return to the living master document and continue reverse engineering, especially FSK/M-FSK.


---
# APPENDIX B — RX OOK / ASK — complete v1.0 reference

### ESP8266 — Standalone OOK / ASK Receiver
#### Official Technical Reference of the Reverse-Engineering Project

**Single edition: v1.0**  
**Date: 2026-09-12**  
**Status: FROZEN — released after 100% closure of the canonical RX software scope**  
**Scope: ESP8266 / mask-ROM + PHY v6 from the analyzed corpus**

> **Important — status of the word “official.”** This document is the official reference **for the reverse-engineering project carried out on the analyzed corpus**. It is not an official Espressif document and must not be presented as one.

> **Maintenance rule.** This edition is deliberately frozen. Later discoveries, speed optimizations, or RF characterization will not modify this edition; they continue in the living master document `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

#### 1. Purpose

This document describes how to use the ESP8266 as a **standalone OOK and ASK/M-ASK receiver**, after one-time RF/PHY initialization, **without decoding 802.11 packets** and without depending on PP/net80211 to obtain the receive metric.

The target is not:

- to receive Wi-Fi frames;
- to use the RSSI of a decoded packet;
- to make OOK depend on a hypothetical `CCA busy` bit;
- to turn the ESP8266 into a raw I/Q SDR;
- to require conversion of the internal metric to dBm;
- to restore Wi-Fi operation between symbols.

The target is:

```text
BOOT
  ↓
one-time RF-PLL-PHY initialization / calibration
  ↓
fix channel / PLL
  ↓
enable normal RF RX
  ↓
keep RX clock active
  ↓
remove Wi-Fi packet RX and take PBUS control
  ↓
set deterministic RX gain
  ↓
┌───────────────────────────────────────────────┐
│ IQ_EST M=1                                   │
│ wait for DONE                                │
│ read 0x600005E4                              │
│ one threshold      → OOK                     │
│ multiple thresholds → ASK / M-ASK            │
└───────────────────────────────────────────────┘
  ↓
repeat acquisition sequence
```

---

#### 2. Definition of “100%” in this reference

The 100% status of this document means:

> **100% of the canonical software path included in the procedure is directly demonstrated by the mask-ROM, `libphy.a`, relocations, MMIO/PBUS accesses, and calibration paths of the analyzed corpus.**

As with the TX reference, the following physical quantities are not classified as software reverse-engineering gaps:

```text
E4 → dBm
gain code → dB
absolute sensitivity
BER
actual START→DONE latency
maximum OOK acquisition rate
actual number of distinguishable ASK levels
saturation / dynamic range
fading / selectivity / interference
```

These quantities depend on silicon and must be measured experimentally.

##### 2.1 Final software-closure table

| Canonical RX item | Status |
|---|---:|
| establishment of normal RF RX state | **100% software** |
| RX clock control | **100% software** |
| removal of Wi-Fi packet RX | **100% software** |
| transition to PBUS debug / manual control | **100% software** |
| programming a fixed RX gain code | **100% software** |
| preservation of control bits during gain changes | **100% software** |
| separation of special RXIQ/loopback mode from IQ_EST | **100% software** |
| `IQ_EST_CTRL` register and enable/start/N/mode/DONE protocol | **100% software** |
| `M=1, N=1024` acquisition | **100% demonstrated** |
| reading metric `0x600005E4` | **100% software** |
| safe re-arm through full ROM sequence | **100% software** |
| OOK classification in calibrated E4 units | **100% software architecture** |
| fixed-gain ASK/M-ASK classification | **100% software architecture** |
| independence from PP/net80211 for IQ_EST | **100% demonstrated** |

No item below 100% is required in the canonical path defined by this reference.

---

#### 3. Corpus and functions used

The model is based on:

- analyzed ESP8266 mask-ROM;
- analyzed `libphy.a` / PHY v6;
- `libpp.a` and `libnet80211.a` audited to eliminate hidden dependencies;
- `g_phyFuns` / `phy_func_tab`;
- RXIQ and RX gain calibration paths;
- PBUS accesses;
- internal IQ-estimator instrumentation.

Directly relevant ROM functions:

```text
0x40006260  rom_get_corr_power
0x40006400  rom_iq_est_disable
0x40006430  rom_iq_est_enable
0x4000754C  rom_pbus_set_rxgain
0x40007688  rom_pbus_xpd_rx_off
0x400076CC  rom_pbus_xpd_rx_on
0x4000804C  rom_rfcal_rxiq
```

Relevant `g_phyFuns` entries:

```text
+0x020  rom_get_corr_power
+0x030  rom_iq_est_disable
+0x034  rom_iq_est_enable
+0x040  rom_set_rxclk_en
+0x054  rom_set_loopback_gain
+0x0A0  PBUS debug mode / RAM v6 patch
+0x0B0  rom_pbus_rd
+0x0B4  rom_pbus_set_rxgain
+0x0BC  rom_pbus_workmode
+0x0C0  rom_pbus_xpd_rx_off
+0x0C4  rom_pbus_xpd_rx_on
+0x0F4  rom_rfcal_rxiq
```

---

### PART I — NORMAL RX STATE

#### 4. Normal RF RX activation

`rom_pbus_xpd_rx_on()` directly establishes the observed normal PBUS RX state:

```text
PBUS(2,1) = 0x184
PBUS(3,2) = 0x006
```

This pair forms the reference normal RF RX state used by the corpus.

The canonical primitive therefore begins with:

```text
rom_pbus_xpd_rx_on()
```

after RF/PLL has been initialized normally.

---

#### 5. RX OFF is not normal RX

`rom_pbus_xpd_rx_off(x)` writes parameter `x` into `PBUS(2,1)` and then sets the other relevant RX stages to zero.

In several calibrations, the observed call is:

```text
rom_pbus_xpd_rx_off(1)
```

which produces:

```text
PBUS(2,1) = 0x001
```

For standalone reception, this OFF state must therefore not be used in the symbol loop.

---

#### 6. RX clock

RF RX and the measurement logic require the RX clock.

The canonical command is:

```text
rom_set_rxclk_en(1)
```

The official RXIQ calibration itself uses this primitive before measurement operations and disables it on exit.

For the standalone target, the RX clock remains active throughout the receive session.

---

### PART II — REMOVE WI-FI WITHOUT DISABLING RX

#### 7. PBUS debug mode

The v6 patch `ram_pbus_debugmode()` has two structural effects:

1. it removes digital Wi-Fi packet RX;
2. it puts PBUS under manual/forced control.

The same hardware bit controlling digital RX is used by the digital RX start/stop primitives and by PBUS debug.

Debug mode also enables the manual PBUS latch around:

```text
0x60000594 bit0
```

The point is precisely to preserve the analog RX chain while removing the Wi-Fi packet decoder.

The canonical sequence is therefore:

```text
normal RF RX ON
RX clock ON
PBUS debug mode
```

and not:

```text
RF RX OFF
```

---

#### 8. `pbus_workmode()` is not needed between symbols

`pbus_workmode()` returns PBUS control to normal Wi-Fi operation.

For a standalone receiver that does not return to Wi-Fi between symbols, the correct architecture is to **remain in PBUS debug** for the entire demodulation session.

Restoring workmode is therefore not part of the symbol loop.

---

### PART III — FIXED RX GAIN

#### 9. Why gain must be fixed for ASK

ASK encodes information in amplitude differences.

If an automatic mechanism changes gain during the symbols, it can reduce or erase those differences.

Espressif's calibration path directly demonstrates the combination:

```text
PBUS debug
   ↓
pbus_set_rxgain(...)
   ↓
IQ_EST
```

Fixed-gain control is therefore not an invented architecture: it is a mode actually used by the PHY for its own measurements.

---

#### 10. Software packing of forced RX gain

For a gain word `g`, the observed mapping is:

```text
g[2:0]   → PBUS(3,2)[5:3]

g[3]     → PBUS(3,1)[6]
g[4]     → PBUS(3,1)[5]
g[5]     → PBUS(3,1)[4]
g[6]     → PBUS(3,1)[3]
g[7]     → PBUS(3,1)[2]
g[8]     → PBUS(3,1)[1]
g[9]     → PBUS(3,1)[0]

g[10]    → PBUS(2,1)[1]
g[14:11] → PBUS(2,1)[6:3]
```

When writing `PBUS(2,1)`, the setter preserves:

```text
old & 0x185
```

that is, control bits:

```text
0, 2, 7, 8
```

The gain code is therefore modified without destroying the preserved path state.

Conversion of these codes to dB is not required for canonical demodulation; it belongs to analog characterization.

---

#### 11. Baseband gain

The PHY also exposes `pbus_set_rxbbgain(...)`.

Its coarse/fine packing is distinct from RF gain. For this RX reference, it is enough that the chosen gain be:

- deterministic;
- fixed before ASK classification;
- unchanged during the frame.

The exact index→dB law is not required for relative classification in E4 units.

---

### PART IV — SEPARATING NORMAL RX FROM RXIQ LOOPBACK

#### 12. Critical point: IQ_EST does not enable loopback

Disassembly of `rom_rfcal_rxiq()` shows that special RXIQ mode is explicitly configured by separate operations around calibration.

Before RXIQ calibration, the observed path notably contains:

```text
rom_set_rxclk_en(1)
I2C block 0x77, host 0, reg16 bit2 = 1
I2C block 0x77, host 0, reg24 bit7 = 1
set_ana_inf_tx_scale(...)
start_tx_tone(...)
rxiq_cover(...)
stop_tx_tone(...)
```

At the common exit of `rom_rfcal_rxiq()`, these special fields are reset:

```text
reg24 bit7 = 0
reg16 bit2 = 0
rom_set_rxclk_en(0)
```

This sequence demonstrates that the special RXIQ state is **an explicit configuration external to IQ_EST**.

---

#### 13. `rom_iq_est_enable()` configures no loopback

`rom_iq_est_enable()` programs neither:

- the TX tone;
- `set_loopback_gain()`;
- the RXIQ I²C bits above;
- nor a dedicated RXIQ mux.

It only drives the `IQ_EST_CTRL` block.

Canonical conclusion:

> **If the system starts from normal RX and the special RXIQ sequence is not executed, IQ_EST measures the RX datapath in its current normal state.**

---

#### 14. What must not be called in canonical external RX

During external-receiver preparation:

```text
DO NOT call set_loopback_gain()
DO NOT enable the special RXIQ I2C bits
DO NOT start a TX tone
```

These primitives belong to internal calibration and are not required by the external RX detector.

---

### PART V — IQ ESTIMATOR

#### 15. Control register

The main control is:

```text
0x6000057C  IQ_EST_CTRL
```

Reconstructed bit fields:

```text
bit 0       ENABLE
bit 1       START / trigger
bits 16:2   N[14:0]
bit 18      MODE
bit 31      DONE / ready
```

The demonstrated mode for RX power/correlation measurement is:

```text
MODE = 1
```

---

#### 16. `rom_iq_est_enable(mode,N)` protocol

The routine:

1. enables the block;
2. programs `MODE`;
3. programs `N[14:0]`;
4. sets `START`;
5. waits in hardware for `DONE=1`;
6. returns only after the measurement is complete.

No arbitrary software delay is used to wait for measurement completion.

---

#### 17. Canonical N value

PHY v6 directly uses:

```text
M = 1
N = 1024
```

for an IQ_EST-based RX calibration.

This reference therefore uses:

```text
N_CANONICAL = 1024
```

as a demonstrated value.

Other values are programmable, but the `N→time` relationship is a performance characterization, not a functional dependency of the canonical receiver.

---

#### 18. Safe disable / re-arm

`rom_iq_est_disable()` returns the block to rest in two phases:

```text
START = 0
then
ENABLE = 0
```

The canonical repetition sequence therefore deliberately uses the complete ROM sequence:

```text
rom_iq_est_enable(1,N)
wait for it to return
read the result
rom_iq_est_disable()
```

and then starts again.

This procedure is 100% closed on the software side.

The optimization:

```text
ENABLE=1 permanently
START low→high only
```

is **not used** in this reference and is therefore not a dependency of the 100% status.

---

### PART VI — RX METRIC

#### 19. Power / energy register

The canonical metric is:

```text
0x600005E4
```

It belongs to the IQ-estimator result block used by RX calibration routines.

`rom_get_corr_power()` reads the same result set around:

```text
0x60000580
0x60000584
0x60000588
0x6000058C
0x600005DC
0x600005E0
0x600005E4
```

---

#### 20. Espressif uses E4 as a decision value

In `set_rx_gain_cal_iq()`, the PHY performs:

```text
IQ_EST(mode=1, N=1024)
   ↓
read 0x600005E4
   ↓
processing / comparison
   ↓
gain adjustment
```

`0x600005E4` is therefore not merely an observed register: it is a metric actually used by the PHY to make an RX calibration decision.

---

#### 21. Why dBm conversion is not required

For OOK and ASK, operation can remain directly in the numerical E4 domain.

The only requirement is that learned classes be separated in this metric.

Conversion:

```text
E4 → dBm
```

is therefore optional and is not part of the canonical path.

---

### PART VII — OOK

#### 22. RX OOK principle

OOK has two classes:

```text
OFF
ON
```

The receiver measures E4 over a known preamble and determines the centers:

```text
μ_OFF
μ_ON
```

Canonical threshold:

```text
T = (μ_OFF + μ_ON) / 2
```

To avoid making any assumption about the numerical direction of E4:

```text
if μ_ON > μ_OFF: ON ⇔ E > T
otherwise      : ON ⇔ E < T
```

This rule makes classification independent of an absolute physical unit.

---

#### 23. Canonical OOK primitive

Pseudo-C:

```c
#include <stdint.h>
#include <stdbool.h>

#define IQ_POWER_ADDR 0x600005E4u

typedef void (*iq_est_enable_fn)(uint32_t mode, uint32_t n);
typedef void (*iq_est_disable_fn)(void);

#define ROM_IQ_EST_ENABLE  ((iq_est_enable_fn)0x40006430u)
#define ROM_IQ_EST_DISABLE ((iq_est_disable_fn)0x40006400u)

static inline uint32_t rd32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline uint32_t rx_measure_e4(void)
{
    ROM_IQ_EST_ENABLE(1u, 1024u);
    uint32_t e = rd32(IQ_POWER_ADDR);
    ROM_IQ_EST_DISABLE();
    return e;
}
```

Classification:

```c
static inline bool ook_classify(uint32_t e,
                                uint32_t threshold,
                                bool on_is_high)
{
    return on_is_high ? (e > threshold) : (e < threshold);
}
```

---

### PART VIII — ASK / M-ASK

#### 24. RX ASK principle

For M-ASK, the receiver fixes the gain and then learns the E4 centers:

```text
μ0, μ1, ... μ(M-1)
```

The centers are sorted by numerical E4 value.

For two neighboring centers:

```text
Ti = (μi + μ(i+1)) / 2
```

The received measurement is then assigned to the corresponding interval.

---

#### 25. Why gain must remain identical between calibration and data

ASK thresholds are defined in the E4 domain under a particular gain state.

The following must therefore remain constant throughout the frame:

```text
same RF gain code
same baseband gain
same PBUS path
same channel / PLL
```

Gain changes must occur between calibration/adaptation phases, not in the middle of ASK symbols used for decision-making.

---

#### 26. Conceptual M-ASK classifier

```c
uint32_t e = rx_measure_e4();

if (e < T0)
    symbol = 0;
else if (e < T1)
    symbol = 1;
else if (e < T2)
    symbol = 2;
else
    symbol = 3;
```

For a constellation whose E4 order is reversed relative to logical order, the class association table is simply reversed after calibration.

---

### PART IX — WHY PACKET RSSI IS REJECTED

#### 27. `RxControl` / packet RSSI

Wi-Fi RX exposes per-packet metadata, including RSSI/noise-floor.

This information only exists after processing a packet recognized by the PHY/MAC.

It is therefore not a general primitive for:

```text
arbitrary OOK carrier
non-802.11 ASK
proprietary M-ASK
```

The canonical RX reference uses no packet RSSI.

---

### PART X — WHY CCA IS NOT REQUIRED

#### 28. CCA

The corpus allows CCA and its threshold to be configured, but no continuous CPU `CCA busy` readout is required by the reference path.

Even if such a bit were identified later, it would primarily provide binary classification.

IQ_EST directly provides a multilevel metric compatible with:

```text
OOK
ASK
M-ASK
```

CCA is therefore deliberately excluded from the dependencies of this reference.

---

### PART XI — INDEPENDENCE FROM PP / NET80211

#### 29. No hidden driver required

The audit of `libpp.a` and `libnet80211.a` found no second driver required for:

```text
0x6000057C
```

Apparent `+0x37C` offset occurrences inspected in those libraries were relative to other bases or were false decodes.

The canonical IQ_EST primitive therefore remains encapsulated by the PHY/ROM and can be used without PP/net80211 packet logic.

---

### PART XII — COMPLETE STANDALONE PROCEDURE

#### 30. Phase A — boot and initial calibration

1. boot the silicon normally;
2. allow the PHY to perform the required initialization and calibrations;
3. allow `rom_rfcal_rxiq()` to finish and restore its special bits;
4. fix the channel / PLL;
5. prevent sleep or return to Wi-Fi during the standalone session.

This reference does not claim to replace all analog initialization from a fully cold reset with a few isolated PBUS writes.

---

#### 31. Phase B — establish external RX

```text
rom_pbus_xpd_rx_on()
rom_set_rxclk_en(1)
PBUS debug mode
program fixed RX gain
```

Then verify that no RXIQ/loopback calibration primitive is called during the session.

---

#### 32. Phase C — calibrate classes

##### OOK

Measure several known OFF and ON windows:

```text
μ_OFF = mean / robust center of OFF measurements
μ_ON  = mean / robust center of ON measurements
T     = midpoint of the two centers
```

##### M-ASK

For each known preamble level:

```text
μ0 ... μM-1
```

Then sort the centers and place thresholds between neighboring centers.

---

#### 33. Phase D — receive loop

```text
LOOP:
    rom_iq_est_enable(1, 1024)
    E = REG32(0x600005E4)
    rom_iq_est_disable()

    if OOK:
        classify with one threshold

    if M-ASK:
        classify with multiple thresholds
```

No PBUS workmode restoration is required between acquisitions for the standalone target.

---

### PART XIII — FIRMWARE SKELETON

#### 34. Recommended abstract API

```c
void rx_phy_boot_and_calibrate_once(void);
void set_channel_and_lock_rf(void);
void keep_rf_awake_and_take_exclusive_control(void);
void rx_rf_normal_on(void);
void rx_clock_on(void);
void pbus_enter_manual_rx_mode(void);
void set_fixed_rx_gain(uint16_t gain_code);
uint32_t rx_measure_e4(void);
```

Loop:

```c
rx_phy_boot_and_calibrate_once();
set_channel_and_lock_rf();
keep_rf_awake_and_take_exclusive_control();

rx_rf_normal_on();
rx_clock_on();
pbus_enter_manual_rx_mode();
set_fixed_rx_gain(RX_GAIN_CODE);

calibrate_symbol_centres();

for (;;) {
    uint32_t e = rx_measure_e4();
    consume_symbol(classify_e4(e));
}
```

---

#### 35. MMIO discipline

ROM paths use Xtensa `MEMW` barriers around critical MMIO accesses.

Implementations that replace ROM wrappers with direct accesses must preserve this discipline:

```c
static inline void memw(void)
{
    __asm__ volatile ("memw" ::: "memory");
}
```

The canonical reference nevertheless prefers the already-demonstrated ROM/PHY primitives for IQ_EST.

---

### PART XIV — DELIBERATELY NON-REQUIRED ITEMS

#### 36. START-only re-arm

The possibility of faster streaming:

```text
ENABLE=1 permanently
START=0
START=1
```

remains a performance optimization.

It is not used in this reference.

The canonical path always uses:

```text
enable/acquire
read
disable
```

and is therefore independent of that optimization.

---

#### 37. Exact electrical names of preserved PBUS bits

`pbus_set_rxgain()` explicitly preserves bits `0,2,7,8` of the `PBUS(2,1)` word.

Their relevant software behavior is known and automatically preserved by the gain primitive.

Their detailed electrical names are not required by the canonical path and are therefore not claimed in this reference.

---

#### 38. IQ_EST M=0 mode

The corpus demonstrates the relevant RX use of:

```text
M=1
```

The detailed physical meaning of `M=0` is not required for OOK/ASK and is excluded from this reference.

---

### PART XV — PHYSICAL VALIDATION

#### 39. Why silicon validation remains necessary

Software reverse engineering answers:

```text
what to enable
what to disable
which path to preserve
how to fix gain
how to trigger measurement
which register to read
how to repeat acquisition
how to classify OOK/ASK without dBm
```

By itself, it cannot determine the exact analog performance of a real device.

---

#### 40. Physical measurements to perform

To characterize the actual receiver:

```text
E4 with noise only
E4 for several input powers
E4 for several gain codes
START→DONE in cycles / µs
OOK sensitivity
BER versus SNR
saturation
maximum number of distinguishable ASK levels
robustness to fading / interference
```

These measurements do not challenge the 100% status of the canonical software path.

---

### PART XVI — WHAT NOT TO DO

#### 41. Do not use packet RSSI as an OOK detector

It depends on a decoded Wi-Fi packet.

---

#### 42. Do not call `set_loopback_gain()` for external RX

That primitive belongs to the special internal calibration state.

---

#### 43. Do not start a TX tone in the external RX path

The TX tone is used in internal RXIQ, not in canonical external reception.

---

#### 44. Do not allow implicit AGC to take control again

Remain in the manual PBUS context and keep deterministic gain during ASK classification.

---

#### 45. Do not change gain during an ASK symbol

E4 centers and thresholds must remain valid for a stable gain state.

---

#### 46. Do not depend on CCA to operate

CCA can be investigated as an OOK accelerator, but it is not part of the reference path.

---

#### 47. Do not present E4 as dBm without calibration

`E4` is an internal metric that can be used directly; its physical conversion requires characterization.

---

### PART XVII — QUICK REFERENCE

#### 48. Cheat sheet

```text
NORMAL RF RX:
    rom_pbus_xpd_rx_on()
    PBUS(2,1) = 0x184
    PBUS(3,2) = 0x006

RX CLOCK:
    rom_set_rxclk_en(1)

MANUAL RX:
    PBUS debug mode
    Wi-Fi packet RX removed

FIXED GAIN:
    pbus_set_rxgain(code)
    keep the same state during the frame

IQ_EST CTRL:
    0x6000057C
    bit0     ENABLE
    bit1     START
    16:2     N
    bit18    MODE
    bit31    DONE

CANONICAL ACQUISITION:
    mode = 1
    N    = 1024
    rom_iq_est_enable(1,1024)
    E = REG32(0x600005E4)
    rom_iq_est_disable()

OOK:
    calibrate μ_OFF and μ_ON
    threshold at midpoint
    determine direction automatically

M-ASK:
    calibrate μ0...μM-1
    sort centers
    thresholds between neighboring centers

DO NOT USE AS A DEPENDENCY:
    packet RSSI
    CCA busy
    set_loopback_gain
    TX tone
    START-only re-arm
    E4→dBm
```

---

#### 49. Recommended complete architecture

```text
                         ESP8266
                            │
                            ▼
                   boot + calibration
                            │
                            ▼
                      channel / RF PLL
                            │
                            ▼
                    normal RF RX ON
                            │
                            ▼
                      RX clock ON
                            │
                            ▼
                    PBUS debug mode
                            │
                  Wi-Fi packet RX removed
                            │
                            ▼
                      fixed RX gain
                            │
                            ▼
                   IQ_EST M=1,N=1024
                            │
                            ▼
                       hardware DONE
                            │
                            ▼
                        read E4
                            │
              ┌─────────────┴─────────────┐
              ▼                           ▼
             OOK                         ASK
        single threshold           multiple thresholds
              │                           │
              └─────────────┬─────────────┘
                            ▼
                          symbol
```

---

#### 50. Exact boundary between “decoded” and “to be measured”

##### 100% decoded in this reference

```text
how to establish normal RX
how to keep the RX clock active
how to remove packet RX
how to take manual PBUS control
how to force a fixed gain code
how to preserve path bits
how to avoid RXIQ loopback
how to trigger IQ_EST
how to wait for DONE
which power register to read
how to safely re-arm
how to build a relative OOK slicer
how to build a relative M-ASK classifier
```

##### To be measured on silicon

```text
E4→dBm relationship
gain-code→dB relationship
sensitivity
dynamic range
latency
maximum rate
BER
number of usable ASK levels
physical selectivity
```

---

#### 51. Project's official conclusion for RX OOK/ASK

For the analyzed ESP8266 corpus, the canonical software path of the standalone OOK/ASK receiver is considered **100% closed**:

```text
normal RX
→ RX clock
→ PBUS debug / fixed gain
→ IQ_EST M=1,N=1024
→ DONE
→ 0x600005E4
→ disable
→ relative classification
```

The receiver depends on neither:

- a Wi-Fi packet;
- PP/net80211;
- packet RSSI;
- a direct CCA bit;
- RXIQ loopback;
- dBm conversion;
- nor the START-only optimization.

OOK is obtained from two calibrated E4 classes. ASK/M-ASK is obtained from multiple calibrated E4 classes under fixed gain.

Absolute performance is deliberately classified as **hardware characterization**, clearly separated from software decoding of the procedure.

---

#### 52. Document status

**Document:** `ESP8266_RX_OOK_ASK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`  
**Edition:** 1.0  
**Release:** single  
**Software status:** **100% closed within the canonical scope described**  
**Future maintenance:** none — frozen document  
**Project continuation:** silicon/RF characterization and optional optimizations in the living master document.

---
# APPENDIX C — TX FSK / M-FSK — complete v1.0 reference

### ESP8266 — Standalone FSK / M-FSK Generator
#### Official Technical Reference of the Reverse-Engineering Project

**Single edition: v1.0**  
**Date: 2026-09-13**  
**Status: FROZEN — 100% closure of the TX FSK software/static scope**  
**Scope: ESP8266 / mask-ROM + PHY v6 from the analyzed corpus**

> **Important — status of the word “official.”** This document is the official reference **for the reverse-engineering project carried out on the analyzed corpus**. It is not an official Espressif document and must not be presented as one.

> **Maintenance rule.** This edition is deliberately frozen. Later discoveries, RF measurements, `tone_control→Hz` laws, data-rate optimizations, or phase/settling characterization will not modify this edition; they continue in the living master document `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

#### 1. Purpose

This document describes the **canonical software/static path** for using the ESP8266 tone generator as a **2-FSK and M-FSK actuator**, after one-time RF/PHY initialization and without returning to normal Wi-Fi traffic between symbols.

The target is not:

- to transmit 802.11 frames;
- to perform a full channel change for every symbol;
- to use `set_rf_freq_offset()` as a fast modulator;
- to relock/recalibrate the RFPLL on every symbol;
- to turn the ESP8266 into a general-purpose I/Q SDR;
- to claim static knowledge of the physical `tone_control → Hz` relationship.

The software target is:

```text
BOOT
  ↓
one-time RF-PLL-PHY initialization / calibration
  ↓
abandon normal Wi-Fi traffic
  ↓
fixed channel / RFPLL
  ↓
keep RF TX + TX clock active
  ↓
normalize tone slot 1
  ↓
keep bit18 gate active
keep scale bits17:10 fixed
  ↓
┌───────────────────────────────────────────────┐
│ 2-FSK  : tone_control K0 ↔ K1               │
│ M-FSK  : tone_control K0 ... K(M-1)          │
└───────────────────────────────────────────────┘
  ↓
targeted RMW of the slot's low field
```

The exact physical response of the generator after a `K0→K1` change remains silicon/RF characterization.

---

#### 2. Definition of “100%” in this reference

The status:

```text
TX FSK software/static = 100%
```

means:

> **100% of the canonical software command path included in this reference is directly demonstrated or closed by the mask-ROM, `libphy.a`, relocations, MMIO accesses, tone-generator packing, and separation of RFPLL paths in the analyzed corpus.**

The following quantities are not classified as software reverse-engineering gaps:

```text
tone_control → exact RF frequency
Δf between two K codes
MMIO write → new-tone latency
K0→K1 settling
phase continuity
frequency-change jitter
spectral transients
internal generator wrap/modulo
exact phase-accumulator / NCO clock
exact accumulator width
actual maximum FSK data rate
absolute power in dBm
```

These quantities depend on silicon and must be measured experimentally.

##### 2.1 Final software-closure table

| Canonical TX FSK item | Status |
|---|---:|
| tone slot 1 address `0x600005B8` | **100% software** |
| raw `tone_control` injection into the low field | **100% software** |
| separation of `tone_control` / scale `bits17:10` | **100% software** |
| normal gate bit18 | **100% software** |
| preservation of scale during FSK | **100% software architecture** |
| keeping the TX clock active between symbols | **100% software** |
| slot normalization before modulation | **100% software** |
| targeted RMW primitive for the `tone_control` field | **100% defined** |
| `MEMW` discipline around MMIO | **100% software** |
| separation of tone-step / `set_rf_freq_offset()` | **100% software architecture** |
| `set_rf_freq_offset()` = RFPLL/recalibration path | **100% functionally classified** |
| absence of software `tone_control→Hz` conversion in the corpus | **100% demonstrated** |
| distinction between software and RF characterization | **100% defined** |

No unknown software element is required to describe the canonical TX FSK command path in this reference.

---

#### 3. Corpus and functions used

The model is based on:

- analyzed ESP8266 mask-ROM;
- analyzed `libphy.a` / PHY v6;
- `libpp.a` and `libnet80211.a` audited for concurrent interactions;
- `g_phyFuns` / `phy_func_tab`;
- `rfcal_pwrctrl`, `rfcal_rxiq`, `rfcal_txcap`, and TXIQ paths;
- direct accesses to the tone block;
- analysis of `set_rf_freq_offset()`, `ram_rfpll_set_freq()`, and RFPLL SDM construction.

Directly relevant ROM / PHY functions:

```text
rom_set_txclk_en
rom_set_ana_inf_tx_scale
rom_start_tx_tone
rom_stop_tx_tone
rom_txtone_linear_pwr
rom_pbus_xpd_tx_off
rom_pbus_xpd_tx_on
rom_pbus_set_txgain
ram_rfpll_set_freq
set_rf_freq_offset
wait_rfpll_cal_end
```

Demonstrated ROM addresses for tone primitives:

```text
rom_start_tx_tone       ≈ 0x400068B4
rom_stop_tx_tone        ≈ 0x4000698C
rom_txtone_linear_pwr   ≈ 0x40006A1C
```

---

#### 4. Tone-generator registers

Three slots are present:

```text
0x600005B8   TONE SLOT 1
0x600005BC   TONE SLOT 2
0x600005C4   TONE SLOT 3
```

The canonical FSK reference uses **slot 1**.

The direct ROM uses found for the main calibrations use slot 1.

---

#### 5. Slot 1 packing

The reconstructed behavior of `rom_start_tx_tone()` is:

```c
r = REG32(slot);
r &= 0xF0000000;
r |= raw_control;
r |= ((uint32_t)((0x100 - digital_scale) & 0xff) << 10);
r |= ((uint32_t)mode_code << 18);
REG32(slot) = r;
```

Practical representation:

```text
31        28 27                  18 17          10 9                    0
┌───────────┬──────────────────────┬──────────────┬──────────────────────┐
│ preserved │ mode/test region     │ scale code   │ raw tone_control     │
│           │ bit18 = normal gate  │ 8 bits       │ stimulus / step      │
└───────────┴──────────────────────┴──────────────┴──────────────────────┘
```

The key point for FSK is:

```text
tone_control
    ≠ digital_scale
    ≠ bit18 gate
    ≠ RFPLL
```

---

#### 6. Practical width of the `tone_control` field

The ROM does not explicitly perform:

```c
raw_control &= 0x3ff;
```

before the OR.

The packing model nevertheless gives a natural 10-bit space for the low field.

For standalone firmware, the recommended local protection is:

```c
#define TONE_CONTROL_MASK 0x000003FFu
```

then:

```c
control_safe = tone_control & TONE_CONTROL_MASK;
```

This mask is a **standalone-firmware discipline**, not a mask observed in `rom_start_tx_tone()`.

---

### PART I — FSK PRINCIPLE

#### 7. 2-FSK principle

2-FSK encodes two symbols using two generator command states:

```text
symbol 0 → K0
symbol 1 → K1
```

Architecture:

```text
RFPLL      fixed
channel    fixed
TX RF      active
TX clock   active
gate       active
scale      fixed
tone_control changes
```

Software must not reinitialize the generator on every symbol.

---

#### 8. M-FSK principle

A software M-FSK constellation is represented by:

```text
K0, K1, ... K(M-1)
```

Each code is written into the same low slot field without modifying:

```text
bit18
bits17:10
bits31:28
```

The physical mapping:

```text
Ki → fi
```

must be measured on silicon.

The reference assumes neither linearity, nor symmetry, nor constant step size.

---

#### 9. Values actually observed in the corpus

Distinct values directly demonstrated in the analyzed paths include:

```text
tone_control = 8
tone_control = 64
```

Their functional role is tied to different stimuli/calibrations:

```text
8   → RXIQ path
64  → TXIQ / power-control / TX-cap / tone-power measurement
```

These two values prove the existence of distinct codes used by the generator.

By themselves they do not prove:

```text
8  = frequency F0
64 = frequency F1
```

nor any scale law.

---

### PART II — CANONICAL TX FSK PRIMITIVE

#### 10. Low-field mask

For standalone firmware:

```c
#define TONE1_ADDR          0x600005B8u
#define TONE_CONTROL_MASK   0x000003FFu
```

---

#### 11. RMW primitive

```c
#include <stdint.h>

static inline void memw(void)
{
    __asm__ volatile ("memw" ::: "memory");
}

static inline uint32_t rd32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void wr32(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

static inline void fsk_set_control(uint16_t tone_control)
{
    uint32_t r;

    memw();
    r = rd32(TONE1_ADDR);

    r &= ~TONE_CONTROL_MASK;
    r |= ((uint32_t)tone_control & TONE_CONTROL_MASK);

    memw();
    wr32(TONE1_ADDR, r);
}
```

This primitive preserves:

```text
ASK scale bits17:10
bit18 gate
upper modes
bits31:28
```

---

#### 12. Why RMW is mandatory

Do not write:

```c
REG32(TONE1_ADDR) = K;
```

because that would destroy:

```text
scale
gate
mode/test
preserved bits
```

Canonical modulation acts only on the targeted field.

---

#### 13. Slot normalization before FSK

A slot left by TXIQ calibration may contain residual test/mode bits.

Before the FSK session, explicitly build a normal state:

```c
#define TONE_GATE_MASK    0x00040000u
#define TONE_SCALE_SHIFT  10u
#define TONE_SCALE_MASK   0x0003FC00u

static inline uint32_t build_tone1_fsk_normal(
    uint32_t previous,
    uint16_t tone_control,
    uint8_t digital_scale,
    bool gate)
{
    uint8_t scale_code;
    uint32_t r;

    if (digital_scale > 63)
        digital_scale = 63;

    scale_code = (uint8_t)(0u - digital_scale);

    r  = previous & 0xF0000000u;
    r |= ((uint32_t)tone_control & TONE_CONTROL_MASK);
    r |= ((uint32_t)scale_code << TONE_SCALE_SHIFT);
    r |= gate ? TONE_GATE_MASK : 0u;

    return r;
}
```

In FSK mode:

```text
gate = 1
digital_scale = constant
tone_control = variable per symbol
```

---

### PART III — WHY THE RFPLL IS NOT THE FAST MODULATOR

#### 14. `set_rf_freq_offset()` is not a simple numerical offset

In the exact `phy_chip_v6_ana.o`:

```text
set_rf_freq_offset
    size = 0x6D bytes
```

Relocations demonstrate:

```text
set_rf_freq_offset()
        ↓
ram_rfpll_set_freq()
        ↓
wait_rfpll_cal_end()
```

The primitive therefore reprograms the RFPLL and waits for its calibration.

---

#### 15. Consequence for fast FSK

The path:

```text
set_rf_freq_offset()
```

is classified as:

```text
RFPLL correction / retuning
```

and not as:

```text
fast symbol-by-symbol modulation
```

Canonical fast-FSK therefore uses the tone generator.

---

#### 16. `chip_v6_set_chan_offset()` is even heavier

The channel/offset path notably involves:

```text
stop RX
change channel
BBPLL / RF
recalibration
restart RX
```

It must not be used in the fast-FSK loop.

---

#### 17. RFPLL SDM construction

Reverse engineering also closes the slow branch:

```text
RF frequency
    ↓
ram_rfpll_set_freq()
    ↓
compute 24-bit SDM word
    ↓
rom_write_rfpll_sdm()
    ↓
RFPLL
```

This closure confirms the architectural separation:

```text
tone_control
    ≠
RFPLL SDM word
```

---

### PART IV — RF / PBUS / TX CLOCK

#### 18. The tone generator does not initialize all RF

`rom_start_tx_tone()` does not replace complete RF initialization.

Calibrations demonstrate the skeleton:

```text
RF/PBUS prepared
    ↓
RX RF off
    ↓
TX XPD on
    ↓
baseline gain / scale
    ↓
TX clock on
    ↓
tone generator
```

---

#### 19. TX clock

`rom_start_tx_tone()` enables the TX clock.

`rom_stop_tx_tone()` performs:

```text
clear bit18
then
TX clock off
```

Fast FSK must therefore keep the TX clock active between symbols.

---

#### 20. Do not use `stop_tx_tone()` between symbols

Incorrect strategy:

```text
K0 → start
transition
stop
K1 → start
```

unnecessarily reinitializes the path.

Canonical strategy:

```text
start / prepare once
    ↓
TX clock remains active
gate remains active
    ↓
K0 ↔ K1 through RMW
```

---

### PART V — COMPLETE STANDALONE PROCEDURE

#### 21. Phase A — boot / calibration

1. boot the silicon normally;
2. allow the PHY to perform its initialization/calibrations;
3. fix the channel / RFPLL;
4. prevent sleep/wakeup during the session;
5. abandon normal Wi-Fi traffic.

This reference does not claim to replace the entire RF cold-start with a few isolated writes.

---

#### 22. Phase B — TX RF preparation

Starting from an initialized RF state:

```text
PBUS debug/test
RX RF off
TX XPD on
deterministic TX gain
deterministic baseline scale
TX clock on
```

---

#### 23. Phase C — tone normalization

Choose:

```text
K_INITIAL
fixed digital_scale
normal mode
gate active
```

Fully rewrite the normal slot once.

---

#### 24. Phase D — characterize FSK codes

Before using a constellation:

```text
measure f(K0)
measure f(K1)
...
measure f(KM-1)
```

Then define the table:

```c
typedef struct {
    uint16_t control;
    /* measured frequency may optionally be stored outside the loop */
} fsk_level_t;
```

This step is RF characterization, not a software gap.

---

#### 25. Phase E — modulation

2-FSK:

```c
static const uint16_t fsk2[2] = {
    K0,
    K1
};

for (;;) {
    uint8_t bit = next_bit();
    fsk_set_control(fsk2[bit & 1u]);
    symbol_delay();
}
```

M-FSK:

```c
static const uint16_t mfsk[4] = {
    K0, K1, K2, K3
};

for (;;) {
    uint8_t sym = next_2_bits();
    fsk_set_control(mfsk[sym & 3u]);
    symbol_delay();
}
```

---

### PART VI — TIMING AND CONCURRENCY

#### 26. Single owner of the slot

RMW is not atomic with respect to another writer.

After Wi-Fi is abandoned:

```text
one owner only of the tone slot
    =
FSK modulation loop
```

---

#### 27. Interrupts

For temporally clean modulation, avoid disturbances from:

```text
Wi-Fi ISR
PHY maintenance timers
sleep/wakeup
channel/PLL reconfiguration
concurrent calibrations
```

---

#### 28. `MEMW`

ROM paths use Xtensa `MEMW` barriers.

The real path is:

```text
LX106
  ↓
MEMW
  ↓
MMIO
  ↓
peripheral bus
  ↓
tone generator
  ↓
DAC/RF
```

CPU time alone therefore does not give RF latency.

---

### PART VII — ITEMS DELIBERATELY OUTSIDE THE SOFTWARE SCOPE

#### 29. `tone_control → Hz` law

No demonstrated software formula exists in the exact corpus to convert:

```text
tone_control
```

into:

```text
Hz
kHz
MHz
```

The code is injected raw into hardware.

---

#### 30. `app_tone_offset_khz` does not close the law

DWARF historically exposes:

```text
app_tx_tone
app_tone_offset_khz
```

but the exact internal global:

```text
chip6_phy_init_ctrl
```

is only 80 bytes long, placing the corresponding offsets outside the object actually allocated in this build.

In addition:

```text
app_test_code()
```

is a `RET.N` stub.

Conclusion:

```text
app_tone_offset_khz
    ≠ proof of
tone_control → kHz
```

---

#### 31. Phase accumulator / NCO

A generic law such as:

```text
f = step × Fclk / 2^N
```

would be compatible with a digital generator, but no constants:

```text
Fclk
N
```

are demonstrated for this ESP8266 tone generator.

It must not be used as an official result.

---

#### 32. Hot update: exact boundary

Software can write:

```text
K0 → K1
```

through RMW without calling `stop_tx_tone()`.

This closes the **software capability for hot writing**.

What remains hardware-dependent:

```text
does hardware apply K1 immediately?
what latency?
what phase?
what settling?
```

---

### PART VIII — PHYSICAL VALIDATION

#### 33. Why silicon validation remains necessary

Software reverse engineering answers:

```text
which register to write
which field to modify
which other fields to preserve
how to keep gate/clock active
which path not to use
how to separate tone-step from RFPLL
```

It cannot determine the generator's internal performance.

---

#### 34. TX measurements to perform

In a controlled RF environment:

```text
f(K) for multiple K
Δf K0↔K1
write→new-frequency latency
settling
jitter
K0→K1 phase
spectrum / transients
power for each K
maximum data rate
```

---

#### 35. 2-FSK validation

Procedure:

1. prepare RF/PLL/TX;
2. normalize the slot;
3. fix gate and scale;
4. measure `K0`;
5. write `K1` by RMW only;
6. measure frequency and timing;
7. return to `K0`;
8. repeat.

Functional validation:

```text
K0 and K1 produce two frequency states sufficiently separated
and stable for the chosen data rate
```

---

#### 36. M-FSK validation

For each `Ki`:

```text
measure fi
measure variance / settling
```

Then select a constellation with:

```text
sufficient separation
comparable power
acceptable transients
```

---

### PART IX — WHAT NOT TO DO

#### 37. Do not use `stop_tx_tone()` per symbol

It disables the TX clock.

---

#### 38. Do not use `set_rf_freq_offset()` per symbol

It goes through RFPLL + calibration wait.

---

#### 39. Do not assume `tone_control = frequency in kHz`

No such identity is demonstrated.

---

#### 40. Do not assume a linear law

The corpus does not demonstrate:

```text
f ∝ K
```

across the whole range.

---

#### 41. Do not overwrite the entire slot

Always preserve:

```text
scale
gate
modes
upper bits
```

with targeted RMW.

---

#### 42. Do not let Wi-Fi retake the slot

The standalone reference assumes one owner of the generator during the session.

---

### PART X — FINAL MODEL STATE

#### 43. Canonical 2-FSK

```text
RF/PLL prepared
TX clock active
slot normalized
gate = 1
scale fixed
    ↓
K0 ↔ K1
by RMW of the low field
```

---

#### 44. Canonical M-FSK

```text
RF/PLL prepared
TX clock active
slot normalized
gate = 1
scale fixed
    ↓
K0, K1, ... K(M-1)
by RMW of the low field
```

---

#### 45. Exact boundary between “decoded” and “to be measured”

##### 100% decoded on the software/static side

```text
tone register
packing
gate
scale
tone_control field
normalization
RMW
MEMW
TX clock
RFPLL separation
slow set_rf_freq_offset path
absence of software K→Hz conversion
```

##### To be measured on silicon

```text
K→f
latency
settling
phase
jitter
spectrum
maximum data rate
```

---

### PART XI — RF / COMPLIANCE NOTE

#### 46. Laboratory use

Standalone tone-generator modulation leaves the normal Wi-Fi path.

Tests should be carried out in a controlled RF environment and in compliance with applicable regulations, using attenuation/load or suitable instrumentation when required.

This note does not change the software model.

---

### PART XII — QUICK REFERENCE

#### 47. Cheat sheet

```text
TONE SLOT 1:
    0x600005B8

FIELD:
    tone_control → low field
    firmware protection: & 0x3FF

GATE:
    bit18 = 1 during active FSK

SCALE:
    bits17:10 fixed during FSK

FAST FSK:
    RMW of tone_control field
    K0 ↔ K1

DO NOT USE PER SYMBOL:
    rom_stop_tx_tone()
    set_rf_freq_offset()
    chip_v6_set_chan_offset()
    channel change

TX CLOCK:
    remains active

MEMW:
    preserve around critical MMIO

PHYSICAL CALIBRATION:
    measure K → frequency
```

---

#### 48. Final minimal pseudo-code

```c
#define TONE1_ADDR        0x600005B8u
#define TONE_CONTROL_MASK 0x000003FFu

static inline void fsk_write(uint16_t k)
{
    uint32_t r;

    memw();
    r = rd32(TONE1_ADDR);

    r &= ~TONE_CONTROL_MASK;
    r |= ((uint32_t)k & TONE_CONTROL_MASK);

    memw();
    wr32(TONE1_ADDR, r);
}

void send_2fsk_bit(uint8_t bit)
{
    static const uint16_t K[2] = { K0, K1 };
    fsk_write(K[bit & 1u]);
}
```

`K0/K1` values are obtained after RF characterization.

---

#### 49. Project's official conclusion for TX FSK/M-FSK

For the analyzed corpus, the project now considers the TX FSK command path **100% closed within the software/static scope**:

```text
RF initialization
    ↓
tone generator
    ↓
slot 1
    ↓
fixed gate / scale
    ↓
RMW tone_control
    ↓
K0 ↔ K1 ↔ ...
```

Reverse engineering does not claim a physically unobservable relationship as a software result:

```text
tone_control → Hz
```

Determination of actual frequencies, settling, phase, jitter, and spectral performance belongs to **silicon/RF validation**.

This separation is deliberate and constitutes the official definition of “100%” in this reference.

---

#### 50. Document status

```text
Document : project TX FSK/M-FSK reference
Version  : 1.0
Status   : FROZEN
Basis    : ESP8266 mask-ROM + PHY v6 + analyzed corpus
Next     : RF characterization and optimizations in the living master
```

---
# APPENDIX D — RX FSK / M-FSK — complete v1.0 reference

### ESP8266 — FSK / M-FSK Receiver
#### Official Technical Reference of the Reverse-Engineering Project — Software/Static Scope

**Single edition: v1.0**  
**Date: 2026-09-13**  
**Status: FROZEN — 100% closure of the RX FSK software/static scope**  
**Scope: ESP8266 / mask-ROM + PHY v6 from the analyzed corpus**

> **Important — status of the word “official.”** This document is the official reference **for the reverse-engineering project carried out on the analyzed corpus**. It is not an official Espressif document and must not be presented as one.

> **Important — scope of the term “FSK receiver.”** The CFO readout/discrimination software path is 100% closed. The baseband's ability to produce a **new valid CFO measurement on a non-802.11 tone/FSK signal** remains a separate silicon/baseband validation. That physical validation is not included in the software score.

> **Maintenance rule.** This edition is frozen for the software/static scope. Silicon validation, update-rate measurements, sensitivity, behavior on non-802.11 tones, and demodulation optimizations continue in the living master document `ESP8266_WIFI_PHY_REVERSE_ENGINEERING.md`.

---

#### 1. Purpose

This document describes the **canonical software/static path** for using the ESP8266 received-frequency measurement (CFO) as a discrimination metric for **2-FSK and M-FSK**.

The software target is:

```text
RF RX / baseband
    ↓
hardware CFO result
0x60009800
    ↓
WDEV context gate
    ↓
bit0 valid?
    ↓
signed raw [15:8]
    ↓
(raw × 107) >> 6
    ↓
CFO metric
    ↓
FSK / M-FSK classification
    ↓
final handshake
0x600098DC |= 0xF
```

The target is not:

- to use packet RSSI;
- to use the IQ_EST E4 metric as a general frequency discriminator;
- to conflate CFO measurement with RFPLL correction;
- to use `phy_freq_offset` as if it were the received measurement;
- to depend on absolute conversion to Hz to classify symbols;
- to claim that the static corpus already proves fresh CFO on any non-802.11 tone.

---

#### 2. Definition of “100%” in this reference

The status:

```text
RX FSK software/static = 100%
```

means:

> **100% of the CPU/software path for CFO readout, validation, conversion, finalization, and measurement/correction separation included in this reference is directly demonstrated by `libphy.a`, `libpp.a`, `libnet80211.a`, DWARF, relocations, MMIO accesses, and the mask-ROM of the analyzed corpus.**

The following points are **outside the software score**:

```text
exact physical instant when 0x60009800.bit0 rises
exact baseband cause of that transition
fresh CFO on non-802.11 tone
measurement refresh rate
hardware latency / jitter
FSK RF sensitivity
BER versus deviation / SNR
minimum detectable deviation
```

##### 2.1 Final software-closure table

| Canonical RX FSK item | Status |
|---|---:|
| CFO result register `0x60009800` | **100% software** |
| validity bit used: `bit0` | **100% software** |
| raw CFO field `[15:8]` | **100% software** |
| signed 8-bit interpretation | **100% software** |
| `(raw*107)>>6` conversion | **100% software** |
| invalid sentinel `0x7FFF` | **100% software** |
| WDEV gate `0x3FF2003C[19:16] < 8` | **100% software** |
| storage in `phy_meas_freq_offset` | **100% software** |
| separation of `phy_meas_freq_offset` / `phy_freq_offset` | **100% software** |
| handshake register `0x600098DC` | **100% software** |
| handshake mask `[3:0] = 0xF` | **100% software** |
| RMW + preservation of other bits | **100% software** |
| handshake also executed after invalid read | **100% software** |
| absence of a second CPU arm/clear path in the corpus | **100% analyzed corpus** |
| standard post-RX-success consumption | **100% software** |
| WDEV bit8 = gate to main RX path | **100% software-structural** |
| individual electrical role ACK/CLEAR/REARM | **outside software score** |
| fresh CFO on non-802.11 tone | **silicon/baseband validation** |

---

#### 3. Corpus and functions used

The model is based on:

- exact `libphy.a`;
- exact `libpp.a`;
- exact `libnet80211.a`;
- 64 KiB ESP8266 mask-ROM;
- DWARF from `wdev.o` and PHY objects;
- exact relocations;
- scans of executable sections and MMIO accesses reconstructed from base+offset.

Main functions / symbols:

```text
phy_get_bb_freqoffset
phy_get_bb_evm
phy_get_freq_param
wDev_ProcessRxSucData
wDev_ProcessFiq
HdlChlFreqCal
chip_v6_set_chan_offset
set_rf_freq_offset
```

Main state variables:

```text
phy_meas_freq_offset
phy_freq_offset
```

---

### PART I — CFO RESULT

#### 4. Main register

The CFO result is read at:

```text
0x60009800
```

The getter uses:

```text
bit0      → CFO validity condition
bits15:8  → raw 8-bit CFO
```

The register also contains other metrics, including an EVM region, but the CFO path specifically uses the fields above.

---

#### 5. WDEV context gate

Before reading CFO, the getter reads:

```text
0x3FF2003C
```

and extracts:

```text
bits19:16
```

Condition:

```text
if field >= 8
    → invalid CFO
```

Therefore the canonical condition is:

```text
((REG32(0x3FF2003C) >> 16) & 0xF) < 8
```

The exact electrical name of this field remains outside scope.

The recommended term is:

```text
WDEV CFO context/state gate
```

---

#### 6. Invalid sentinel

The value:

```text
0x7FFF
```

is used when:

```text
invalid WDEV gate
or
0x60009800.bit0 == 0
```

Constant:

```c
#define CFO_INVALID ((int16_t)0x7FFF)
```

---

#### 7. Signed raw CFO

On the valid path:

```text
raw_u8 = 0x60009800[15:8]
```

then:

```text
raw_s8 = sign_extend(raw_u8)
```

The field is therefore a signed 8-bit value.

---

#### 8. Exact conversion

The reconstructed formula is:

```text
CFO = (raw_s8 × 107) >> 6
```

that is:

```text
≈ raw_s8 × 1.671875
```

using signed integer arithmetic.

The historical public output is classified as kHz with very high confidence, but a relative demodulator does not require absolute unit calibration.

---

### PART II — CFO HANDSHAKE

#### 9. Finalization register

After every CFO attempt, the getter uses:

```text
0x600098DC
```

Exact behavior:

```c
r = REG32(0x600098DC);
r |= 0x0000000F;
REG32(0x600098DC) = r;
```

with `MEMW` barriers.

---

#### 10. The handshake is unconditional

The getter has a common epilogue.

The paths:

```text
valid CFO
invalid bit0
invalid WDEV gate
```

all converge to:

```text
MEMW
read 0x600098DC
OR 0xF
MEMW
write 0x600098DC
store phy_meas_freq_offset
return
```

Therefore:

> **the handshake must be executed after every attempt, including when the returned value is `0x7FFF`.**

---

#### 11. Final software role of `0x600098DC[3:0]`

The software behavior is closed as:

```text
CFO-cycle finalization / handshake
```

The exact electrical sub-role:

```text
ACK
CLEAR
REARM
W1C
strobe
```

is not required to reproduce the software.

---

#### 12. No second CPU re-arm

An exhaustive corpus scan finds no separate CPU sequence for:

```text
clear CFO
arm CFO
start CFO
```

The standard path is:

```text
hardware does or does not produce CFO
    ↓
CFO getter
    ↓
read / convert / sentinel
    ↓
0x600098DC |= 0xF
    ↓
return
```

---

### PART III — CANONICAL CFO PRIMITIVE

#### 13. Registers and constants

```c
#include <stdint.h>
#include <stdbool.h>

#define WDEV_CFO_CTX_ADDR   0x3FF2003Cu
#define BB_CFO_RESULT_ADDR  0x60009800u
#define BB_CFO_HS_ADDR      0x600098DCu

#define CFO_INVALID         ((int16_t)0x7FFF)
```

---

#### 14. MMIO discipline

```c
static inline void memw(void)
{
    __asm__ volatile ("memw" ::: "memory");
}

static inline uint32_t rd32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void wr32(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}
```

---

#### 15. Reconstructed canonical primitive

```c
static inline int16_t fsk_cfo_read(void)
{
    int16_t out = CFO_INVALID;

    uint32_t ctx = rd32(WDEV_CFO_CTX_ADDR);

    if (((ctx >> 16) & 0x0Fu) < 8u) {
        uint32_t bb = rd32(BB_CFO_RESULT_ADDR);

        if (bb & 1u) {
            int8_t raw = (int8_t)((bb >> 8) & 0xFFu);
            out = (int16_t)(((int32_t)raw * 107) >> 6);
        }
    }

    memw();
    uint32_t h = rd32(BB_CFO_HS_ADDR);
    h |= 0x0Fu;
    memw();
    wr32(BB_CFO_HS_ADDR, h);

    return out;
}
```

This primitive reproduces the **observed CPU contract**.

---

#### 16. Variant with explicit status

```c
static inline bool fsk_cfo_try_read(int16_t *out)
{
    int16_t v = fsk_cfo_read();

    if (v == CFO_INVALID)
        return false;

    *out = v;
    return true;
}
```

The caller must never classify a symbol from `0x7FFF`.

---

### PART IV — MEASUREMENT VS CORRECTION

#### 17. `phy_meas_freq_offset`

The CFO getter stores its result in:

```text
phy_meas_freq_offset
```

This variable represents the latest computed/read CFO measurement.

---

#### 18. `phy_freq_offset`

A distinct variable:

```text
phy_freq_offset
```

is used by correction / channel / retuning paths.

It is incorrect to conflate them.

---

#### 19. `phy_get_freq_param()`

The reconstructed path explicitly separates:

```text
*corr_out = phy_freq_offset
*meas_out = phy_meas_freq_offset
```

The measurement/correction distinction is therefore closed.

---

### PART V — STANDARD WDEV / PP CONSUMPTION

#### 20. CFO consumption after RX success

In the standard SDK:

```text
wDev_ProcessFiq()
    ↓
RX processing
    ↓
discard or success
    ↓ success
wDev_ProcessRxSucData()
    ↓
phy_get_bb_freqoffset()
```

The standard CFO getter is not called on the discard branch.

---

#### 21. WDEV event bit8

The dispatcher reads:

```text
0x3FF20C20
```

as the WDEV event word.

The bit:

```text
bit8
```

gates the main RX processing path.

It is observed **before** the final decision:

```text
discard
or
RX-success
```

This provides a hardware timing landmark but does not prove that CFO is already valid at that moment.

---

#### 22. Why this does not limit software closure

Standard software chooses to consume CFO after RX success.

The question:

```text
had the baseband already produced CFO earlier?
```

is a hardware timing property.

It is therefore separate from the getter's CPU contract.

---

### PART VI — FSK CLASSIFICATION

#### 23. 2-FSK principle

If two frequency states produce stable CFO centers:

```text
μ0
μ1
```

define:

```text
T = (μ0 + μ1) / 2
```

Then classify according to the observed ordering:

```text
if μ1 > μ0:
    symbol 1 ⇔ CFO > T
otherwise:
    symbol 1 ⇔ CFO < T
```

This classification is a project software architecture derived from the CFO metric.

---

#### 24. Why an absolute unit is not required

As with ASK in E4 units, a relative discriminator can operate directly in the CFO output domain.

The only requirement is:

```text
separable classes
```

It is therefore unnecessary to convert the measurement into absolute RF frequency in order to classify two states.

---

#### 25. M-FSK

For M levels:

```text
μ0, μ1, ... μ(M-1)
```

sort the centers by CFO value.

Thresholds:

```text
Ti = (μi + μ(i+1)) / 2
```

Then classify the measurement into the corresponding interval.

---

#### 26. Invalid values

When an acquisition returns:

```text
0x7FFF
```

it must be treated as:

```text
no valid decision
```

and not as an M-FSK level.

Firmware must provide for:

```text
erasure
retry
resynchronization
or symbol loss
```

depending on its protocol.

---

### PART VII — CANDIDATE STANDALONE ARCHITECTURE

#### 27. Software architecture

The candidate standalone target is:

```text
one-time RF/PHY initialization
    ↓
fixed channel / PLL
    ↓
RX/baseband kept active
    ↓
read CFO
    ↓
validate
    ↓
classify FSK
    ↓
handshake
    ↓
repeat
```

---

#### 28. Point not yet physically demonstrated

The corpus alone does not demonstrate:

```text
non-802.11 tone
    ↓
bit0 = 1
    ↓
fresh raw CFO
```

This property must be verified on silicon.

---

#### 29. Why this validation does not invalidate the 100% software status

Software is fully defined for both cases:

```text
valid CFO
invalid CFO
```

What remains to determine is whether the **hardware** produces a valid value for the target signal.

That is functional qualification of the baseband block, not an unknown CPU function.

---

### PART VIII — SILICON VALIDATION PROCEDURE

#### 30. Test A — valid Wi-Fi frame

Establish the reference:

```text
signal with known CFO
    ↓
0x60009800.bit0
[15:8]
getter output
```

Verify sign and variation consistency.

---

#### 31. Test B — entry into WDEV bit8 path

When:

```text
event.bit8 = 1
```

passively capture:

```text
0x60009800
0x600098DC
0x3FF2003C
CCOUNT
```

without calling the getter before the capture.

---

#### 32. Test C — frame ultimately rejected

Capture CFO before the discard decision.

If:

```text
bit0 = 1
```

on a frame that is later rejected, this proves that CFO can be produced before RX success.

---

#### 33. Test D — non-802.11 tone / FSK

Inject a signal with known frequency offset.

Observe whether:

```text
0x60009800.bit0
```

returns to 1 and whether:

```text
[15:8]
```

tracks the offset.

Positive result:

```text
standalone RX FSK hardware-validated
```

Negative result:

```text
estimator depends on PHY synchronization
```

and further synchronizer investigation would be required.

---

### PART IX — TIMING AND ACQUISITION

#### 34. Do not conflate software rate and hardware rate

The CPU function can be called quickly.

But the rate of fresh values depends on:

```text
baseband
synchronization
CFO estimator
handshake cycle
```

Maximum symbol rate therefore cannot be derived from CPU instruction count alone.

---

#### 35. Handshake after every attempt

Even when a read is invalid:

```text
result = 0x7FFF
```

one must still reproduce:

```text
0x600098DC |= 0xF
```

when replacing the standard getter with a direct primitive.

---

### PART X — WHAT NOT TO DO

#### 36. Do not use `phy_freq_offset` as the RX measurement

It is correction state, not the received raw measurement.

---

#### 37. Do not ignore the `0x7FFF` sentinel

An invalid value must never be classified as a symbol.

---

#### 38. Do not omit the handshake on error

The handshake is unconditional in the exact getter.

---

#### 39. Do not call the getter early merely as a passive probe

The getter modifies:

```text
0x600098DC
```

Early instrumentation must read the result directly without triggering the handshake before capture.

---

#### 40. Do not equate `0x3FF2003C[19:16]` with `rxend_state`

The former is a 4-bit WDEV MMIO field used as a CFO gate.

`RxControl.rxend_state` is a distinct descriptor field.

No direct binary identity is demonstrated.

---

#### 41. Do not use IQ_EST E4 as a general FSK discriminator

E4 is an energy/power metric suited to OOK/ASK.

It does not replace CFO for general FSK.

---

#### 42. Do not present non-802.11 CFO as already validated

The software is closed; autonomous availability of the metric remains hardware validation.

---

### PART XI — EXACT MODEL BOUNDARY

#### 43. 100% decoded on the software/static side

```text
CFO result address
valid bit
raw field
sign
conversion
sentinel
WDEV gate
measurement storage
measurement/correction separation
handshake
handshake mask
RMW
MEMW
handshake on success and failure
absence of a second CPU re-arm
standard consumption point
WDEV bit8 landmark
```

---

#### 44. To be measured on silicon/baseband

```text
valid bit0 at event.bit8?
CFO on rejected frame?
CFO on non-802.11 tone?
refresh rate?
latency?
minimum detectable deviation?
sensitivity?
BER?
```

---

### PART XII — QUICK REFERENCE

#### 45. Cheat sheet

```text
CFO RESULT:
    0x60009800

VALID:
    bit0

RAW CFO:
    bits15:8
    signed int8

CONVERSION:
    cfo = (raw * 107) >> 6

INVALID:
    0x7FFF

WDEV CONTEXT GATE:
    0x3FF2003C[19:16] < 8

HANDSHAKE:
    0x600098DC
    RMW |= 0xF
    executed even when invalid

MEASUREMENT STATE:
    phy_meas_freq_offset

CORRECTION STATE:
    phy_freq_offset

STANDARD CONSUMPTION:
    post-RX-success

WDEV RX GATE:
    event bit8

AUTONOMOUS NON-WIFI CFO:
    silicon validation required
```

---

#### 46. Final minimal pseudo-code

```c
bool rx_fsk_sample(int16_t *cfo)
{
    uint32_t ctx = rd32(0x3FF2003Cu);
    int16_t v = CFO_INVALID;

    if (((ctx >> 16) & 0x0Fu) < 8u) {
        uint32_t r = rd32(0x60009800u);

        if (r & 1u) {
            int8_t raw = (int8_t)((r >> 8) & 0xFFu);
            v = (int16_t)(((int32_t)raw * 107) >> 6);
        }
    }

    memw();
    uint32_t h = rd32(0x600098DCu);
    h |= 0x0Fu;
    memw();
    wr32(0x600098DCu, h);

    if (v == CFO_INVALID)
        return false;

    *cfo = v;
    return true;
}
```

---

#### 47. Conceptual 2-FSK example

```c
bool demod_2fsk(int16_t cfo,
                int16_t threshold,
                bool one_is_high)
{
    return one_is_high
        ? (cfo > threshold)
        : (cfo < threshold);
}
```

The threshold must be learned or calibrated from the two received centers.

---

#### 48. Project's official conclusion for RX FSK/M-FSK

For the analyzed corpus, the project now considers the CFO CPU path required for an FSK discriminator **100% closed within the software/static scope**:

```text
0x60009800
    ↓
gate + valid
    ↓
signed raw CFO
    ↓
conversion
    ↓
software classification
    ↓
0x600098DC |= 0xF
```

Separation from RF correction is closed:

```text
phy_meas_freq_offset
    ≠
phy_freq_offset
```

The CPU protocol contains no second hidden re-arm in the corpus.

The only remaining boundary is physical:

> **does the ESP8266 baseband produce a new valid CFO measurement for a non-802.11 FSK/tone signal in the intended standalone state?**

This question belongs to silicon/baseband validation and is not a software reverse-engineering gap.

---

#### 49. Document status

```text
Document : project RX FSK/M-FSK reference
Version  : 1.0
Status   : FROZEN — software/static scope
Basis    : ESP8266 mask-ROM + PHY v6 + PP/WDEV from analyzed corpus
Next     : standalone CFO validation on silicon in the living master
```

---
# APPENDIX E — Complete QPSK/QAM History v0.60 → v0.83

> This appendix reproduces the complete QAM segment from the living master document, from the opening QPSK/QAM work in v0.60 through closure of the standard RX interface in v0.83. New `librftest.a` findings are included in the normative v2.0 synthesis and supersede ATE hypotheses that became directly verifiable.

### 36BY. QPSK/QAM TX — Hardware I/Q Gain/Phase Correction Actuators — v0.60

This pass officially opens the **QPSK/QAM** work after software closure of OOK/ASK/FSK.
The first question is deliberately more primitive than “can we do QAM?”:

```text
does the silicon expose separate TX I/Q gain
and TX I/Q phase actuators to the CPU?
```

The answer is now **yes**, in the context of TXIQ calibrations.

---

#### 36BY.1 Relevant ROM functions

The Espressif ROM linker and the `g_phyFuns` table give:

```text
rom_rfcal_txiq_set_reg  = 0x40008A70   g_phyFuns + 0x108
rom_set_txiq_cal        = 0x40008D34   g_phyFuns + 0x114
```

They are distinct from:

```text
rom_rfcal_txiq_cover    = 0x400088B8
rom_start_tx_tone
rom_set_ana_inf_tx_scale
```

The new analysis is based on the exact mask-ROM bytes from the corpus.

---

#### 36BY.2 Closed reminder: the TXIQ selector separates gain and phase

The already reconstructed `txiq_cover()` flow closes:

```text
sel = 1
    → test states 0x4 → 0x8
    → result byte[0]
    → txiq_gain

sel = 0
    → test states 0x0 → 0x1
    → result byte[1]
    → txiq_phase
```

This mapping is now reused to name the two branches of `rom_rfcal_txiq_set_reg()`.

---

#### 36BY.3 `selector=1` branch: TX I/Q gain trim

`rom_rfcal_txiq_set_reg()` receives a signed coefficient and the selector.

When:

```text
selector != 0
```

the coefficient is transformed into:

```text
sign
magnitude = min(abs(value), 15)
```

then written by two `rom_i2c_writeReg_Mask()` operations:

```text
block = 0x77
host  = 0

reg16 bit0    := sign
reg16 bits7:3 := magnitude
```

The physical magnitude field is 5 bits wide, but the observed calibration path limits the useful value to:

```text
0..15
```

The demonstrated software contract is therefore a **signed TX I/Q gain trim**.

---

#### 36BY.4 `selector=0` branch: TX I/Q phase trim

When:

```text
selector == 0
```

the same primitive uses:

```text
sign
magnitude = min(abs(value), 31)
```

then programs:

```text
block = 0x77
host  = 0

reg15 bit6    := sign
reg17 bits5:0 := magnitude
```

The magnitude field is 6 bits wide and calibration limits the useful magnitude to:

```text
0..31
```

The demonstrated software contract is therefore a **signed TX I/Q phase trim**.

---

#### 36BY.5 Compact reconstruction

```text
                        rom_rfcal_txiq_set_reg(value, selector, ...)
                                      │
                    ┌─────────────────┴─────────────────┐
                    │                                   │
               selector=1                          selector=0
                    │                                   │
              TXIQ GAIN                             TXIQ PHASE
                    │                                   │
       abs(value) clamped to 15            abs(value) clamped to 31
                    │                                   │
       0x77/reg16 bit0 = sign             0x77/reg15 bit6 = sign
       0x77/reg16[7:3] = magnitude        0x77/reg17[5:0] = magnitude
```

This separation is no longer merely an interpretation of symbol names: it comes from combining the `txiq_cover()` flow with the exact ROM I²C writes.

---

#### 36BY.6 `rom_set_txiq_cal()` exposes two signed fields in `0x60009860`

The routine:

```text
rom_set_txiq_cal()
```

reads:

```text
0x60009860
```

under two exact masks:

```text
0x1F000000
0x00FC0000
```

that is:

```text
bits28:24  → 5-bit field
bits23:18  → 6-bit field
```

The routine then reconstructs signed representations:

```text
5-bit field: if value >= 16, value -= 32
6-bit field: if value >= 32, value -= 64
```

then propagates sign information to:

```text
0x77/reg16.bit0
0x77/reg15.bit6
```

The widths match exactly the gain/phase families described above.

The recommended classification therefore becomes:

```text
0x60009860[28:24] → 5-bit-width TXIQ state/correction
0x60009860[23:18] → 6-bit-width TXIQ state/correction
```

with a very strong functional link respectively to the gain and phase paths.

---

#### 36BY.7 What this changes for QPSK/QAM

Before v0.60, we knew only that the PHY:

```text
measures I/Q gain mismatch
measures I/Q phase mismatch
```

v0.60 adds:

```text
the CPU can actually apply
a signed I/Q gain trim
and a signed I/Q phase trim
through internal I²C
```

This is new evidence that TX contains an adjustable vector I/Q chain.

---

#### 36BY.8 What this still does NOT demonstrate

It would be incorrect to conclude:

```text
phase_trim = 31  → +90°
phase_trim = -31 → -90°
```

No:

```text
code → degrees
```

law is provided by the ROM.

More importantly, these controls are **mismatch corrections around the nominal I/Q modulator**.
Their physical range may be small.

Therefore:

```text
phase-trim actuator          → demonstrated
90° constellation rotation  → not demonstrated
gain-trim actuator           → demonstrated
independent I and Q amplitude → not demonstrated
```

QPSK therefore still requires an I/Q quadrant/sign-selection mechanism, or evidence that the phase trim spans a sufficiently large range.

---

#### 36BY.9 Consequence for the TX QPSK research strategy

TX priority becomes:

```text
1. determine the physical scale of the phase trim
2. determine whether trim can be hot-updated without rerunning calibration
3. identify an I/Q sign/quadrant control
4. decode the physical role of TXIQ tone_mode states
5. test whether multiple tone slots combine into orthogonal paths
```

The path:

```text
ASK + FSK = QAM
```

is explicitly rejected: that produces amplitude + frequency, not amplitude + phase.

---

#### 36BY.10 TX QPSK/QAM status after v0.60

| Item | Status |
|---|---:|
| existence of a TX I/Q datapath | **very strongly demonstrated indirectly** |
| TX I/Q gain calibration | **100% software function** |
| TX I/Q phase calibration | **100% software function** |
| signed I/Q gain-trim actuator | **~99% software** |
| signed I/Q phase-trim actuator | **~99% software** |
| gain I²C mapping | **~99%** |
| phase I²C mapping | **~99%** |
| TXIQ fields in `0x60009860` | **~95% structural** |
| phase trim → degrees | **open** |
| gain trim → I/Q ratio | **open** |
| per-symbol hot update of trims | **open** |
| ±I/±Q quadrant selection | **open** |
| **proprietary QPSK TX** | **~55–65% architectural** |
| **proprietary QAM TX** | **~50–60% architectural** |

The progress is real: the lock is no longer “does an I/Q phase control exist?” but rather “can this calibration setting be repurposed as a modulation actuator, and where is quadrant selection?”

---

### 36BZ. QPSK/QAM RX — `rom_dc_iq_est()` as an Integrated Vector Primitive — v0.61

After the v0.60 discovery of TX I/Q trims, this pass searches for the RX counterpart:

```text
can the CPU obtain two separate I and Q components
without accessing the raw ADC/IQ stream?
```

The software answer is now **yes**, in the form of a window-integrated IQ_EST mean.

---

#### 36BZ.1 Reconstructed ABI of `rom_dc_iq_est()`

Official address and exact binary:

```text
rom_dc_iq_est = 0x4000615C
```

The first three Xtensa arguments are used as:

```text
a2 → mode
a3 → N
a4 → output pointer
```

The beginning of the routine loads:

```text
g_phyFuns + 0x34
    = rom_iq_est_enable
```

then directly calls:

```text
iq_est_enable(mode, N)
```

without transforming either parameter.

---

#### 36BZ.2 Two separate I and Q outputs

After the estimator returns:

```text
base = 0x60000200
```

the routine reads:

```text
base + 0x3DC = 0x600005DC
base + 0x3E0 = 0x600005E0
```

For each:

```text
value = REG32(...) >> 6
```

where the shift is arithmetic.

It then computes:

```text
divisor = N + 1
```

and calls:

```text
__divsi3 @ 0x4000DC88
```

therefore a **signed** division.

The two results are written into two consecutive 32-bit words:

```text
out[0] = ((int32_t)REG32(0x600005DC) >> 6) / (N + 1)
out[1] = ((int32_t)REG32(0x600005E0) >> 6) / (N + 1)
```

then:

```text
rom_iq_est_disable()
```

is called before returning.

---

#### 36BZ.3 Exact software-level pseudo-code

```c
void dc_iq_est(uint32_t mode, uint32_t N, int32_t out[2])
{
    iq_est_enable(mode, N);

    int32_t i_acc = (int32_t)REG32(0x600005DC);
    int32_t q_acc = (int32_t)REG32(0x600005E0);

    i_acc >>= 6;
    q_acc >>= 6;

    int32_t d = (int32_t)N + 1;

    out[0] = i_acc / d;
    out[1] = q_acc / d;

    iq_est_disable();
}
```

This reconstruction does not depend on a loose interpretation of register names: both reads, both signed divisions, and both stores are directly visible in the mask-ROM.

---

#### 36BZ.4 `N=0` is accepted by the CPU path

`rom_iq_est_enable()`:

```text
N &= 0x7FFF
N <<= 2
```

then programs:

```text
0x6000057C[16:2]
```

No check for:

```text
N > 0
N >= 1
N >= threshold
```

is performed.

For its part, `rom_dc_iq_est()` computes:

```text
N + 1
```

before division.

Therefore, for:

```text
N = 0
```

software requests the minimum encodable window and uses:

```text
divisor = 1
```

Exact conclusion:

> **`N=0` is a valid software call to the ROM API.**

What still requires measurement is the physical duration and exact number of hardware samples actually integrated when `N=0`.

---

#### 36BZ.5 This is still NOT a raw I/Q stream

One must distinguish:

```text
classic SDR:
I[0],Q[0], I[1],Q[1], I[2],Q[2] ... at ADC rate
```

from:

```text
ESP8266 IQ_EST:
trigger a window
        ↓
accumulate in hardware
        ↓
return mean I / mean Q
        ↓
disable
```

v0.61 therefore discovers no ADC-sample FIFO.

It discovers a primitive closer to:

```text
integrate-and-dump I/Q
```

or:

```text
windowed vector sampler
```

with hardware assistance.

---

#### 36BZ.6 Why this is relevant to QPSK

An ideal QPSK symbol can be represented as:

```text
s = I + jQ
```

with four quadrants.

If a short IQ_EST window correctly aligned with an external symbol returns a pair whose sign/angle follows the constellation:

```text
(+I,+Q)
(-I,+Q)
(-I,-Q)
(+I,-Q)
```

then the CPU can classify the quadrant without receiving the complete I/Q stream.

Candidate architecture:

```text
RF RX
  ↓
mixer / hardware baseband
  ↓
short IQ_EST window
  ↓
rom_dc_iq_est()
  ↓
(I_mean, Q_mean)
  ↓
DC / rotation / gain correction
  ↓
quadrant
  ↓
2 QPSK bits
```

---

#### 36BZ.7 QAM extension

For rectangular QAM:

```text
I ∈ multiple levels
Q ∈ multiple levels
```

If the relationship between `(I_mean,Q_mean)` and the external signal is sufficiently monotonic and stable, the same primitive could provide:

```text
16-QAM → 4 I levels × 4 Q levels
64-QAM → 8 I levels × 8 Q levels
```

The decision software would be trivial compared with the hardware question.

The lock is the physical fidelity of the primitive, not the classification algorithm.

---

#### 36BZ.8 Important limitations

The ROM name is:

```text
dc_iq_est
```

not:

```text
symbol_iq_sample
```

The function was designed for DC I/Q estimation/calibration.

Therefore, it remains to demonstrate:

```text
1. the external RF signal reaches the I/Q accumulators in the intended standalone RX state
2. a single-carrier symbol produces a usable non-zero mean vector
3. internal DC cancellation / filtering does not destroy that information
4. small N terminates correctly and fast enough
5. the pair preserves sign and phase stably
6. CFO / phase error can be corrected at the required rate
```

v0.61 promotes none of these points to facts.

---

#### 36BZ.9 Correlators `0x60000580..58C` as a secondary path

The block also exposes:

```text
0x60000580
0x60000584
0x60000588
0x6000058C
```

already used for RXIQ calibration.

Observed combinations include:

```text
Re-like = R0 + R3
Im-like = R1 - R2
```

and `rom_get_corr_power()` then computes:

```text
(Re-like)^2 + (Im-like)^2
```

This structure demonstrates an **internal complex correlation**, but does not yet close what serves as the correlator's reference.

For QPSK/QAM, these registers are therefore classified as a:

```text
candidate vector/correlation path
```

but priority remains `dc_iq_est`, whose two I and Q outputs are more direct.

---

#### 36BZ.10 RX QPSK/QAM status v0.61

| Item | Status |
|---|---:|
| two separate CPU-accessible I/Q accumulators | **~99% software** |
| signed normalization `/ (N+1)` | **100% software** |
| ROM API returning two components | **~99%** |
| `N` programmable over 15 bits | **~99%** |
| `N=0` software-accepted | **100% software** |
| raw ADC I/Q stream to LX106 | **not identified** |
| mean I/Q pair on external RX | **strong candidate, silicon validation** |
| preservation of QPSK quadrant | **open** |
| short-window cadence | **open** |
| software rotation/CFO correction | **known architecture, performance open** |
| **PHY-assisted QPSK RX** | **~60–70% architectural** |
| **PHY-assisted QAM RX** | **~55–65% architectural** |
| **raw I/Q SDR** | **still not demonstrated** |

---

#### 36BZ.11 Next discriminating test

The next conceptual validation to prepare is:

```text
controlled-phase single-carrier RF input
        ↓
four known phases
        ↓
dc_iq_est(demonstrated mode, short N)
        ↓
plot (I_mean,Q_mean)
```

The expected result for a usable vector primitive would be four distinct clusters that rotate with input phase.

This validation must be performed in a controlled RF environment; if no other vector interface appears, further disassembly cannot substitute for it.

---

### 36CA. QPSK/QAM TX — Exact Digital TXIQ Packing of `0x60009860` — v0.62

v0.60 closed the two gain/phase **I²C trims**.
This pass searches for where the values computed by calibration are stored and applied in the digital baseband.

The result is more important for QPSK/QAM: the ROM directly programs an MMIO register with both coefficients.

---

#### 36CA.1 Exact write in `rom_rfcal_txiq()`

In:

```text
rom_rfcal_txiq @ 0x40008610
```

the base:

```text
0x60009600
```

is loaded, then offset:

```text
0x260
```

is used.

Final address:

```text
0x60009860
```

The final sequence performs:

```text
old = REG32(0x60009860)
old &= 0xE000FFFF
old |= gain_field
old |= phase_field
old |= 0x00030000
REG32(0x60009860) = old
```

The mask:

```text
0xE000FFFF
```

preserves:

```text
bits31:29
bits15:0
```

and frees exactly:

```text
bits28:16
```

for the TXIQ block.

---

#### 36CA.2 Exact gain saturation

The first result of `rfcal_txiq_cover()` comes from the path:

```text
selector = 1
    → txiq_gain
```

The ROM clamps it:

```text
gain < -15 → -15
gain > +15 → +15
```

that is:

```text
gain ∈ [-15,+15]
```

It then forms the code:

```text
gain >= 1:
    gain_code = 32 - gain

gain <= 0:
    gain_code = -gain
```

which reduces exactly to:

```c
gain_code = (-gain) & 0x1F;
```

The code is then placed in:

```text
0x60009860[28:24]
```

Thus:

```text
bits28:24 = (-txiq_gain_clamped) mod 32
```

---

#### 36CA.3 Exact phase saturation

The second result of `rfcal_txiq_cover()` is:

```text
selector = 0
    → txiq_phase
```

The ROM clamps:

```text
phase < -31 → -31
phase > +31 → +31
```

Then:

```text
phase >= 0:
    phase_code = phase

phase < 0:
    phase_code = phase + 64
```

which is simply:

```c
phase_code = phase & 0x3F;
```

The field is then shifted by 18 bits:

```text
0x60009860[23:18] = phase_code
```

Thus:

```text
bits23:18 = txiq_phase_clamped mod 64
```

---

#### 36CA.4 Bits17:16 forced to `11`

The ROM explicitly ORs:

```text
0x00030000
```

before the write.

Therefore:

```text
0x60009860[17:16] = 0b11
```

in the TXIQ state configured by this routine.

Their exact physical role is unnamed, but their required state is now demonstrated.

---

#### 36CA.5 Reconstructed digital TXIQ layout

The subfield becomes:

```text
31 29 28          24 23               18 17 16 15            0
┌─────┬─────────────┬───────────────────┬─────┬────────────────┐
│keep │ gain_code   │ phase_code        │  11 │ keep           │
│     │ 5 bits      │ 6 bits            │     │                │
└─────┴─────────────┴───────────────────┴─────┴────────────────┘
```

with:

```text
gain_code  = (-clamp(txiq_gain,-15,+15)) & 0x1F
phase_code = ( clamp(txiq_phase,-31,+31)) & 0x3F
```

---

#### 36CA.6 Calibration memory format

Immediately before the MMIO write, the routine also builds a storable calibration word.

Observed packing:

```text
low byte  = phase_code
high byte = gain_code
```

conceptually:

```c
uint16_t txiq_cal_word =
      (uint16_t)phase_code
    | ((uint16_t)gain_code << 8);
```

This word is stored through a pointer supplied to the calibration path.

---

#### 36CA.7 Restore path without recomputation

A second path in `rom_rfcal_txiq()` is taken when input state indicates that an existing calibration should be reused.

It then reads the two bytes of the saved word:

```text
low byte  → phase_code → <<18
high byte → gain_code  → <<24
```

then applies exactly the same:

```text
RMW 0x60009860
```

Conclusion:

> `0x60009860` is not merely a calibration-result register: it is used as a **digital TXIQ configuration register reloaded from stored coefficients**.

---

#### 36CA.8 Relationship to `rom_set_txiq_cal()`

v0.60 showed that:

```text
rom_set_txiq_cal()
```

rereads:

```text
0x60009860[28:24]
0x60009860[23:18]
```

as two signed 5/6-bit values, then adjusts analog sign bits:

```text
0x77/reg16.bit0
0x77/reg15.bit6
```

v0.62 therefore closes the loop:

```text
txiq_cover()
   ↓
gain / phase
   ↓
digital encoding
   ↓
0x60009860
   ↓
rom_set_txiq_cal()
   ↓
synchronization of analog I²C signs
```

---

#### 36CA.9 Why this register matters for QPSK/QAM

Unlike internal I²C:

```text
0x60009860
```

is direct MMIO.

That opens a new experimental hypothesis:

```text
modify a TXIQ coefficient
without rerunning the entire calibration
```

The potential CPU cost is therefore much lower than a pair of I²C transactions.

But v0.62 still does not conclude that this constitutes a phase modulator.

---

#### 36CA.10 Fundamental limit: correction ≠ constellation

The phase field is calibration-clamped to:

```text
[-31,+31]
```

but no mapping:

```text
1 code = X degrees
```

is known.

The block is designed to correct an **I/Q mismatch**, so the physical range may be only a few degrees around nominal quadrature.

QPSK requires states separated by:

```text
90°
```

The next question therefore becomes quantitative:

```text
what RF rotation results from changing phase_code?
```

---

#### 36CA.11 TX status after v0.62

| Item | Status |
|---|---:|
| digital TXIQ register address | **~99%** |
| gain field bits28:24 | **~99%** |
| phase field bits23:18 | **~99%** |
| bits17:16 forced to 11 | **100% software** |
| gain saturation ±15 | **100% software** |
| phase saturation ±31 | **100% software** |
| gain encoding `(-g)&0x1F` | **100% software** |
| phase encoding `p&0x3F` | **100% software** |
| saved word `phase | gain<<8` | **~99%** |
| coefficient restore without recomputation | **~99%** |
| register usable for hot update | **plausible, not physically demonstrated** |
| phase_code → degrees | **open** |
| ability to reach multiple quadrants | **open** |
| proprietary QPSK TX | **~65–70% architectural** |
| proprietary QAM TX | **~60–65% architectural** |

---

#### 36CA.12 Next TX target

Two research tests remain the highest priorities:

```text
A. search for a runtime writer of 0x60009860 outside calibration
B. search for an I/Q sign/quadrant primitive distinct from trim
```

If neither exists statically, the next lock becomes controlled RF characterization of the `phase_code` field, exactly as `tone_control→Hz` did for FSK.

---

### 36CB. QPSK/QAM TX — Activation and Persistence of `0x60009860` — v0.63

v0.62 closed the digital packing:

```text
bits28:24 = gain_code
bits23:18 = phase_code
bits17:16 = 11
```

The next question is:

```text
how is this state activated and retained in the PHY?
```

The exact binary provides two new answers.

---

#### 36CB.1 Exact order in `chip_v6_initialize_bb()`

Relocations in `phy_chip_v6.o` give:

```text
+0x2E88 → ram_rfcal_txiq
+0x2E9B → phy_bb_rx_cfg
```

Disassembly around this region confirms the order:

```text
prepare TXIQ arguments
        ↓
ram_rfcal_txiq(...)
        ↓
optional channel branch
        ↓
phy_bb_rx_cfg()
```

Thus TXIQ coefficients are calculated/applied **before** the following BB configuration step.

---

#### 36CB.2 `phy_bb_rx_cfg()` sets `0x60009860.bit0`

In `phy_bb_rx_cfg()`:

```text
+0x28B6  movi.n a13, 1
...
+0x28F5  MEMW
+0x28F8  l32i a2, a14, 608
         a14 = 0x60009600
         → read 0x60009860

+0x28FB  or a2, a2, a13
         → OR 1

+0x28FE  MEMW
+0x2903  s32i a2, a14, 608
         → write 0x60009860
```

Pseudo-code:

```c
REG32(0x60009860) |= 0x00000001u;
```

This write occurs after TXIQ calibration in the initialization sequence.

---

#### 36CB.3 Interaction with TXIQ packing

`rom_rfcal_txiq()` applies:

```c
r  = REG32(0x60009860);
r &= 0xE000FFFFu;
r |= gain_code << 24;
r |= phase_code << 18;
r |= 0x00030000u;
REG32(0x60009860) = r;
```

The mask:

```text
0xE000FFFF
```

preserves:

```text
bits15:0
```

and therefore:

```text
bit0
```

is never destroyed by a gain/phase coefficient update.

The structure becomes:

```text
0x60009860
    ├── bit0       : persistent state enabled by phy_bb_rx_cfg()
    ├── bits17:16  : 11 in the programmed TXIQ state
    ├── bits23:18  : phase_code
    └── bits28:24  : gain_code
```

---

#### 36CB.4 Cautious interpretation of bit0

The order:

```text
calibrate / write coefficients
        ↓
set bit0
```

is highly compatible with:

```text
enable correction
apply calibration
active latch
```

but no symbol or branch explicitly names this bit.

The correct status is therefore:

```text
bit0 = candidate TXIQ enable/latch      → very strong inference
bit0 = official "TXIQ_ENABLE"          → not demonstrated
```

The document does not promote inference to fact.

---

#### 36CB.5 `register_chipv6_phy()` saves and restores `0x60009860`

A second access group appears in:

```text
register_chipv6_phy()
```

The routine reads several consecutive/neighboring PHY configuration words and uses `0x60009860` in comparison with saved state.

Observed sequence:

```text
read 0x60009854/58/5C-like neighbors
read 0x60009860
read 0x60009864
...
compare with RTC backup
...
if restoration required:
    write saved state → 0x60009860
    write saved state → 0x60009864
```

The exact `0x60009860` access appears as:

```text
l32i ... offset 608
...
s32i ... offset 608
```

relative to the same PHY base.

Conclusion:

> **the complete `0x60009860` word belongs to a saveable/restorable PHY configuration state.**

This behavior would be unusual for a simple ephemeral result register.

---

#### 36CB.6 Consequence for a future vector modulator

The digital TXIQ block is now better described as:

```text
calibrated coefficients
      ↓
0x60009860 gain/phase
      ↓
probable bit0 activation/latch
      ↓
persistent configuration
      ↓
restoration after RTC/wakeup context
```

This strengthens the possibility of repurposing this register as a vector actuator.

But one fundamental proof is still missing:

```text
modify phase_code while TX is active
        ↓
instantaneous and deterministic RF rotation?
```

The fact that a register is persistent and MMIO does not guarantee that the modulator samples it symbol-by-symbol.

---

#### 36CB.7 TX QPSK/QAM status v0.63

| Item | Status |
|---|---:|
| gain/phase packing in `0x60009860` | **~99%** |
| `bit0` set after calibration | **100% software** |
| bit0 preserved during TXIQ RMW | **100% software** |
| register save/restore | **~99% structural** |
| register = persistent PHY configuration state | **very high confidence** |
| bit0 = TXIQ enable/latch | **~85–90% inference** |
| direct CPU MMIO write possible | **100%** |
| hot update immediately applied to modulator | **open** |
| phase_code → degrees | **open** |
| ±I/±Q quadrant control | **still open** |
| proprietary QPSK TX | **~68–72% architectural** |
| proprietary QAM TX | **~62–67% architectural** |

---

#### 36CB.8 Next priority

The next static gain must come from one of these two results:

```text
A. find a writer of 0x60009860 while TX is already active
B. identify a field/mode that explicitly selects I/Q signs or quadrants
```

Failing that, the `phase_code → RF phase` relationship becomes a hardware-characterization boundary, just as `tone_control → Hz` did for FSK.

---

### 36CC. QPSK/QAM TX — Exact TXIQ Gain/Phase Error Formulas — v0.64

v0.60/v0.62 had identified two signed coefficients named by the flow:

```text
txiq_gain
txiq_phase
```

A dangerous ambiguity remained for QPSK:

```text
is txiq_phase a geometric phase expressed in degrees/rotation codes,
or only a mismatch-correction coefficient?
```

The body of `txiq_cover()` now answers this.

---

#### 36CC.1 Gain measurement: two independent powers

For:

```text
sel = 1
```

`txiq_get_mis_pwr()` produces two 16-bit powers.

Let:

```text
P0
P1
```

be the two results.

The routine constructs:

```text
den = min(P0, P1)
if den == 0:
    den = 1

num = (P1 - P0) << 11
q   = num / den
gain = (q + 16) >> 5
```

Thus, up to rounding:

```text
gain ≈ 64 · (P1 - P0) / max(min(P0,P1),1)
```

A context condition can then invert the sign.

This form is typical of a **relative gain error**: difference between two powers normalized by a reference power.

---

#### 36CC.2 Phase measurement: difference normalized by the sum

For:

```text
sel = 0
```

two other measurements are obtained.

The code performs:

```text
num = (P0 - P1) << 12

den = P0 + P1
if den == 0:
    den = 1

q = num / den
phase = (q + 16) >> 5
```

Therefore:

```text
phase ≈ 128 · (P0 - P1) / max(P0 + P1,1)
```

with a possible final sign inversion depending on a context flag.

This is much stronger evidence than the simple name `txiq_phase`.

---

#### 36CC.3 `txiq_phase` is not expressed in degrees

The formula:

```text
(P0-P1)/(P0+P1)
```

is dimensionless.

The factor:

```text
128
```

gives a normalized-error format of roughly Q7 / scale 1/128.

Therefore:

```text
phase_code = 1
```

does not mean:

```text
1°
```

and no conversion to degrees appears in software.

Certain software-side conclusion:

> **`txiq_phase` is an I/Q mismatch-correction coordinate, not a geometric angle.**

---

#### 36CC.4 The ±31 saturation is now interpretable

`rom_rfcal_txiq()` then clamps:

```text
txiq_phase ∈ [-31,+31]
```

Numerically this corresponds to:

```text
|phase_error_coordinate| <= 31/128
                              ≈ 0.2421875
```

in the normalized coordinate produced by `txiq_cover()`.

This does not directly yield a range in degrees, because the exact relationship between this power ratio and angular error depends on the test vectors.

But it shows that the block applies a **bounded local correction**, consistent with a mismatch compensator rather than a full constellation rotator.

---

#### 36CC.5 Gain formula is also normalized

The applied gain is clamped to:

```text
[-15,+15]
```

while the computed metric is approximately:

```text
64·ΔP/Pmin
```

The gain coefficient is therefore also a normalized error, not an arbitrary I or Q amplitude.

Consequence:

```text
gain_code
    ≠ independent I-amplitude command
    ≠ independent Q-amplitude command
```

at the currently demonstrated level.

---

#### 36CC.6 Direct impact on the QPSK path via `0x60009860`

The naive path:

```text
phase_code = K0 → 0°
phase_code = K1 → 90°
phase_code = K2 → 180°
phase_code = K3 → 270°
```

is now **strongly downgraded**.

The register remains useful for:

```text
correcting the I/Q modulator
maintaining a clean constellation
compensating gain/phase mismatch
```

but not as a demonstrated quadrant rotator.

---

#### 36CC.7 TXIQ stimulus states become more interesting

The phase calibrator does not compare one tone under two small trims.

It forces two **distinct hardware states** of `tone_mode`:

```text
0x00B → 0x04B
```

and computes the normalized difference of their powers.

The gain calibrator forces:

```text
0x10B → 0x20B
```

and compares the two powers.

The algorithmic structure is compatible with:

```text
gain test  : two orthogonal paths measured separately
phase test : two vector combinations of those paths
```

but this physical mapping remains a **strong inference**, not yet proof.

For QPSK, priority therefore shifts from:

```text
modify phase_code
```

to:

```text
understand exactly the RF vectors produced by
tone_mode 0x00B / 0x04B / 0x10B / 0x20B
```

---

#### 36CC.8 I±Q hypothesis — precise status

In conventional I/Q calibration, comparing the power of two combined states can estimate quadrature error through vectors of the form:

```text
I + Q
I - Q
```

The ratio:

```text
(Pplus - Pminus)/(Pplus + Pminus)
```

is naturally sensitive to phase error.

The formula observed on ESP8266 is structurally compatible with this method.

However, the binary itself does not name the states:

```text
I+Q
I-Q
```

v0.64 therefore classifies:

```text
0x00B / 0x04B = two I/Q phase-test stimuli
    → demonstrated

exact I+Q / I-Q interpretation
    → strong hypothesis to test

90° RF separation between these two stimuli
    → not demonstrated
```

---

#### 36CC.9 New classification of TX QPSK paths

| Path | Status |
|---|---:|
| phase trim `0x60009860` as 90° rotator | **strongly downgraded** |
| gain trim as independent I/Q amplitude | **strongly downgraded** |
| trims for cleaning a future constellation | **very relevant / demonstrated** |
| `tone_mode 0x00B↔0x04B` as vector states | **very promising** |
| `tone_mode 0x10B↔0x20B` as I/Q selection | **strong inference** |
| exact I+Q / I−Q | **open / strong hypothesis** |
| global 180° inversion | **not identified** |
| complete four QPSK quadrants | **not closed** |

---

#### 36CC.10 Project status after this correction

Hardware understanding increases, but the QPSK TX percentage must not be artificially raised: an overly optimistic path has just been eliminated.

```text
QPSK TX architectural : ~65–70%
QAM TX architectural  : ~60–65%
```

The next decisive discovery must concern the **TXIQ stimuli**, not the magnitude of the small correction trims.

---

### 36CD. QPSK/QAM TX — Exhaustive Inventory of `0x60009860` Accesses — v0.65

This pass searches for direct software proof of:

```text
TX active
    ↓
repeated gain_code / phase_code updates
    ↓
new RF vector
```

No such user is present in the analyzed corpus.

---

#### 36CD.1 Objects containing PHY base `0x60009600`

Binary search for the literal:

```text
0x60009600
```

in extracted archives:

```text
libphy:
    phy_chip_v6_ana.o
    phy_chip_v6_cal.o
    phy_chip_v6.o

libpp:
    no literal occurrence

libnet80211:
    no relevant occurrence
```

The mask-ROM contains the general base only once in its literal pool, shared by already-mapped routines.

---

#### 36CD.2 `phy_chip_v6_cal.o`

Instructions with displacement:

```text
+0x260 = 608
```

relative to base `0x60009600` fall within:

```text
ram_rfcal_txiq()
```

Sequence:

```text
read  0x60009860
mask / OR gain+phase
write 0x60009860
```

This is the digital calibration writer already closed in v0.62.

No second writer to this displacement is found in the object.

---

#### 36CD.3 `phy_chip_v6.o`

Four `l32i/s32i` instructions with displacement `608` are found.

##### Group A — `phy_bb_rx_cfg()`

```text
read  0x60009860
OR    1
write 0x60009860
```

Role:

```text
probable persistent activation/latch after calibration
```

##### Group B — `register_chipv6_phy()`

The function:

```text
read  0x60009860
...
compare / RTC backup
...
write saved state → 0x60009860
```

Role:

```text
PHY configuration save / restore
```

Neither group is a TX modulation loop.

---

#### 36CD.4 `phy_chip_v6_ana.o`

This object contains the general PHY base but no:

```text
l32i/s32i base+0x260
```

in its analyzed code section.

RFPLL/analog paths therefore add no hidden writer of `0x60009860`.

---

#### 36CD.5 Mask-ROM

The already completed exact ROM mapping gives:

```text
rom_rfcal_txiq()
    → RMW 0x60009860

rom_set_txiq_cal()
    → read TXIQ fields then synchronize I²C signs
```

Other ROM routines using the same PHY base add no additional dynamic writer of this register.

---

#### 36CD.6 Final functional inventory

```text
0x60009860
    │
    ├── WRITE gain/phase
    │      rom_rfcal_txiq
    │      ram_rfcal_txiq
    │
    ├── SET bit0
    │      phy_bb_rx_cfg
    │
    ├── SAVE/RESTORE full word
    │      register_chipv6_phy
    │
    └── READ / propagate signs
           rom_set_txiq_cal
```

Demonstrated absence in the corpus:

```text
no PP writer
no net80211 writer
no symbol-level LMAC writer
no identified normal packet-TX writer
no modulation loop based on phase_code
```

---

#### 36CD.7 Consequence for QPSK/QAM

The distinction becomes clear:

##### Demonstrated

```text
CPU can write TXIQ fields
persistent register
known packing
known coefficients
```

##### Not demonstrated

```text
hardware instantaneously samples a new coefficient during TX
glitch-free behavior
phase continuity
write→RF latency
symbol-rate use
```

The mere fact that the register is MMIO is not enough to promote hot update to fact.

---

#### 36CD.8 Research decision

The path:

```text
phase_code in 0x60009860 as a fast QPSK modulator
```

has now reached its **static boundary in the standard corpus**.

Two possibilities remain:

```text
1. old factory / ATE firmware containing dynamic use
2. controlled silicon validation
```

In parallel, the `tone_mode` path remains statically attackable because it is already hot-modified by `txiq_get_mis_pwr()` while the TX clock remains active.

It therefore becomes TX QPSK priority #1.

---

#### 36CD.9 Status after v0.65

| Path | Status |
|---|---:|
| standard `0x60009860` writers inventoried | **~99% corpus** |
| TXIQ coefficient hot update used by SDK | **not found** |
| CPU can write MMIO | **100%** |
| RF effect of manual hot update | **silicon validation** |
| `tone_mode` hot-modified during TXIQ | **100% software** |
| `tone_mode` as candidate QPSK path | **top priority** |
| QPSK TX architectural | **~65–70%** |
| QAM TX architectural | **~60–65%** |

---

### 36CE. QPSK/QAM TX — Vector Model of `tone_mode` Stimuli — v0.66

This pass seeks a physically coherent meaning for the four already-demonstrated TXIQ states:

```text
phase : 0x00B → 0x04B
gain  : 0x10B → 0x20B
```

The new insight does not come from a hidden symbol name, but from the **exact coupling between each state pair and the mathematical equation the SDK applies to their powers**.

---

#### 36CE.1 Software facts used

The exact corpus already closes:

```text
sel = 1
    → tone_mode 0x10B then 0x20B
    → two powers P0/P1
    → I/Q gain correction

sel = 0
    → tone_mode 0x00B then 0x04B
    → two powers P0/P1
    → I/Q phase/quadrature correction
```

The formulas closed in v0.64 are:

```text
gain_step  ≈ 64  * (P1-P0) / min(P0,P1)

phase_step ≈ 128 * (P0-P1) / (P0+P1)
```

These two equations do not measure the same property.

---

#### 36CE.2 Interpretation of the gain pair: two separate rails

To measure gain imbalance between two quadrature branches, the most direct measurement is:

```text
state G0 → power of rail A
state G1 → power of rail B
```

then:

```text
gain_error ∝ PB - PA
```

normalized by a reference power.

That is exactly the structure of the observed calculation:

```text
(P1-P0) / min(P0,P1)
```

The recommended classification becomes:

```text
tone_mode 0x10B = quadrature-rail A stimulus
tone_mode 0x20B = quadrature-rail B stimulus
```

with:

```text
A/B = probably I/Q or Q/I
```

but **the I-versus-Q ordering is not yet demonstrated**.

Confidence level:

```text
two separate orthogonal rails : very high
A=I / B=Q                   : open
A=Q / B=I                   : open
```

---

#### 36CE.3 Interpretation of the phase pair: two cross-combinations

The formula:

```text
(P0-P1)/(P0+P1)
```

is qualitatively different from a simple gain difference.

For two equal-amplitude sinusoidal rails A and B, the powers of the combinations:

```text
A+B
A-B
```

are:

```text
|A+B|² = |A|² + |B|² + 2 Re(A·B*)
|A-B|² = |A|² + |B|² - 2 Re(A·B*)
```

Therefore:

```text
|A+B|² - |A-B|² = 4 Re(A·B*)
```

and:

```text
(|A+B|² - |A-B|²) /
(|A+B|² + |A-B|²)
```

directly isolates the normalized cross-correlation term.

If A and B are ideally in quadrature:

```text
Re(A·B*) = 0
```

the two powers become equal.

A small quadrature error makes them unequal.

This is precisely the property needed by the SDK calculation:

```text
phase_step ≈ 128*(P0-P1)/(P0+P1)
```

---

#### 36CE.4 Promoted physical model

The best model becomes:

```text
GAIN TEST
    0x10B → rail A only
    0x20B → rail B only

PHASE TEST
    0x00B → combination A+B
    0x04B → combination A-B
```

or the equivalent permutation:

```text
0x00B ↔ A-B
0x04B ↔ A+B
```

depending on physical polarity and the sign chosen by the calibration path.

The binary still does not allow the sign ordering to be chosen.

What is now very strongly constrained is the **vector family**, not its exact electrical labels.

---

#### 36CE.5 Why this model is stronger than a generic analogy

The model simultaneously explains:

```text
1. why 0x10B/0x20B are used exclusively for I/Q gain correction
2. why their powers are compared separately
3. why 0x00B/0x04B are used exclusively for I/Q phase correction
4. why the phase path uses a difference normalized by the sum
5. why phase error vanishes when the two powers become equal
6. why the final trim remains small around a nominal quadrature state
```

Interpreting these four states as simple arbitrary amplitude levels would poorly explain all six properties at once.

---

#### 36CE.6 General external corroboration

Conventional I/Q transmitter architectures separately correct:

```text
gain imbalance
quadrature phase imbalance
```

by measuring differences between the I/Q branches and applying small digital/analog corrections.

General I/Q-modulator literature describes precisely the role of:

```text
- gain correction between I and Q
- phase correction around 90°
- image / cross-term measurement to estimate quadrature error
```

This literature **corroborates the architecture**, but is not proof of ESP8266 bit mapping. The mapping remains grounded in the exact binary corpus.

---

#### 36CE.7 QPSK consequence: we have vectors, not yet four quadrants

If the model is correct, the known states correspond approximately to:

```text
rail A
rail B
A+B
A-B
```

These are already **vector stimuli**.

But full QPSK requires four sign states:

```text
+A +B
-A +B
-A -B
+A -B
```

or an equivalent rotated basis.

The corpus does not yet demonstrate a control explicitly providing:

```text
-A alone
-B alone
-(A+B)
-(A-B)
```

at symbol rate.

Therefore:

> `tone_mode` now provides strong evidence of access to the geometry of the two quadrature rails, but **not yet to their four sign combinations**.

---

#### 36CE.8 Physical bit 25: structural candidate, no promotion

In the physical TXIQ nibble `bits27:24`, demonstrated states use:

```text
bit24
bit26
bit27
```

Bit:

```text
bit25
```

does not appear in any of the four observed states.

It would be tempting to assign it a hypothetical:

```text
sign / invert / quadrant
```

role, but **no corpus use demonstrates this**.

Status:

```text
bit25 = unused in observed TXIQ stimuli
role = open
```

It becomes a research target, not a conclusion.

---

#### 36CE.9 Possibility of a partial constellation

The corpus already demonstrates hot `tone_mode` changes in `txiq_get_mis_pwr()` while the TX clock remains active.

If the physical vectors match the model above, hardware can therefore switch at least among multiple vector orientations during a tone session.

This fact is stronger than the mere existence of a trim:

```text
TXIQ contains a mini test-vector generator
```

The question becomes:

```text
does this mini-generator expose the missing inversions/signatures
through bit combinations unused by calibration?
```

---

#### 36CE.10 Implication for QAM

QAM requires:

```text
signs of both rails
+
independent levels on both rails
```

Current findings provide:

```text
orthogonal rails: strong evidence
cross-combinations: strong evidence
global tone amplitude: demonstrated
fine I/Q gain: demonstrated
fine I/Q phase: demonstrated
independent signs: not demonstrated
fast independent I/Q amplitudes: not demonstrated
```

QAM therefore remains more difficult than QPSK.

---

#### 36CE.11 New TX QPSK/QAM confidence map

| Item | Status |
|---|---:|
| `0x10B/0x20B` = pair used for I/Q gain | **100% software** |
| gain pair = two separate orthogonal rails | **~95% architectural** |
| exact I/Q ordering of the two rails | **open** |
| `0x00B/0x04B` = pair used for I/Q phase | **100% software** |
| phase pair = cross sum/difference combinations | **~90–95% physical/inferential** |
| exact sum-versus-difference polarity | **open** |
| hot `tone_mode` switching | **100% software** |
| four QPSK signs/quadrants | **not found** |
| bit25 as sign/quadrant | **hypothesis only** |
| proprietary QPSK TX | **~65–70% architectural** |
| proprietary QAM TX | **~55–60% architectural** |

---

#### 36CE.12 Next research

The most discriminating static searches become:

```text
1. search for any write using physical bit25 of the tone slot
2. search for tone_mode values other than:
       0x001
       0x00B
       0x04B
       0x10B
       0x20B
3. search factory-test firmware/ROM for additional TXIQ stimuli
4. trace the native OFDM mapper toward the modulation datapath
5. on RX, determine whether DC_I/DC_Q can be measured in normal external RX
   without loopback and with short N
```

---
### 36CF. QPSK/QAM RX — separation of `phy_ops` / `phy_func_tab` and status of `rom_dc_iq_est` — v0.67

v0.61 had closed the primitive:

```text
rom_dc_iq_est(mode,N,out)
    ↓
IQ_EST
    ↓
I_mean, Q_mean
```

The next question is:

```text
does the v6 PHY already use it in its normal RX path?
```

An initial automated search produced an important false positive. This pass corrects it.

---

#### 36CF.1 The `phy_enable_agc() +0x10` false positive

In `phy.o`:

```text
phy_enable_agc @ 0xB4
```

the exact disassembly is:

```text
L32R    a0, pointer_slot
L32I.N  a0, a0, 0
L32I.N  a0, a0, 16
CALLX0  a0
```

Taken in isolation, this:

```text
+16 = +0x10
```

appears to correspond to:

```text
phy_func_tab + 0x10 = rom_dc_iq_est
```

But that interpretation is wrong.

---

#### 36CF.2 `register_phy_ops()` installs a different table

Still in `phy.o`:

```text
register_phy_ops @ .text+0x08
```

essentially does:

```c
local_phy_ops = argument_a2;
```

The pointer subsequently used by the wrappers is therefore **provided by the caller**.

It does not come from:

```text
phy_get_romfuncs()
0x3FFFC730
0x3FFFC734
```

The `phy_enable_agc()` wrapper operates on this registered local table.

---

#### 36CF.3 Reconstructed layout of the small `phy_ops` table

The wrappers in `phy.o` load:

```text
rf_init()
    → table +0x00

RFChannelSel()
    → table +0x08

phy_delete_channel()
    → table +0x0C

phy_enable_agc()
    → table +0x10

phy_disable_agc()
    → table +0x14

phy_initialize_bb()
    → table +0x18

phy_set_sense()
    → table +0x1C
```

`bb_init()` also uses the:

```text
+0x18
```

entry in its wrapper.

This layout functionally proves that this table represents **high-level PHY operations**.

It is distinct from the ROM table:

```text
phy_func_tab / g_phyFuns
```

which contains at `+0x10`:

```text
rom_dc_iq_est
```

---

#### 36CF.4 New methodological rule

Starting with v0.67:

> **a vtable offset is meaningful only after the identity of the table has been established.**

It is forbidden to automatically turn:

```text
indirect_call +0x10
```

into:

```text
rom_dc_iq_est
```

without proving that the base really is `g_phyFuns/phy_func_tab`.

This correction is important for all subsequent QPSK/SDR searches.

---

#### 36CF.5 Scan of the real `g_phyFuns` calls

After separating the two tables, indirect calls through the real `g_phyFuns` were
re-inventoried in:

```text
phy_chip_v6.o
phy_chip_v6_cal.o
phy_chip_v6_ana.o
phy_sleep.o
```

The recovered offsets cover, among others, known primitives for:

```text
I2C
PBUS
tone
SAR
RX init
RFPLL
calibration
```

but no standard patched v6 call uses:

```text
g_phyFuns + 0x10
```

in the analyzed paths.

Therefore:

```text
rom_dc_iq_est exists in the ROM table
but
no identified standard v6 caller uses it
```

---

#### 36CF.6 What this says about `rom_dc_iq_est`

##### Demonstrated

```text
real ROM primitive
callable through the ROM table
controls IQ_EST
returns signed I_mean/Q_mean
programmable N
N=0 accepted by the CPU path
```

##### Not demonstrated

```text
automatic call during normal reception
consumption by WDEV/PP
use on every Wi-Fi symbol
existing external QPSK use in the SDK
```

The best classification becomes:

```text
latent / reusable PHY vector instrument
```

rather than:

```text
standard RX demodulator output
```

---

#### 36CF.7 Consequence for QPSK RX

This correction does not reduce the value of the primitive.

On the contrary, it makes the candidate architecture more precise:

```text
normal RF RX
    ↓
active baseband datapath
    ↓
proprietary firmware explicitly triggers IQ_EST
    ↓
rom_dc_iq_est(mode,N,out)
    ↓
(I_mean,Q_mean)
    ↓
quadrant classification
```

The remaining hardware question is now very clear:

> **does IQ_EST see a useful external vector in the normal RX state when firmware
> explicitly triggers it, without the RXIQ loopback state?**

This is exactly the validation already required for OOK/ASK on the external path, but with
the two signed accumulators instead of only the E4 energy value.

---

#### 36CF.8 Why the absence of a standard caller matters

If `rom_dc_iq_est()` were already called in normal RX, one could directly search for:

```text
where do I_mean/Q_mean go?
```

That path does not exist in the identified v6 patches.

We will therefore have to build our own consumer:

```text
trigger
wait DONE
read I/Q
disable/re-arm
classify
```

This makes the architecture closer to an **SDR assisted by a vector accumulator**, rather than
a reuse of an existing packet output.

---

#### 36CF.9 QPSK/QAM RX status after correction

| Item | Status |
|---|---:|
| existence of `rom_dc_iq_est()` | **100%** |
| ABI `mode,N,out[2]` | **~99%** |
| signed I/Q normalized by `N+1` | **~99%** |
| availability through phy_func_tab | **100%** |
| `phy_enable_agc()+0x10` = dc_iq_est | **FALSE — corrected** |
| distinct `phy_ops` table | **100% software** |
| standard v6 caller of `phy_func_tab+0x10` | **none identified** |
| deliberate external RX use | **main candidate** |
| external quadrant preserved | **silicon validation** |
| PHY-assisted QPSK RX | **~60–65% architectural** |
| PHY-assisted QAM RX | **~50–60% architectural** |

The slight decrease avoids confusing availability of a primitive with standard use of that primitive.

---

#### 36CF.10 Next RX target

The next pass must investigate:

```text
1. exact location of IQ_EST in the chain relative to:
       DC cancellation
       AGC
       channel filter
       derotation/CFO
2. registers configuring DC-removal / RX DC calibration
3. behavior of I_mean/Q_mean at short N
4. possibility of disabling or freezing a possible DC suppressor
5. correlators 0x60000580..58C as an alternative if DC_I/DC_Q
   cancel a centered symbol
```

---


### 36CG. QPSK/QAM TX — bit25 explicitly cleared in TXIQ stimuli — v0.68

v0.66 had left:

```text
physical bit25 of nibble bits27:24
    → unused
    → role open
```

This pass closes the question more strongly: in the TXIQ stimulus generator of the corpus,
bit25 is not merely absent from the observed constants; it is **explicitly kept at zero**
by the slot reconstruction logic.

---

#### 36CG.1 First TXIQ write

The direct ROM path around `0x600005B8` begins by preserving only:

```text
old & 0xF0000000
```

Therefore:

```text
bits27:0
```

are rebuilt from scratch.

The logic then adds:

```text
tone_control
scale
0x002C0000
```

and the first TXIQ state:

```text
sel=0 → 0x0 << 24
sel=1 → 0x4 << 24 = bit26
```

Thus, in the first write:

```text
bit24 = 0
bit25 = 0
bit26 = sel
bit27 = 0
```

for the relevant states.

Bit25 cannot inherit an old state because `0xF0000000` removed all of `bits27:0`
before reconstruction.

---

#### 36CG.2 Second TXIQ write

After the first measurement, the code rereads the slot and applies:

```text
old & 0xF0FFFFFF
```

Mask:

```text
1111 0000 1111 1111 1111 1111 1111 1111
     ^^^^
   bits27:24 cleared
```

Then the second-state value is ORed in:

```text
sel=0 → 0x1 << 24 = bit24
sel=1 → 0x8 << 24 = bit27
```

Therefore:

```text
sel=0:
    bits27:24 = 0001

sel=1:
    bits27:24 = 1000
```

and in both cases:

```text
bit25 = 0
```

by construction.

---

#### 36CG.3 Exact set of generated states

The standard TXIQ mini-generator therefore produces exactly:

```text
0000
0001
0100
1000
```

on:

```text
bits27:24
```

that is:

```text
0x0
0x1
0x4
0x8
```

The code never produces:

```text
0x2
```

nor any value containing:

```text
bit25 = 1
```

in these sequences.

---

#### 36CG.4 Cross-check with the exhaustive tone-slot audit

The supplied mask ROM has only three real families of slot accesses:

```text
rom_start_tx_tone()
rom_stop_tx_tone()
direct TXIQ
```

The:

```text
rom_stop_tx_tone()
```

path only clears:

```text
bit18
```

and creates no high mode bit.

The demonstrated normal uses of:

```text
rom_start_tx_tone()
```

program:

```text
tone_mode = 0x001
```

therefore no bit25.

The direct TXIQ path has just been demonstrated to force bit25 to zero.

Conclusion for the exact corpus:

> **no identified standard writer of the tone slots produces bit25=1.**

---

#### 36CG.5 What this conclusion does not mean

The hardware could technically assign a meaning to bit25.

The ROM:

```text
rom_start_tx_tone(mode,...)
```

injects the supplied `mode` without explicit 10-bit validation.

Experimental firmware could therefore write an unobserved mode.

But:

```text
bit25 may have a hardware role
```

does not imply:

```text
bit25 = I inversion
bit25 = Q inversion
bit25 = quadrant
```

None of those meanings is supported by the corpus.

---

#### 36CG.6 Consequence for QPSK

The standard model no longer contains a simple candidate:

```text
bit25 → sign/quadrant
```

Complete QPSK must therefore be sought in:

```text
1. the hardware OFDM mapper/modulator
2. tone_mode states unused by the standard SDK
3. physical multi-slot combination
4. another register in the I/Q datapath
```

The TXIQ mini-generator remains useful because it demonstrates the two rails and their combinations,
but in the standard corpus it does not expose the four required signs.

---

#### 36CG.7 Status

| Item | Status |
|---|---:|
| TXIQ states `0/1/4/8` | **100% software** |
| bit25 absent merely by chance | **no: it is explicitly zero** |
| first write clears old bit25 | **100%** |
| second write clears old bit25 | **100%** |
| bit25 used as standard quadrant | **ruled out** |
| hypothetical hardware role of bit25 | **unknown** |
| QPSK through standard bit25 use | **ruled out** |

---


### 36CH. SDR/QAM RX — `phy_adc_read_fast()` = SAR/TOUT, not RF I/Q ADC — v0.69

The search for an SDR interface lower than `IQ_EST` surfaced the symbol:

```text
phy_adc_read_fast
```

in:

```text
phy_chip_v6_ana.o
```

Because its name contains `adc_read_fast`, it was essential to verify whether it could provide
samples from the RF front end.

The answer is now no.

---

#### 36CH.1 Exact symbol

The symbol is:

```text
phy_adc_read_fast
offset .irom0.text = 0x16EC
size                = 627 bytes
```

It resides in the same analog object as channel/RF functions.

The name alone was therefore insufficient to determine which ADC was involved.

---

#### 36CH.2 Reconstructed ABI

The prologue saves:

```text
a2 → stack +28
a3 → stack +32
a4 → a14
```

The body then uses:

```text
original a2 → output pointer
original a3 → sample count
original a4 → division/timing parameter
```

The reconstructed signature is therefore compatible with:

```c
void phy_adc_read_fast(uint16_t *adc_addr,
                       uint16_t adc_num,
                       uint8_t adc_clk_div);
```

This is also the signature publicly associated with this older ESP8266 PHY helper.

---

#### 36CH.3 MMIO block used

The function loads, among others:

```text
0x60000600
0x60000A00
```

then operates on:

```text
0x60000600 + 0x110 = 0x60000710

0x60000A00 + 0x350 = 0x60000D50
0x60000A00 + 0x354 = 0x60000D54
0x60000A00 + 0x358 = 0x60000D58
0x60000A00 + 0x35C = 0x60000D5C
0x60000A00 + 0x360 = 0x60000D60
0x60000A00 + 0x380 = 0x60000D80
```

The function:

```text
- saves the configuration
- programs timing / conversion count
- triggers the block
- waits for its state
- reads/aggregates the results
- writes 16-bit values to the buffer
- restores the state
```

The register:

```text
0x60000D50
```

is publicly known in ESP8266 SAR implementations as a SAR configuration/status register.

---

#### 36CH.4 Public Espressif cross-check

The ESP8266 RTOS SDK documentation describes:

```text
adc_read_fast()
```

as:

```text
Measure the input voltage of TOUT(ADC) pin
```

and states that:

```text
Wi-Fi and interrupts need to be turned off
```

during this fast measurement.

The associated configuration structure exposes:

```text
clk_div
```

for the ADC collection clock.

This documentation functionally matches the block manipulated by the v6 binary.

---

#### 36CH.5 Historical cross-check with the exact name

Historical community documentation for the ESP8266 PHY explicitly gives:

```c
void phy_adc_read_fast(uint16 *adc_addr,
                       uint16 adc_num,
                       uint8 adc_clk_div);
```

with:

```text
adc_addr    = ADC sample buffer
adc_num     = number of samples
adc_clk_div = collection-clock divisor
```

The name and ABI match the symbol in the corpus.

This external cross-check is not required for the disassembly, but it closes the nature
of the measured block.

---

#### 36CH.6 Why this is not an RF I/Q ADC

An RF I/Q path would need to expose at least two components or a stream tied to the RX datapath:

```text
I[n]
Q[n]
```

or buffers associated with the RF baseband.

Here the function exposes:

```text
a single scalar 16-bit stream
```

from the SAR/TOUT block.

It is tied to SAR registers and external analog/VDD measurement, not to the registers:

```text
0x600005DC  I accumulator
0x600005E0  Q accumulator
0x60000580..58C IQ correlations
```

used by `IQ_EST`.

---

#### 36CH.7 Consequence for raw SDR

The path:

```text
phy_adc_read_fast
    ↓
raw RF ADC samples
    ↓
software SDR
```

is closed as:

```text
FALSE
```

The correct classification is:

```text
phy_adc_read_fast
    ↓
SAR ADC / TOUT / VDD analog sampling
```

This block can be used for general analog acquisition, but not for direct I/Q capture of the Wi-Fi RF signal.

---

#### 36CH.8 Consequence for QPSK/QAM RX

The RX hierarchy therefore remains:

```text
RAW RF I/Q FIFO       → not identified
        │
        X

vector IQ_EST
    ├── DC_I
    ├── DC_Q
    ├── complex correlations
    └── energy
        ↓
best currently known CPU observation point
```

For proprietary QPSK/QAM, effort must remain focused on:

```text
IQ_EST
correlators
CFO
RX/baseband state
```

and not on the SAR ADC.

---

#### 36CH.9 SDR status after eliminating this path

| Item | Status |
|---|---:|
| `phy_adc_read_fast` exists | **100%** |
| ABI buffer/count/clk_div | **~99%** |
| use of SAR block `0x60000Dxx` | **100% software** |
| TOUT/VDD subsystem | **~99% by cross-checking** |
| access to RF I/Q ADCs through this function | **no** |
| raw RF I/Q CPU stream identified elsewhere | **still no** |
| raw sample-stream SDR | **not supported by the current corpus** |
| IQ_EST-assisted SDR | **remains the main path** |

---

#### 36CH.10 Public cross-check sources

- Espressif ESP8266 RTOS SDK — ADC API: `adc_read_fast()` measures the TOUT(ADC) pin.
- Historical ESP8266 documentation of the `phy_adc_read_fast(buffer,count,clk_div)` helper.
- Public ESP8266 SAR implementations using `0x60000D50` as SAR configuration/status.

---

### 36CI. QPSK/QAM TX — native selection by `rate`, constellation mapper behind hardware — v0.70

QPSK/QAM research must distinguish two questions:

```text
1. does the ESP8266 actually have an accessible QPSK/QAM mapper?
2. can proprietary I/Q symbols be supplied to it outside 802.11?
```

The first answer is now closed at a lower software/hardware interface level.

---

#### 36CI.1 Exact TX structure

The DWARF of:

```text
rate_control.o
```

describes:

```text
struct esf_tx_desc_s
```

with:

```text
size = 32 bytes
```

Among its fields:

```text
offset +8:
    rate : 8 bits
```

The descriptor also contains:

```text
qid
retry counters
acktime
crypto_type
antenna
status
timestamp
rcSched
```

but **no per-symbol I/Q pair or amplitude/phase**.

---

#### 36CI.2 OFDM codes recovered from scheduling tables

The file:

```text
trc.o
```

contains the tables:

```text
rc11GSchedTbl
rc11NSchedTbl
rcP2P11GSchedTbl
rcP2P11NSchedTbl
BasicOFDMSched
```

The rate bytes used include exactly:

```text
0x08
0x09
0x0A
0x0B
0x0C
0x0D
0x0E
0x0F
```

These codes are therefore not merely theoretical external constants:
they are present in the tables of the exact corpus.

---

#### 36CI.3 Official Espressif meaning

ESP8266 Non-OS documentation gives:

```text
PHY_RATE_48 = 0x08
PHY_RATE_24 = 0x09
PHY_RATE_12 = 0x0A
PHY_RATE_6  = 0x0B
PHY_RATE_54 = 0x0C
PHY_RATE_36 = 0x0D
PHY_RATE_18 = 0x0E
PHY_RATE_9  = 0x0F
```

These values exactly match the bytes recovered from `trc.o`.

The link:

```text
software rate byte ↔ hardware PHY rate
```

is therefore closed.

---

#### 36CI.4 Rate → OFDM modulation mapping

For 802.11a/g OFDM:

```text
6  Mb/s → BPSK
9  Mb/s → BPSK

12 Mb/s → QPSK
18 Mb/s → QPSK

24 Mb/s → 16-QAM
36 Mb/s → 16-QAM

48 Mb/s → 64-QAM
54 Mb/s → 64-QAM
```

Thus the ESP8266 codes group as:

```text
0x0B / 0x0F → BPSK

0x0A / 0x0E → QPSK

0x09 / 0x0D → 16-QAM

0x08 / 0x0C → 64-QAM
```

The second code in each pair mainly changes the FEC code rate.

---

#### 36CI.5 What the CPU actually controls

The visible software supplies:

```text
payload / descriptor
rate
retries
flags
```

Then:

```text
rate
  ↓
hardware PHY
  ↓
FEC / interleaving
  ↓
constellation mapper
  ↓
pilots / OFDM
  ↓
IFFT / I/Q datapath
  ↓
DAC/RF
```

The modulation choice therefore genuinely exists, but the CPU does not directly provide:

```text
I_symbol
Q_symbol
```

in this descriptor.

---

#### 36CI.6 Consequence for proprietary QPSK

It is possible to request from the standard PHY:

```text
rate = 0x0A or 0x0E
```

and obtain a **normal 802.11 OFDM QPSK** transmission.

That still does not provide:

```text
proprietary QPSK without Wi-Fi framing/FEC/OFDM
```

For that, an injection point must be found after:

```text
descriptor/rate control
```

and before or at the level of the:

```text
constellation mapper
```

---

#### 36CI.7 Consequence for 16-QAM / 64-QAM

Same conclusion:

```text
rate 0x09/0x0D → native 16-QAM mapper
rate 0x08/0x0C → native 64-QAM mapper
```

The QAM hardware is therefore no longer a hypothesis.

What remains open is access to its **constellation coordinates** outside the normal pipeline.

---

#### 36CI.8 Why this boundary matters

Before this pass, two architectures could still be confused:

```text
A. CPU directly constructs I/Q and requests transmission
B. CPU selects a mode/rate and the PHY builds the constellation
```

The exact descriptor clearly supports model:

```text
B
```

at the known MAC/LMAC level.

Thus:

> **looking for an I/Q field in `esf_tx_desc_s` is a false path.**

The search must move deeper into the digital baseband.

---

#### 36CI.9 Relationship with the TXIQ generator

We now have two distinct interfaces:

```text
TXIQ/tone test path
    → vector calibration stimuli
    → rails / sum-difference
    → test/calibration instrument

normal OFDM TX path
    → descriptor.rate
    → hardware BPSK/QPSK/QAM mapper
```

The challenge is to find:

```text
an exploitable bridge to the normal mapper
```

or:

```text
a way to extend the TXIQ mini-generator to all four signs/levels
```

---

#### 36CI.10 QAM TX status after v0.70

| Item | Status |
|---|---:|
| QPSK hardware exists | **100%** |
| 16-QAM hardware exists | **100%** |
| 64-QAM hardware exists | **100%** |
| ESP8266 rate codes 0x08..0x0F | **100%** |
| `rate` field in TX descriptor | **100% DWARF** |
| modulation selected behind `rate` | **100% native architecture** |
| per-symbol I/Q in TX descriptor | **absent** |
| proprietary access to mapper | **open** |
| pre-mapper injection point | **not identified** |
| native Wi-Fi QAM | **YES** |
| proprietary QAM outside Wi-Fi | **still not demonstrated** |

---

#### 36CI.11 Next TX target

The research must now target the MMIO writes triggered when the rate code changes among:

```text
0x0B → BPSK
0x0A → QPSK
0x09 → 16-QAM
0x08 → 64-QAM
```

Objective:

```text
identify the hardware register that receives
the modulation/rate code
```

then trace:

```text
that register
    ↓
mapper / encoder / OFDM engine
```

If that register separates:

```text
modulation
FEC
OFDM mode
```

then a command interface lower than the descriptor might become accessible.

---


### 36CJ. QPSK/QAM TX — `descriptor.rate` → exact WDEV PHY/PLCP register — v0.71

v0.70 had closed:

```text
esf_tx_desc_s.rate @ +8
    ↓
codes 0x08..0x0F
    ↓
BPSK / QPSK / 16-QAM / 64-QAM
```

What was missing was the point where this information leaves CPU structures and enters the modem.

That boundary is now localized.

---

#### 36CJ.1 WDEV base used by `lmacSetTxFrame()`

The literal pool of `.text.lmacSetTxFrame` contains:

```text
0x3FF20A00
```

The function loads this base into the register used for four nearby hardware stores.

The observed offsets are:

```text
+0x2DC → 0x3FF20CDC
+0x2E0 → 0x3FF20CE0
+0x2E4 → 0x3FF20CE4
+0x2E8 → 0x3FF20CE8
```

They therefore belong to the same WDEV TX block.

---

#### 36CJ.2 Exact rate load

In the TX path:

```text
L8UI a2, a8, 8
```

where:

```text
a8 = pointer to esf_tx_desc_s
```

and the DWARF closes:

```text
esf_tx_desc_s.rate
    offset +8
    size 8 bits
```

The `rate` used to program the hardware is therefore indeed **the descriptor byte** described in v0.70.

---

#### 36CJ.3 Exact legacy packing in `0x3FF20CE0`

The reconstructed sequence is:

```text
rate = desc->rate

rate4 = rate & 0x0F
rate4 <<= 12

length12 = frame_length & 0x0FFF
```

These fields are then combined in the word ultimately written to:

```text
0x3FF20CE0
```

For the legacy path:

```text
rate < 16
```

the demonstrated packing contains at least:

```text
bits 11:0   = LENGTH[11:0]
bits 15:12  = RATE[3:0]
```

Other fields in the same word come from the descriptor and TX-path flags, but they are
orthogonal to the QAM question studied here.

---

#### 36CJ.4 Why this packing is typical of a PLCP/PHY word

For 802.11 OFDM, before transmission the PHY must receive at minimum:

```text
RATE
LENGTH
```

in order to build SIGNAL/PLCP and configure the mapper/FEC.

The observed word places exactly:

```text
LENGTH 12 bits
RATE    4 bits
```

in a WDEV TX register adjacent to the other PHY-preparation words.

The recommended neutral name therefore becomes:

```text
0x3FF20CE0 = TX PHY/PLCP rate-length control word
```

Confidence:

```text
address / packing        : 100% software
PHY rate-length function : ~99% structural
electrical name PLCP0    : very likely, not officially named in the corpus
```

---

#### 36CJ.5 Direct QAM mapping in the hardware nibble

For the legacy codes closed in v0.70:

```text
rate 0x0B / 0x0F → BPSK
rate 0x0A / 0x0E → QPSK
rate 0x09 / 0x0D → 16-QAM
rate 0x08 / 0x0C → 64-QAM
```

the hardware therefore receives these PHY-selection nibbles in:

```text
0x3FF20CE0[15:12]
```

Conceptual example:

```text
desc.rate = 0x0A
        ↓
0x3FF20CE0[15:12] = 0xA
        ↓
OFDM PHY QPSK
```

This is no longer merely a rate-control table:
the constellation choice really reaches a **concrete hardware MMIO field**.

---

#### 36CJ.6 HT/MCS path

When:

```text
rate >= 16
```

`lmacSetTxFrame()` handles the descriptor differently.

The:

```text
rate >= 16
```

path notably sets a flag:

```text
0x01000000
```

that is:

```text
bit24
```

in the `0x3FF20CE0` word.

It then builds a second word written to:

```text
0x3FF20CE4
```

whose low bits include:

```text
(rate - 16) & 7
```

The separation therefore becomes:

```text
legacy OFDM:
    rate nibble in CE0

HT:
    HT flag in CE0
    MCS / additional HT parameters in CE4
```

This architecture is consistent with a legacy PLCP / HT-SIG separation.

---

#### 36CJ.7 Adjacent registers written in the same preparation

The same function also writes:

```text
0x3FF20CDC
0x3FF20CE8
```

with control/length/duration fields coming from the TX context.

v0.71 does not assign definitive electrical names to them, because the useful discovery here is the
propagation of `rate`.

The complete block nevertheless appears as a small coherent bank of **TX PHY/PLCP parameters**.

---

#### 36CJ.8 Uniqueness of writes in `libpp`

A scan of the executable sections of all extracted objects from `libpp.a` searched for 32-bit stores
at offsets:

```text
0x2DC
0x2E0
0x2E4
0x2E8
```

Result:

```text
.text.lmacSetTxFrame
    S32I +0x2DC
    S32I +0x2E8
    S32I +0x2E0
    S32I +0x2E4
```

No other corresponding store was recovered in the other analyzed `libpp` objects.

Therefore:

> `lmacSetTxFrame()` is the canonical CPU point for programming this TX PHY bank
> in the exact corpus.

---

#### 36CJ.9 Cross-generation architectural corroboration

In public ROMs from later Espressif generations, the following symbols explicitly appear:

```text
mac_tx_set_duration
mac_tx_set_htsig
mac_tx_set_plcp0
mac_tx_set_plcp1
mac_tx_set_plcp2
```

around `lmacSetTxFrame`.

This observation is **not** used to automatically rename the ESP8266 registers.

It only corroborates the reconstructed architecture:

```text
lmacSetTxFrame
    ↓
preparation of duration / HT-SIG / PLCP words
    ↓
hardware TX PHY
```

---

#### 36CJ.10 Consequence for proprietary QAM

We now have a hardware actuator lower than the descriptor:

```text
0x3FF20CE0[15:12] = rate / modulation code
```

But this field selects only:

```text
which constellation / which PHY mode
```

It still does not provide:

```text
I_symbol
Q_symbol
```

nor:

```text
arbitrary constellation-point index
```

Thus directly writing:

```text
0xA into RATE
```

selects QPSK in the OFDM modem, but the mapper continues to receive its bits from the normal
FEC/interleaver pipeline.

---

#### 36CJ.11 New exact boundary

The pipeline becomes:

```text
esf_tx_desc_s.rate
        ↓
lmacSetTxFrame()
        ↓
0x3FF20CE0[15:12]
        ↓
────────────────────────────────
hardware modulation selection
────────────────────────────────
        ↓
FEC / interleaver / constellation mapper
        ↓
OFDM I/Q
        ↓
DAC / RF
```

The next research step must therefore no longer ask:

```text
where is rate written?
```

That is closed.

It must ask:

```text
where do coded bits enter the mapper?
where does the mapper produce/consume the constellation index?
is there a test mode bypassing FEC/interleaver?
```

---

#### 36CJ.12 QAM TX status after v0.71

| Item | Status |
|---|---:|
| descriptor `rate` at +8 | **100%** |
| WDEV base `0x3FF20A00` | **100%** |
| rate/length register `0x3FF20CE0` | **100% software** |
| `RATE` in bits15:12 | **100% software** |
| `LENGTH` in bits11:0 | **100% software** |
| HT flag bit24 for rate>=16 | **100% software** |
| low MCS bits in `0x3FF20CE4` | **~99% software** |
| QPSK/16-QAM/64-QAM selection through MMIO | **100% native architecture** |
| arbitrary constellation index | **not found** |
| FEC/interleaver bypass | **not found** |
| proprietary QAM | **still open** |

---

### 36CK. QPSK/QAM TX — exact correspondence with the OFDM SIGNAL RATE field — v0.72

v0.71 had closed the register:

```text
0x3FF20CE0
```

with:

```text
bits11:0  = LENGTH
bits15:12 = rate nibble
```

The remaining question was:

```text
is this nibble an internal Espressif index,
or is it already the OFDM PLCP RATE?
```

The bit-for-bit cross-check now answers: **it is the exact OFDM RATE encoding**.

---

#### 36CK.1 IEEE RATE-field table

The 802.11a/g OFDM SIGNAL field encodes the eight legacy rates with the four bits:

```text
R1 R2 R3 R4
```

The standard table is:

```text
6  Mb/s → 1101
9  Mb/s → 1111
12 Mb/s → 0101
18 Mb/s → 0111
24 Mb/s → 1001
36 Mb/s → 1011
48 Mb/s → 0001
54 Mb/s → 0011
```

The field is transmitted in the bit order defined by the PHY.

---

#### 36CK.2 Conversion to CPU nibble, LSB-first

In the CPU register, the four bits occupy:

```text
bits15:12
```

with:

```text
R1 = bit12
R2 = bit13
R3 = bit14
R4 = bit15
```

The integer nibble value therefore becomes:

```text
R1 + 2*R2 + 4*R3 + 8*R4
```

Applying this gives:

| Rate | R1→R4 | CPU nibble | ESP8266 code |
|---:|:---:|---:|---:|
| 6  | `1101` | `0xB` | `0x0B` |
| 9  | `1111` | `0xF` | `0x0F` |
| 12 | `0101` | `0xA` | `0x0A` |
| 18 | `0111` | `0xE` | `0x0E` |
| 24 | `1001` | `0x9` | `0x09` |
| 36 | `1011` | `0xD` | `0x0D` |
| 48 | `0001` | `0x8` | `0x08` |
| 54 | `0011` | `0xC` | `0x0C` |

The correspondence is exact for all eight values.

---

#### 36CK.3 Consequence: no legacy rate translation table is required

The observed software path is simply:

```text
desc->rate
    ↓
rate & 0x0F
    ↓
<< 12
    ↓
0x3FF20CE0[15:12]
```

and the resulting value is already:

```text
PLCP SIGNAL.RATE
```

The PHY can therefore use these four bits directly to:

```text
- build SIGNAL
- select the modulation / code-rate combination
- configure the corresponding DATA datapath
```

---

#### 36CK.4 Exact RATE + LENGTH coupling

The same word contains:

```text
bits11:0 = LENGTH[11:0]
```

The OFDM SIGNAL field defines precisely:

```text
RATE   = 4 bits
LENGTH = 12 bits
```

as parameters required by the PHY.

The observed pair in the register is therefore too specific to be a mere scheduling coincidence.

The classification becomes:

```text
0x3FF20CE0
    = legacy TX SIGNAL/PLCP preparation register
      with very high confidence
```

The official ESP8266 symbolic name is still absent from the inspected public headers,
but the functional semantics are now almost completely determined.

---

#### 36CK.5 Direct modulation/FEC mapping

The table becomes:

```text
0xB → BPSK   1/2
0xF → BPSK   3/4

0xA → QPSK   1/2
0xE → QPSK   3/4

0x9 → 16-QAM 1/2
0xD → 16-QAM 3/4

0x8 → 64-QAM 2/3
0xC → 64-QAM 3/4
```

Thus a single nibble simultaneously encodes:

```text
constellation family
+
coding rate
```

for legacy OFDM rates.

---

#### 36CK.6 What this contributes to the QAM project

We now have an exact hardware primitive for selecting the native mode:

```c
legacy_rate_nibble = 0xA; /* QPSK 1/2 */
```

or:

```c
legacy_rate_nibble = 0x9; /* 16-QAM 1/2 */
```

or:

```c
legacy_rate_nibble = 0x8; /* 64-QAM 2/3 */
```

in the normal TX-preparation context.

But this still does not allow direct selection of:

```text
QAM point #k
```

The hardware still applies, downstream:

```text
scrambler
FEC
interleaver
mapper
```

to the data stream.

---

#### 36CK.7 Refined proprietary-QAM boundary

Before v0.72:

```text
rate code
    ↓
unknown hardware
```

After v0.72:

```text
exact PLCP RATE nibble
    ↓
modulation + code-rate selection
    ↓
hardware FEC/interleaver/mapper
```

The next target is therefore even more precise:

> **determine whether a test mode can supply already coded/interleaved bits to the mapper,
> or directly select a constellation index.**

---

#### 36CK.8 Cross-generation corroboration

Later Espressif ROMs export around `lmacSetTxFrame` the primitives:

```text
mac_tx_set_duration
mac_tx_set_htsig
mac_tx_set_plcp0
mac_tx_set_plcp1
mac_tx_set_plcp2
```

This nominal neighborhood is consistent with the register bank reconstructed on ESP8266.

It is used only as architectural corroboration:
the v0.72 conclusions rest first on the exact ESP8266 binary and the standard RATE table.

---

#### 36CK.9 v0.72 status

| Item | Status |
|---|---:|
| `CE0[15:12]` receives `desc.rate & 0xF` | **100% software** |
| codes 8..F = IEEE RATE bit-for-bit | **100% correspondence** |
| `CE0[11:0]` = LENGTH | **100% software** |
| CE0 = legacy SIGNAL/PLCP preparation | **~99% functional** |
| modulation + FEC selection through RATE | **100% native architecture** |
| intermediate CPU legacy translation table | **unnecessary / absent from path** |
| arbitrary constellation point | **not identified** |
| scrambler/FEC/interleaver bypass | **not identified** |

---

#### 36CK.10 Public cross-check sources

The RATE table was cross-checked against:

```text
IEEE 802.11a — SIGNAL field RATE table
```

and public references reproducing the patterns:

```text
6→1101, 9→1111, 12→0101, 18→0111,
24→1001, 36→1011, 48→0001, 54→0011
```

Conversion to a CPU nibble is then purely bit-for-bit and exactly matches the codes
present in the ESP8266 corpus.

---


### 36CL. QPSK/QAM TX — closure of `0x3FF20CE0` as PLCP1/PPDU control — v0.73

v0.71–0.72 had closed:

```text
0x3FF20CE0[11:0]  = LENGTH
0x3FF20CE0[15:12] = RATE
```

This pass resolves the upper bits directly from the DWARF and code literals.

---

#### 36CL.1 `esf_tx_desc_s + 12` = `kid`

The exact DWARF of `esf_tx_desc_s` describes the word at offset:

```text
+12
```

with, among other fields:

```text
kid         : 8 bits
crypto_type : 4 bits
antenna     : 4 bits
reserved    : 8 bits
status      : 8 bits
```

Given the little-endian layout generated by the compiler, the first byte:

```text
desc + 12
```

corresponds to:

```text
kid
```

In `lmacSetTxFrame()`:

```text
L8UI a5, a8, 12
SLLI a5, a5, 16
```

therefore:

```text
kid → bits23:16
```

of the word ultimately written to `0x3FF20CE0`.

---

#### 36CL.2 Exact word construction

The legacy/HT path builds:

```text
length12 = data_length & 0x0FFF
rate4    = desc->rate & 0x0F
kid8     = desc->kid
```

then:

```text
word =
      length12
    | (rate4 << 12)
    | (kid8  << 16)
```

For:

```text
rate >= 16
```

the code additionally retains the constant loaded from the literal pool:

```text
0x01000000
```

therefore:

```text
word |= BIT(24)
```

before the store.

---

#### 36CL.3 Exact HT literal

The literal pool of the first 0x50 bytes of `.text.lmacSetTxFrame` contains at offset:

```text
+0x14 → 0x01000000
```

The control flow is:

```text
if (rate < 16)
    ht_flag = 0;
else
    ht_flag = 0x01000000;
```

The bit:

```text
bit24
```

is therefore the software/hardware marker of the HT path in this word for the analyzed ESP8266.

The bit number observed on other generations must not automatically be transferred here:
the semantics may be the same while packing can differ.

---

#### 36CL.4 Final layout of `0x3FF20CE0`

For the analyzed path:

```text
31                             25 24 23               16 15         12 11             0
┌────────────────────────────────┬──┬───────────────────┬─────────────┬────────────────┐
│             0                  │HT│       KID         │    RATE     │     LENGTH     │
│                                │  │      8 bits       │    4 bit    │     12 bit     │
└────────────────────────────────┴──┴───────────────────┴─────────────┴────────────────┘
```

That is:

```c
plcp1 =
      (length & 0x0FFF)
    | ((rate & 0x0F) << 12)
    | ((uint32_t)kid << 16)
    | (is_ht ? 0x01000000u : 0);
```

---

#### 36CL.5 Why `KID` matters for the PLCP1/PPDU classification

The key index is not part of the SIGNAL field transmitted over the air.

It belongs to **TX PPDU control**: the hardware must know which key to use while
processing the frame.

The simultaneous presence of:

```text
LENGTH
RATE
KID
HT
```

in the same word therefore shows that:

```text
0x3FF20CE0
```

is better described as:

```text
TX PLCP1 / PPDU control word
```

than as a simple binary mirror of the over-the-air SIGNAL.

The RATE/LENGTH subfield directly reproduces PLCP parameters, while KID/HT serve
the hardware sequencer.

---

#### 36CL.6 Cross-generation corroboration

The ESP32 Open MAC project documents on ESP32 a register:

```text
PLCP1
```

with fields:

```text
LEN
RATE
IS_80211_N
```

and encryption work also shows that the crypto key index is passed
to hardware in this PLCP/TX-preparation context.

This cross-check is not used to impose the same offset on other chips.

It corroborates the architectural role identified independently on the ESP8266.

---

#### 36CL.7 Consequence for proprietary QAM

This word does not carry a constellation point.

It tells the PHY:

```text
which mode/rate to use
what length to process
which key to apply
whether the PPDU is HT
```

The mapper therefore receives its symbols from a later stage.

The QAM boundary remains:

```text
PLCP1 / PPDU config
        ↓
encoder / interleaver / mapper
        ↓
constellation
```

---

#### 36CL.8 v0.73 status

| `0x3FF20CE0` field | Status |
|---|---:|
| LENGTH bits11:0 | **100% software** |
| RATE bits15:12 | **100% software** |
| KID bits23:16 | **100% DWARF + flow** |
| HT flag bit24 | **100% flow + literal** |
| bits31:25 zero in this construction | **100% analyzed path** |
| PLCP1/PPDU control role | **~99% functional** |
| direct constellation index | **absent** |

---


### 36CM. QPSK/QAM TX — `0x3FF20CE4` = exact 32-bit HT-SIG — v0.74

This pass closes the word written only for:

```text
rate >= 16
```

at:

```text
0x3FF20CE4
```

The correspondence with the 802.11n HT-SIG format is direct.

---

#### 36CM.1 Exact construction in `lmacSetTxFrame()`

The code computes:

```text
mcs = (rate - 16) & 7
```

It then recovers the length saved earlier:

```text
data_length = eb->hdr_len + eb->data_len
```

and performs:

```text
data_length << 8
```

The word therefore begins with:

```text
bits2:0   = MCS index 0..7
bits7:3   = 0
bits23:8  = 16-bit length
```

---

#### 36CM.2 Why `data_length` is indeed `hdr_len + data_len`

At the beginning of `lmacSetTxFrame()`:

```text
L16UI eb+20 → hdr_len
L16UI eb+22 → data_len
ADD
EXTUI 16 bits
```

The result is saved on the stack and later reused for CE0/CE4.

The DWARF of `esf_buf_s` closes:

```text
+20 hdr_len
+22 data_len
```

Therefore the field written into `CE4[23:8]` is indeed the total PSDU/MPDU length prepared by the TX path.

---

#### 36CM.3 Espressif HT rate codes

The code family is:

```text
0x10..0x17 → MCS0..MCS7, Long GI
0x18..0x1F → MCS0..MCS7, Short GI
```

The code tests:

```text
rate < 24
```

that is:

```text
rate < 0x18
```

to choose the high byte.

This exactly matches the:

```text
LGI / SGI
```

boundary in the PHY rate enum.

---

#### 36CM.4 High byte built by the SDK

The code initializes:

```text
0x87
```

then replaces it with:

```text
0x07
```

if:

```text
rate < 0x18
```

It optionally adds:

```text
0x08
```

depending on a bit in `tx_desc.flags`.

The initial HT-SIG2 byte is therefore:

```text
LGI, non-aggregated : 0x07
LGI, aggregated     : 0x0F

SGI, non-aggregated : 0x87
SGI, aggregated     : 0x8F
```

before the 24-bit shift.

---

#### 36CM.5 Bit-for-bit correspondence with HT-SIG1

The HT-SIG1 format is:

```text
bits 0..6   MCS
bit  7      CBW 20/40
bits 8..23  HT-LENGTH
```

In CE4:

```text
bits0..2    = MCS0..7
bits3..6    = 0
bit7        = 0
bits8..23   = length
```

Therefore:

```text
MCS[6:0] = value 0..7
CBW      = 0
```

which corresponds to:

```text
1 spatial stream
20 MHz
MCS0..7
```

for the ESP8266.

---

#### 36CM.6 Bit-for-bit correspondence with the beginning of HT-SIG2

HT-SIG2 begins after the 24 bits of HT-SIG1.

Its first eight bits are:

```text
0  Smoothing
1  Not Sounding
2  Reserved
3  Aggregation
4  STBC[0]
5  STBC[1]
6  FEC Coding
7  Short GI
```

In the byte produced by the SDK:

```text
bit0 = 1
bit1 = 1
bit2 = 1
bit3 = conditional flag
bit4 = 0
bit5 = 0
bit6 = 0
bit7 = SGI
```

that is exactly:

```text
Smoothing    = 1
Not Sounding = 1
Reserved     = 1
Aggregation  = source tx_desc.flags
STBC         = 00
FEC          = BCC / 0
Short GI     = rate >= 0x18
```

The correspondence is complete.

---

#### 36CM.7 Descriptor source bit becomes `HT-SIG2.Aggregation`

In the code:

```text
word0 = tx_desc[0]
flag  = (word0 >> 28) & 1

if (flag)
    high_byte |= 0x08;
```

Then:

```text
high_byte << 24
```

Therefore this bit becomes:

```text
CE4.bit27
```

which corresponds precisely to:

```text
HT-SIG2 bit3 = Aggregation
```

The recommended name for this source bit therefore becomes:

```text
tx_desc aggregate/AMPDU flag
```

with very high functional confidence, even though the DWARF `flags` subfield does not provide
an individual name for each bit.

---

#### 36CM.8 Exact CE4 layout

```text
31 30 29 28 27 26 25 24 23                         8 7 6       3 2      0
┌──┬──┬─────┬──┬──┬──┬──┬───────────────────────────┬─┬─────────┬────────┐
│GI│FEC│STBC │AG│RS│NS│SM│        HT-LENGTH          │0│ MCS[6:3]│MCS[2:0]│
└──┴──┴─────┴──┴──┴──┴──┴───────────────────────────┴─┴─────────┴────────┘

SM = Smoothing        = 1
NS = Not Sounding     = 1
RS = Reserved         = 1
AG = Aggregation flag
STBC                  = 00
FEC                   = 0 / BCC
GI                    = 0 LGI, 1 SGI
CBW bit7              = 0 / 20 MHz
MCS                   = 0..7
```

---

#### 36CM.9 Final pseudocode

```c
uint32_t htsig_lo32 =
      ((uint32_t)((rate - 0x10) & 7))
    | ((uint32_t)data_length << 8)
    | (0x07u << 24);

if (aggregate)
    htsig_lo32 |= 0x08u << 24;

if (rate >= 0x18)
    htsig_lo32 |= 0x80u << 24;

REG32(0x3FF20CE4) = htsig_lo32;
```

The pseudocode reproduces the observed construction for the fields under study.

---

#### 36CM.10 HT-SIG CRC and tail

The complete HT-SIG is:

```text
48 bits
```

The first 32 bits are now explicitly supplied by CE4.

The remaining 16 bits include, among other things:

```text
extension spatial streams
CRC
tail
```

In the analyzed `lmacSetTxFrame()` path, no second dedicated HT word is constructed from
these fields.

Because CRC and tail are derivable from the preceding bits, the best model becomes:

> **the PHY engine automatically generates the end of HT-SIG, including CRC/tail,
> from the supplied parameter word.**

Confidence:

```text
hardware generation of CRC/tail : strong
exact internal location           : not CPU-visible
```

---

#### 36CM.11 Consequence for QAM

The field:

```text
MCS
```

selects HT modulation/coding, but CE4 remains a **PHY signaling** word.

It does not carry the constellation symbols themselves.

The proprietary-QAM search therefore remains below:

```text
HT-SIG / PLCP
        ↓
DATA encoder/interleaver
        ↓
constellation mapper
```

---

#### 36CM.12 v0.74 status

| CE4 item | Status |
|---|---:|
| MCS bits2:0 | **100% software** |
| high MCS bits3:6 = 0 | **100% path** |
| CBW bit7 = 0 | **100% path / HT20** |
| HT-LENGTH bits23:8 | **100% software** |
| Smoothing bit24 | **100% mapping** |
| Not Sounding bit25 | **100% mapping** |
| Reserved bit26 | **100% mapping** |
| Aggregation bit27 | **~99% structural** |
| STBC bits29:28 = 00 | **100% path** |
| FEC bit30 = 0/BCC | **100% path** |
| Short GI bit31 | **100% via rate enum** |
| CE4 = HT-SIG[31:0] | **~99–100%** |
| CRC/tail generated by hardware | **strong inference** |

---

### 36CN. QPSK/QAM TX — closure of `CDC` DMA/control and `CE8` Duration/ID — v0.75

Versions v0.71–v0.74 had closed:

```text
0x3FF20CE0 → PLCP1 / PPDU control
0x3FF20CE4 → HT-SIG[31:0]
```

Two hardware stores from `lmacSetTxFrame()` remained:

```text
0x3FF20CDC ← a12
0x3FF20CE8 ← a9
```

This pass fully traces the provenance of the useful fields.

---

#### 36CN.1 The four exact stores of `lmacSetTxFrame()`

In the exact section:

```text
+0x1D7  S32I a12, a0, 0x2DC
+0x1DD  S32I a9,  a0, 0x2E8
+0x200  S32I a13, a0, 0x2E0
+0x23C  S32I a10, a0, 0x2E4
```

with:

```text
a0 = 0x3FF20A00
```

we obtain:

```text
0x3FF20CDC ← a12
0x3FF20CE8 ← a9
0x3FF20CE0 ← a13
0x3FF20CE4 ← a10
```

The last two were already closed in previous versions.

---

#### 36CN.2 Provenance of `CDC`: `esf_buf_s.ds_head`

At the beginning of the function, the current buffer pointer is retained as:

```text
eb
```

The DWARF of:

```text
struct esf_buf_s
```

closes:

```text
offset +4  → ds_head
type       → lldesc_t *
offset +16 → buf_begin
offset +20 → hdr_len
offset +22 → data_len
offset +36 → desc
```

In the flow:

```text
L32I.N a0, a0, 4
```

therefore loads:

```text
a0 = eb->ds_head
```

Then:

```text
L32R a6, 0x0003FFFF
AND  a6, a0, a6
```

gives exactly:

```text
dma_low18 = ((uintptr_t)eb->ds_head) & 0x0003FFFF
```

---

#### 36CN.3 The `lldesc_t` type is explicitly a DMA descriptor

The DWARF describes:

```text
struct lldesc_s
size = 12 bytes
```

with fields:

```text
size    : 12 bits
length  : 12 bits
offset  : 5 bits
sosf    : 1 bit
eof     : 1 bit
owner   : 1 bit
buf     : pointer
next/qe : pointer/queue link
```

This layout is that of a **linked-list DMA descriptor**.

The low field of `0x3FF20CDC` therefore indeed receives provenance information from the TX DMA chain.

---

#### 36CN.4 Construction of the upper `CDC` bits

The word is not, however, a raw pointer.

Immediately before the store, the code combines:

```text
dma_low18
```

with several fields:

```text
bit22       → conditional
bits26:24   → value 1 or 2 depending on TX state
bit27       → conditional
bit28       → conditional
```

through a series of:

```text
OR
```

operations with the exact literals:

```text
0x00400000
0x08000000
0x10000000
```

and a 3-bit field shifted by 24.

The best working name is therefore:

```text
0x3FF20CDC = TX DMA descriptor / control word
```

and not:

```text
raw DMA pointer
```

---

#### 36CN.5 Why this strongly resembles PLCP0/DMA control

On later Espressif MACs publicly reverse-engineered, the first TX-preparation word
named `PLCP0` contains precisely the low bits of the `dma_item` address.

This cross-check is architectural, not a claim of identical layout between chips.

For the exact ESP8266, we independently demonstrate:

```text
CDC low18 = low bits of lldesc_t* address
CDC upper = TX control flags
```

The classification:

```text
PLCP0-like DMA/control
```

is therefore very strongly supported.

---

#### 36CN.6 Exact provenance of `CE8`

The same `eb` provides:

```text
eb->buf_begin
```

at offset:

```text
+16
```

The code then performs:

```text
L16UI a9, a6, 2
SLLI  a9, a9, 16
...
S32I  a9, WDEV, 0x2E8
```

where:

```text
a6 = eb->buf_begin
```

Therefore:

```c
uint16_t v = *(uint16_t *)(eb->buf_begin + 2);
REG32(0x3FF20CE8) = ((uint32_t)v) << 16;
```

---

#### 36CN.7 `buf_begin + 2` = `ieee80211_frame.i_dur`

The DWARF contains:

```text
struct ieee80211_frame
size = 24
```

with:

```text
offset +0 → i_fc[2]
offset +2 → i_dur[2]
offset +4 → i_addr1[6]
...
```

Consequently:

```text
*(uint16_t *)(eb->buf_begin + 2)
```

is exactly the:

```text
Duration/ID
```

of the 802.11 header.

The layout of `CE8` becomes:

```text
31                           16 15                         0
┌──────────────────────────────┬────────────────────────────┐
│        Duration / ID         │             0              │
│           16 bits            │          16 bits           │
└──────────────────────────────┴────────────────────────────┘
```

that is:

```text
CE8[31:16] = ieee80211 Duration/ID
CE8[15:0]  = 0
```

in this path.

---

#### 36CN.8 Corroboration with later Espressif MACs

Later Espressif ROMs export around `lmacSetTxFrame()`:

```text
mac_tx_set_duration
mac_tx_set_htsig
mac_tx_set_plcp0
mac_tx_set_plcp1
mac_tx_set_plcp2
```

and the ESP32 Open MAC project separately documents:

```text
PLCP0    → DMA_ADDR
PLCP1    → LEN/RATE/HT
PLCP2    → other control
DURATION → Duration
```

The ESP8266 does not necessarily have the same offsets or field widths.

But the general structure remarkably cross-checks the bank we have just closed.

---

#### 36CN.9 Final functional map of the bank

The best model becomes:

```text
0x3FF20CDC
    TX DMA descriptor / control
    low18 = eb->ds_head & 0x3FFFF
    upper = TX flags

0x3FF20CE0
    PLCP1 / PPDU control
    LENGTH | RATE | KID | HT

0x3FF20CE4
    HT-SIG[31:0]
    MCS | HT-LENGTH | AGG | GI | ...

0x3FF20CE8
    Duration/ID control
    Duration/ID in bits31:16
```

This is the first coherent map of this WDEV TX bank obtained entirely from the exact corpus.

---

#### 36CN.10 Decisive consequence for proprietary QAM

This bank is **not** a constellation port.

It carries:

```text
DMA source
PPDU/PLCP configuration
HT configuration
Duration/ID
```

The DATA stream is therefore supplied as bytes through DMA descriptors, then processed by the PHY:

```text
DMA bytes
   ↓
scrambler
   ↓
FEC
   ↓
interleaver
   ↓
BPSK/QPSK/QAM mapper
   ↓
OFDM/IQ
```

None of the four words:

```text
CDC
CE0
CE4
CE8
```

provides:

```text
I
Q
constellation index
direct coded-bit input
```

---

#### 36CN.11 New research boundary

The search for proprietary QAM must now leave this bank.

The candidates become:

```text
1. digital-baseband register/test mode
2. scrambler bypass
3. FEC bypass
4. interleaver bypass
5. coded-bit injection
6. mapper-test / constellation-test
7. factory/ATE paths unused by the standard SDK
```

Another possibility is that none of these bypasses is exposed to the CPU.

In that case, proprietary QAM must either:

```text
- repurpose the 802.11 pipeline itself
```

or:

```text
- return to the TXIQ/tone path for vector synthesis
```

---

#### 36CN.12 v0.75 status

| Item | Status |
|---|---:|
| `CDC` low18 = `ds_head` | **100% DWARF + flow** |
| `ds_head` type `lldesc_t *` | **100% DWARF** |
| `CDC` = DMA/control word | **~99% functional** |
| `CE8[31:16]` = Duration/ID | **100% DWARF + flow** |
| `CE8[15:0]` = 0 in this path | **100% software** |
| CDC/CE0/CE4/CE8 bank functionally closed | **~99%** |
| constellation point in this bank | **no** |
| DATA source = linked DMA descriptors | **100% software architecture** |
| mapper bypass through this bank | **not identified / not exposed by closed fields** |

---


### 36CO. QPSK/QAM TX — `tx_cont_*` audit: continuous mode, no mapper bypass — v0.76

After closing the bank:

```text
DMA/control → PLCP1 → HT-SIG → Duration
```

the next natural path was the mode:

```text
tx_cont_en
tx_cont_dis
tx_cont_cfg
```

because RF test modes can sometimes bypass part of the modem.

In the analyzed ESP8266 corpus, that is not the case at the observable software level.

---

#### 36CO.1 `tx_cont_cfg()` takes only a selector

The exact symbol is:

```text
tx_cont_cfg
offset  = 0x2D70
size    = 24 bytes
```

The flow is essentially:

```c
void tx_cont_cfg(int enable)
{
    if (enable == 1)
        tx_cont_en();
    else
        tx_cont_dis();
}
```

No other argument is passed.

There is therefore no input for:

```text
rate
payload
coded bits
constellation index
I
Q
```

in this wrapper.

---

#### 36CO.2 `tx_cont_en()` saves a PHY test state

The exact symbol is:

```text
tx_cont_en
offset = 0x2C74
size = 158 bytes
```

Before modifying the mode, the function saves the current values of registers:

```text
0x60000594
0x60000598
0x6000059C
```

into PHY backup globals.

These registers are already classified in the:

```text
PBUS / PHY test / continuous-TX
```

block and are distinct from the tone slots:

```text
0x600005B8
0x600005BC
0x600005C4
```

as well as from the WDEV TX bank:

```text
0x3FF20CDC
0x3FF20CE0
0x3FF20CE4
0x3FF20CE8
```

---

#### 36CO.3 Exact continuous-mode transformations

After RF/PBUS preparation, the code applies:

```c
REG32(0x6000059C) |= 0x0FE03F80u;
REG32(0x60000598) |= 0x0FFFFFFFu;
REG32(0x60000594) &= 0xFFCFFFFFu;
```

The last operation therefore clears:

```text
0x00300000
```

that is:

```text
bits20 and 21
```

of `0x60000594`.

The accesses are serialized with:

```text
MEMW
```

like other critical PHY MMIO operations.

---

#### 36CO.4 `tx_cont_dis()` is the inverse restoration

`tx_cont_dis()`:

```text
offset = 0x2D20
size = 79 bytes
```

restores the three previously saved values into:

```text
0x60000594
0x60000598
0x6000059C
```

then clears the continuous-TX software-state flag.

This confirms that the mechanism is a **test-configuration overlay** around an existing
PHY state.

It does not replace the DATA pipeline with a second programmable symbol engine.

---

#### 36CO.5 No access to the DMA/PLCP path closed in v0.75

In the three functions:

```text
tx_cont_en()
tx_cont_dis()
tx_cont_cfg()
```

there is no access to:

```text
0x3FF20CDC  DMA/control
0x3FF20CE0  PLCP1/PPDU
0x3FF20CE4  HT-SIG
0x3FF20CE8  Duration/ID
```

No:

```text
lldesc_t *
```

pointer is received or reconstructed.

No `rate` is received.

Therefore continuous mode does not provide a second CPU source of QAM symbols.

---

#### 36CO.6 Cross-check with Espressif factory documentation

ESP8266 Factory Test documentation exposes three separate commands:

```text
tx_contin_en <0|1>
esp_tx <channel> <rate> <attenuation>
wifiscwout <enable> <channel> <attenuation>
```

It describes:

```text
tx_contin_en 1
    → continuous packet transmission
      with approximately 92% duty cycle

tx_contin_en 0
    → test mode used with iqview

esp_tx
    → separate selection of channel, rate and attenuation

wifiscwout
    → single-carrier
```

This public separation exactly matches the corpus architecture:

```text
continuous mode   ≠ rate selection
continuous mode   ≠ single-carrier tone
```

The public documentation does not prove that its command directly calls our symbol
`tx_cont_cfg`, but it strongly corroborates the same functional separation.

---

#### 36CO.7 Consequence for QAM

The path:

```text
tx_cont mode
    ↓
encoder/interleaver bypass
    ↓
direct mapper
```

is not supported.

Continuous mode may modify:

```text
timing / gating / test state / duty
```

but packet modulation continues to be selected by the path:

```text
rate → PLCP/HT-SIG → PHY
```

and data continues to come from the standard DMA chain.

---

#### 36CO.8 What `iqview` does not allow us to conclude

The public term:

```text
iqview test mode
```

is a mode intended for RF instruments.

It does not mean:

```text
CPU access to raw I/Q
```

and proves no interface:

```text
I[n], Q[n]
```

or:

```text
constellation index
```

accessible to the LX106.

No such stream is visible in `tx_cont_*` code.

---

#### 36CO.9 State of bypass candidates after v0.76

| Candidate | Result |
|---|---|
| CDC/CE0/CE4/CE8 bank | DMA/PPDU parameters, **not a direct mapper** |
| `tx_cont_*` | continuous/test mode, **no symbol injection** |
| TXIQ tone | vector stimuli, complete signs not exposed |
| single-carrier | RF tone/test, not QAM |
| native OFDM mapper | exists, proprietary input still hidden |
| coded-bit bypass | **not identified** |
| FEC/interleaver bypass | **not identified** |

---

#### 36CO.10 Remaining boundary

The search must now target functions or registers genuinely located between:

```text
DMA payload bytes
        ↓
scrambler
        ↓
convolutional encoder / puncturing
        ↓
interleaver
        ↓
QAM mapper
```

and not modes that only change:

```text
duty
RF continuous state
tone
PLCP metadata
```

---

#### 36CO.11 Public cross-check sources

- Espressif, **ESP8266 RTOS SDK — Factory Test**:
  description of `tx_contin_en`, `esp_tx`, `wifiscwout`.
- Espressif, **ESP8266 Wi-Fi Non-Signaling Test**:
  separation of `TX packet`, `TX continues`, `TX tone`.

---

### 36CP. QPSK/QAM TX — raw `freedom` and fixed-rate converge on the standard pipeline — v0.77

After the audit of:

```text
PLCP/PPDU
HT-SIG
DMA/control
continuous-TX
```

one particularly important public path remained to be closed:

```text
wifi_send_pkt_freedom()
```

This API allows transmission of user-constructed 802.11 frames.

The question was:

```text
does "raw packet" mean "raw PHY"?
```

For the analyzed corpus, the answer is clearly **no**.

---

#### 36CP.1 Exact ABI of `ieee80211_freedom_output()`

The DWARF of `ieee80211_output.o` describes:

```text
ieee80211_freedom_output
    conn
    outbuf
    buflen
    sys_seq
```

The four parameters are:

```text
connection/context
byte buffer
length
sequence-number handling
```

There is no parameter for:

```text
rate
MCS
coded bits
interleaver state
constellation index
I
Q
```

in this function's ABI.

---

#### 36CP.2 Direct call to `ppTxPkt()`

The exact relocations of:

```text
.rela.text.ieee80211_freedom_output
```

contain:

```text
+0x219 → ppTxPkt
```

Earlier in the same path are:

```text
ieee80211_getmgtframe
ets_memcpy
```

which corresponds to creating an `esf_buf_s`, copying the user frame, then
injecting it into the normal PP engine.

The freedom primitive therefore does not go directly to the digital baseband.

---

#### 36CP.3 Exact ABI of `ppTxPkt()`

The DWARF of `pp.o` closes:

```text
ppTxPkt(eb)
```

with a single argument:

```text
eb : esf_buf_s *
```

It does not receive:

```text
raw constellation
QAM point
coded-bit stream
I/Q buffer
```

The payload is represented in the same `esf_buf_s` form as normal transmissions.

---

#### 36CP.4 `ppTxPkt()` reuses rate control

Within the exact `ppTxPkt()` range:

```text
0x8EC .. 0xA61
```

the relocations demonstrate a call to:

```text
rcGetSched()
```

at the functional offset:

```text
approximately +0x5A / .irom0.text 0x946
```

then the buffer is mapped/enqueued into the PP queues.

Rate selection is therefore not replaced by a "raw" property.

---

#### 36CP.5 `ppProcessTxQ()` calls `lmacTxFrame()`

The relocation of:

```text
.text.ppProcessTxQ
```

demonstrates:

```text
+0x83 → lmacTxFrame
```

and its DWARF closes its main argument as the selected TX buffer.

The complete chain becomes:

```text
ieee80211_freedom_output
        ↓
ppTxPkt
        ↓
rcGetSched
        ↓
PP TX queue
        ↓
ppProcessTxQ
        ↓
lmacTxFrame
        ↓
lmacSetTxFrame
        ↓
CDC / CE0 / CE4 / CE8
        ↓
standard PHY
```

---

#### 36CP.6 Cross-check with the public `wifi_send_pkt_freedom` API

Espressif documentation states that:

```text
wifi_send_pkt_freedom()
```

sends a:

```text
user-defined 802.11 packet
```

without an FCS supplied by the user.

It also states that the transmit rate remains that of the system/management mechanism
for this API.

This exactly matches the binary flow:

```text
user frame bytes
    ↓
normal rate control
    ↓
standard TX pipeline
```

---

#### 36CP.7 Fixed rate: separate control of the native constellation

The SDK exposes:

```c
wifi_set_user_fixed_rate(enable_mask, rate)
```

with the codes:

```text
0x08  48 Mb/s
0x09  24 Mb/s
0x0A  12 Mb/s
0x0B   6 Mb/s
0x0C  54 Mb/s
0x0D  36 Mb/s
0x0E  18 Mb/s
0x0F   9 Mb/s
```

v0.72 closed their mapping:

```text
0x0B/0x0F → BPSK
0x0A/0x0E → QPSK
0x09/0x0D → 16-QAM
0x08/0x0C → 64-QAM
```

The SDK therefore genuinely allows selection/fixing of a **native constellation** by choosing
a compatible rate.

---

#### 36CP.8 Internal corroboration of rate control

The exact corpus contains:

```text
set_rate_limit()
set_max_fixed_rate()
clean_rate_set()
rc_set_rate_limit_id()
```

and the globals:

```text
max_11b_rate
max_11g_rate
max_11n_rate
```

The relocations of `set_rate_limit()` demonstrate multiple calls to:

```text
rc_set_rate_limit_id()
```

The notion of rate limiting/fixing is therefore implemented in the internal
rate-control engine, not as a parallel PHY mode.

---

#### 36CP.9 Important conceptual separation

The ESP8266 offers two distinct controls:

```text
A. choose the frame bytes
   → freedom/raw 802.11

B. choose/fix the rate
   → native QPSK / 16-QAM / 64-QAM
```

But the hardware then combines these two pieces of information in the same pipeline:

```text
payload bytes
    +
rate
    ↓
DMA / PLCP
    ↓
scrambler
    ↓
FEC
    ↓
interleaver
    ↓
native mapper
```

No third public or statically identified parameter exists for:

```text
"here are my coded bits"
```

or:

```text
"here is my I/Q point"
```

---

#### 36CP.10 What "raw" means here

The best terminology becomes:

```text
raw MAC frame injection
```

and not:

```text
raw PHY symbol injection
```

The user chooses the 802.11 frame, but the PHY retains responsibility for:

```text
PLCP
scrambling
FEC
interleaving
constellation mapping
OFDM
```

---

#### 36CP.11 Practical consequence for the QAM project

To transmit using native QAM:

```text
YES:
    select a QPSK/16-QAM/64-QAM rate
    provide an 802.11 frame
    let the PHY map the bits
```

To transmit proprietary QAM where software chooses every point:

```text
NOT WITH:
    freedom_output
    fixed_rate
    tx_cont
    PLCP registers
```

It is still necessary to find:

```text
coded-bit bypass
mapper test mode
constellation-index injection
or alternative TXIQ vector synthesis
```

---

#### 36CP.12 v0.77 status

| Item | Status |
|---|---:|
| freedom ABI without rate/IQ | **100% DWARF** |
| freedom → `ppTxPkt` | **100% relocation** |
| `ppTxPkt` → `rcGetSched` | **100% relocation** |
| TX queue → `lmacTxFrame` | **100% relocation** |
| raw frame uses standard pipeline | **~99% architecture** |
| native fixed-rate API | **official Espressif** |
| fixed-rate = rate-control control | **very high confidence** |
| fixed native QAM selectable | **YES** |
| raw PHY / coded-bit injection through freedom | **NO** |
| constellation index through fixed-rate | **NO** |

---


### 36CQ. QPSK/QAM TX — exhaustive audit of named mapper APIs — v0.78

After closing the paths:

```text
raw frame
fixed rate
PLCP/HT-SIG
DMA
continuous-TX
tone/TXIQ
```

it remained to verify whether a lower-level API was simply present in the corpus but had
not yet been noticed by name.

This pass performs an exhaustive symbolic audit.

---

#### 36CQ.1 Audited scope

All extracted objects were scanned:

```text
libphy/*.o
libpp/*.o
libnet80211/*.o
```

The ELF symbol tables were searched for the families:

```text
scram*
interleav*
fec*
mapper*
qam*
qpsk*
punct*
convolution*
encoder*
coded-bit*
constell*
```

Result:

```text
libphy : 0 relevant hits
libpp  : 0 relevant hits
libnet : 0 relevant hits
```

This absence concerns the **symbol names** present in the exact corpus.

---

#### 36CQ.2 What the corpus does name around the TX PHY

Conversely, the corpus explicitly exposes functions related to:

```text
rate control
lmac
PLCP/HT preparation
tone generator
TXIQ calibration
power control
RFPLL
continuous TX
PBUS
MAC enable/disable
```

This shows that debug symbols and exports are not completely stripped:
several important PHY layers do retain functional names.

The complete absence of terms related to encoder/interleaver/mapper is therefore meaningful.

---

#### 36CQ.3 Audit of the official ROM linker

The official Espressif ROM linker:

```text
eagle.rom.addr.v6.ld
```

exports, among others:

```text
phy_get_romfuncs
rom_start_tx_tone
rom_stop_tx_tone
rom_txtone_linear_pwr
rom_rfcal_txiq
rom_rfcal_txiq_cover
rom_rfcal_txiq_set_reg
rom_set_txiq_cal
rom_tx_mac_disable
rom_tx_mac_enable
rom_set_txclk_en
rom_set_txbb_atten
rom_write_rfpll_sdm
...
```

but no named primitive for:

```text
scrambler
interleaver
FEC encoder
puncturer
QAM mapper
constellation mapper
coded-bit input
```

The lexical test on the official file finds no:

```text
scram
interleav
fec
mapper
```

---

#### 36CQ.4 Consequence: the mapper is behind an unexported hardware boundary

The known pipeline is:

```text
CPU
 ↓
DMA / PPDU metadata
 ↓
MAC/PHY hardware
 ↓
scrambler / FEC / interleaver / mapper
 ↓
OFDM/IQ
```

The internal stages:

```text
scrambler
encoder
puncturing
interleaving
constellation mapping
```

therefore have, on the currently known surface, no individually exported CPU API.

The hardware obviously can implement them, because Wi-Fi works.

The conclusion concerns only their **named software accessibility**.

---

#### 36CQ.5 Audit of the public factory-test surface

Espressif factory-test documentation exposes:

```text
rftest_init

tx_contin_en(mode)

esp_tx(channel, rate, attenuation)

esp_rx(channel, rate)

wifiscwout(enable, channel, attenuation)

cmdstop
```

For `esp_tx`, the only modulation parameter is indirectly carried by:

```text
rate
```

There is no public parameter for:

```text
scrambler enable
FEC enable/bypass
interleaver bypass
coded bits
symbol index
I/Q
constellation point
```

---

#### 36CQ.6 `esp_tx` remains Wi-Fi packet transmission

The documentation explicitly describes:

```text
esp_tx → start transmitting Wi-Fi packets
```

with:

```text
channel
rate
power attenuation
```

The separation from:

```text
wifiscwout → single carrier
```

and:

```text
tx_contin_en → test/continuous mode
```

once again confirms that the public factory-test surface does not expose an arbitrary mapper.

---

#### 36CQ.7 What this closure rules out

We can now rule out as a priority path:

```text
"there is probably already an SDK function called
  set_qam_symbol() / mapper_bypass() / disable_fec()"
```

Nothing of the sort appears in:

```text
exact SDK objects
official ROM exports
public factory-test surface
```

---

#### 36CQ.8 What this closure does NOT rule out

Three possibilities remain open:

##### A. Anonymous MMIO register

The mapper may have test bits in an unnamed register:

```text
CPU → hidden MMIO → mapper/test path
```

##### B. Non-exported function in a different factory library

A separate binary ATE/factory library may contain functions absent from the supplied corpus.

##### C. No CPU bypass

The mapper may be fully hard-wired behind the PHY sequencer, with no accessible arbitrary input.

---

#### 36CQ.9 Impact on strategy

Static work must now stop looking for **obvious names**.

The useful strategy becomes:

```text
1. inventory MMIO changed by modulation/rate changes
2. compare BPSK ↔ QPSK ↔ 16-QAM ↔ 64-QAM
3. identify unexplained digital-baseband registers
4. search test sequences in a real librftest/ATE if obtained
5. otherwise dynamically characterize candidate registers on silicon
```

---

#### 36CQ.10 Impact on proprietary QPSK/QAM

The current software verdict becomes more precise:

```text
native Wi-Fi QAM
    → YES, fully selectable

proprietary QAM through an existing named API
    → NOT found / named surface closed negatively

proprietary QAM through hidden MMIO
    → still open

proprietary QAM through vector TXIQ
    → still partially open
```

---

#### 36CQ.11 v0.78 status

| Item | Status |
|---|---:|
| mapper/FEC/interleaver symbols in corpus | **none** |
| ROM mapper/FEC/interleaver exports | **none** |
| factory coded-bit/constellation API | **none documented** |
| native hardware QAM mapper | **100% exists** |
| named mapper-bypass API | **closed negatively** |
| hidden bypass MMIO | **open** |
| unsupplied ATE library with bypass | **possible, not demonstrated** |
| need for MMIO/differential research | **strong** |

---

### 36CR. QPSK/QAM TX — legacy rate fully decoded behind a single nibble — v0.79

v0.72 had closed the correspondence:

```text
CE0[15:12] = PLCP RATE
```

but one question remained open:

```text
does the CPU also program, elsewhere in lmacSetTxFrame(),
a separate modulation/FEC register according to rate?
```

The exact disassembly now answers: **no for the legacy path**.

---

#### 36CR.1 Rate load

In `.text.lmacSetTxFrame`:

```text
+0x1E3  L8UI  a2, a8, 8
```

with:

```text
a8 = esf_tx_desc_s *
```

and the DWARF:

```text
esf_tx_desc_s.rate @ +8
```

Therefore:

```text
a2 = desc->rate
```

---

#### 36CR.2 PLCP nibble construction

Immediately afterward:

```text
+0x1E9  EXTUI a13, a2, 0, 4
+0x1EC  SLLI  a13, a13, 12
```

that is:

```c
rate4 = desc->rate & 0x0F;
rate_field = rate4 << 12;
```

This field is combined with:

```text
LENGTH
KID
HT flag
```

then:

```text
+0x200  S32I a13, WDEV, 0x2E0
```

therefore:

```text
REG32(0x3FF20CE0) = plcp1;
```

---

#### 36CR.3 The only structural branch is legacy versus HT

Immediately before the store:

```text
+0x1F2  BGEUI a2, 16, ...
```

The only test is:

```text
rate >= 16 ?
```

It is used to add the:

```text
HT flag
```

to `CE0`.

It does not distinguish:

```text
BPSK
QPSK
16-QAM
64-QAM
```

from one another.

---

#### 36CR.4 Second rate read after CE0

After the store:

```text
+0x203  L8UI a2, a8, 8
+0x209  BGEUI a2, 16, +HT_path
+0x20C  J common_continuation
```

Therefore:

```text
if rate < 16:
    jump directly to the common path

if rate >= 16:
    build CE4 / HT-SIG
```

The legacy path **passes through no other rate-specific logic** in PHY preparation.

---

#### 36CR.5 Consequence for the eight legacy OFDM rates

The eight values:

```text
0x0B / 0x0F → BPSK
0x0A / 0x0E → QPSK
0x09 / 0x0D → 16-QAM
0x08 / 0x0C → 64-QAM
```

are therefore distinguished by the CPU only through:

```text
CE0[15:12]
```

The hardware receives the PLCP RATE code and itself derives:

```text
constellation
coding rate
puncturing
interleaver geometry
mapper behavior
```

according to the standard OFDM mode.

---

#### 36CR.6 No separate CPU `modulation register` in this path

The code does not do:

```text
if QPSK:
    write MOD=QPSK

if 16-QAM:
    write MOD=16QAM

if 64-QAM:
    write MOD=64QAM
```

nor does it separately do:

```text
write FEC_RATE
write PUNCTURE_MODE
write INTERLEAVER_MODE
```

for legacy rates.

The only visible selector is:

```text
RATE nibble
```

---

#### 36CR.7 Hardware implication

The best model becomes:

```text
CE0.RATE
   ↓
internal PHY decoder
   ├─ modulation family
   ├─ coding rate
   ├─ puncturing
   ├─ interleaver parameters
   └─ constellation mapper mode
```

This logic is therefore **more deeply buried in the digital baseband** than the WDEV bank
programmed by the LX106.

---

#### 36CR.8 Consequence for proprietary QAM

This discovery further reduces the probability of a simple bypass through a neighboring register.

To obtain an arbitrary QAM point, one would have to:

```text
either
    bypass the internal RATE decoder

or
    enter after its mapper selection

or
    use a distinct hardware test path
```

None of these paths is exposed in `lmacSetTxFrame()`.

---

#### 36CR.9 Difference from the HT path

For:

```text
rate >= 16
```

the CPU must provide more parameters:

```text
MCS
HT-LENGTH
Aggregation
GI
...
```

in:

```text
0x3FF20CE4
```

But even in this path, it still supplies **PHY parameters**, not QAM symbols.

The mapper remains internal.

---

#### 36CR.10 v0.79 status

| Item | Status |
|---|---:|
| `desc.rate` loaded at +8 | **100%** |
| legacy RATE → CE0[15:12] | **100%** |
| main rate branch = `<16` / `>=16` | **100%** |
| second legacy rate-specific write | **none in lmacSetTxFrame** |
| separate legacy modulation register | **absent from this path** |
| hardware RATE→QAM/FEC decoder | **~99% architecture** |
| mapper behind hardware decoder | **very high confidence** |
| CPU-visible bypass at same level | **no** |

---


### 36CS. QPSK/QAM TX — downstream closure `lmacSetTxFrame → wDev_EnableTransmit` — v0.80

v0.79 demonstrated that at the `lmacSetTxFrame()` level, the only CPU control that distinguishes
the eight legacy OFDM rates is:

```text
0x3FF20CE0[15:12] = PLCP RATE
```

One possibility remained:

```text
after lmacSetTxFrame(),
does a downstream function program a second
modulation/FEC/mapper register?
```

This pass closes the canonical CPU path up to transmission arming.

---

#### 36CS.1 Position of the two calls in `lmacTxFrame()`

The exact relocations of:

```text
.text.lmacTxFrame
```

demonstrate:

```text
+0x134 → lmacSetTxFrame
+0x161 → wDev_EnableTransmit
```

The flow is therefore:

```text
descriptor preparation
    ↓
lmacSetTxFrame(...)
    ↓
a few local MAC operations
    ↓
wDev_EnableTransmit(...)
```

There is no other external call between the two that could translate rate into a
second set of PHY parameters.

---

#### 36CS.2 After `lmacSetTxFrame()`, descriptor rate is no longer read

The disassembly after the call to:

```text
lmacSetTxFrame
```

shows loads from the buffer/context to prepare:

```text
index
AIFS
backoff
```

then the final WDEV call.

There is no read of the byte:

```text
esf_tx_desc_s.rate @ +8
```

in this downstream portion.

The `rate` has therefore completed its CPU path when `lmacSetTxFrame()` writes `CE0`.

---

#### 36CS.3 Exact ABI of `wDev_EnableTransmit()`

The DWARF of `wdev.o` closes:

```c
wDev_EnableTransmit(index, aifs, backoff)
```

with the three parameters:

```text
index
aifs
backoff
```

Their ABI registers are:

```text
a2 = index
a3 = aifs
a4 = backoff
```

No parameter for:

```text
rate
MCS
FEC
modulation
descriptor pointer
payload pointer
constellation index
I
Q
```

is present.

---

#### 36CS.4 `backoff` processing

The beginning of the disassembly performs:

```text
EXTUI a9, a4, 0, 10
```

that is:

```c
backoff10 = backoff & 0x3FF;
```

The function then prepares a WDEV address dependent on `index` and programs the associated
contention/arming state.

The role of `backoff` is therefore explicitly MAC/CSMA, not PHY modulation.

---

#### 36CS.5 Exact WDEV base

The first literal in the section is:

```text
0x3FF20A00
```

which is the same WDEV/MAC base already reconstructed in the project.

The function uses this base to program transmission registers associated with the queue/index.

It does not use the PHY `0x6000xxxx` block to select a constellation.

---

#### 36CS.6 `0xC0000000` control

A second exact literal in the function is:

```text
0xC0000000
```

The code:

```text
reads a WDEV register
ORs 0xC0000000
rewrites the control state
```

in the activation path.

This pattern is compatible with MAC queue arming/start, not with a
QAM/FEC definition.

The individual electrical names of the two high bits are not required for the QAM conclusion.

---

#### 36CS.7 No rate transfer to `wDev_EnableTransmit`

The boundary therefore becomes:

```text
rate
 ↓
lmacSetTxFrame
 ↓
CE0.RATE
 ↓
CPU return
 ↓
wDev_EnableTransmit(index,aifs,backoff)
 ↓
MAC arming
```

There is no:

```text
wDev_EnableTransmit(..., rate, ...)
```

nor:

```text
wDev_EnableTransmit(..., modulation, ...)
```

---

#### 36CS.8 Architectural consequence

Behind:

```text
CE0.RATE
```

the hardware must possess a decoder selecting:

```text
BPSK / QPSK / 16-QAM / 64-QAM
coding rate
puncturing
interleaver geometry
constellation mapper
```

When the MAC is subsequently armed, it consumes the already prepared PHY state.

This architecture cleanly separates:

```text
PHY configuration
        ↓
MAC arming / contention
```

---

#### 36CS.9 Scope of the closure

This pass closes the **canonical CPU path**:

```text
lmacSetTxFrame
        ↓
wDev_EnableTransmit
```

It does not prove that absolutely no hidden test register exists anywhere in the silicon.

It proves that:

> **the standard ESP8266 transmit path programs no second modulation control
> after CE0.RATE before WDEV arming.**

---

#### 36CS.10 Consequence for proprietary QAM

The search for an arbitrary constellation point must no longer target:

```text
lmacTxFrame
wDev_EnableTransmit
AIFS/backoff registers
```

These layers are now closed as:

```text
MAC configuration/arming
```

and not:

```text
mapper input
```

The target remains:

```text
internal digital baseband
or
anonymous test mode
or
alternative vector TXIQ
```

---

#### 36CS.11 v0.80 status

| Item | Status |
|---|---:|
| `lmacTxFrame → lmacSetTxFrame` | **100% relocation** |
| `lmacTxFrame → wDev_EnableTransmit` | **100% relocation** |
| WDEV ABI = index/aifs/backoff | **100% DWARF** |
| backoff masked to 10 bits | **100% instruction-level** |
| rate passed to WDEV Enable | **no** |
| second downstream modulation control | **none in canonical CPU path** |
| CPU modulation boundary = CE0.RATE | **~99–100% architecture** |
| internal mapper behind RATE | **very high confidence** |

---

### 36CT. QPSK/QAM RX — `DC_I/DC_Q` = mean/DC statistic, correlator becomes priority — v0.81

v0.61 had identified:

```text
rom_dc_iq_est(mode,N,out)
    ↓
out[0] = mean I
out[1] = mean Q
```

This primitive was an attractive candidate for an assisted QPSK receiver.

v0.81 now clarifies **what these two outputs actually represent inside the IQ_EST block**.

---

#### 36CT.1 `rom_dc_iq_est()` explicitly computes a mean

The exact disassembly of:

```text
rom_dc_iq_est @ 0x4000615C
```

performs:

```text
iq_est_enable(mode, N)
```

then:

```text
REG32(0x600005DC)
    ↓
arithmetic_shift_right 6
    ↓
signed_divide by (N+1)
    ↓
out[0]
```

and:

```text
REG32(0x600005E0)
    ↓
arithmetic_shift_right 6
    ↓
signed_divide by (N+1)
    ↓
out[1]
```

before:

```text
iq_est_disable()
```

The:

```text
sum / (N+1)
```

behavior is therefore directly demonstrated.

---

#### 36CT.2 The two accumulators are signed

The code uses:

```text
SRAI
```

then:

```text
__divsi3
```

rather than unsigned division.

The quantities can therefore have either sign:

```text
I_DC < 0 / > 0
Q_DC < 0 / > 0
```

which is consistent with vector DC/offset components.

---

#### 36CT.3 `rom_get_corr_power()` reads seven distinct IQ_EST results

At:

```text
rom_get_corr_power @ 0x40006260
```

the MMIO base is the region:

```text
0x60000200
```

The exact reads correspond to:

```text
+0x380 → 0x60000580
+0x384 → 0x60000584
+0x388 → 0x60000588
+0x38C → 0x6000058C

+0x3DC → 0x600005DC
+0x3E0 → 0x600005E0
+0x3E4 → 0x600005E4
```

The same estimator therefore simultaneously exposes:

```text
matrix/correlation
DC I
DC Q
total energy
```

---

#### 36CT.4 Separate processing of the DC component

The code takes the two DC accumulators and computes:

```text
DC_I_scaled = signed_shift(DC_I)
DC_Q_scaled = signed_shift(DC_Q)

dc_power =
      DC_I_scaled²
    + DC_Q_scaled²
```

then applies a normalization and stores this quantity separately in the output structure.

This processing is very strong functional evidence that:

```text
0x600005DC/E0
```

represent the **complex DC/mean component** of the window.

---

#### 36CT.5 The complex correlation comes from other registers

In parallel:

```text
R0 = 0x60000580
R1 = 0x60000584
R2 = 0x60000588
R3 = 0x6000058C
```

are combined as:

```text
X = R0 + R3
Y = R1 - R2
```

then:

```text
corr_power = X² + Y²
```

This pair:

```text
(X,Y)
```

is the construction actually compatible with the two components of a **complex correlation**.

Thus the block explicitly separates:

```text
DC vector
≠
correlation vector
```

---

#### 36CT.6 Mathematical consequence for a balanced QAM constellation

A standard QPSK/QAM constellation is centered around the origin:

```text
E[I_symbol] ≈ 0
E[Q_symbol] ≈ 0
```

over a balanced sequence.

A primitive that computes:

```text
mean(I)
mean(Q)
```

over several symbols therefore tends toward:

```text
(0,0)
```

even when individual symbols are perfectly distinct.

Thus:

> **a large `rom_dc_iq_est()` window cannot be used as a symbol-by-symbol
> QPSK/QAM constellation demodulator.**

---

#### 36CT.7 The `N=0` case remains physically open

The software accepts:

```text
N = 0
```

and then divides by:

```text
N+1 = 1
```

This means only that the CPU path allows a minimum window.

What static analysis does not demonstrate:

```text
- exact physical duration of one N unit
- sampling instant
- tap location in the RX datapath
- relation N=0 → one real RF/baseband I/Q sample
- bandwidth / phase coherence
```

It would therefore be incorrect to turn:

```text
N=0 is accepted
```

into:

```text
N=0 provides an instantaneous QPSK point
```

---

#### 36CT.8 New priority: the correlation vector

The most interesting RX candidate becomes:

```text
X = R0 + R3
Y = R1 - R2
```

with:

```text
R0..R3 = 0x60000580..58C
```

because this pair preserves **correlation phase** information before the SDK reduces it to:

```text
X² + Y²
```

for its calibrations.

If the external signal can be correlated against a useful reference, then:

```text
atan2(Y,X)
```

could conceptually provide a relative angle.

But the correlator's internal reference has not yet been identified.

---

#### 36CT.9 Exact QPSK RX blocker now

The question is no longer generally:

```text
"can I and Q be read?"
```

It becomes:

```text
what is the IQ_EST correlator reference?
and can it track an external signal with coherent phase?
```

Two scenarios:

```text
A. exploitable/coherent reference
   → correlator potentially usable as vector detector

B. purely internal/calibration reference
   → no external constellation accessible through this path
```

---

#### 36CT.10 Impact on RX feasibility

The classification must be corrected:

```text
rom_dc_iq_est as per-symbol vector sampler
    → downgraded

rom_dc_iq_est as DC/mean probe
    → demonstrated

IQ_EST correlation vector
    → main candidate

raw RF I/Q stream
    → still absent
```

Assisted QPSK/QAM RX remains **possible as a research path**, but the DC_I/DC_Q path
is no longer considered an almost-direct constellation output.

---

#### 36CT.11 v0.81 status

| Item | Status |
|---|---:|
| `DC_I/DC_Q` = signed mean | **100% software** |
| division by `N+1` | **100%** |
| `DC_I²+DC_Q²` treated as DC energy | **100% instruction-level** |
| complex correlation from R0..R3 | **~99% structural** |
| `DC_I/Q` = constellation stream | **ruled out as standard interpretation** |
| `N=0` = instantaneous sample | **not demonstrated** |
| R0..R3 for relative angle | **main candidate** |
| correlator reference | **open** |
| QPSK RX via IQ_EST | **still open, more constrained** |

---


### 36CU. QPSK/QAM RX — IQ_EST = integrated statistics, not a symbol port — v0.82

v0.81 corrected the interpretation of the accumulators:

```text
0x600005DC/E0
    → mean / DC I,Q
```

and positioned:

```text
0x60000580..58C
```

as a complex-correlator candidate.

This pass asks whether those four registers already form a constellation output.

The behavior of `ram_rxiq_get_mis()` shows that they primarily belong to an
**I/Q mismatch statistical estimator**.

---

#### 36CU.1 Real sequence in RXIQ calibration

The demonstrated v6 path is:

```text
internal TX tone / calibration loopback
        ↓
iq_est_enable(1, N)
        ↓
wait for hardware DONE
        ↓
ram_rxiq_get_mis(...)
        ↓
iq_est_disable()
```

Therefore the values read by `ram_rxiq_get_mis()` correspond to the **integration window**
configured by IQ_EST.

They are not continuously read sample-by-sample.

---

#### 36CU.2 Exact reads of `ram_rxiq_get_mis()`

The v6 function:

```text
ram_rxiq_get_mis @ 0x1D54
size = 444 bytes
```

loads the PHY base then reads:

```text
0x60000580 = R0
0x60000584 = R1
0x60000588 = R2
0x6000058C = R3
```

with `MEMW`.

Each value is then normalized/shifted before use.

---

#### 36CU.3 Explicit matrix combinations

The reconstructed flow notably computes:

```text
R0 - R3
R0 + R3

R1 + R2
R1 - R2
```

then selects/orients these quantities according to calibration parameters.

This pattern is characteristic of an **I/Q matrix/correlation** intended to measure:

```text
gain mismatch
phase/quadrature mismatch
```

and not of a simple pair:

```text
I_sample
Q_sample
```

---

#### 36CU.4 64-bit correction arithmetic

The relocations of the function demonstrate several calls to:

```text
__muldi3
__divdi3
```

The function builds 64-bit products/ratios from the preceding statistics,
then reduces the results to signed corrections.

This processing is that of a parameter estimator.

It does not resemble a path:

```text
read X
read Y
return symbol
```

---

#### 36CU.5 Final outputs = mismatch corrections

The flow ends by storing signed-byte results in the output structure
of the RXIQ path.

These results are consumed by:

```text
rxiq_cover / rfcal_rxiq
```

to correct receiver imbalances.

The embedded debug string:

```text
rxiq_get_mis: total_pwr=%d, ...
```

confirms the measurement/calibration purpose.

---

#### 36CU.6 Set of IQ_EST output families

After v0.81–0.82, the block is better classified as:

```text
0x600005DC
0x600005E0
    → signed mean/DC statistics

0x600005E4
    → accumulated energy / power

0x60000580..58C
    → I-Q correlation / matrix statistics
```

The block therefore provides:

```text
first order
second order
energy
```

over an integration window.

---

#### 36CU.7 What a true SDR/QAM output would require

For an SDR or a general software QAM demodulator, one would want:

```text
I[0], Q[0]
I[1], Q[1]
I[2], Q[2]
...
```

or at minimum:

```text
I_symbol[k], Q_symbol[k]
```

for every symbol.

No such interface has been identified.

IQ_EST instead provides:

```text
SUM / MEAN / CORRELATION / POWER
```

over a window.

---

#### 36CU.8 Why a short window remains interesting nonetheless

The previous closure does not imply:

```text
IQ_EST is completely useless for QPSK/QAM
```

A short synchronized window may, depending on the physical tap location, produce
symbol-dependent statistics.

In particular, if a correlator has a coherent reference:

```text
(X,Y)
```

may still contain useful relative phase.

But three proofs are missing:

```text
1. exact correlator reference
2. N window → duration/symbol relationship
3. behavior on external signal without calibration loopback
```

---

#### 36CU.9 Consequence for `N=0`

`N=0` remains accepted by the software.

But v0.82 reinforces the caution:

```text
N=0
```

means only:

```text
minimum estimator window
```

and not:

```text
guaranteed access to an I/Q ADC sample
```

The result remains of the IQ_EST block's statistical nature.

---

#### 36CU.10 New QPSK/QAM RX classification

##### Ruled out

```text
DC_I/DC_Q as constellation stream
R0..R3 as raw I/Q FIFO
phy_adc_read_fast as RF I/Q ADC
```

##### Still candidates

```text
short synchronized correlation
CFO + correlator
hardware Wi-Fi demapper
digital-baseband test mode
```

##### Not found

```text
raw I/Q stream
RX constellation index
accessible soft symbols / LLR
mapper/demapper bypass
```

---

#### 36CU.11 Implication for the QAM RX verdict

The hardware obviously knows how to receive/demodulate Wi-Fi QPSK/QAM.

However, for **proprietary QAM**:

```text
IQ_EST alone
```

is no longer considered an almost-closed path.

It is a potential statistical instrument.

The most promising path now becomes:

```text
native hardware demodulator/demapper
        ↓
search for soft/hard-symbol output before MAC decoding
```

or, failing that:

```text
use IQ_EST statistics with a protocol designed around them
```

which would then be an assisted proprietary modulation/detection method, but not classic SDR/QAM.

---

#### 36CU.12 v0.82 status

| Item | Status |
|---|---:|
| R0..R3 read after IQ_EST window | **100% path** |
| R0±R3 / R1±R2 combinations | **100% instruction-level** |
| mismatch calculation via 64-bit mul/div | **100% relocations + flow** |
| IQ_EST = window statistics engine | **~99% functional** |
| R0..R3 = raw I/Q samples | **ruled out** |
| R0..R3 = direct QAM symbols | **not demonstrated / strongly unsupported** |
| short correlation as detector | **open** |
| native demapper as next target | **high priority** |
| classic proprietary QAM RX | **more constrained than in v0.67** |

---

### 36CV. QPSK/QAM RX — standard demapper boundary: bytes + metadata only — v0.83

v0.81–0.82 closed `IQ_EST` as a window-statistics engine, not as a
constellation port. The next target was therefore the **native Wi-Fi demodulator/demapper**:

```text
QPSK/QAM RF
    ↓
OFDM demodulation
    ↓
demapper
    ↓
?
    ↓
CPU
```

This pass closes the visible standard software interface after that block.

---

#### 36CV.1 Exact `RxControl`: 12 bytes

The DWARF of `libpp/wdev` describes:

```text
struct RxControl
size = 12 bytes
```

The named fields are:

```text
rssi
rate
is_group
sig_mode
legacy_length

damatch0
damatch1
bssidmatch0
bssidmatch1

MCS
CWB
HT_length
Smoothing
Not_Sounding
Aggregation
STBC
FEC_CODING
SGI

rxend_state
ampdu_cnt
channel
noise_floor
```

These fields describe:

```text
quality / level
PHY mode
rate / MCS
lengths
HT parameters
RX end state
channel / noise
```

---

#### 36CV.2 What does not exist in `RxControl`

No DWARF member corresponds to:

```text
I
Q
I_symbol
Q_symbol
constellation
symbol_index
softbit
soft_bit
LLR
per-bit confidence
per-symbol EVM
per-symbol phase
```

The standard descriptor is therefore not a soft-demapper output structure.

It is a **received-packet metadata** structure.

---

#### 36CV.3 Exact `esf_buf_s`: 40 bytes

The DWARF of `wdev.o` closes:

```text
struct esf_buf_s
size = 40 bytes
```

with:

```text
+0   pbuf
+4   ds_head
+8   ds_tail
+12  ds_len
+16  buf_begin
+20  hdr_len
+22  data_len
+24  chl_freq_offset
+28  trc
+32  bqentry
+36  desc
```

The structure therefore carries:

```text
buffer / DMA chain
received bytes
lengths
measured CFO
bookkeeping state
descriptor
```

but still no symbol or soft-bit array.

---

#### 36CV.4 CPU payload is already a byte stream

`buf_begin`, `hdr_len`, and `data_len` show that the CPU receives a frame buffer.

The visible architecture is therefore:

```text
RF
 ↓
OFDM demod
 ↓
QPSK/QAM demapper
 ↓
deinterleave / FEC decode
 ↓
frame bytes
 ↓
esf_buf_s.buf_begin
```

and not:

```text
RF
 ↓
I/Q symbols
 ↓
CPU
 ↓
software decoder
```

This distinction is decisive for the proprietary SDR/QAM project.

---

#### 36CV.5 `wDev_ProcessRxSucData()` does not call a soft-symbol export

The exact relocations of:

```text
wDev_ProcessRxSucData()
```

include, among others:

```text
chm_get_current_channel
phy_get_bb_freqoffset
wDev_DiscardFrame
wDev_IndicateFrame
rcUpdateDataRxDone
```

The standard path calls:

```text
phy_get_bb_freqoffset()
```

to complete frequency metadata.

No relocation targets:

```text
phy_get_bb_evm
rom_dc_iq_est
iq_est_enable
soft demapper
LLR
constellation readout
```

in this function.

---

#### 36CV.6 Global audit of `phy_get_bb_evm()`

An audit of all extracted objects from:

```text
libphy
libpp
libnet80211
```

searches relocations to:

```text
phy_get_bb_evm
```

The only recovered call is:

```text
phy_chip_v6_cal.o
    fix_cache_bug()
        ↓
    phy_get_bb_evm()
```

This path has already been reclassified as an init/cache workaround, not as a normal RX EVM export.

Therefore:

> **standard packet RX does not even export the BB EVM metric through its normal WDEV path.**

---

#### 36CV.7 Consequence for the hardware demapper

The hardware necessarily knows how to produce the decisions required for:

```text
BPSK
QPSK
16-QAM
64-QAM
```

and then decode the frame.

But the demonstrated CPU boundary is located **after** those operations.

The best representation is:

```text
              hardware digital baseband
┌───────────────────────────────────────────────┐
│ FFT / equalization                            │
│ derotation                                    │
│ QPSK/QAM demapper                             │
│ deinterleaver                                 │
│ FEC decode                                    │
│ CRC / RX state                                │
└───────────────────────────────────────────────┘
                 │
                 ▼
          bytes + RxControl
                 │
                 ▼
               LX106
```

Intermediate demapper objects are not visible in the standard CPU structures.

---

#### 36CV.8 What this eliminates

The path:

```text
promiscuous RX / RxControl
    ↓
hidden soft symbols
    ↓
software QAM
```

is negatively closed for the exact corpus.

Likewise:

```text
esf_buf_s
    ↓
LLR or constellation index
```

is not supported by the DWARF layout.

---

#### 36CV.9 What this does not eliminate

There may still exist:

```text
1. an anonymous test MMIO providing access to a pre-FEC stage
2. an internal SRAM/FIFO not referenced by the standard SDK
3. a factory/ATE library not present in the corpus
4. a hidden digital-baseband test mode
```

v0.83 does not prove the universal physical nonexistence of such a point.

It closes only:

> **the corpus's standard RX interface as a bytes + metadata output, with no soft-symbol port.**

---

#### 36CV.10 Impact on proprietary QPSK/QAM RX

The standard native path cannot simply be repurposed by reading `RxControl`.

To receive conventional proprietary QAM, one of two scenarios would now be required:

##### Scenario A — find a hidden pre-FEC / pre-demapper tap

```text
baseband
 ↓
soft symbols / constellation
 ↓
CPU
```

##### Scenario B — use the native Wi-Fi pipeline

The signal remains sufficiently Wi-Fi-compatible for:

```text
synchronization + OFDM + demapper + FEC
```

to be performed in hardware, while the project mainly changes the data/protocol above it.

Without one of these scenarios, `RxControl` alone is not sufficient for proprietary QAM modulation.

---

#### 36CV.11 v0.83 status

| Item | Status |
|---|---:|
| `RxControl` size 12 | **100% DWARF** |
| PHY metadata list | **100% DWARF** |
| LLR in `RxControl` | **absent** |
| I/Q in `RxControl` | **absent** |
| constellation index in `RxControl` | **absent** |
| `esf_buf_s` size 40 | **100% DWARF** |
| bytes/pointers/length/CFO in `esf_buf_s` | **100% DWARF** |
| soft-symbol buffer in `esf_buf_s` | **absent** |
| `phy_get_bb_freqoffset` in RX success | **100% relocation** |
| `phy_get_bb_evm` in RX success | **absent** |
| only `phy_get_bb_evm` caller = `fix_cache_bug` | **100% supplied corpus** |
| standard RX interface = bytes + metadata | **~99% functional** |
| hidden pre-FEC tap | **still open** |

---

# APPENDIX F — Provenance and fingerprints

The input files used for this consolidation are:

- `ESP8266_TX_FSK_MFSK_REFERENCE_OFFICIELLE_PROJET_v1.0(2).md` — 21891 bytes — SHA-256 `f623b290519f14fb25b58e881b19103eed6012e6fae3c34ead6bfd54b966b9cc`
- `ESP8266_RX_OOK_ASK_REFERENCE_OFFICIELLE_PROJET_v1.0 (1).md` — 27708 bytes — SHA-256 `97ed0d60f2848029188b0290977ca34c7de0969e88e371c589071f208eb57b8c`
- `ESP8266_RX_FSK_MFSK_REFERENCE_OFFICIELLE_PROJET_v1.0(2).md` — 19158 bytes — SHA-256 `4a2111e8a62e1cac14f017d8ca2cd169aa0c76e6bd916579f1612d75ca9560f9`
- `ESP8266_ASK_OOK_REFERENCE_OFFICIELLE_PROJET_v1.0 (1).md` — 29137 bytes — SHA-256 `a37e29542d3f74715d11bccc4897471ed82d13ef64c1831224dac9fbfd63c4da`
- `ESP8266_WIFI_PHY_REVERSE_ENGINEERING_v0.83_QAM_RX_STANDARD_DEMAPPER_EXPORT_BYTES_METADATA_ONLY(2).md` — 770584 bytes — SHA-256 `e5070386bb78ed86e0c29176a54022400ad87038ade0cd84a7c0dff2afb96bd5`
- `librftest.a` — 83626 bytes — SHA-256 `01c9b9712cd5772823b6b647fa2181c8b594162b5b7091663b8b3bd482400b8e`

# ESP8266 PHY — Detailed ASK / M-ASK Reference

> **Status**: empirical programming reference based on RF measurements, IQ_EST/E4 experiments, and ESP8266 TX/RX benches.  
> **Important**: this is not an official Espressif specification. It documents behavior observed and validated on the test bench.

---

## 1. Practical summary

| Item | Current reference |
|---|---|
| RF band | 2.4 GHz |
| Reference channel | Channel 6, 2.437 GHz |
| Main observed TX register | `0x600005B8` |
| TX GATE | bit 18 |
| ASK / digital scale | bits 17:10 |
| tone_control | bits 9:0 |
| Applied ASK coding | `askCode = (-ASK) mod 256` |
| Clean / monotonic ASK region | mainly `ASK = 0..128` |
| Observed discontinuity | `ASK 128 -> 129` |
| Selected 8-ASK set | `{70, 83, 94, 102, 108, 114, 120, 127}` |
| Bits per symbol | 3 |
| Validated subcarrier | up to **8 kHz** |
| 10 kHz | synchronization failure with the current bench |
| Conservative raw data rate | **100 bit/s** |
| Current demonstrated ceiling | **150 bit/s**, still with errors |
| 200 bit/s | classification failure in the current implementation |
| RX metric | subcarrier correlation / amplitude |
| RX block | IQ_EST / E4 |
| RX gain | fixed gain recommended for reproducible measurements |

---

## 2. ASK TX architecture

### Register `0x600005B8`

| Bits | Name | Function |
|---|---|---|
| 18 | `GATE` | Rapidly enables / disables the TX tone |
| 17:10 | `ASK / digital scale` | Digital amplitude scaling |
| 9:0 | `tone_control` | Raw tone control, used notably for FSK / tone generation |

The ASK field behaves as a **digital amplitude scale code**, not as a direct dB value or percentage.

### Validated software coding

```cpp
uint8_t askCode = (uint8_t)(0u - ask);
```

Then:

```cpp
toneShadow &= ~SCALE_MASK;
toneShadow |= ((uint32_t)askCode << SCALE_SHIFT) & SCALE_MASK;
wr32(0x600005B8, toneShadow);
```

with:

```cpp
SCALE_SHIFT = 10;
SCALE_MASK  = 0x0003FC00;
GATE_MASK   = 0x00040000;
```

---

## 3. APWR — empirical behavior

APWR is not linear across `0..255`.

Empirical model:

```text
APWR = [bits 7:6][bits 5:0]
```

Bits 7:6 select a regime; bits 5:0 provide finer control inside that regime.

| Bits 7:6 | Range | Observed behavior |
|---|---|---|
| `00xxxxxx` | `0x00..0x3F` | about -31.6 dBm, nearly constant |
| `01xxxxxx` | `0x40..0x7F` | progressive attenuation |
| `10xxxxxx` | `0x80..0xBF` | attenuated regime |
| `11xxxxxx` | `0xC0..0xFF` | return to a strong regime |

### APWR reference measurements, ASK = 0

| APWR | Hex | Observed power |
|---:|---:|---:|
| 0 | 0x00 | -31.6 dBm |
| 63 | 0x3F | -31.6 dBm |
| 64 | 0x40 | -30.7 dBm |
| 65 | 0x41 | -31.1 dBm |
| 72 | 0x48 | -32.1 dBm |
| 80 | 0x50 | -33.1 dBm |
| 96 | 0x60 | -36.1 dBm |
| 127 | 0x7F | -43.1 to -44.6 dBm |
| 128 | 0x80 | -43.6 dBm |
| 192 | 0xC0 | -31.7 dBm |
| 193 | 0xC1 | -30.7 dBm |
| 224 | 0xE0 | -31.2 dBm |

### APWR approximation in regime `0x40..0x7F`

With:

```text
F = APWR - 64
```

empirical approximation:

```text
P(dBm) ≈ -31.04 - 0.1125·F - 0.001441·F²
```

This is a bench approximation, not a guaranteed silicon law.

---

## 4. ASK — empirical behavior

### RF measurements at APWR = 64

| ASK | askCode | Measured power | Spectrum |
|---:|---:|---:|---|
| 0 | 0x00 | -30.7 dBm | 1 peak |
| 32 | 0xE0 | -39.3 dBm | 1 peak |
| 64 | 0xC0 | -46.3 dBm | 1 peak |
| 127 | 0x81 | -62.2 dBm | dip |
| 128 | 0x80 | -61.7 dBm | dip |
| 160 | 0x60 | -33.7 dBm | ~8 peaks |
| 192 | 0x40 | -31.7 dBm | multiple peaks |
| 224 | 0x20 | -32.1 dBm | multiple peaks |
| 255 | 0x01 | -32.1 dBm | multiple peaks |

### Useful empirical law

For `0 <= ASK <= 128`, at APWR = 64:

```text
P(dBm) ≈ -30.99 - 0.2429·ASK
```

roughly:

```text
4.1 ASK steps ≈ 1 dB
```

### Exact observed discontinuity

Detailed sweeps showed:

```text
ASK 128 -> weak regime
ASK 129 -> abrupt return to strong regime
```

RX correlation metric:

```text
ASK 128 ≈ 148.7k
ASK 129 ≈ 476.1k
```

about a `x3.2` jump in the RX correlation metric.

This corresponds to:

```text
ASK 128 -> askCode 0x80
ASK 129 -> askCode 0x7F
```

The boundary is therefore strongly associated with the MSB transition in the hardware code.

---

## 5. ASK sweep 0..128 — RX behavior

The RX metric is a subcarrier correlation amplitude.  
**It is not RF power in dBm.**

General observations:

- `ASK 0..~64`: compressed region on RX.
- `ASK ~70..128`: much more useful slope for M-ASK.
- `ASK 127` and `128` are very close.
- `ASK >=129`: return to the strong regime; globally non-monotonic.

Selected measured points:

| ASK | Approx. RX amplitude |
|---:|---:|
| 0 | 491659 |
| 32 | 484999 |
| 64 | 476513 |
| 70 | 456144 |
| 80 | 422497 |
| 96 | 357401 |
| 104 | 307520 |
| 112 | 245819 |
| 120 | 187662 |
| 127 | 146612 |
| 128 | 147767 |

---

## 6. Selected 8-ASK symbol set

The levels were chosen to obtain approximately spaced RX centers.

| Symbol | Bits | ASK |
|---:|---|---:|
| 0 | `000` | 70 |
| 1 | `001` | 83 |
| 2 | `010` | 94 |
| 3 | `011` | 102 |
| 4 | `100` | 108 |
| 5 | `101` | 114 |
| 6 | `110` | 120 |
| 7 | `111` | 127 |

Example calibrated centers from an autonomous run:

| Bits | ASK | RX center |
|---|---:|---:|
| 000 | 70 | 404493 |
| 001 | 83 | 371364 |
| 010 | 94 | 341467 |
| 011 | 102 | 304014 |
| 100 | 108 | 268425 |
| 101 | 114 | 225059 |
| 110 | 120 | 184963 |
| 111 | 127 | 149032 |

These absolute center values must not be treated as universal constants.  
They depend on RF geometry, fixed RX gain, channel, distance, orientation, and subcarrier frequency.

### Recommended decision rule

```text
symbol = argmin_i |A_measured - center_i|
```

where each `center_i` is learned during calibration.

---

## 7. RX — IQ_EST / E4

### Observed registers

| Item | Address |
|---|---:|
| IQ_EST controller | `0x6000057C` |
| E4 result | `0x600005E4` |
| RX control | `0x60009B08` |

### Main IQ_EST fields

| Bit(s) | Function |
|---|---|
| 31 | DONE |
| 18 | MODE |
| 16:2 | N |
| 1 | START |
| 0 | ENABLE |

### Validated measurement sequence

1. RX RF ON.
2. RX clock ON.
3. PBUS manual/debug mode.
4. Fixed RX gain.
5. Configure IQ_EST.
6. `START=0`, wait for `DONE=0`.
7. `START=1`, wait for `DONE=1`.
8. Read E4.
9. Disable / re-arm cleanly before the next measurement.

E4 is a **latched measurement result**, not a continuous RSSI value.

---

## 8. Subcarrier detection

The robust detector uses frequency-selective correlation:

- target frequency `f0`,
- two control frequencies around it,
- local mean removal,
- square-wave I/Q correlation,
- magnitude approximated as `|I| + |Q|`.

Initial 1 kHz example:

```text
target = 1000 Hz
controls = 700 / 1300 Hz
```

The control frequencies are then scaled for higher subcarriers.

### RX integration window

The comparison rule that worked well is:

```text
RX window ≈ 10 subcarrier periods
```

Examples:

| Subcarrier | RX window |
|---:|---:|
| 1 kHz | 10 ms |
| 1.5 kHz | 6.666 ms |
| 2 kHz | 5 ms |
| 2.5 kHz | 4 ms |
| 3 kHz | 3.333 ms |
| 4 kHz | 2.5 ms |
| 5 kHz | 2 ms |
| 6 kHz | 1.666 ms |
| 8 kHz | 1.25 ms |

---

## 9. Subcarrier results — 8-ASK at 100 bit/s raw

Symbol duration fixed at `30 ms`, giving `100 bit/s raw`.

| Subcarrier | RX window | Sent | Correct | Coverage | Accuracy | Sync |
|---:|---:|---:|---:|---:|---:|---:|
| 1 kHz | 10000 us | 320 | 305 | 100% | 95.3% | 89.3% |
| 1.5 kHz | 6666 us | 320 | 314 | 100% | 98.1% | 94.3% |
| 2 kHz | 5000 us | 320 | 320 | 100% | 100.0% | 96.7% |
| 2.5 kHz | 4000 us | 320 | 315 | 100% | 98.4% | 94.0% |
| 3 kHz | 3333 us | 320 | 319 | 100% | 99.7% | 98.9% |
| 4 kHz | 2500 us | 320 | 313 | 100% | 97.8% | 99.7% |
| 5 kHz | 2000 us | 320 | 320 | 100% | 100.0% | 98.9% |
| 6 kHz | 1666 us | 320 | 320 | 100% | 100.0% | 99.2% |
| 8 kHz | 1250 us | 320 | 319 | 100% | 99.7% | 99.4% |

At `10 kHz`, the current bench failed synchronization.  
This documents a limit of the current implementation, not a proven absolute silicon limit.

### Subcarrier conclusion

- **8 kHz is cleanly validated.**
- **10 kHz is not validated in the current implementation.**
- Particularly strong observed range: `2..8 kHz`.

---

## 10. 8-ASK data rate at fixed 8 kHz subcarrier

Corrected RX window: `1250 us`, approximately 10 subcarrier periods.

| Symbol duration | Raw bit rate | Sent | Decisions | Correct | Coverage | Accuracy | Sync |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 30 ms | 100 bit/s | 320 | 320 | 298 | 100% | 93.1% | 98.0% |
| 25 ms | 120 bit/s | 320 | 320 | 265 | 100% | 82.8% | 99.6% |
| 20 ms | 150 bit/s | 320 | 320 | 299 | 100% | 93.4% | 98.5% |
| 15 ms | 200 bit/s | 320 | 320 | 16 | 100% | 5.0% | 94.9% |

### Interpretation

- 100% coverage through 200 bit/s shows that the RX still produces decisions.
- The collapse at 200 bit/s mainly concerns **amplitude classification**.
- The non-monotonic behavior at 100, 120 and 150 bit/s strongly suggests dependence on **RX-window timing / phase alignment**.
- Current demonstrated ceiling: **150 bit/s raw**, but still with about 6.6% symbol errors on this run.
- For a conservative modem setting, **100 bit/s raw** remains the recommended operating point.

---

## 11. RF synchronization

The autonomous benchmark showed that explicit RF synchronization is required.

Working structure:

1. long OFF interval,
2. strong ON marker,
3. ASK calibration,
4. alternating ON/OFF sync train,
5. timing/phase recovery,
6. known payload,
7. local comparison inside the RX ESP8266.

Error counting should be performed **inside the RX**, not in the browser, to avoid USB/WebSerial timing artifacts.

---

## 12. Programming best practices

### TX

- Fix the RF channel.
- Fix APWR.
- Keep ASK inside the selected monotonic region.
- Modify only the ASK bits when changing amplitude.
- Preserve GATE and tone_control.
- Avoid whole-register rewrites without masks.
- Use a stable symbol-to-ASK mapping.
- Include a calibration phase.

### RX

- Use fixed RX gain for reproducible experiments.
- Perform decisions locally on the ESP8266.
- Keep Serial output out of the timing-critical loop.
- Measure multiple windows per symbol when timing allows.
- Reject windows crossing symbol transitions.
- Use median or robust averaging to reject outliers.
- Recalibrate when RF geometry, channel, gain, or subcarrier changes.

---

## 13. What must not be assumed

- `ASK = x` does not mean `x %` power.
- APWR is not linear across 0..255.
- `amp1k` / `amp_f` is not dBm.
- ASK > 128 is not a monotonic continuation.
- Absolute RX centers are not universal.
- A higher subcarrier does not automatically imply a higher symbol rate.
- Good synchronization does not guarantee good ASK classification.
- A short test of a few dozen symbols is not enough to characterize a robust modem.

---

## 14. Recommended reference profile

### Conservative current profile

```text
RF channel      : 6 / 2.437 GHz
Modulation      : 8-ASK
ASK levels      : 70,83,94,102,108,114,120,127
Subcarrier      : 8 kHz
RX window       : 1250 us
Symbol duration : 30 ms
Raw bit rate    : 100 bit/s
Bits/symbol     : 3
Calibration     : mandatory
RX gain         : fixed
Decision        : nearest calibrated center
```

### Experimental faster profile

```text
Subcarrier      : 8 kHz
Symbol duration : 20 ms
Raw bit rate    : 150 bit/s
Observed run    : 299 / 320 correct
Accuracy        : 93.4%
Status          : demonstrated, not yet robust
```

---

## 15. Validation status

| Item | Status |
|---|---|
| GATE bit 18 of `0x600005B8` | validated |
| ASK bits 17:10 | validated |
| askCode = -ASK mod 256 | validated |
| ASK 0..128 region | empirically validated |
| 128 -> 129 discontinuity | empirically validated |
| selected 8-ASK set | validated |
| IQ_EST / E4 RX measurement | validated |
| 1..8 kHz subcarrier | validated on bench |
| 10 kHz | not validated / sync failure |
| 100 bit/s raw | validated conservative point |
| 150 bit/s raw | demonstrated but still error-prone |
| 200 bit/s raw | currently unusable |

---

## 16. Reference statement

> On ESP8266, the PHY ASK field should be treated as a digital amplitude scale, with the useful monotonic region mainly between ASK 0 and 128. M-ASK requires RX calibration against measured amplitude centers. An 8-ASK set `{70,83,94,102,108,114,120,127}` has been validated with subcarrier frequencies up to 8 kHz and a demonstrated raw data rate up to 150 bit/s in the current bench implementation.

# ESP8266 "QAM16" Lab — 16/8-State Envelope-Subcarrier Modem (Diagnostic Bench)

An experimental, runtime-configurable **bench/diagnostic modem** built on the ESP8266
Wi-Fi radio, driven from the browser over **WebSerial**. It does **not** use normal
Wi-Fi: it directly drives the low-level PHY tone generator and the `IQ_EST` / `E4`
energy path documented by the
[`esp8266_WLAN_PHY`](https://github.com/kaboom748/esp8266_WLAN_PHY) reverse-engineering
project.

This folder contains two files:

| File | Role |
|---|---|
| `ESP8266_SUBCARRIER_16STATE_DIAG.ino` | ESP8266 firmware (the modem: TX **and** RX, plus a serial console). Internal name: `ESP8266_QAM16_LAB_DYNAMIC.ino`. |
| `ESP8266_QAM16_WebSerial_V5_4_8STATE_LOCKED.html` | Browser control panel (WebSerial). Drives **two** boards — one TX, one RX — from a single tab. |

---

## Honest summary — what this is, and what it is not

**What it is**

* A **laboratory / diagnostic instrument** to explore how far you can push a custom
  multi-state waveform through the ESP8266 RF front-end and recover it again on a second
  board.
* A **non-coherent, energy-detection** link. The receiver only measures received
  **energy** over a time window (register `E4`); it does not sample raw I/Q.
* A tool for **characterisation and error-rate measurement**, with live plots and a
  copy-paste results panel.

**What it is NOT**

* **Not real QAM, and not APSK.** The name "QAM16" is a legacy label. There is no
  quadrature amplitude modulation of an RF carrier here. The scheme is closer to
  **On-Off-Keying (OOK) of a fixed tone, shaped into a square-wave subcarrier**, whose
  amplitude, phase and frequency carry the bits — recovered by **energy detection**.
  The parent reverse-engineering project establishes that arbitrary per-symbol QAM
  constellation injection is **not** available on this chip (no CPU-visible I/Q TX FIFO,
  no mapper bypass), which is exactly why this design exists in the form it does.
* **Not a production modem.** Timing is soft-real-time, framing is deliberately simple,
  and the "payload" is a known 0..15 test sequence used to measure errors — not
  arbitrary user data.
* **Not calibrated.** Power settings are raw hardware controls, not dBm. Range,
  linearity and spectral behaviour are unknown unless you measure them.
* **Not portable across all silicon.** It calls **undocumented ROM addresses and MMIO
  registers**. It is tied to specific ESP8266 revisions / SDK builds and can break
  silently on others.

---

## ⚠️ Safety, thermal and regulatory warning — read before transmitting

This project directly manipulates low-level PHY/RF registers and can keep the transmit
chain active far more aggressively than ordinary Wi-Fi traffic. The same cautions as the
parent `esp8266_WLAN_PHY` project apply here.

* **Use a shielded/attenuated setup** (Faraday cage or a properly attenuated conducted
  connection). A shield is a risk-control measure, **not** a legal exemption — leakage
  and the real emitted spectrum still matter.
* **Comply with the radio regulations of your country.** "2.4 GHz licence-exempt" does
  **not** mean any waveform, bandwidth, power or frequency is allowed. Non-standard
  continuous emissions can violate the rules even inside the band.
* **Modifying low-level RF firmware can void a module's original certification.** Do not
  assume a certified dev board stays certified once running this firmware.
* **Never use this to jam, block, degrade or deny radio communications.** That is
  illegal in most jurisdictions and is not the purpose of this project.
* **Thermal:** there is **no software temperature protection**. Keep bursts short. If the
  module gets unusually hot, unstable, resets repeatedly, or draws abnormal current,
  **disconnect power immediately and let it cool**.
* **Power:** `TX_APWR` and the `ask` "digital scale" are **raw experimental controls,
  not dBm**. Keep them conservative. A larger number does not mean a safe or linearly
  higher output.

This documentation is general information, **not legal advice**. You are responsible for
your own test setup and for complying with the rules that apply to you.

---

## How it works

### The modulation (the important part)

The ESP8266 emits a **fixed 2.4 GHz tone** (the PHY test-tone generator, on RF channel 6
by default). The information is not in that carrier — it is in **how the tone is gated
on and off**.

* A **chip** is the smallest timing unit. On each chip the tone is either gated **ON**
  or **OFF**.
* A **symbol** spans `symbolchips` chips. The ON/OFF pattern within a symbol forms a
  **square-wave subcarrier**; its **phase**, **frequency** and **amplitude** encode the
  bits.

A **16-state** symbol carries **4 bits**, on three independent axes:

| Bit(s) | Axis | Meaning |
|---|---|---|
| bit 3 | **amplitude** | two "digital-scale" levels `ask0` / `ask1` (an OOK/ASK depth) |
| bits 2–1 | **phase** | `0 / 90 / 180 / 270°` of the subcarrier square wave |
| bit 0 | **frequency** | subcarrier `F0` or `F1` |

Subcarrier frequencies and rates follow directly from the chip clock:

```
F0            = chiprate / period0
F1            = chiprate / period1
symbol rate   = chiprate / symbolchips
raw bit rate  = symbol rate x bits_per_symbol
```

Two concrete operating points:

| | chiprate | symbolchips | period0/1 | F0 / F1 | sym rate | states | raw rate |
|---|---:|---:|---:|---|---:|---:|---:|
| Firmware defaults | 28 000 Hz | 16 | 16 / 8 | 1.75 / 3.5 kHz | 1750 /s | 16 (4 bit) | 7000 bit/s |
| **8-state locked preset** | 3 500 Hz | 32 | 32 / 16 | ~109 / ~219 Hz | ~109 /s | 8 (3 bit) | ~328 bit/s |

> **Why "8-state" is the locked default.** In practice the **amplitude bit is the least
> reliable** axis, because the OOK depth is entangled with phase and frequency. The
> shipped "8-state locked" preset therefore **neutralises amplitude** (`ask0 = ask1`) and
> uses only **phase × frequency = 8 states = 3 bits/symbol**, with a wider subcarrier and
> stricter sync/quality gates for robustness.

### Transmit (one diagnostic frame)

```
[pre-guard] [SYNC preamble] [post-guard] [TRAINING sweeps] [DATA sweeps] [align pad]
```

* **SYNC preamble** — a 13-block pseudo-Barker pattern (`1111100110101`) of ON/OFF
  blocks, giving good autocorrelation so the receiver can find the frame.
* **TRAINING** — `trainreps` full sweeps of all 16 states, so the receiver can measure
  the real energy response of every amplitude/phase/frequency combination and calibrate.
* **DATA** — `datareps` sweeps of the known 0..15 sequence, used to count errors.
* Timing uses a fractional accumulator so the long-term average chip rate stays exact
  even when `1 000 000 / chiprate` is not an integer.

### Receive and decode

The receiver samples the **energy register `E4`** at the chip rate (after enabling the
`IQ_EST` integrator for `iq` samples):

1. **Sync search** — a sliding correlation of recent `E4` samples against the known
   preamble. It passes only if the ON/OFF **contrast**, a **quality** metric
   (contrast/residual) and a **relative** metric (contrast/mean) all clear their
   thresholds (`syncq`, `syncrel`, `syncc`).
2. **Frame capture** — one full frame of `E4` samples is stored.
3. **Gap recovery** — the ESP8266 scheduler can steal CPU. Instead of dropping a frame,
   missed chip slots are **padded** with the last value to preserve the time axis
   (toggle with `recover`, bounded by `maxpad`).
4. **Fine alignment** — an offset search over the align pad finds where the training
   sweep best matches the expected 0..15 phase/frequency order.
5. **Calibration** — per-state reference values (mean contrast, mean ON/OFF, normalised
   contrast) are built from the training sweeps.
6. **Symbol decode** — per data symbol, the best (frequency, phase) shape is correlated;
   the amplitude bit is decided by the selected metric; results are compared to the
   expected symbol and tallied.

Because the raw signal is **scalar energy**, the constellation shown in the UI is
**reconstructed**: the envelope is correlated against 0° and 90° versions of the
subcarrier square wave to produce a pseudo-I / pseudo-Q. It is a useful visualisation,
not a measured I/Q constellation.

**Decode modes (`decode`):**

| Value | Decoder |
|---:|---|
| 0 | legacy amplitude (single contrast reference) |
| 1 | dynamic amplitude — see `ampmode` 0..5 |
| 2 | **template-16** — nearest mean-centred 16-state template (most robust; removes DC drift) |

---

## Hardware requirements

* **Two ESP8266 boards** (e.g. Wemos D1 mini / NodeMCU). One is flashed and used as
  **TX**, the other as **RX**. (You can also drive a single board for open-loop tests,
  but closed-loop error measurement needs two.)
* Both boards connected by **USB** to the **same computer**.
* A **WebSerial-capable browser**: Chrome, Edge, or another Chromium-based browser.
  Firefox and Safari do not support WebSerial.
* The page must run in a **secure context**: open it from `localhost` or over `https://`
  (a `file://` path will not expose serial). See "Serving the interface" below.
* A **shielded / attenuated RF setup** (see the safety section).

---

## Getting started

### 1. Flash the firmware

Open `ESP8266_SUBCARRIER_16STATE_DIAG.ino` in the Arduino IDE (with the ESP8266 board
package installed) or `arduino-cli`, select your board, and flash it to **both** ESP8266
modules. The serial console runs at **115200 baud**.

You can sanity-check the firmware on the console alone: connect a serial terminal, type
`rx` then Enter, then `txframe` — but the intended workflow is the WebSerial UI below.

### 2. Serve the interface

Because WebSerial needs a secure context, serve the HTML from `localhost` rather than
opening it directly. For example, from this folder:

```bash
python3 -m http.server 8000
# then open http://localhost:8000/ESP8266_QAM16_WebSerial_V5_4_8STATE_LOCKED.html
```

Any static file server works. Hosting it over `https://` is also fine.

### 3. Connect the two boards

In the page:

* Click **Connecter TX** and pick the serial port of the board you will use as
  transmitter.
* Click **Connecter RX** and pick the other board's port.

The pills turn green when each side is connected.

### 4. Apply a preset and run a test

* Click **Preset 8-états verrouillé** (the recommended locked default). With both boards
  connected it applies the configuration automatically.
* Press **▶ Lancer** to run the default script. It stops any auto-TX, reads stats, and
  (with auto-apply enabled) pushes the current config first.
* Watch the **Constellation** (green = correctly decoded, red = error), the **amplitude
  analysis** panel, and the **Résultat** log.

### 5. Read and share results

Press **📋 Copier résultat** to copy the filtered log (the noisy `EVT,POINT`, `EVT,CFG`
and per-symbol `SYM` lines are removed), and **📋 Copier analyse ampli** for the
amplitude-separability summary.

---

## The interface in detail

### Dynamic settings

A grid of inputs mapped to firmware `set` commands. Settings split into two groups:

* **Shared (sent to BOTH boards)** — the PHY and frame structure must match on both ends:
  `chiprate`, `symbolchips`, `period0`, `period1`, `ask0`, `ask1`, `syncblock`,
  `syncguard`, `preguard`, `alignpad`, `trainreps`, `datareps`.
* **RX-only (detection / decoding)** — these only affect the receiver:
  `iq`, `decode`, `ampmode`, `ampmix`, `alignmin`, `alignmax`, `framematch`, `frameq`,
  `syncstride`, `recover`, `maxpad`, `syncq`, `syncrel`, `syncc`, `errprint`
  (the UI also forces `telemetry 1` and `verbose 0` on RX so the plots work and the log
  stays light).

**Appliquer au modem** sends the current grid; **Lire config** asks each board to print
its live configuration; **auto-appliquer avant script** re-applies the grid before each
run. Each `set` is confirmed with an ACK handshake (the UI waits for the echoed command
and an `OK` line).

### Presets

| Preset | Purpose | Notable values |
|---|---|---|
| **Preset 8-états verrouillé** (default) | Robust phase×frequency link; amplitude disabled | `chiprate 3500`, `symbolchips 32`, `period0/1 32/16`, `ask0=ask1=40`, `iq 64`, `decode 1`, `ampmode 5`, `framematch 14`, `syncq 1200`, `alignmax 48`, `trainreps 2` |
| **Preset 3.5K verrouillé** | 16-state link at 3.5 kHz chip rate | `chiprate 3500`, `symbolchips 16`, `period0/1 8/4`, `ask0/1 8/40`, `iq 16`, `decode 1`, `ampmode 5`, `syncq 640`, `alignmin/max 5/16` |
| **Preset 14K ancien** | Older baseline, template decoder, gap recovery on | `chiprate 14000`, `symbolchips 32`, `ask0/1 0/40`, `iq 32`, `decode 2`, `syncblock 32`, `recover on`, `syncc 10000` |

### Test script

A small line-based runner. Each line is dispatched to a side:

```
# comments start with '#'
TX> txframe          # send one command to the TX board
RX> stats            # send one command to the RX board
BOTH> set iq 64      # send to both boards
WAIT 500             # pause 500 ms (SLEEP also works)
```

The default script is:

```
TX> auto 0
RX> stats
TX> stats
```

**Arrêter script** aborts the run; **■ STOP TX** sends `auto 0` to stop repeated
transmission.

### Constellation, amplitude analysis, results

* **Constellation** — two panels (F0 and F1). Points are the reconstructed pseudo-I/Q
  from `EVT,POINT`; green when the decoded symbol equals the expected one, red otherwise.
  Drawing is throttled and sub-sampled for performance.
* **Analyse amplitude** — groups every decoded point by (true amplitude bit, frequency)
  and reports mean/min/max contrast, per-group accuracy, the A1/A0 mean **ratio** and
  whether the A0 and A1 ranges **overlap**. This is the diagnostic that tells you whether
  the amplitude bit is recoverable at all.
* **Résultat pour ChatGPT** — the important log lines only (`STATUS`, `SYNC`,
  `FRAME_SUM`, `OK`, `ERR`, …), ready to copy.

---

## Serial command reference

All commands are case-insensitive; Enter may be CR, LF or CR+LF. Baud 115200.

| Command | Purpose |
|---|---|
| `help` | list commands |
| `status` / `stats` | print full status and statistics |
| `config` | print the current configuration as an `EVT,CFG` line |
| `clear` | reset statistics |
| `rx` | enter receive mode and start searching |
| `txframe` | transmit one diagnostic frame |
| `auto <ms>` | repeat the diagnostic frame every `<ms>` (0 disables) |
| `txstate <0..15> <count>` | transmit one state for `count` symbols, then return to RX |
| `hold <0..15> <100..10000 ms>` | hold one state for a fixed time (for arming a raw capture) |
| `phasesweep <period_chips> <ms> [amp]` | repeat 0/90/180/270° blocks at a given period |
| `phasesweep1750 <ms>` / `phasesweep875 <ms>` | compatibility aliases |
| `raw <16..1024>` | capture N raw `E4` samples immediately |
| `rawdelay <n> <delay_ms>` | wait, then capture N raw samples |
| `rawtrig <n> <E4_threshold>` | capture N samples once `E4` crosses a threshold |
| `set <key> <value>` | change a runtime parameter (see below) |

Legacy aliases: `amp <a0> <a1>`, `iq <n>`, `gain <hex>`, `verbose <0|1>`,
`syncq <q> <rel>`.

---

## Configuration parameter reference (`set <key> <value>`)

| Key | Range | Meaning |
|---|---|---|
| `chiprate` | 1000–40000 Hz | chip clock |
| `symbolchips` | 8–64 | chips per symbol |
| `period0` / `period1` | 4–128, ÷4 | subcarrier periods in chips → `F = chiprate/period` |
| `ask0` / `ask1` | 0–63 | the two amplitude "digital-scale" codes |
| `iq` | 16–1024 | `IQ_EST` integration count (higher = cleaner, slower) |
| `gain` | hex | fixed RX gain code |
| `decode` | 0–2 | decoder (see table above) |
| `ampmode` | 0–5 | amplitude metric: 0 pair 3-feature, 1 pair contrast, 2 pair normalised, 3 global contrast, 4 global normalised, 5 hybrid |
| `ampmix` | 0–1000 | pair weight (per-mille) for hybrid `ampmode 5` |
| `framematch` | 0–16 | required phase/frequency matches in the training sweep to accept a frame |
| `frameq` | ≥0 | minimum average training quality to accept a frame |
| `alignmin` / `alignmax` | 0–256 | accepted range for the alignment offset |
| `syncblock` | 4–64 | chips per sync block (× 13 blocks = sync length) |
| `syncguard` | 0–256 | quiet chips after the preamble |
| `preguard` | 0–256 | quiet chips before the preamble |
| `alignpad` | 0–256 | quiet tail after the payload |
| `trainreps` | 1–4 | training sweeps per frame |
| `datareps` | 1–8 | data sweeps per frame |
| `syncstride` | 1–32 | how often the sync window is tested (decimation) |
| `syncq` | ≥0 | sync quality threshold (contrast/residual × 1000) |
| `syncrel` | ≥0 | sync relative threshold (contrast/mean, per-mille) |
| `syncc` | ≥0 | absolute `E4` ON−OFF contrast threshold |
| `recover` | 0/1 | enable scheduler-gap padding |
| `maxpad` | 0–2048 | maximum chips padded during recovery |
| `sweepblock` / `sweepgap` | 16–2048 / 0–512 | phase-sweep block and gap length |
| `telemetry` | 0/1 | emit `EVT,POINT` lines |
| `verbose` | 0/1 | per-symbol `SYM` log |
| `errprint` | 0/1 | print only erroneous symbols |

Frame size is bounded; if a combination of `symbolchips`, `trainreps`, `datareps` and
`alignpad` is too large the firmware rejects it with a "frame buffer" error.

---

## Understanding the output

* **`EVT,POINT,exp,got,amp,phase_deg,freq,ci,cq,contrast,margin,shapeQ`** — one per
  decoded data symbol. `exp` is the transmitted state (ground truth), `got` is the
  decoded state; `ci`/`cq` are the reconstructed pseudo-I/Q; `contrast`, `margin` and
  `shapeQ` are quality metrics. Consumed by the constellation and amplitude analysis.
* **`FRAME_SUM err=e/N amp=.. phase=.. freq=..`** — per-frame error counts, split by
  axis, plus average/min margin and shape quality.
* **`FRAME_DROP …`** — a frame was rejected by the alignment/quality gates
  (`alignmin/max`, `framematch`, `frameq`).
* **`EVT,CFG,…`** — the live configuration, key/value pairs (the UI parses this back into
  the settings grid).
* **`STATUS …`** — mode, frequencies, rates, gain, all thresholds, buffer usage and
  running statistics.
* **`RAW_BEGIN / RAW,i,dt_us,e4 / RAW_END`** — a raw `E4` capture dump.

A healthy phase×frequency link (8-state preset) shows `FRAME_SUM` with low `freq` and
`phase` error counts; the amplitude analysis will typically show large A0/A1 overlap,
which is precisely why the default preset does not rely on the amplitude bit.

---

## Limitations and honest caveats

* **Energy detection only.** No coherent I/Q, no true carrier phase. The "phase" axis is
  the time-shift of a square-wave envelope, recovered by correlation.
* **The amplitude axis is fragile.** OOK depth depends on phase and frequency; separating
  `ask0` from `ask1` reliably is hard. The default configuration drops it.
* **Soft real time.** Chip timing is done in software; the Wi-Fi/system scheduler can
  steal cycles. Gap recovery mitigates this but cannot fully hide it — expect occasional
  dropped or padded frames.
* **Undocumented hardware access.** Fixed ROM entry points and MMIO addresses are used.
  This is inherently tied to specific chip revisions and SDK builds and may not work
  everywhere.
* **Uncalibrated RF.** No dBm, no verified spectrum, no linearity guarantees. Measure
  before you conclude anything about power or bandwidth.
* **Short range by design.** Low subcarrier rates and conservative power make this a
  short-range bench experiment, not a communication system.
* **The label "QAM16" is historical.** The waveform is an OOK-gated subcarrier with
  amplitude/phase/frequency states, detected non-coherently — not QAM and not APSK.

---

## Relationship to the parent project

The register map, the tone-gate mechanism (`TONE1 = 0x600005B8`, gate bit 18, the
`bits 17:10` digital-scale field encoded as `(-scale) & 0xff`), and the `IQ_EST` / `E4`
energy-detection RX path all come from the
[`esp8266_WLAN_PHY`](https://github.com/kaboom748/esp8266_WLAN_PHY) reverse-engineering
project. That project's central conclusion — **OOK/ASK/FSK are closed at the software
level, but arbitrary QAM constellation injection is not exposed** — is the reason this
modem lives entirely in the OOK/ASK domain and synthesises phase and frequency in the
*envelope* rather than in the RF carrier. See that repository for the register-level
details and the RF measurements that remain open.

---

## Disclaimer

This material is experimental and is provided **"as is", without warranty of any kind**.
No guarantee is made about hardware safety, thermal safety, RF output, spectral or
regulatory compliance, frequency or modulation accuracy, or suitability for any purpose.
You are responsible for your own test setup, for protecting your hardware and test
equipment, for preventing harmful interference, and for complying with the radio and
equipment regulations that apply to you. A disclaimer does not make prohibited radio
operation legal. Start conservatively, measure, and stop immediately if the hardware
behaves abnormally.

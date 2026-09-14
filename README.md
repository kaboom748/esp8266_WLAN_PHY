<img width="600" height="600" alt="image" src="https://github.com/user-attachments/assets/05e28102-9b0c-4e8a-b61f-666eecc18d5f" />
<img width="652" height="412" alt="image" src="https://github.com/user-attachments/assets/7b87339e-abc0-40c9-b453-c0279f67dcd0" />

<img width="1024" height="619" alt="image" src="https://github.com/user-attachments/assets/ea7dcbfe-1267-413a-81f7-4e9e113d9f2b" />

# ESP8266 WLAN PHY — Reverse Engineering & RF Test Bench

> **Experimental research project for ESP8266 PHY/RF testing.**
>
> This repository explores low-level ESP8266 radio behavior for **OOK / ASK / FSK / M-FSK** and documents research around **QPSK / QAM**.
>
> It is **not** an official Espressif project, product, certification, or documentation.

---

## ⚠️ IMPORTANT SAFETY WARNING

**Read this before flashing or transmitting.**

This project directly manipulates low-level ESP8266 PHY/RF registers and ROM routines. It can keep the RF transmit chain active continuously, generate non-standard RF signals, and make the chip run **very hot**.

### Project safety rules

- **Use a Faraday cage, shielded RF enclosure, or a properly attenuated conducted setup for RF testing.**
- **Comply with the radio-frequency regulations and permitted operating conditions in your country.**
- **Do not leave the ESP8266 transmitting unattended.**
- **If the ESP8266 becomes very hot, unstable, smells unusual, resets repeatedly, draws abnormal current, or changes color: immediately disconnect power and let it cool completely.**
- **Do not assume that a larger numeric power value means a safe or linearly higher RF output.**
- **`APWR` is an internal experimental control, not a calibrated dBm setting.**
- **Project safety limit for routine tests: keep `APWR <= 63` unless you are deliberately characterizing hardware on an instrumented bench and have independently established safe operating conditions.**
- **Do not use `APWR=255` for normal testing.**
- **Do not use this project to intentionally interfere with, block, degrade, or deny radio communications.**

### ⚠️ Critical note about `TEST-TONEv5.yaml`

`TEST-TONEv5.yaml` intentionally preserves the experimental raw defaults used during development, including:

```yaml
current_apwr: 255
ultimate_apwr: 255
```

These values are kept as part of the research history.

**Do not interpret those defaults as recommended operating values.**

The repository owner has observed that the ESP8266 can become **very hot** with aggressive APWR settings and prolonged TX activity.

For routine bench characterization, the current project recommendation is:

```text
APWR <= 63
```

This is a **project-defined conservative limit**, not an Espressif-certified maximum and not a calibrated RF power value.

Before using normal TX, explicitly set a conservative value first:

```text
PHY
APWR 32
ASK 63
K 8
ON
```

or, if you intentionally want to use the project ceiling for routine testing:

```text
PHY
APWR 63
ASK 63
K 8
ON
```

Before using Ultimate mode, explicitly set its power control first:

```text
UAPWR 63
```

or lower.

`ULTIMATE ON` is especially demanding because it repeatedly enables TX while sweeping channels and tone-control values. Use it only for short, supervised measurements in a controlled RF test environment.

There is currently **no software temperature protection** in `TEST-TONEv5.yaml`.

---

## Project goal

The goal is to understand and experimentally validate parts of the ESP8266 WLAN PHY that are normally hidden behind Espressif's binary PHY implementation.

The project separates three kinds of results:

- **Software/static reverse-engineering:** behavior demonstrated from ROM, binaries, MMIO access, relocations, and control flow.
- **Hardware/RF characterization:** relationships such as code-to-frequency, code-to-power, settling time, phase continuity, spectral purity, and thermal behavior that must be measured on real silicon.
- **Open questions:** behavior that has not yet been demonstrated reliably.

The project does **not** assume that an internal register code has a simple physical interpretation unless it has been measured.

---

## Repository contents

The repository contains both living reverse-engineering notes and frozen reference documents.

### Main references

- [`ESP8266_PHY_MODULATIONS_REFERENCE_CONSOLIDEE_v2.0.md`](ESP8266_PHY_MODULATIONS_REFERENCE_CONSOLIDEE_v2.0.md)  
  Consolidated French reference for OOK, ASK, FSK, M-FSK, QPSK and QAM.

- [`ESP8266_PHY_MODULATIONS_CONSOLIDATED_REFERENCE_v2.0_EN.md`](ESP8266_PHY_MODULATIONS_CONSOLIDATED_REFERENCE_v2.0_EN.md)  
  English consolidated reference.

- [`ESP8266_ASK_OOK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`](ESP8266_ASK_OOK_REFERENCE_OFFICIELLE_PROJET_v1.0.md)  
  Frozen TX OOK/ASK project reference.

- [`ESP8266_TX_FSK_MFSK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`](ESP8266_TX_FSK_MFSK_REFERENCE_OFFICIELLE_PROJET_v1.0.md)  
  Frozen TX FSK/M-FSK project reference.

- [`ESP8266_RX_OOK_ASK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`](ESP8266_RX_OOK_ASK_REFERENCE_OFFICIELLE_PROJET_v1.0.md)  
  Frozen RX OOK/ASK project reference.

- [`ESP8266_RX_FSK_MFSK_REFERENCE_OFFICIELLE_PROJET_v1.0.md`](ESP8266_RX_FSK_MFSK_REFERENCE_OFFICIELLE_PROJET_v1.0.md)  
  Frozen RX FSK/M-FSK project reference.

- `ESP8266_WIFI_PHY_REVERSE_ENGINEERING*.md`  
  Historical/living reverse-engineering documents.

### Test firmware

The `TEST-TONE*.yaml` files are ESPHome-based experimental RF test firmwares.

The newest version currently present is:

```text
TEST-TONEv5.yaml
```

It targets a **Wemos D1 mini / ESP8266**, runs the CPU at 160 MHz, disables the normal ESPHome logger UART, and exposes a simple **115200-baud serial console** on UART0.

No normal Wi-Fi application connection is required for the test console.

The `TEST-TONE*.yaml` files should be treated as **research artifacts**, not production firmware and not pre-certified radio configurations.

---

## Current TX model

### OOK

The project identifies the normal tone gate as:

```text
Tone slot 1: 0x600005B8
Gate: bit 18
Mask: 0x00040000
```

Fast OOK changes only the gate while keeping the RF path and TX clock active.

Conceptually:

```text
OOK 0 -> clear bit 18
OOK 1 -> set bit 18
```

The `OFF` command is different: it shuts down the TX path rather than representing a fast OOK symbol.

---

## ASK / digital scale

The software field used by the tone generator is:

```text
bits 17:10
encoding = (-digital_scale) & 0xff
canonical digital_scale range = 0..63
```

The serial command is:

```text
ASK 0..63
```

### Important

`ASK` is the project's name for this **digital-scale control field**.

It must **not** be interpreted as:

- a calibrated RF voltage,
- a percentage of output power,
- a dBm value,
- or a guaranteed monotonic amplitude control.

Its physical spectral effect must be measured.

---

## FSK / M-FSK

Fast FSK uses the low tone-control field while keeping the following fixed:

```text
RFPLL       fixed
channel     fixed
TX RF       active
TX clock    active
gate        active
digital scale fixed
```

Only `tone_control` changes between symbols.

The firmware protects the field with:

```text
K mask = 0x3ff
K range = 0..1023
```

For 2-FSK:

```text
symbol 0 -> K0
symbol 1 -> K1
```

For M-FSK:

```text
K0, K1, ... K(M-1)
```

### Important

The project deliberately does **not** assume:

```text
K -> frequency in Hz
```

to be linear, symmetric, or constant-step.

`K` is a raw hardware control code. The actual RF frequencies, wrap behavior, settling, jitter, phase behavior, occupied bandwidth, and unwanted emissions must be measured on the device.

---

## QPSK / QAM status

The repository also documents the ESP8266 native QPSK / 16-QAM / 64-QAM paths.

What is established:

```text
Wi-Fi-native BPSK/QPSK/16-QAM/64-QAM exists
modulation is selected by the native PHY/rate path
```

What has **not** been demonstrated:

```text
arbitrary per-symbol QAM constellation injection
CPU-visible raw I/Q TX FIFO
general mapper bypass
arbitrary constellation index API
```

Therefore the tone-test firmware should not be described as an arbitrary QAM transmitter.

---

## Serial console

Default serial configuration:

```text
115200 baud
8 data bits
no parity
1 stop bit
```

Useful commands in `TEST-TONEv5.yaml`:

| Command | Purpose |
|---|---|
| `PHY` | Initialize/wake the PHY |
| `CH n` | Select Wi-Fi channel 1..14 when accepted by the SDK/domain |
| `FREQ MHz` | Select 2412..2472 MHz in 5 MHz steps, or attempt 2484 MHz |
| `ON` | Enable the experimental TX tone path |
| `OFF` | Stop sweep/test and shut down TX |
| `K n` | Set raw tone-control code, 0..1023 |
| `ASK n` | Set digital-scale field, 0..63 |
| `APWR n` | Set experimental analog/scale control |
| `OOK 0` / `OOK 1` | Toggle only the fast OOK gate while TX remains active |
| `KSWEEP min max` | Configure a K sweep |
| `KSTEP n` | Set K increment |
| `DWELL ms` | Set sweep dwell time |
| `SWEEP ON/OFF` | Start/stop the K sweep |
| `USTEP n` | Set Ultimate-mode K step |
| `UDWELL ms` | Set Ultimate-mode dwell |
| `UPASSES n` | Set Ultimate-mode pass count |
| `UAPWR n` | Set Ultimate-mode APWR |
| `ULTIMATE ON/OFF` | Start/stop the multi-channel stress/sweep test |
| `STATUS` | Show current state |
| `HELP` | Show firmware command list |

The presence of a command in the firmware does **not** imply that every setting is lawful for over-the-air operation in every jurisdiction.

---

## Recommended first test

Use a shielded setup and begin conservatively.

Example:

```text
PHY
APWR 32
ASK 63
K 8
ON
STATUS
```

Observe the signal with a spectrum analyzer.

Then stop TX:

```text
OFF
```

If the device temperature rises quickly, disconnect power and do not continue until the cause has been understood.

---

## Recommended K characterization

A safer way to characterize the tone-control range is to keep one RF channel fixed and sweep `K`.

Example:

```text
PHY
CH 6
APWR 32
ASK 63
ON
KSWEEP 0 1023
KSTEP 16
DWELL 5
SWEEP ON
```

Use a spectrum analyzer with suitable attenuation and, if useful, Max Hold.

Stop with:

```text
SWEEP OFF
OFF
```

After identifying interesting regions, reduce `KSTEP` for finer characterization.

---

## ⚠️ Ultimate mode

`ULTIMATE ON` is a **stress / coverage experiment**, not a normal modulation mode.

In `TEST-TONEv5.yaml`, it attempts to:

- walk through channels 1..14,
- sweep `K` across 0..1023,
- run a phase using `ASK=63`,
- run another phase using `ASK=0`,
- repeat for the configured number of passes.

This can create long periods of RF activity, substantial device heating, and uncharacterized spectral output.

### Before using Ultimate mode

For routine characterization, explicitly set:

```text
UAPWR 63
```

or lower **before**:

```text
ULTIMATE ON
```

Prefer a short first measurement:

```text
USTEP 16
UDWELL 1
UPASSES 1
UAPWR 63
ULTIMATE ON
```

Always keep the serial stop command ready:

```text
ULTIMATE OFF
```

If the module becomes noticeably hotter than during ordinary Wi-Fi operation, stop the test and disconnect power.

Do **not** use Ultimate mode as a burn-in test.

Do **not** use Ultimate mode over the air to occupy spectrum, interfere with other systems, or test range against third-party networks.


## OOK Reception Test

The `TEST-RX-OOK-SPEED-v3.yaml` file can be used to directly test OOK reception using the ESP8266 PHY `IQ_EST` block.

The test flow is:

1. enable the RX PHY;
2. start an `IQ_EST` measurement;
3. wait for the `DONE` bit;
4. read the RX metric from register `0x600005E4`;
5. calibrate a known `OFF` state and a known `ON` state;
6. automatically compute an OOK decision threshold.

### Initialization

After flashing:

```text
PHY
```

Example:

```text
RX PHY: START
RX PHY: STATION_MODE=1
RX PHY: NONE_SLEEP=1
RX PHY: CH=6 CENTER=2437MHz SET_OK=1
RX PHY: READY (RX RF ON + RX CLOCK ON)
```

### Check the RX Registers

```text
REGS
```

Example:

```text
REGS CTRL_DONE=80041003 DONE=1 CTRL_AFTER_DISABLE=00001000 E4=00004BD7(19415)
IQRES 580=00000000 584=00000000 588=00000000 58C=00000000 5DC=FFFDC400 5E0=FFFD85C0 5E4=00004BD7
```

The important field is:

```text
DONE=1
```

This indicates that the `IQ_EST` measurement completed successfully.

A single measurement can also be triggered with:

```text
SAMPLE
```

Example:

```text
SAMPLE CH=6 E4=29233 CTRL_DONE=0x80041003 DONE=1 OOK=? TH=0 MIN=29233 MAX=29233 SPAN=0
```

### OOK Calibration

With the transmitter in a known **OFF** state:

```text
CAL OFF
```

Then, with a known **ON** carrier:

```text
CAL ON
```

The firmware then automatically computes a threshold between the two measured levels.

It does not assume that `E4` must increase when the carrier is present. The decision direction is determined automatically from the two calibration results.

### Continuous Reception

```text
RATE 50
STREAM ON
```

Example:

```text
RX t=12345 CH=6 E4=26921 CTRL=80041003 DONE=1 OOK=1 TH=110787 MIN=17420 MAX=207399 SPAN=189979
RX t=12395 CH=6 E4=160769 CTRL=80041003 DONE=1 OOK=0 TH=110787 MIN=17420 MAX=207399 SPAN=189979
```

Stop streaming with:

```text
STREAM OFF
```

The output:

```text
OOK=0
```

or:

```text
OOK=1
```

is the decision made from the `E4` metric and the threshold obtained during calibration.

### Test RX Measurement Speed

The YAML can also benchmark the maximum measurement rate of the `IQ_EST` engine.

Run:

```text
BENCH 1000
```

Example measured with `N=1024`:

```text
BENCH N=1024 COUNT=1000 TOTAL_US=28488 AVG_US=27 MIN_US=27 MAX_US=40 SPS=35102 1SAMPLE_BIT_CEIL=37037 DONE=1000/1000 E4MIN=7177 E4MAX=1274862
```

This corresponds to roughly:

```text
35,000 RX measurements per second
```

An automatic sweep can be run with:

```text
BENCHSWEEP
```

Measured results on the tested hardware:

| N | Measurements/s |
|---:|---:|
| 16 | 305810 |
| 32 | 292397 |
| 64 | 235294 |
| 128 | 169491 |
| 256 | 110375 |
| 512 | 64977 |
| 1024 | 35323 |
| 2048 | 18556 |
| 4096 | 9515 |

The integration window can be changed with:

```text
IQN 16
```

or, for example:

```text
IQN 256
```

After changing `IQN`, recalibration is recommended:

```text
CAL OFF
CAL ON
```

because the `E4` distribution may change with the integration window.

> Important: the number of measurements per second is **not** the same as the maximum reliable OOK bit rate.
> To determine the real maximum OOK data rate, transmit a known bit pattern at increasing bit rates and measure detection errors.


### Channel 14 / 2484 MHz note

`TEST-TONEv5.yaml` can attempt channel 14 / 2484 MHz.

Treat that function as **laboratory-only** unless you have independently established that your use is permitted.

For example, current Canadian RSS-247 requirements identify the relevant 2.4 GHz licence-exempt DTS/FHS band as:

```text
2400 MHz to 2483.5 MHz
```

A center frequency of 2484 MHz is therefore outside that band edge.

Also remember that a center frequency being inside a permitted band is not sufficient by itself: sidebands, occupied bandwidth, out-of-band emissions, power, antenna gain, certification status, and other requirements can matter.

---

## Thermal warning

The ESP8266 module and its RF output stage are not being operated here through a normal application-level Wi-Fi transmit workflow.

This firmware may:

- force the RF TX chain on,
- keep the TX clock active,
- bypass normal packet duty cycles,
- run continuous tones,
- repeatedly retune/sweep,
- and modify undocumented/internal controls.

Those conditions can produce much higher thermal stress than ordinary intermittent Wi-Fi traffic.

**No software temperature protection is currently provided by `TEST-TONEv5.yaml`.**

Treat abnormal heating as a stop condition.

---

## RF measurement precautions

When connecting the ESP8266 directly to RF test equipment:

- use appropriate attenuation,
- verify the analyzer/input power rating,
- avoid DC or RF conditions outside the instrument's specified limits,
- prefer a shielded/conducted test arrangement,
- start at conservative settings,
- and verify the complete emitted spectrum rather than only the strongest peak.

When testing over an antenna, use a Faraday cage or equivalent shielded environment.

A Faraday cage is a **risk-control measure**, not a blanket legal exemption. Leakage and the actual emitted spectrum still matter.

---

## Regulatory and legal warning

This section provides general information only and is **not legal advice**.

This repository can generate continuous or non-standard signals in the 2.4 GHz region.

The fact that the hardware can generate a signal does **not** mean that transmitting that signal over the air is permitted.

### Licence-exempt does not mean unrestricted

Radio rules vary by country.

In Canada, RSS-247 currently covers relevant licence-exempt digital transmission/frequency-hopping systems in the **2400–2483.5 MHz** band.

Licence-exempt operation is still subject to the applicable technical, certification, emission, power, and equipment requirements.

In other words:

> **"2.4 GHz licence-exempt" does not mean "any waveform, bandwidth, output power, firmware configuration, or frequency is permitted."**

### Modified radio firmware and certification

Low-level RF firmware changes can affect whether an originally certified radio remains covered by its original certification.

Current Canadian RSS-Gen guidance recognizes firmware changes that affect RF characteristics as potentially relevant to certification and model status.

Therefore:

> **Do not assume that the original certification of an ESP8266 module or development board automatically covers the experimental PHY/RF modes in this repository.**

### Interference / jammer warning

This project is intended for **controlled RF research and characterization**, not for interference.

Do not use this project to intentionally block, disrupt, degrade, or deny radio communications.

In Canada, the Radiocommunication Act contains specific prohibitions concerning jammers and harmful/interfering radio operation.

### Other jurisdictions

FCC, EU/RED, UK, Japanese, Australian, and other national rules differ.

Before any over-the-air use, check the current requirements of the regulator that applies to you.

### Official Canadian references

- ISED RSS-247:  
  https://ised-isde.canada.ca/site/spectrum-management-telecommunications/en/devices-and-equipment/radio-equipment-standards/radio-standards-specifications-rss/rss-247-digital-transmission-systems-dtss-frequency-hopping-systems-fhss-and-licence-exempt-local

- ISED RSS-Gen:  
  https://ised-isde.canada.ca/site/spectrum-management-telecommunications/en/devices-and-equipment/radio-equipment-standards/radio-standards-specifications-rss/rss-gen-general-requirements-compliance-radio-apparatus

- Radiocommunication Act:  
  https://laws-lois.justice.gc.ca/eng/acts/R-2/

Regulations and standards change. Re-check the current versions before relying on this summary.

---

## Reverse-engineering / third-party material notice

This repository primarily contains original experimental firmware, factual observations, and reverse-engineering notes about ESP8266 behavior.

Do not add or redistribute proprietary SDK archives, binary libraries, firmware images, source code, documentation, or other third-party material unless you have the right to do so.

Reverse-engineering, interoperability, security-research, copyright, anti-circumvention, and redistribution rules vary by jurisdiction and can depend on the specific facts.

This README does **not** claim that every possible reverse-engineering activity or redistribution is automatically lawful.

---

## Repository licence status

At the time of this README update, the repository does not include a root `LICENSE` file.

Without an explicit licence, ordinary copyright rules apply by default. Publishing source code in a public GitHub repository does not automatically grant a general open-source licence for reuse, modification, or redistribution beyond the permissions provided by GitHub's terms.

If the repository owner later wants to make the project explicitly open source, a separate licence can be added.

This README does **not** itself grant a software licence.

---

## Research status summary

| Area | Project status |
|---|---|
| TX OOK software control | Closed at the documented software/MMIO level |
| TX ASK digital-scale control | Closed at the software/MMIO level; physical RF law requires measurement |
| TX FSK/M-FSK command path | Closed at the software/static level |
| `K -> Hz` | Hardware/RF characterization required |
| RX OOK/ASK software path | Documented in the project references; physical performance requires measurement |
| RX FSK CFO software path | Documented; fresh CFO on arbitrary non-802.11 tones requires silicon/baseband validation |
| Native Wi-Fi QPSK/QAM | Demonstrated through the native PHY/rate path |
| Arbitrary QAM constellation injection | Not demonstrated |

---

## Terminology

When the documents say **"official project reference"**, this means:

> the frozen reference document used by this reverse-engineering project.

It does **not** mean official Espressif documentation.

"ESP8266" and "Espressif" are used only to identify the platform being studied.

No affiliation, sponsorship, certification, or endorsement by Espressif is implied.

---

## Disclaimer

This repository contains experimental reverse-engineering notes, firmware, and RF test material.

**THE MATERIAL IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.**

To the maximum extent permitted by applicable law, no guarantee is made regarding:

- hardware safety,
- thermal safety,
- RF output level,
- spectral compliance,
- regulatory compliance,
- frequency accuracy,
- modulation accuracy,
- device-to-device repeatability,
- compatibility with every ESP8266 revision, SDK, board, or toolchain,
- protection of connected RF test equipment,
- or suitability for any particular purpose.

Users are responsible for:

- their own test setup,
- protecting hardware and test equipment,
- preventing harmful interference,
- complying with applicable radio and equipment regulations,
- determining whether a modified radio configuration remains covered by any certification,
- and respecting third-party copyrights, licences, and trademarks.

A disclaimer does **not** make prohibited radio operation legal and does not override mandatory law.

Start conservatively, measure, and stop immediately if the hardware behaves abnormally.

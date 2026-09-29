# ESP8266 — Practical 2‑FSK Reference using IQ_EST (v2.0)

## What actually works

This method uses two different ESP8266 PHY blocks:

- **TX**: the internal tone generator around `0x600005B8`.
- **RX**: the **IQ_EST** block around `0x6000057C`, used with an ultra-short `N=4` window.

The key empirical discovery is that **N=4 refreshes I and Q on every acquisition**, and the **phase rotation between consecutive acquisitions** is an excellent frequency discriminator.

> This is not an absolute RF frequency meter. It measures an **effective / aliased baseband rotation frequency**, but that value cleanly separates the two FSK tones.

---

## 1. TX tone register

Main tested register:

`0x600005B8`

| Bits | Name | Function |
|---|---|---|
| 9:0 | `TONE` | tone / frequency offset |
| 17:10 | `ASK` | amplitude |
| 18 | `GATE` | tone enable |

Empirical relation:

`Δf_RF ≈ TONE × 78.125 kHz`, modulo ~80 MHz around the Wi‑Fi channel.

Validated pair:

- **bit 0** → `TONE_A = 1016`
- **bit 1** → `TONE_B = 0`
- `ASK = 32`
- `APWR = 64`
- Wi‑Fi channel 6

Keep RF, TX clock, gate and amplitude active; change only `TONE` during the frame.

---

## 2. RX IQ_EST control

Control register:

`0x6000057C`

| Bit(s) | Name | Function |
|---|---|---|
| 0 | ENABLE | enable IQ_EST |
| 1 | START | start acquisition |
| 2..16 | FIELD / N | integration length |
| 18 | MODE | mode 0/1 |
| 31 | DONE | fresh result ready |

Fast modem configuration:

- `N = 4`
- `MODE = 0`

Result registers:

| Address | Value |
|---|---|
| `0x600005DC` | signed I |
| `0x600005E0` | signed Q |
| `0x600005E4` | latched energy, useful for diagnostics |

`0x600005EC` and `0x600005F0` are statistics/accumulators, **not raw I/Q**.

---

## 3. Why N=4 matters

With `N=4`, each IQ_EST acquisition is extremely short.

In our captures, consecutive acquisitions used for phase discrimination were typically about **3 µs apart** as observed through `micros()` in the running code.

This is fast enough to track complex rotation:

`z[n] = I[n] + jQ[n]`

---

## 4. Fresh acquisition handshake

To avoid stale DONE/results:

1. write `START=0`
2. wait for `DONE=0`
3. write `START=1`
4. wait for `DONE=1`
5. read I and Q
6. repeat

---

## 5. Phase frequency discriminator

For two consecutive I/Q samples:

```text
cross = Iprev*Qnow - Qprev*Inow
dot   = Iprev*Inow + Qprev*Qnow
dphi  = atan2(cross, dot)
```

Then:

```text
f_eff = dphi / (2*pi*dt)
```

`f_eff` is an effective RX-path frequency, not absolute RF offset.

Our working implementation uses **8 acquisitions**, yielding up to **7 phase differences**, then takes a circular mean.

---

## 6. Measured calibration

Tested with:

- channel 6
- `TONE_A=1016`
- `TONE_B=0`
- `ASK=32`
- `APWR=64`
- `N=4`
- `MODE=0`

Typical RX values:

- **bit 0 / TONE_A=1016** → `f_eff ≈ +19 to +21 kHz`
- **bit 1 / TONE_B=0** → `f_eff ≈ −26 to −28 kHz`

Working threshold:

```cpp
static constexpr int32_t FREQ_THRESHOLD_HZ = -3000;
```

Decision:

```cpp
bit = (f_eff < FREQ_THRESHOLD_HZ) ? 1 : 0;
```

Always calibrate the sign/mapping empirically.

---

## 7. Working frame format

```text
20 ms TONE_A
20 ms TONE_B
32 data bits
20 ms TONE_A
```

Test word:

`0xD3A5C69B`, MSB first.

At `SYMBOL_US = 1500`, long runs of `ERR=0/32` were observed.

At `SYMBOL_US = 750`, the discriminator is still clean, but an occasional one-symbol synchronization slip appears.

---

## 8. Key result

The successful receive path is:

**TX TONE → IQ_EST N=4 → I/Q phase rotation → threshold → bit**

not:

**TX TONE → E4 energy only → bit**


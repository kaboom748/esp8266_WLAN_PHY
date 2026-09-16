## ESP8266 OOK PHY v32.18 — 3500 bit/s

This sketch turns the ESP8266 Wi-Fi radio into a **half-duplex OOK/ASK data modem** without using an external RF transmitter or receiver module.

It directly controls low-level ESP8266 RF hardware and ROM routines, generates an OOK carrier for transmission, measures the receiver's analog IQ power for reception, and implements a VirtualWire/RH_ASK-inspired framing and clock-recovery scheme entirely in software.

The design is intentionally simple at the link layer:

- no session layer
- no peer addressing
- no acknowledgements
- no retransmission
- no CSMA / listen-before-talk
- arbitrary binary payloads
- CRC-protected frames

The version documented here runs at a **3500 bit/s raw OOK line rate** and uses an **8× receive PLL**, so the receiver processes approximately 28,000 decision samples per second.

### Main configuration

| Parameter | Value |
|---|---:|
| RF channel | Wi-Fi channel 6 |
| Raw line rate | 3500 bit/s |
| RX oversampling | 8 samples/bit |
| TX bit period | 285/286 µs fractional pattern |
| RX sample period | 35/36 µs fractional pattern |
| Test payload | 8 bytes |
| Maximum payload | 32 bytes |
| TX analog power | `APWR = 255` |
| TX ASK digital scale | `0` |
| RX gain | fixed `0x7C03` |
| E4 integration length | `IQ_N = 192` |
| CRC | reversed CRC-CCITT, polynomial `0x8408` |
| Start symbol | `0xB38` |
| Line coding | balanced 4b/6b |

> **Important:** this code relies on undocumented/low-level ESP8266 RF registers and ROM entry points. It is tightly coupled to the ESP8266 radio implementation and should not be treated like a normal portable Arduino RF driver.

---

### High-level architecture

The modem can be viewed as four layers:

```text
Application test traffic
        │
        ▼
Length + payload + CRC16
        │
        ▼
Balanced 4b/6b encoding
        │
        ▼
36-bit training + 0xB38 start symbol
        │
        ▼
ESP8266 RF PHY
   TX: analog scale + RF gate
   RX: E4 IQ power measurement
```

The transmitter and receiver share the same RF hardware, so operation is **half-duplex**. The code explicitly switches the ESP8266 RF path between transmit and receive modes.

---

## Wire format

The over-the-air framing is inspired by the classic VirtualWire / RH_ASK format.

A frame contains:

```text
[ 36-bit alternating training ]
[ 12-bit START = 0xB38 ]
[ encoded COUNT ]
[ encoded PAYLOAD ]
[ encoded CRC/FCS ]
```

The actual preamble is stored as eight 6-bit symbols:

```cpp
0x2A, 0x2A, 0x2A, 0x2A,
0x2A, 0x2A, 0x38, 0x2C
```

The first six symbols generate the alternating training sequence, while the final two encode the 12-bit start pattern.

Every byte after the start symbol is converted into **two balanced 6-bit symbols**.

### Count byte

The first decoded byte is a count field:

```text
count = payload_length + 3
```

The extra three bytes are:

- the count byte itself
- CRC/FCS low byte
- CRC/FCS high byte

With the current `MAX_PAYLOAD_LEN = 32`, the largest valid count is therefore 35.

### 4b/6b encoding

Each nibble is mapped to one of 16 legal six-bit codewords:

```cpp
0x0D, 0x0E, 0x13, 0x15,
0x16, 0x19, 0x1A, 0x1C,
0x23, 0x25, 0x26, 0x29,
0x2A, 0x2C, 0x32, 0x34
```

Each legal symbol contains three `1` bits and three `0` bits. This balanced coding helps prevent long DC-biased runs and makes clock recovery easier.

Six-bit symbols are transmitted **LSB first**.

### CRC/FCS

CRC calculation starts from:

```text
0xFFFF
```

and uses the reversed CCITT polynomial:

```text
0x8408
```

The transmitter complements the resulting CRC and appends it little-endian.

The receiver runs the CRC over the complete decoded count/data/FCS sequence and accepts a packet only when the final residue is:

```text
0xF0B8
```

This means a packet reported as `RX OK` has passed the complete frame CRC check.

---

## Fractional timing

3500 bit/s cannot be represented by an integer number of microseconds per bit:

```text
1,000,000 / 3500 = 285.714285... µs
```

The code therefore uses a fractional accumulator instead of rounding every bit to a fixed integer delay.

TX alternates between 285 µs and 286 µs intervals so that the long-term average remains exactly 3500 bit/s.

The receiver uses eight samples per bit:

```text
3500 × 8 = 28,000 samples/s
1,000,000 / 28,000 = 35.714285... µs/sample
```

The RX scheduler therefore produces a 35/36 µs pattern.

This avoids cumulative timing drift that would occur if the modem simply used 286 µs TX bits or 36 µs RX ticks continuously.

> There is one stale source-code comment in `serviceRx()` that still mentions a 12 kHz / 83,83,84 µs schedule from an older 1500 bit/s build. The actual constants and runtime scheduler in this 3500 bit/s version correctly use the 28 kHz, 35/36 µs timing described above.

---

## Transmitter

### RF path

Transmission uses the ESP8266 tone-generator/RF path directly.

The important runtime behavior is:

```text
logical 1:
    setAnaScale(255)
    RF gate ON

logical 0:
    RF gate OFF
    setAnaScale(0)
```

The sketch deliberately keeps:

```text
APWR = 255
ASK digital scale = 0
```

The TONE1 register is used for the tone/gate configuration, with bit 18 acting as the RF gate in this implementation.

Before transmitting, the code:

1. disables the receive IQ estimator
2. disables the RX clock/path
3. enables the TX RF path through PBUS writes
4. programs the analog TX scale
5. enables the TX clock
6. waits for the TX path to settle

After the frame, the process is reversed and the receiver is restored.

### Packet construction

`buildEncodedPacket()` performs the complete framing operation:

1. copy the preamble/start symbols
2. create the count byte
3. update the CRC with the count
4. encode count high nibble then low nibble
5. encode every payload byte
6. complement the CRC
7. append low FCS byte
8. append high FCS byte

`transmitPacket()` then sends:

```text
1 quiet bit
encoded frame
1 trailing quiet bit
```

For an 8-byte payload, the complete transmission occupies 182 raw bits:

```text
182 / 3500 ≈ 52 ms
```

For the current maximum 32-byte payload, the transmission occupies 470 raw bits, or approximately 134.3 ms at 3500 bit/s.

---

## Receiver

The receiver does not use a normal Wi-Fi packet-decoding path. Instead, it repeatedly measures an analog RF/IQ-power metric exposed through the ESP8266 hardware.

### E4 power measurement

Each RX scheduler tick calls `Phy::measureE4()`.

The code enables the ESP8266 IQ estimator with:

```cpp
IQ_N = 192
```

reads the power value from:

```text
0x600005E4
```

and then disables the estimator again.

There is exactly **one E4 integration per PLL tick**. The code does not average several independent E4 reads at each tick.

The received E4 value is passed to `AskDecoder::addSample()`.

---

## Adaptive envelope and slicer

While searching for a packet, the receiver tracks two slowly moving envelope values:

```text
envLow
envHigh
```

The decision threshold is:

```text
threshold = envLow + (envHigh - envLow) / 2
```

The envelope uses asymmetric attack/release behavior:

- new extrema are followed relatively quickly
- movement back toward the center is deliberately slow

This allows the detector to adapt to changes in received RF level without making the threshold move as quickly as individual symbols.

### Search hysteresis

During packet search and preamble acquisition, hysteresis is approximately:

```text
contrast / 12
```

with a minimum of 20 E4 units.

This makes random noise less likely to generate artificial edges and false start symbols.

### Frozen DATA threshold

Once a valid start symbol is accepted, the code **stops updating the envelope for the entire DATA + CRC frame**.

This is an important design feature.

Without threshold freezing, the alternating preamble and subsequent payload energy can pull the adaptive envelope around while the frame is being decoded. Freezing the threshold after START keeps the slicer reference stable for the rest of the packet.

During DATA, hysteresis is also narrowed from roughly `contrast/12` to `contrast/48`.

The active-data thresholds are asymmetric:

```text
LOW → HIGH : threshold T
HIGH → LOW : T - h
```

This was designed to tolerate weaker HIGH levels without changing the frozen center threshold.

---

## 8× digital PLL

Clock recovery follows the classic RH_ASK/VirtualWire ramp concept.

The PLL constants are:

```text
ramp length      = 160
transition point = 80
normal increment = 20
retard increment = 11
advance increment= 29
```

Because there are eight nominal samples per bit:

```text
160 / 8 = 20 ramp units per sample
```

When an input transition occurs:

- before the ramp midpoint, the PLL uses the smaller increment (`11`)
- after the midpoint, it uses the larger increment (`29`)
- without a transition, it advances normally by `20`

When the ramp reaches 160, one recovered data bit is emitted.

### Bit decision

The receiver integrates HIGH/LOW slicer decisions during one PLL bit period.

A recovered bit is considered HIGH when:

```cpp
highSamples >= 5
```

This is intentionally a fixed threshold, rather than a majority of however many samples happened to land in the current bit.

PLL correction can make an individual recovered bit contain more or fewer than exactly eight scheduler samples while phase is being acquired. The diagnostics therefore track the observed minimum and maximum sample count per recovered bit.

---

## Preamble qualification

Detecting `0xB38` alone is not sufficient to accept a packet.

Before START is accepted, the receiver requires a convincing alternating preamble.

The current qualifier requires at least:

```text
20 consecutive alternating transitions
```

The real 36-bit training sequence contains 35 transitions, so this still leaves room for the PLL to acquire phase before the complete preamble has been observed.

After the alternating run arms the receiver, START must appear within a configurable window of:

```text
24 recovered bits
```

Otherwise the armed state expires.

This allows the code to distinguish:

- raw START hits
- START rejected because no valid preamble was seen
- START rejected because the armed preamble window expired
- accepted START symbols

### PRE36 diagnostic

The decoder also keeps the most recent 48 recovered bits while searching.

When a raw START is found, it looks at the exact 36 decisions immediately preceding the 12-bit START and counts how many transitions occurred there.

A perfect alternating 36-bit preamble has:

```text
35 transitions
```

This `P36` value is diagnostic only; it helps determine whether rejected or accepted starts were preceded by a genuine training sequence.

---

## 4b/6b decoding and analog ML rescue

Normally, each received 6-bit symbol must exactly match one of the 16 legal 4b/6b codewords.

If both symbols are valid, the byte is decoded normally and **no analog correction is attempted**.

If one or both six-bit symbols are invalid, v32.18 can perform a limited analog maximum-likelihood rescue.

### Why analog rescue is possible

For each recovered bit, the decoder already records:

- average E4
- minimum E4
- maximum E4
- number of samples

For ML scoring, v32.18 builds a more robust per-bit E4 estimate by removing one minimum and one maximum sample when enough samples are available. This reduces the influence of isolated E4 spikes.

Each legal 4b/6b symbol is then scored against the six analog bit values.

Because every legal codeword contains exactly three HIGH bits and three LOW bits, common-mode E4 offsets mostly cancel when candidates are compared.

The best candidate is accepted only when its score margin over the second-best candidate is sufficiently large.

The confidence threshold is based on:

```text
contrast / 12
```

The rescue mechanism is intentionally conservative:

- it is used **only when the ordinary binary 4b/6b symbol is invalid**
- an already-valid symbol is never changed by ML
- CRC remains the final integrity check for the complete packet

The diagnostics expose:

```text
ML[try=... use=... ok=...]
```

where:

- `try` = blocks where analog rescue was attempted
- `use` = blocks where an analog candidate was accepted
- `ok` = CRC-valid packets that ultimately contained at least one rescued block

---

## Receive gain

The sketch fixes the ESP8266 RX gain code to:

```text
0x7C03
```

Although the source still contains code capable of reading PBUS gain fields, the captured hardware gain is intentionally ignored and replaced with this fixed value.

The fixed gain is restored whenever the code returns from TX to RX.

This removes AGC behavior from the experiment and keeps received E4 levels more predictable between packets.

---

## Half-duplex TX/RX switching

The radio cannot receive while this implementation is transmitting.

When a local packet is sent:

```text
RX
 ↓
stop IQ/RX path
 ↓
configure TX path
 ↓
transmit frame
 ↓
disable TX path
 ↓
restore fixed RX gain
 ↓
RX settle delay
 ↓
short post-TX holdoff
 ↓
resume sampling
```

The post-TX holdoff is:

```text
2500 µs
```

and prevents the receiver from immediately interpreting its own TX/RX switching transient as incoming data.

There is **no carrier sensing or CSMA** before transmission. Two units can therefore collide if they transmit at the same time.

---

## RX scheduler and missed-tick handling

The receiver is serviced cooperatively from `loop()` rather than from a dedicated hardware interrupt.

At 3500 bit/s, the intended tick interval is approximately 35.7 µs.

If the program reaches an RX sample slightly late, it advances the next deadline while preserving the fractional cadence.

If it is late by at least three RX ticks:

```cpp
late >= RX_TICK_US_CEIL * 3
```

the decoder timing state is reset instead of trying to continue from a badly corrupted sampling phase.

Envelope state is preserved by `resetTimingOnly()`, but packet/PLL/preamble state is cleared.

This protects the decoder from long scheduler stalls, but it also means that long blocking operations or excessive serial output can make an in-progress packet disappear.

---

## Watchdog handling

The sketch frequently feeds the ESP8266 software watchdog while waiting on tight TX deadlines and while running the RX loop.

It also records the largest observed interval between watchdog feeds as:

```text
WDTmax
```

This is useful when evaluating whether additional application code or serial logging is starving time-critical RF processing.

---

## Built-in test application

The `.ino` includes a simple two-node arbitrary-data test.

Every unit:

1. remains in RX most of the time
2. waits a random interval between 900 and 1800 ms
3. generates an 8-byte random payload
4. transmits it
5. switches back to RX
6. reports valid incoming packets

There is no master/slave assignment, so both devices behave identically.

The randomized TX interval reduces—but does not eliminate—the probability of collisions.

Example TX output:

```text
TX len=8 data=34F943BE9A7F0D61 APWR=255 DS=0 TXMODE=ANA-BIT airtime=52.000ms
```

Example RX output:

```text
RX OK len=8 data=E5244BA2D1CBF274
```

---

## Diagnostics

The firmware intentionally exposes a large amount of PHY information.

A typical `STAT` line contains several groups.

### `ASK[...]`

General decoder counters:

```text
samp     total E4 samples
bit      recovered PLL bits
edge     slicer transitions
start    accepted START symbols
ok       CRC-valid packets
symBad   invalid 4b/6b failures
lenBad   invalid length/count failures
crc      CRC failures
to       active-frame timeouts
```

### `Q[...]`

```text
ok / accepted START
```

This is a useful packet-completion ratio once START has been acquired.

### `ML[...]`

Analog rescue statistics:

```text
try / use / ok
```

### `PRE[...]`

Preamble qualification statistics:

```text
arm     qualifier armed
raw     raw 0xB38 detections
accept  accepted START
reject  rejected START
rejNA   rejected: qualifier not armed
rejEXP  rejected: qualifier expired
expEvt  qualifier expiration events
altMax  longest observed alternating transition run
```

### `P36[...]`

Transition density in the exact 36 decisions preceding detected START symbols.

The reported format is:

```text
count:average[min..max]
```

for accepted, not-armed-rejected and expired-rejected STARTs.

### `PLL[...]`

```text
ret   early-transition retard corrections
adv   late-transition advance corrections
bs    min..max scheduler samples used per recovered bit
```

### `E4[...]`

Analog envelope/slicer state:

```text
lo       tracked low envelope
hi       tracked high envelope
th       slicer center threshold
sep      current envelope separation
minPkt   minimum separation seen during active packet
maxSep   maximum separation observed
weak     samples taken while contrast was very small
raw      minimum..maximum raw E4 values
```

### Timing diagnostics

```text
E4dt    time spent performing one E4 integration/read
sampdt  actual interval between E4 samples
WDTmax  largest interval between watchdog feeds
```

These are especially important when increasing the bitrate, because the CPU must complete one RX iteration before the next scheduled PLL sample.

---

## FAILBLOCK diagnostics

When a frame fails because of:

- invalid symbol
- invalid length
- CRC failure
- timeout

v32.18 stores a snapshot of the current 12-bit encoded byte.

The diagnostic includes:

- recovered bits
- HIGH vote count / total sample count for every bit
- PLL residual phase
- largest sampling gap inside each recovered bit
- E4 average/minimum/maximum for every bit
- frozen slicer threshold
- active DATA hysteresis
- raw six-bit symbols

For the first byte after START, the firmware also knows the expected count byte for the built-in fixed 8-byte test payload, so it can print the expected 12 encoded bits beside the received ones.

This acts as a small PHY-level “microscope” for investigating exactly where a failing packet diverged.

---

## Arduino `setup()` and `loop()`

### `setup()`

Startup performs the following operations:

1. start the serial port at 115200 baud
2. print the active PHY configuration
3. seed the random generator
4. initialize the low-level ESP8266 RF hardware
5. reset the decoder
6. apply fixed RX gain `0x7C03`
7. wait for RX settling
8. schedule the first random TX
9. print the initial diagnostics

### `loop()`

The main loop continuously runs:

```text
service watchdog
service one due RX sample
service completed packets / diagnostics / possible TX
service watchdog again
periodic STAT output
occasional yield while idle
```

The program deliberately avoids frequent `yield()` calls because long scheduler pauses would disturb the 35/36 µs RX cadence. A yield is only performed periodically while the decoder is searching rather than actively receiving a frame.

---

## Important design limitations

This version is an experimental PHY, not a complete MAC protocol.

### No CSMA

The radio does not listen for an idle channel immediately before transmitting. Simultaneous transmissions can collide.

### No ACK or retry

A sender does not know whether a packet was received.

### No addressing

Every valid frame is accepted regardless of which node transmitted it.

### Half-duplex only

The ESP8266 RF path is explicitly switched between TX and RX.

### Cooperative RX timing

RX timing depends on `loop()` running frequently enough. Long blocking operations, heavy serial output, or other time-consuming application work can cause missed RX ticks.

### Maximum payload is currently 32 bytes

The underlying framing can be resized, but this build intentionally defines:

```cpp
MAX_PAYLOAD_LEN = 32;
```

### 3500 bit/s is close to the practical limit of this exact 8× implementation

At 3500 bit/s the receiver requires one E4/PLL service approximately every 35.7 µs. The code includes timing diagnostics specifically because increasing the line rate further leaves less CPU time for each E4 integration and for the rest of the Arduino loop.

---

## Key tuning constants

Most experimental behavior can be changed near the top of the sketch:

```cpp
RF_CHANNEL
TONE_K
TX_ASK
TX_APWR
RX_GAIN_CODE
IQ_N
BIT_RATE
RX_SAMPLES_PER_BIT
TX_SETTLE_US
RX_SETTLE_US
POST_TX_HOLDOFF_US
PREAMBLE_MIN_TRANSITIONS
PREAMBLE_START_WINDOW_BITS
MAX_PAYLOAD_LEN
```

Changing `BIT_RATE` automatically recalculates both the TX fractional bit scheduler and the RX fractional sample scheduler.

The PLL geometry itself remains defined in phase units and is tied to `RX_SAMPLES_PER_BIT`.

---

## Summary

`ESP8266_OOK_v32_18_3500BPS...ino` is a software-defined OOK modem built directly on the ESP8266 RF hardware.

Its main features are:

- direct RF transmit gating through the ESP8266 PHY
- analog E4-based receive detection
- fixed RX gain
- exact-average fractional timing
- RH_ASK-style 8× digital PLL
- 36-bit alternating training preamble
- qualified `0xB38` START detection
- balanced 4b/6b line coding
- arbitrary binary payloads
- CRC16/FCS integrity checking
- frozen DATA slicer threshold
- asymmetric DATA hysteresis
- conservative analog ML rescue for invalid 4b/6b symbols
- extensive PHY and failure diagnostics

The result is a compact experimental data link that uses the ESP8266 radio itself as the OOK transmitter and receiver, while keeping the higher-level protocol intentionally minimal so that RF behavior can be measured and tuned directly.

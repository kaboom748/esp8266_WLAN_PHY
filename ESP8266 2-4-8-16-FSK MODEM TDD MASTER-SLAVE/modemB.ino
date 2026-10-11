// AirModem 0.5.13 step 8.6 manual 16-FSK, signed tones, IF +/-6500Hz — stage 8 restored wide-spaced 8-FSK + control-only frequency tracking selectable 2/4/8-FSK data, universal two-tone SYNC and passive eight-tone monitor SYNC / CTRL CRC16 / DATA CRC32 (4-FSK, TDD master/slave) — restored 0.3.0 RF PHY + UART 19200 + optional diagnostics.
// Primary Serial: 19200 8N1 XON/XOFF; diagnostics share this UART when enabled.
// This version intentionally DISABLES the experimental 9600 PHY and Reed-Solomon.
// DIAGNOSTICS: 1 = bracketed text on SAME Serial 19200 port, 0 = transparent binary.
#define AIRMODEM_DIAGNOSTICS 1
// STEP 8: Set 2, 4, 8 or 16 independently on A and B. All three frame types are decoded.
// Manual TX setting only; no automatic rate adaptation in this step.
#define AIRMODEM_DATA_FSK_MODE 16
// Stage 8.5: common signed IF target (Hz) for 2/4/8-FSK.
// Set to 9000 to restore the original 0.5.10 center without changing RF code.
#define AIRMODEM_IF_TARGET_HZ 6500
#if AIRMODEM_IF_TARGET_HZ < 5000 || AIRMODEM_IF_TARGET_HZ > 9500
#error "AIRMODEM_IF_TARGET_HZ outside tested conservative range"
#endif
#if AIRMODEM_DATA_FSK_MODE != 2 && AIRMODEM_DATA_FSK_MODE != 4 && AIRMODEM_DATA_FSK_MODE != 8 && AIRMODEM_DATA_FSK_MODE != 16
#error "AIRMODEM_DATA_FSK_MODE must be 2, 4, 8 or 16"
#endif
// Serial clock only; DOES NOT change 4800 symbols/s physical RF.
#define AIRMODEM_UART_BAUD 19200UL
// UART 8N1 requires ten transmitted bits per character.
#define AIRMODEM_UART_CHAR_US ((10000000UL + AIRMODEM_UART_BAUD - 1) / AIRMODEM_UART_BAUD)
// WARNING: diagnostic text will corrupt a PPP/binary stream while enabled.
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
#include <new>
extern "C" {
#include "user_interface.h"
}

// ===== ModemProtocol.h =====
#line 1 "ModemProtocol.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace AirModem {
constexpr unsigned PAYLOAD = 50, PACKET_BYTES = 80, BUFFER_BYTES = 2048;
constexpr unsigned TWO_FSK_BYTES = 40, TWO_FSK_PAYLOAD = 10;
constexpr unsigned EIGHT_FSK_BYTES = 68; // 26B robust header + 38B data + CRC32
constexpr unsigned EIGHT_FSK_PAYLOAD = 38; // 208 2-FSK + 112 8-FSK symbols
constexpr unsigned SIXTEEN_FSK_BYTES = 82; // 26B CTRL + 56B (52 data + 4 CRC32)
constexpr unsigned SIXTEEN_FSK_PAYLOAD = PAYLOAD; // 50B protocol ceiling

constexpr unsigned LOCAL_TX_PAYLOAD = AIRMODEM_DATA_FSK_MODE == 2 ? TWO_FSK_PAYLOAD : (AIRMODEM_DATA_FSK_MODE == 8 ? EIGHT_FSK_PAYLOAD : PAYLOAD);
constexpr int16_t NO_TUNE = 32767;
// Common IF target shared by all M-FSK modes. Experimental move from
// +/-9000 to +/-6500 Hz; does not change data-tone separation, symbol rate,
// TDD, payload format, oscillator timebase or the external UART.
// This is an actual tuning target change, NOT a post-sampling IQ translation.
constexpr int32_t CAL_TARGET_ABS_HZ = AIRMODEM_IF_TARGET_HZ;
constexpr int32_t CAL_DEADBAND_HZ = 450;
constexpr int32_t CAL_MAX_REQUEST_DELTA_HZ = 1200;
constexpr int32_t CAL_MAX_APPLY_DELTA_HZ = 600;
constexpr int32_t CAL_MIN_OBS_HZ = 1000; // near zero, signed IF is ambiguous
constexpr int32_t CAL_MAX_OBS_HZ = 18750; // too near Nyquist => risk of alias
constexpr int32_t CAL_MIN_UNIT_HZ = 100;
constexpr int32_t CAL_MAX_UNIT_HZ = 5000;
inline int32_t absCal(int32_t v) { return v < 0 ? -v : v; }
inline int32_t clampCal(int32_t v, int32_t limit) {
  return v < -limit ? -limit : v > limit ? limit : v;
}
inline bool validCalMeasurement(int32_t received, int32_t unit) {
  return absCal(received) >= CAL_MIN_OBS_HZ &&
         absCal(received) <= CAL_MAX_OBS_HZ &&
         absCal(unit) >= CAL_MIN_UNIT_HZ && absCal(unit) <= CAL_MAX_UNIT_HZ;
}
// The command is an ABSOLUTE desired TX shift on the remote ESP, expressed
// relative to the remote's *advertised* txHz, not to our previous request.
// This prevents feedback wind-up when the RF return path has a frame of delay.
// The sign of unit accounts for a reversed FSK frequency discriminator.
inline int16_t tuneRequest(int16_t transmitted, int32_t received, int32_t unit) {
  if (transmitted < -12000 || transmitted > 12000 ||
      !validCalMeasurement(received, unit)) return NO_TUNE;
  const int32_t target = received < 0 ? -CAL_TARGET_ABS_HZ : CAL_TARGET_ABS_HZ;
  const int32_t residual = target - received;
  if (absCal(residual) <= CAL_DEADBAND_HZ) return transmitted;
  int32_t delta = residual * (unit < 0 ? -1 : 1);
  delta = clampCal(delta, CAL_MAX_REQUEST_DELTA_HZ);
  int32_t request = int32_t(transmitted) + delta;
  if (request > 12000) request = 12000;
  if (request < -12000) request = -12000;
  return int16_t(request);
}
// A full CRC32-valid frame is required before applying a REMOTE tuning order.
// Enforce the per-frame slew and a hard RF limit without ever applying NO_TUNE.
inline int16_t boundedTxCorrection(int16_t current, int16_t requested) {
  if (requested == NO_TUNE || requested < -12000 || requested > 12000) return current;
  int32_t delta = clampCal(int32_t(requested) - current, CAL_MAX_APPLY_DELTA_HZ);
  int32_t next = int32_t(current) + delta;
  return int16_t(next < -12000 ? -12000 : next > 12000 ? 12000 : next);
}
enum class Type : uint8_t { Idle, Open, Ring, Accept, Data, Hangup, Busy, CableOpen };
struct Packet {
  Type type = Type::Idle;
  uint8_t role = 0, length = 0;
  uint32_t boot = 0, session = 0;
  uint16_t seq = 0, ack = 0, credit = 0;
  int16_t txHz = 0, tuneHz = NO_TUNE;
  uint8_t data[PAYLOAD] = {};
};
inline uint32_t crcByte(uint32_t c, uint8_t b) {
  c ^= b;
  for (unsigned k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
  return c;
}
inline uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xffffffffu;
  while (n--) c = crcByte(c, *p++);
  return ~c;
}
inline uint16_t ctrlCrc16(const uint8_t* p, size_t n) {
  uint16_t crc = 0xffff;
  while (n--) {
    crc ^= uint16_t(*p++) << 8;
    for (unsigned b = 0; b < 8; ++b)
      crc = (crc & 0x8000u) ? uint16_t((crc << 1) ^ 0x1021u) : uint16_t(crc << 1);
  }
  return crc;
}
inline uint16_t get16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t get32(const uint8_t* p) { return uint32_t(get16(p)) | uint32_t(get16(p + 2)) << 16; }
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { put16(p, uint16_t(v)); put16(p + 2, uint16_t(v >> 16)); }
inline bool valid(const Packet& p) {
  return (p.role == 1 || p.role == 2) && p.boot && unsigned(p.type) <= unsigned(Type::CableOpen) &&
    p.length <= PAYLOAD && p.credit <= BUFFER_BYTES &&
    p.txHz >= -12000 && p.txHz <= 12000 &&
    (p.tuneHz == NO_TUNE || (p.tuneHz >= -12000 && p.tuneHz <= 12000)) &&
    (p.type == Type::Data || !p.length) &&
    (p.type == Type::Idle ? p.session == 0 : p.session != 0);
}
inline bool encode(const Packet& p, uint8_t out[PACKET_BYTES]) {
  if (!valid(p)) return false;
  memset(out, 0, PACKET_BYTES);
  out[0] = 0xa4; out[1] = 0x80; out[2] = 3; out[3] = p.role;
  put32(out + 4, p.boot); put32(out + 8, p.session);
  out[12] = uint8_t(p.type); out[13] = p.length;
  put16(out + 14, p.seq); put16(out + 16, p.ack); put16(out + 18, p.credit);
  put16(out + 20, uint16_t(p.txHz)); put16(out + 22, uint16_t(p.tuneHz));
  memcpy(out + 24, p.data, p.length); put16(out + 74, ctrlCrc16(out, 24)); put32(out + 76, crc32(out, 76));
  return true;
}
inline bool decodeControl(const uint8_t in[PACKET_BYTES], Packet& out) {
  if (in[0] != 0xa4 || in[1] != 0x80 || in[2] != 3 ||
      get16(in + 74) != ctrlCrc16(in, 24)) return false;
  Packet p; p.role = in[3]; p.boot = get32(in + 4); p.session = get32(in + 8);
  p.type = Type(in[12]); p.length = in[13]; p.seq = get16(in + 14);
  p.ack = get16(in + 16); p.credit = get16(in + 18);
  p.txHz = int16_t(get16(in + 20)); p.tuneHz = int16_t(get16(in + 22));
  if (!valid(p)) return false;
  out = p; return true;
}
inline bool decode(const uint8_t in[PACKET_BYTES], Packet& out) {
  if (get32(in + 76) != crc32(in, 76) || !decodeControl(in, out)) return false;
  Packet p; p.role = in[3]; p.boot = get32(in + 4); p.session = get32(in + 8);
  p.type = Type(in[12]); p.length = in[13]; p.seq = get16(in + 14);
  p.ack = get16(in + 16); p.credit = get16(in + 18);
  p.txHz = int16_t(get16(in + 20)); p.tuneHz = int16_t(get16(in + 22));
  if (!valid(p)) return false;
  memcpy(p.data, in + 24, p.length); out = p; return true;
}
template<unsigned N> class RingBuffer {
public:
  unsigned size() const { return used; }
  unsigned free() const { return N - used; }
  bool push(uint8_t v) {
    if (used == N) return false;
    bytes[(head + used) % N] = v; ++used; return true;
  }
  int pop() { if (!used) return -1; int v = bytes[head]; drop(1); return v; }
  uint8_t peek(unsigned n) const { return n < used ? bytes[(head + n) % N] : 0; }
  void drop(unsigned n) { if (n > used) n = used; head = (head + n) % N; used -= n; }
  void clear() { head = used = 0; }
private:
  uint8_t bytes[N] = {};
  unsigned head = 0, used = 0;
};
}


namespace AirModem {
// STEP7 compact 2-FSK: 24-byte header, 10-byte payload, CRC16(24), CRC32(36).
// One information bit per physical symbol and exactly 320 data symbols + 24 SYNC.
inline bool encodeTwo(const Packet& p, uint8_t out[TWO_FSK_BYTES]) {
  if (!valid(p) || p.length > TWO_FSK_PAYLOAD) return false;
  memset(out,0,TWO_FSK_BYTES);
  out[0]=0xa4;out[1]=0x80;out[2]=4;out[3]=p.role;
  put32(out+4,p.boot);put32(out+8,p.session);
  out[12]=uint8_t(p.type);out[13]=p.length;
  put16(out+14,p.seq);put16(out+16,p.ack);put16(out+18,p.credit);
  put16(out+20,uint16_t(p.txHz));put16(out+22,uint16_t(p.tuneHz));
  memcpy(out+24,p.data,p.length);
  put16(out+34,ctrlCrc16(out,24));
  put32(out+36,crc32(out,36));
  return true;
}
inline bool decodeTwo(const uint8_t in[TWO_FSK_BYTES],Packet& p,bool& dataValid) {
  dataValid=false;
  if (in[0]!=0xa4 || in[1]!=0x80 || in[2]!=4 ||
      get16(in+34)!=ctrlCrc16(in,24) || in[13]>TWO_FSK_PAYLOAD) return false;
  uint8_t canonical[PACKET_BYTES]={};
  memcpy(canonical,in,24);canonical[2]=3;
  memcpy(canonical+24,in+24,TWO_FSK_PAYLOAD);
  put16(canonical+74,ctrlCrc16(canonical,24));
  if (!decodeControl(canonical,p)) return false;
  dataValid=(get32(in+36)==crc32(in,36));
  if (dataValid) memcpy(p.data,in+24,p.length);
  return true;
}

}

namespace AirModem {
// Step 8 robust header: 208 2-FSK symbols (24B header + CRC16),
// followed by 112 8-FSK symbols (38B data + CRC32). Same 344 symbols.
// Receiver can authenticate control without correctly decoding rapid data.
inline bool encodeEight(const Packet& p, uint8_t out[EIGHT_FSK_BYTES]) {
  if (!valid(p) || p.length > EIGHT_FSK_PAYLOAD) return false;
  memset(out,0,EIGHT_FSK_BYTES);
  out[0]=0xa4;out[1]=0x80;out[2]=5;out[3]=p.role;
  put32(out+4,p.boot);put32(out+8,p.session);
  out[12]=uint8_t(p.type);out[13]=p.length;
  put16(out+14,p.seq);put16(out+16,p.ack);put16(out+18,p.credit);
  put16(out+20,uint16_t(p.txHz));put16(out+22,uint16_t(p.tuneHz));
  put16(out+24,ctrlCrc16(out,24));
  memcpy(out+26,p.data,p.length);
  put32(out+64,crc32(out,64));
  return true;
}
inline bool decodeEight(const uint8_t in[EIGHT_FSK_BYTES],Packet& p,bool& dataValid) {
  dataValid=false;
  if (in[0]!=0xa4 || in[1]!=0x80 || in[2]!=5 ||
      get16(in+24)!=ctrlCrc16(in,24) || in[13]>EIGHT_FSK_PAYLOAD) return false;
  uint8_t canonical[PACKET_BYTES]={};
  memcpy(canonical,in,24);canonical[2]=3;
  put16(canonical+74,ctrlCrc16(canonical,24));
  if (!decodeControl(canonical,p)) return false;
  dataValid=(get32(in+64)==crc32(in,64));
  if (dataValid) memcpy(p.data,in+26,p.length);
  return true;
}
// Experimental 16-FSK payload: CTRL stays 26 bytes / 208 robust 2-FSK symbols.
// 112 payload symbols carry 4 bits each. All data fragments CRC32-validated.
inline bool encodeSixteen(const Packet& p, uint8_t out[SIXTEEN_FSK_BYTES]) {
  if (!valid(p) || p.length > SIXTEEN_FSK_PAYLOAD) return false;
  memset(out, 0, SIXTEEN_FSK_BYTES);
  out[0]=0xa4; out[1]=0x80; out[2]=6; out[3]=p.role;
  put32(out+4,p.boot); put32(out+8,p.session);
  out[12]=uint8_t(p.type); out[13]=p.length;
  put16(out+14,p.seq); put16(out+16,p.ack); put16(out+18,p.credit);
  put16(out+20,uint16_t(p.txHz)); put16(out+22,uint16_t(p.tuneHz));
  put16(out+24,ctrlCrc16(out,24));
  memcpy(out+26,p.data,p.length);
  put32(out+78,crc32(out,78));
  return true;
}
inline bool decodeSixteen(const uint8_t in[SIXTEEN_FSK_BYTES],Packet& p,bool& dataValid) {
  dataValid=false;
  if (in[0]!=0xa4 || in[1]!=0x80 || in[2]!=6 ||
      get16(in+24)!=ctrlCrc16(in,24) || in[13]>SIXTEEN_FSK_PAYLOAD) return false;
  uint8_t canonical[PACKET_BYTES]={};
  memcpy(canonical,in,24); canonical[2]=3;
  put16(canonical+74,ctrlCrc16(canonical,24));
  if (!decodeControl(canonical,p)) return false;
  dataValid=(get32(in+78)==crc32(in,78));
  if (dataValid) memcpy(p.data,in+26,p.length);
  return true;
}
inline uint8_t gray16(uint8_t x) {return uint8_t((x^(x>>1))&15u);}
inline uint8_t ungray16(uint8_t x) {x^=x>>1; x^=x>>2; x^=x>>4; return x&15u;}
// Tone index is the INVERSE Gray code of the three transmitted data bits.
// Then adjacent physical frequencies differ by exactly one data bit.
// This matches ESP8266TDMAdaptive8FSK-0.7.0's Fsk8::toneIndex().
inline uint8_t gray8(uint8_t x) { return uint8_t(x ^ (x >> 1)); }
inline uint8_t ungray8(uint8_t x) { x ^= x >> 1; x ^= x >> 2; return x & 7; }
inline unsigned encodedBit8(const uint8_t* p, unsigned i) {return (p[i>>3]>>(7-(i&7)))&1u;}
inline void setEncodedBit8(uint8_t* p, unsigned i, unsigned bit) {
  p[i>>3] |= uint8_t((bit&1u)<<(7-(i&7)));
}

}
// ===== ModemLink.h =====
#line 1 "ModemLink.h"
namespace AirModem {
enum class State : uint8_t { Idle, Dialing, Ringing, Answering, Connected, Closing };
enum class Reason : uint8_t { None, Local, Remote, Timeout, Retries, Overflow, Restart, Busy, Uart };
enum Event : uint8_t { Ring = 1, Connect = 2, Disconnect = 4 };
class Link {
public:
  struct Stats {
    uint32_t frames = 0, dataTx = 0, retries = 0, duplicates = 0, rejected = 0;
    uint32_t txBytes = 0, rxBytes = 0, busy = 0, disconnects = 0;
  } stats;
  Link(uint8_t role, uint32_t boot) : role_(role), boot_(boot ? boot : 1) {}
  void tick(uint32_t now);
  bool dial(uint32_t session, uint32_t now, bool cable = false);
  bool answer(uint32_t now);
  void close(Reason reason, uint32_t now);
  Packet outgoing(uint32_t now);
  bool incoming(const Packet& p, uint32_t now);
  bool write(uint8_t b) { return state_ == State::Connected && tx.push(b); }
  int read() { return rx.pop(); }
  unsigned queued() const { return tx.size(); }
  unsigned available() const { return rx.size(); }
  State state() const { return state_; }
  Reason reason() const { return reason_; }
  uint8_t role() const { return role_; }
  uint32_t session() const { return session_; }
  bool cable() const { return cable_; }
  uint8_t takeEvents() { uint8_t e = events; events = 0; return e; }
  uint32_t connectTimeout = 60000, lossTimeout = 15000;
private:
  void clearStream();
  RingBuffer<BUFFER_BYTES> tx, rx;
  uint8_t role_, events = 0, pending = 0;
  uint32_t boot_, peerBoot = 0, session_ = 0, started = 0, lastPeer = 0, lastRing = 0;
  uint16_t txSeq = 0, rxSeq = 0, peerCredit = 0, attempts = 0;
  State state_ = State::Idle;
  Reason reason_ = Reason::None;
  bool cable_ = false;
};
}


// ===== AirModem.h =====
#line 1 "AirModem.h"
namespace AirModem {
class Modem {
public:
  struct Stats {
    uint32_t xoff = 0, xon = 0, hostPauses = 0, uartErrors = 0, overflows = 0;
    uint32_t maxQueued = 0;
    uint32_t inputBytes = 0, outputBytes = 0, inputCrc = 0xffffffffu, outputCrc = 0xffffffffu;
    uint32_t cableDropped = 0;
  } stats;
  struct RadioStats {
    uint32_t tx = 0, rx = 0, aborts = 0;
    int32_t offset = 0, unit = 0, shift = 0;
  } radioStats;
  Modem(uint8_t role, uint32_t boot) : link(role, boot), random_(boot ? boot : 1) {}
  void input(uint8_t byte, uint32_t now);
  int output(uint32_t now);
  void tick(uint32_t now);
  void uartFailure(uint32_t now);
  bool online() const { return link.state() == State::Connected; }
  bool cableMode() const { return true; }
  bool inputPaused() const { return inputStopped; }
  bool hostPaused() const { return hostStopped; }
  void schedulePause(bool pause, uint32_t now) { if (scheduleStopped != pause) { scheduleStopped = pause; updateFlow(now); } }
  Link link;
private:
  void updateFlow(uint32_t now);
  uint32_t nonce();
  RingBuffer<1024> cablePending;
  bool inputStopped = false, hostStopped = false, sessionEstablished = false;
  bool scheduleStopped = false;
  int flowPending = 0x11;
  uint32_t lastFlow = 0, random_;
};
}


// ===== RHCompat.h =====
#line 1 "RHCompat.h"
#ifndef RHCompat_h
#define RHCompat_h
#include <Arduino.h>
#define RH_BROADCAST_ADDRESS 0xff
#define RH_FLAGS_RESERVED 0xf0
#define RH_FLAGS_APPLICATION_SPECIFIC 0x0f
#define RH_FLAGS_NONE 0
#define RH_FLAGS_ACK 0x80
#define RH_FLAGS_RETRY 0x40
#define RH_DEFAULT_TIMEOUT 1500
#define RH_DEFAULT_RETRIES 3
#endif


// ===== RHGenericDriver.h =====
#line 1 "RHGenericDriver.h"
#ifndef RHGenericDriver_h
#define RHGenericDriver_h
class RHGenericDriver {
public:
  enum RHMode { RHModeInitialising=0, RHModeSleep, RHModeIdle, RHModeTx, RHModeRx, RHModeCad };
  RHGenericDriver();
  virtual ~RHGenericDriver() {}
  virtual bool init();
  virtual bool available() = 0;
  virtual bool recv(uint8_t* buf, uint8_t* len) = 0;
  virtual bool send(const uint8_t* data, uint8_t len) = 0;
  virtual uint8_t maxMessageLength() = 0;
  virtual void waitAvailable();
  virtual bool waitPacketSent();
  virtual bool waitPacketSent(uint16_t timeout);
  virtual bool waitAvailableTimeout(uint16_t timeout);
  virtual bool waitCAD();
  virtual bool isChannelActive();
  virtual bool supportsAckBeacon();
  virtual bool sendAckBeacon(uint8_t repeats=3, uint16_t gapMs=80);
  virtual bool waitAckBeacon(uint16_t timeoutMs);
  void setCADTimeout(unsigned long cad_timeout);
  virtual void setThisAddress(uint8_t thisAddress);
  virtual void setHeaderTo(uint8_t to);
  virtual void setHeaderFrom(uint8_t from);
  virtual void setHeaderId(uint8_t id);
  virtual void setHeaderFlags(uint8_t set, uint8_t clear = RH_FLAGS_APPLICATION_SPECIFIC);
  virtual void setPromiscuous(bool promiscuous);
  virtual uint8_t headerTo();
  virtual uint8_t headerFrom();
  virtual uint8_t headerId();
  virtual uint8_t headerFlags();
  virtual int16_t lastRssi();
  virtual RHMode mode();
  virtual void setMode(RHMode mode);
  virtual bool sleep();
  static void printBuffer(const char* prompt, const uint8_t* buf, uint8_t len);
  virtual uint16_t rxBad();
  virtual uint16_t rxGood();
  virtual uint16_t txGood();
protected:
  volatile RHMode _mode;
  uint8_t _thisAddress, _txHeaderTo, _txHeaderFrom, _txHeaderId, _txHeaderFlags;
  uint8_t _rxHeaderTo, _rxHeaderFrom, _rxHeaderId, _rxHeaderFlags;
  bool _promiscuous;
  int16_t _lastRssi;
  uint16_t _rxBad, _rxGood, _txGood;
  unsigned long _cad_timeout;
};
#endif


// ===== Fsk4.h =====
#line 1 "Fsk4.h"
#ifndef ESP8266_FSK4_H
#define ESP8266_FSK4_H
#include <stdint.h>
#include <string.h>

namespace Fsk4 {
static const uint16_t TONES[4]={1012,1020,4,12};
static const uint16_t TRAINING_SIZE=128;
inline uint8_t gray(uint8_t index){return index^(index>>1);}
inline uint8_t toneIndex(uint8_t dibit){return gray(dibit&3);}
inline uint8_t dibitAt(const uint8_t* packed,uint16_t bit){
  return (packed[bit>>3]>>(6-(bit&7)))&3;
}

// Learn four separate clusters, not four evenly spaced ADC values. Phase
// approximation and oscillator offsets can make the measured spacing unequal.
inline bool calibrate(const int32_t* input,uint16_t count,int32_t minSep,int32_t centers[4]){
  if(count<32 || count>TRAINING_SIZE || minSep<=0)return false;
  int32_t sorted[TRAINING_SIZE];
  memcpy(sorted,input,count*sizeof(int32_t));
  for(uint16_t i=1;i<count;i++){
    int32_t x=sorted[i];uint16_t j=i;
    while(j && sorted[j-1]>x){sorted[j]=sorted[j-1];j--;}
    sorted[j]=x;
  }
  uint16_t cuts[3]={};int64_t gaps[3]={};
  for(uint16_t i=1;i<count;i++){
    int64_t gap=int64_t(sorted[i])-sorted[i-1];
    for(uint8_t k=0;k<3;k++)if(gap>gaps[k]){
      for(uint8_t n=2;n>k;n--){gaps[n]=gaps[n-1];cuts[n]=cuts[n-1];}
      gaps[k]=gap;cuts[k]=i;break;
    }
  }
  if(gaps[2]<minSep)return false;
  for(uint8_t i=1;i<3;i++){
    uint16_t x=cuts[i];uint8_t j=i;
    while(j && cuts[j-1]>x){cuts[j]=cuts[j-1];j--;}cuts[j]=x;
  }
  int32_t candidate[4],spread[4];uint16_t first=0;
  for(uint8_t k=0;k<4;k++){
    uint16_t last=k<3?cuts[k]:count,n=last-first;
    if(n<4)return false;
    uint16_t a=first+n/4,b=first+(3*n)/4;
    candidate[k]=int32_t((int64_t(sorted[a])+sorted[b])/2);
    int64_t width=int64_t(sorted[b])-sorted[a];
    if(width>INT32_MAX)return false;
    spread[k]=int32_t(width);first=last;
  }
  for(uint8_t k=0;k<4;k++){
    int64_t sep=k?int64_t(candidate[k])-candidate[k-1]:INT32_MAX;
    if(k<3 && int64_t(candidate[k+1])-candidate[k]<sep)sep=int64_t(candidate[k+1])-candidate[k];
    if(sep<minSep || spread[k]>sep/3)return false;
  }
  memcpy(centers,candidate,sizeof(candidate));return true;
}

inline int8_t classify(int32_t hz,const int32_t centers[4]){
  uint8_t best=0;int64_t distance=INT64_MAX;
  for(uint8_t k=0;k<4;k++){
    int64_t delta=int64_t(hz)-centers[k];if(delta<0)delta=-delta;
    if(delta<distance){best=k;distance=delta;}
  }
  int64_t sep=best?int64_t(centers[best])-centers[best-1]:INT32_MAX;
  if(best<3 && int64_t(centers[best+1])-centers[best]<sep)sep=int64_t(centers[best+1])-centers[best];
  // Reject midpoint ambiguity and out-of-band estimates.
  return sep>0 && distance*20<sep*9?int8_t(best):int8_t(-1);
}

inline int8_t winner(const uint16_t votes[4]){
  uint8_t best=0;bool tie=false;
  for(uint8_t k=1;k<4;k++){
    if(votes[k]>votes[best]){best=k;tie=false;}
    else if(votes[k]==votes[best])tie=true;
  }
  return votes[best] && !tie?int8_t(best):int8_t(-1);
}
inline int8_t decide(const uint8_t bins[8][4],uint32_t bin){
  uint16_t votes[4]={};
  for(uint8_t k=0;k<5;k++)for(uint8_t tone=0;tone<4;tone++)votes[tone]+=bins[(bin-k)&7][tone];
  return winner(votes);
}
}
#endif


// ===== FskCodec.h =====
#line 1 "FskCodec.h"
#include <stdint.h>
namespace ESP8266FSKCodec {
inline uint8_t encode(uint8_t n){
  uint8_t a=(n>>3)&1,b=(n>>2)&1,c=(n>>1)&1,d=n&1;
  return ((a^b^d)<<6)|((a^c^d)<<5)|(a<<4)|((b^c^d)<<3)|(b<<2)|(c<<1)|d;
}
inline uint8_t decode(uint8_t code,uint8_t& corrected){
  uint8_t a=(code>>6)&1,b=(code>>5)&1,c=(code>>4)&1,d=(code>>3)&1;
  uint8_t e=(code>>2)&1,f=(code>>1)&1,g=code&1;
  uint8_t syndrome=(a^c^e^g)|((b^c^f^g)<<1)|((d^e^f^g)<<2);
  corrected=syndrome?1:0;
  if(syndrome)code^=1u<<(7-syndrome);
  return (((code>>4)&1)<<3)|(((code>>2)&1)<<2)|(((code>>1)&1)<<1)|(code&1);
}
inline uint16_t crc16(const uint8_t* data,uint8_t len){
  uint16_t crc=0xffff;
  for(uint8_t i=0;i<len;i++){
    crc^=(uint16_t)data[i]<<8;
    for(uint8_t bit=0;bit<8;bit++)crc=(crc&0x8000)?(crc<<1)^0x1021:crc<<1;
  }
  return crc;
}
// Eight Hamming words are transmitted column-wise to spread short error bursts.
inline void encodeBlock(const uint8_t* data,uint8_t len,uint8_t block[7]){
  uint8_t words[8]={};
  for(uint8_t i=0;i<4;i++){
    uint8_t value=i<len?data[i]:0;
    words[2*i]=encode(value>>4);words[2*i+1]=encode(value&15);
  }
  for(uint8_t row=0;row<7;row++){
    block[row]=0;
    for(uint8_t col=0;col<8;col++)block[row]|=((words[col]>>(6-row))&1)<<(7-col);
  }
}
inline void decodeBlock(const uint8_t block[7],uint8_t data[4],uint8_t& corrections){
  corrections=0;
  for(uint8_t col=0;col<8;col++){
    uint8_t word=0,corrected=0;
    for(uint8_t row=0;row<7;row++)word=(word<<1)|((block[row]>>(7-col))&1);
    uint8_t n=decode(word,corrected);corrections+=corrected;
    if(!(col&1))data[col/2]=n<<4;else data[col/2]|=n;
  }
}
}


// ===== FskFrame.h =====
#line 1 "FskFrame.h"
#ifndef ESP8266_FSK_FRAME_H
#define ESP8266_FSK_FRAME_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace FskFrame {
static const uint8_t MAX_PAYLOAD=32,RAW_SIZE=39;
inline uint16_t bitCount(uint8_t length,uint8_t preamble){
  return preamble+12+14+56*((length+9)/4);
}
inline void append(uint8_t* wire,uint16_t& count,bool bit){
  if(bit)wire[count>>3]|=1u<<(7-(count&7));
  count++;
}
inline uint16_t encode(const uint8_t* data,uint8_t length,const uint8_t headers[4],
                       uint8_t preamble,uint8_t* wire,size_t capacity){
  if(length>MAX_PAYLOAD || (!data && length) || preamble<24 || preamble>64 ||
     (preamble&1) || capacity<(bitCount(length,preamble)+7u)/8u)return 0;
  uint8_t raw[RAW_SIZE];raw[0]=length+4;
  memcpy(raw+1,headers,4);if(length)memcpy(raw+5,data,length);
  uint8_t n=length+5;uint16_t crc=ESP8266FSKCodec::crc16(raw,n);
  raw[n++]=crc>>8;raw[n++]=crc;
  memset(wire,0,capacity);uint16_t count=0;
  for(uint8_t i=0;i<preamble;i++)append(wire,count,i&1);
  for(int8_t i=11;i>=0;i--)append(wire,count,(0xb38>>i)&1);
  for(int8_t half=1;half>=0;half--){
    uint8_t h=ESP8266FSKCodec::encode((raw[0]>>(half*4))&15);
    for(int8_t k=6;k>=0;k--)append(wire,count,(h>>k)&1);
  }
  for(uint8_t i=1;i<n;i+=4){
    uint8_t block[7],size=n-i;if(size>4)size=4;
    ESP8266FSKCodec::encodeBlock(raw+i,size,block);
    for(uint8_t row=0;row<7;row++)for(int8_t bit=7;bit>=0;bit--)append(wire,count,(block[row]>>bit)&1);
  }
  return count;
}

struct Decoder {
  enum Result { NONE,SYNC,CORRECTION_REJECT,CRC_REJECT,FRAME };
  uint32_t shift=0;
  uint8_t code=0,codeBits=0,high=0,nibble=0,count=0,expected=0,corrections=0;
  bool active=false,inverted=false;
  uint8_t block[7]={},raw[RAW_SIZE]={};

  Result feed(bool bit,bool firstInSymbol,bool fourFsk,uint8_t maxCorrections){
    if(!active){
      shift=((shift<<1)|bit)&0xffffff;
      if(fourFsk && firstInSymbol)return NONE;
      uint32_t normal=shift^0x555b38u;
      // Reversing the order of Gray-coded 4-FSK tones flips only the MSB.
      uint32_t inverse=shift^(0x555b38u^(fourFsk?0xaaaaaau:0xffffffu));
      bool a=__builtin_popcount(normal&0xfff)<=1&&__builtin_popcount(normal>>12)<=2;
      bool b=__builtin_popcount(inverse&0xfff)<=1&&__builtin_popcount(inverse>>12)<=2;
      if(!a && !b)return NONE;
      inverted=b;active=true;code=0;codeBits=0;nibble=0;count=0;expected=0;corrections=0;
      return SYNC;
    }
    bit^=inverted && (!fourFsk || firstInSymbol);
    if(count==0){
      code=(code<<1)|bit;if(++codeBits<7)return NONE;
      uint8_t corrected=0,value=ESP8266FSKCodec::decode(code,corrected);
      corrections+=corrected;code=0;codeBits=0;
      if(!nibble){high=value;nibble=1;return NONE;}
      raw[0]=(high<<4)|value;count=1;nibble=0;
      if(raw[0]<4 || raw[0]>4+MAX_PAYLOAD){active=false;shift=0;return NONE;}
      expected=raw[0]+3;memset(block,0,sizeof(block));return NONE;
    }
    if(bit)block[codeBits>>3]|=1u<<(7-(codeBits&7));
    if(++codeBits<56)return NONE;
    uint8_t bytes[4],corrected=0;
    ESP8266FSKCodec::decodeBlock(block,bytes,corrected);
    corrections+=corrected;codeBits=0;memset(block,0,sizeof(block));
    if(corrections>maxCorrections){active=false;shift=0;return CORRECTION_REJECT;}
    for(uint8_t i=0;i<4&&count<expected;i++)raw[count++]=bytes[i];
    if(count<expected)return NONE;
    active=false;shift=0;
    return ESP8266FSKCodec::crc16(raw,count-2)==((uint16_t)raw[count-2]<<8|raw[count-1])?FRAME:CRC_REJECT;
  }
};
}
#endif


// ===== RH_ESP8266FSK.h =====
#line 1 "RH_ESP8266FSK.h"
#ifndef RH_ESP8266FSK_h
#define RH_ESP8266FSK_h
#define RH_ESP8266FSK_MAX_MESSAGE_LEN 32
#define RH_ESP8266FSK_HEADER_LEN 4
class RH_ESP8266FSK: public RHGenericDriver {
public:
  enum ModemConfigChoice {
    FSK_Rb250Reliable,
    FSK_Rb333Reliable,
    FSK_Rb500Experimental,
    FSK_Rb667Experimental,
    FSK_Rb1000Experimental,
    FSK_Rb500Conservative=FSK_Rb333Reliable,
    FSK_Rb1000Portable=FSK_Rb333Reliable,
    FSK_Rb2000Experimental=FSK_Rb500Experimental,
    FSK_Rb4000Experimental=FSK_Rb667Experimental
  };
  struct ModemConfig {
    uint32_t bitUs;
    uint16_t toneZero;
    uint16_t toneOne;
    uint8_t apwr;
    uint8_t ask;
    uint16_t frameGapMs;
    uint8_t txRepeats;
    uint8_t preambleBits;
    uint32_t calUs;
    int32_t minToneSepHz;
  };
  struct Calibration {
    bool valid;
    int32_t zeroHz;
    int32_t oneHz;
    int32_t sepHz;
    bool inverted;
    uint32_t atMs;
  };
  struct Statistics {
    uint32_t polls=0, workUs=0, maxPollUs=0, samples=0, slowPolls=0, txLateUs=0, txAborts=0, syncs=0, correctionRejects=0;
    uint32_t sampleStepNs=0,sampleJitterNs=0,lastEnergy=0;
    uint32_t crcFrames=0,addressRejects=0,decodedCandidates=0,candidateDrops=0;
    uint32_t fillBits=0,iqSamples=0;
    uint8_t lastTo=0,lastFrom=0,lastLength=0;
    int32_t lastHz=0;
  };
  Statistics statistics() const {return _stats;}
  bool setFourFsk(bool enabled){
    if(_radioReady || _mode==RHModeTx)return false;
    _fourFsk=enabled;return true;
  }
  bool fourFsk() const {return _fourFsk;}
  uint32_t symbolUs() const {return _bitUs;}
  bool fourToneCenters(int32_t centers[4]) const {
    if(!_fourFsk || !_trained)return false;
    memcpy(centers,_centers4,sizeof(_centers4));return true;
  }
  struct RfState {uint16_t path,rfWord,bbWord;bool manual,rxStopped;uint8_t apwr,ask,txAskField;};
  RfState rfState() const;
  bool setReceiveGain(int8_t rf,uint8_t bb=2);
  bool setIqMode(uint8_t mode);
  uint8_t iqMode() const {return _iqMode;}
  void resetStatistics(){_stats=Statistics();}
  void setCalibrationMemory(uint32_t ms){_calMemoryMs=ms;}
  void restartReceive(bool forget=false){if(forget)_lastCal.valid=false; resetReceiver();}
  uint32_t lastReceiveUs() const {return _lastReceiveUs;}
  void setReceiveDeadline(uint32_t deadline){_receiveDeadline=deadline;}
  uint8_t lastCorrections() const {return _lastCorrections;}
  bool receivingFrame() const {for(const auto& d:_decoders)if(d.active)return true;return false;}
  RH_ESP8266FSK(uint8_t channel=6);
  void poll();
  void setMode(RHMode mode) override;
  bool sleep() override;
  bool waitPacketSent();
  bool waitPacketSent(uint16_t timeout);
  // One calibration per burst; retain the RF TX path between its frames.
  bool sendBurstFrame(const uint8_t* data,uint8_t len,bool last);
  void endTxBurst();
  bool init(); bool available(); bool recv(uint8_t* buf,uint8_t* len); bool send(const uint8_t* data,uint8_t len); uint8_t maxMessageLength(); bool supportsAckBeacon(); bool sendAckBeacon(uint8_t repeats=3,uint16_t gapMs=80); bool waitAckBeacon(uint16_t timeoutMs);
  bool setModemConfig(ModemConfigChoice choice);
  bool setModemConfig(const ModemConfig& cfg);
  bool setBitRate(uint16_t bitsPerSecond);
  void setTones(uint16_t zero,uint16_t one); void setPower(uint8_t apwr,uint8_t ask); void setBitUs(uint32_t bitUs); void setFrameGap(uint16_t ms); void setTxRepeats(uint8_t repeats); void setPollBudgetUs(uint16_t us); void setMaxCorrections(uint8_t corrections); void setTxFillUntil(uint32_t deadlineUs);
  uint32_t frameDurationUs(uint8_t len,bool continuation=false) const {
    if(len>RH_ESP8266FSK_MAX_MESSAGE_LEN)return 0;
    uint32_t bits=FskFrame::bitCount(len,_preambleBits);
    return (continuation?0:(_fourFsk?4:2)*_calUs)+(_fourFsk?(bits+1)/2:bits)*_bitUs;
  }
  uint32_t bitUs() const; uint16_t bitRate() const; Calibration lastCalibration() const; uint8_t maxCorrections() const;
protected:
  struct FreqEst{uint32_t t; int32_t hz; uint32_t e4; bool valid;};
  bool initRadioBase(); bool initRxPath(); bool initTxPath(bool gate=true); void txTone(uint16_t tone); bool receiveRaw(uint8_t* raw,uint8_t* rawLen);
  bool acquireFreq(FreqEst& f); uint8_t ham74Encode(uint8_t n); uint8_t ham74Decode(uint8_t code,uint8_t& corrected); uint16_t crc16(const uint8_t* data,uint8_t len);
  bool _radioReady=false, _txSucceeded=true;
  bool _fourFsk=false;
  bool _txHeld=false,_holdTx=false;
  bool startFrame(const uint8_t* data,uint8_t len,bool continuation,bool holdTx);
  int8_t _rxGain=-1;
  uint8_t _rxBbGain=2,_lastTxAskField=0,_iqMode=1;
  void applyReceiveGain();
  uint8_t _wire[(64+12+14+56*((RH_ESP8266FSK_MAX_MESSAGE_LEN+9)/4)+7)/8] = {};
  uint16_t _wireBits=0,_wireIndex=0;
  uint8_t _txStage=0,_txRepeatIndex=0,_pilotIndex=0;
  uint32_t _txDeadline=0,_txFillUntil=0;
  bool _txFillBit=false;
  void serviceTx();
  bool transmitBits();
  void appendBit(bool bit);
  Statistics _stats;
  typedef FskFrame::Decoder Decoder;
  Decoder _decoders[8] = {};
  int32_t _training4[Fsk4::TRAINING_SIZE]={},_centers4[4]={};
  uint16_t _training4N=0,_training4Pos=0;
  uint8_t _votes4[8][4]={};
  int32_t _training[16] = {};
  uint8_t _trainingN=0, _trainingPos=0;
  uint32_t _nextSample=0, _nextTraining=0, _epoch=0, _bin=0, _lockAt=0,_receiveDeadline=0;
  int16_t _scores[8] = {};
  uint8_t _counts[8] = {};
  int32_t _low=0, _high=0;
  bool _trained=false;
  int64_t _dcI=0,_dcQ=0;
  uint32_t _calMemoryMs=0, _lastReceiveUs=0;
  uint8_t _lastCorrections=0;
  uint8_t _candidate[1+RH_ESP8266FSK_HEADER_LEN+RH_ESP8266FSK_MAX_MESSAGE_LEN+2]={};
  uint8_t _candidateLen=0,_candidateCorrections=0;
  uint32_t _candidateAt=0;
  Calibration _candidateCal{};
  bool finishCandidate(uint8_t* raw,uint8_t* rawLen);
  void resetReceiver();
  bool train(int32_t hz);
  bool trainFour(int32_t hz);
  bool decodeBit(Decoder& d,bool bit,uint8_t* raw,uint8_t* rawLen,bool firstInSymbol=false);
  uint8_t _channel; uint16_t _toneZero,_toneOne; uint8_t _apwr,_ask; uint32_t _bitUs; uint16_t _gapMs; uint8_t _txRepeats; uint16_t _pollBudgetUs; uint8_t _preambleBits; uint32_t _calUs; int32_t _minToneSepHz; uint8_t _maxCorrections; Calibration _lastCal; bool _bufValid; uint8_t _buf[RH_ESP8266FSK_MAX_MESSAGE_LEN]; uint8_t _bufLen;
};
#endif


// ===== ModemSignal.h =====
#line 1 "ModemSignal.h"
namespace AirModem { namespace Phy {
constexpr unsigned SPS = 8, SAMPLE_RATE = 38400, SYMBOL_RATE = 4800;
constexpr unsigned FRAME_BYTES = PACKET_BYTES + 6, SYMBOLS = FRAME_BYTES * 4;
constexpr unsigned SLOT_SAMPLES = (SYMBOLS + 12) * SPS, MAX_CAPTURE = 4096;
constexpr unsigned RRC_TAPS = 81, RRC_DELAY = 40, FIRST_CENTER = 52, SYNC_AT = 160;
constexpr uint64_t SYNC = 0xd3916ac5f027ULL;
struct Frame { uint8_t bytes[FRAME_BYTES] = {}; uint8_t eight[EIGHT_FSK_BYTES] = {}; uint8_t sixteen[SIXTEEN_FSK_BYTES] = {}; uint8_t mode = 4; };
struct Acquisition { Packet packet; int32_t firstCenter = 0, offsetHz = 0, unitHz = 0; bool dataValid = false; uint8_t mode = 0; };
inline int8_t symbol(uint8_t d) { return d == 0 ? 1 : d == 1 ? 3 : d == 2 ? -1 : -3; }
// Stage 6: map the known middle SYNC to the two OUTERMOST legacy 4-FSK tones.
// The 24 timing symbols, packet duration, and all ordinary data tones are unchanged.
// The sign pattern is inherited from the original 48-bit SYNC; only magnitudes change.
inline int8_t syncExtreme(unsigned index) {
  const uint8_t dibit = uint8_t((SYNC >> (46 - 2 * index)) & 3);
  return symbol(dibit) < 0 ? -3 : 3;
}

void makeFrame(const Packet& packet, Frame& frame);
bool readFrame(const Frame& frame, Packet& packet, bool& dataValid);
int32_t packIq(int16_t i, int16_t q);
int32_t phaseHz(int32_t cross, int32_t dot);
int32_t demodulate(int32_t* samples, unsigned count);
class Signal {
public:
  Signal();
  void waveform(const Frame& frame, int32_t out[SLOT_SAMPLES], int32_t offsetHz = 0) const;
  bool acquire(const int32_t* hz, unsigned count, Acquisition& result);
  bool lastSyncCandidate = false; // Correlation only; not an authenticated header.
  // Passive 8-tone monitor. Never participates in packet decisions/calibration.
  uint32_t eightCtrlValid = 0, eightPayloadValid = 0, eightPayloadFailed = 0, eightRefinedRecovery = 0;
  uint32_t shadowFrames = 0, shadowSymbols = 0, shadowMatch = 0, shadowMismatch = 0;
  uint32_t shadowAmbiguous = 0, shadowTimeMaxUs = 0;
  void inspectEightTones(unsigned start, int32_t offset, int32_t unit,
                         const int8_t* knownSync, unsigned count);
private:
  int32_t tx[RRC_TAPS];
  int16_t rx[RRC_TAPS], filtered[MAX_CAPTURE];
};
}}


// ===== ModemRadio.h =====
#line 1 "ModemRadio.h"
namespace AirModem { namespace Phy {
class Radio : private RH_ESP8266FSK {
public:
  struct Stats {
    uint32_t tx = 0, txAbort = 0, captureAbort = 0, sampleLateCycles = 0, syncCandidateOnly = 0;
    uint32_t prepareUs = 0, setupUs = 0, startLateUs = 0, demodUs = 0, acquireUs = 0;
    uint32_t eightCtrlValid = 0, eightPayloadValid = 0, eightPayloadFailed = 0, eightRefinedRecovery = 0;
    uint32_t shadowFrames = 0, shadowSymbols = 0, shadowMatch = 0;
    uint32_t shadowMismatch = 0, shadowAmbiguous = 0, shadowTimeMaxUs = 0;
  } stats;
  explicit Radio(uint8_t channel = 6) : RH_ESP8266FSK(channel) {}
  bool begin();
  bool prepare(const Frame& frame, int32_t offsetHz = 0);
  bool transmitAt(uint32_t atUs);
  bool receive(unsigned samples, Acquisition& result, uint32_t& capturedAtUs);
  bool captureIQ(int32_t* samples, unsigned count);
  bool testCarrier(uint16_t code = 0);
private:
  int32_t buffer[MAX_CAPTURE];
  Signal signal;
  bool ready = false, prepared = false;
  bool IRAM_ATTR transmitSamples(uint32_t atUs);
  bool IRAM_ATTR capture(unsigned count, uint32_t& atUs);
};
}}


// ===== FskPhase.h =====
#line 1 "FskPhase.h"
#ifndef ESP8266_FSK_PHASE_H
#define ESP8266_FSK_PHASE_H
#include <stdint.h>

namespace FskPhase {
inline uint32_t magnitude(int32_t x){return x<0?0u-uint32_t(x):uint32_t(x);}
inline void correlate(int32_t ai,int32_t aq,int32_t bi,int32_t bq,int32_t& cross,int32_t& dot){
  uint32_t peak=magnitude(ai)|magnitude(aq)|magnitude(bi)|magnitude(bq);
  if(!peak){cross=dot=0;return;}
  int shift=19-__builtin_clz(peak);
  if(shift>0){ai>>=shift;aq>>=shift;bi>>=shift;bq>>=shift;}
  cross=ai*bq-aq*bi;dot=ai*bi+aq*bq;
  peak=magnitude(cross)|magnitude(dot);
  shift=peak?20-__builtin_clz(peak):0;
  if(shift>0){cross>>=shift;dot>>=shift;}
}
// One turn is 65536. The bounded approximation is sufficient for tone slicing.
inline int32_t angle(int32_t y,int32_t x){
  if(!x && !y)return 0;
  int32_t ay=magnitude(y),a;
  if(x>=0)a=8192-8192*(x-ay)/(x+ay);
  else a=24576-8192*(x+ay)/(ay-x);
  return y<0?-a:a;
}
}
#endif


// ===== ModemFirmware.h =====
#line 1 "ModemFirmware.h"
namespace AirModem {
class Firmware {
public:
  explicit Firmware(uint8_t role) : modem(role, 1), role(role) {}
  void begin();
  void poll();
private:
  static constexpr uint32_t HALF = 140000, CYCLE = 2 * HALF;
  // The 8-FSK generator has more symbols per byte, but the TX deadline remains
  // fixed on the 280-ms TDD grid. Extra lead is NOT a change in radio timing.
  static constexpr uint32_t PREPARE = AIRMODEM_DATA_FSK_MODE >= 8 ? 26000 : 20000;
  static constexpr uint32_t MIN_TX_LEAD_US = 2800;
  static constexpr uint32_t UART_PRE_GUARD_US = 11000; // no UART traffic before RF boundary
  static constexpr uint32_t UART_XOFF_LEAD_US = 30000; // send XOFF while UART is still permitted
  static constexpr uint32_t UART_TX_DRAIN_US = 8500; // >= 8 chars @19200 plus headroom
  static constexpr uint32_t UART_SAFETY_US = 2500;
  Modem modem;
  Phy::Radio radio;
  uint8_t role, phase = 0, misses = 0;
  uint32_t next = 0, epoch = 0;
  int16_t txHz = 0, tuneHz = NO_TUNE;
  int32_t lastCalOffset = 0, lastCalUnit = 0;
  int16_t lastRemoteRequest = NO_TUNE;
  int32_t lastCalError = 0;
  uint32_t calAccepted = 0, calRejected = 0, calSlewed = 0, calCentered = 0;
  uint32_t ctrlOnlyCalApplied = 0, ctrlOnlyCalRejected = 0;
  uint32_t lastControlOnlyAtMs = 0, lastControlOnlyBoot = 0;
  int32_t lastControlOnlyOffset = 0;
  uint8_t consistentControlOnly = 0;
  bool ready = false, locked = false;
  void serial(uint32_t deadlineUs, bool permitted);
  uint32_t nextRadioBoundary() const;
  bool beforeTxQuiet(uint32_t txAt);
  uint32_t lastUartWriteUs = 0;
  uint32_t uartWindowOverruns = 0, txNotDrained = 0;
  // Observational counters only; no changes to TDD scheduling or RF modulation.
  bool diagnosticTimingTurn = false;
  uint32_t txAttempts = 0, txFailed = 0, txSlotMissed = 0;
  uint32_t txPrepLastUs = 0, txBudgetLastUs = 0;
  uint32_t txGuardUart = 0, txGuardTime = 0;
  uint32_t rxAttempts = 0, rxRejected = 0, rxWrongRole = 0, ctrlOnly = 0;
  // Diagnostics are multiplexed into the existing Serial TX, NEVER Serial1.
  void diagnosticTick();
  void diagnosticEvent(const char* name);
  void diagnosticQueue(const char* msg);
  void diagnosticDrain();
#if AIRMODEM_DIAGNOSTICS
  static constexpr unsigned DIAG_QUEUE_SIZE = 384;
  char diagBuf[DIAG_QUEUE_SIZE] = {};
  unsigned diagHead = 0, diagTail = 0;
  uint32_t diagDrops = 0;
#endif
  uint32_t lastDiagnostic = 0;
  uint32_t diagnosticMisses = 0;
  uint32_t consecutiveRxMisses = 0;
  uint32_t lastRxCount = 0, lastTxCount = 0, lastAbortCount = 0;
  uint32_t lastUartErrors = 0;
  int16_t lastReportedTune = NO_TUNE;
  State lastState = State::Idle;
  bool lastLocked = false;

  bool transmit(uint32_t at);
  bool receive(unsigned samples, uint32_t& start);
};
}


// ===== RHGenericDriver.cpp =====
#line 1 "RHGenericDriver.cpp"
RHGenericDriver::RHGenericDriver():_mode(RHModeInitialising),_thisAddress(RH_BROADCAST_ADDRESS),_txHeaderTo(RH_BROADCAST_ADDRESS),_txHeaderFrom(RH_BROADCAST_ADDRESS),_txHeaderId(0),_txHeaderFlags(0),_rxHeaderTo(0),_rxHeaderFrom(0),_rxHeaderId(0),_rxHeaderFlags(0),_promiscuous(false),_lastRssi(0),_rxBad(0),_rxGood(0),_txGood(0),_cad_timeout(0) {}
bool RHGenericDriver::init(){_mode=RHModeIdle;return true;}
void RHGenericDriver::waitAvailable(){while(!available()) yield();}
bool RHGenericDriver::waitPacketSent(){while(_mode==RHModeTx) yield(); return true;}
bool RHGenericDriver::waitPacketSent(uint16_t timeout){uint32_t t=millis(); while(_mode==RHModeTx){if((uint32_t)(millis()-t)>=timeout)return false; yield();} return true;}
bool RHGenericDriver::waitAvailableTimeout(uint16_t timeout){uint32_t t=millis(); while((uint32_t)(millis()-t)<timeout){if(available())return true; yield();} return false;}
bool RHGenericDriver::waitCAD(){return true;} bool RHGenericDriver::isChannelActive(){return false;} void RHGenericDriver::setCADTimeout(unsigned long t){_cad_timeout=t;}
void RHGenericDriver::setThisAddress(uint8_t a){_thisAddress=a;} void RHGenericDriver::setHeaderTo(uint8_t v){_txHeaderTo=v;} void RHGenericDriver::setHeaderFrom(uint8_t v){_txHeaderFrom=v;} void RHGenericDriver::setHeaderId(uint8_t v){_txHeaderId=v;} void RHGenericDriver::setHeaderFlags(uint8_t set,uint8_t clear){_txHeaderFlags&=~clear;_txHeaderFlags|=set;}
void RHGenericDriver::setPromiscuous(bool p){_promiscuous=p;} uint8_t RHGenericDriver::headerTo(){return _rxHeaderTo;} uint8_t RHGenericDriver::headerFrom(){return _rxHeaderFrom;} uint8_t RHGenericDriver::headerId(){return _rxHeaderId;} uint8_t RHGenericDriver::headerFlags(){return _rxHeaderFlags;} int16_t RHGenericDriver::lastRssi(){return _lastRssi;} RHGenericDriver::RHMode RHGenericDriver::mode(){return _mode;} void RHGenericDriver::setMode(RHMode m){_mode=m;} bool RHGenericDriver::sleep(){_mode=RHModeSleep;return true;} uint16_t RHGenericDriver::rxBad(){return _rxBad;} uint16_t RHGenericDriver::rxGood(){return _rxGood;} uint16_t RHGenericDriver::txGood(){return _txGood;}
void RHGenericDriver::printBuffer(const char* prompt,const uint8_t* buf,uint8_t len){Serial.print(prompt);for(uint8_t i=0;i<len;i++){Serial.print(' '); if(buf[i]<16)Serial.print('0'); Serial.print(buf[i],HEX);} Serial.println();}

bool RHGenericDriver::supportsAckBeacon(){return false;}
bool RHGenericDriver::sendAckBeacon(uint8_t,uint16_t){return false;}
bool RHGenericDriver::waitAckBeacon(uint16_t){return false;}


// ===== RH_ESP8266FSK.cpp =====
#line 1 "RH_ESP8266FSK.cpp"

static const uint32_t TONE1=0x600005B8u,TONE2=0x600005BCu,TONE3=0x600005C4u,PBUS_CMD=0x60000594u,PBUS_STAT=0x600005A0u,RX_CTRL=0x60009B08u;
static const uint32_t IQ_CTRL=0x6000057Cu,IQ_DCI=0x600005DCu,IQ_DCQ=0x600005E0u,IQ_E4=0x600005E4u;
static const uint32_t K_MASK=0x3FFu,SCALE_MASK=0x3FC00u,GATE_MASK=0x40000u,SCALE_SHIFT=10u,RX_STOP=0x08000000u;
static const uint32_t IQ_MODE=0x40000u,IQ_FIELD_MASK=0x3FFFCu,IQ_START=2u,IQ_ENABLE=1u,IQ_DONE31=0x80000000u,IQ_KEEP_MASK=0x7FF80000u;
static uint32_t cfgKeep=0;
static bool txSaved=false;
static uint16_t savedPbus[4];
static const uint8_t txSelectors[4]={2,7,1,6};
typedef uint32_t(*PbusReadFn)(uint32_t,uint32_t);
static PbusReadFn pbusRead=(PbusReadFn)0x400074D8u;
typedef void(*PbusModeFn)();
static PbusModeFn pbusWorkMode=(PbusModeFn)0x40007648u;
static PbusModeFn pbusDebugMode=(PbusModeFn)0x4000737Cu;
typedef void(*PbusForceFn)(uint32_t,uint32_t,uint32_t);
static PbusForceFn pbusForce=(PbusForceFn)0x4000747Cu;
static inline void memw_fsk(){__asm__ volatile("memw" ::: "memory");} static inline uint32_t rd32(uint32_t a){return *reinterpret_cast<volatile uint32_t*>(a);} static inline void wr32(uint32_t a,uint32_t v){*reinterpret_cast<volatile uint32_t*>(a)=v;} static inline uint8_t askCode(uint8_t ask){return (uint8_t)(0u-ask);} typedef void(*SetTxClkFn)(int); typedef uint8_t(*SetAnaScaleFn)(uint8_t); static SetTxClkFn set_txclk=(SetTxClkFn)0x4000650Cu; static SetAnaScaleFn set_ana_scale=(SetAnaScaleFn)0x4000678Cu;
RH_ESP8266FSK::RH_ESP8266FSK(uint8_t ch):_channel(ch),_toneZero(16),_toneOne(1016),_apwr(0),_ask(0),_bitUs(3000),_gapMs(120),_txRepeats(1),_pollBudgetUs(12000),_preambleBits(36),_calUs(40000),_minToneSepHz(40000),_maxCorrections(16),_lastCal{false,0,0,0,false,0},_bufValid(false),_bufLen(0){}
static const RH_ESP8266FSK::ModemConfig modemConfigs[] = {
  {4000,16,1016,0,0,120,1,36,40000,40000},
  {3000,16,1016,0,0,120,1,36,40000,40000},
  {2000,16,1016,0,0,120,1,36,40000,40000},
  {1500,16,1016,0,0,120,1,36,40000,40000},
  {1000,16,1016,0,0,120,1,36,40000,40000}
};
bool RH_ESP8266FSK::setModemConfig(ModemConfigChoice choice){if((uint8_t)choice>=sizeof(modemConfigs)/sizeof(modemConfigs[0]))return false;return setModemConfig(modemConfigs[(uint8_t)choice]);}
bool RH_ESP8266FSK::setModemConfig(const ModemConfig& c){if(_mode==RHModeTx||c.bitUs<100||c.bitUs>10000||c.toneZero>1023||c.toneOne>1023||c.toneZero==c.toneOne||c.preambleBits<24||c.preambleBits>64||(c.preambleBits&1)||c.calUs<20000||c.calUs>200000||c.minToneSepHz<1)return false;_bitUs=c.bitUs;_toneZero=c.toneZero;_toneOne=c.toneOne;_apwr=c.apwr;_ask=c.ask;_gapMs=c.frameGapMs;_txRepeats=c.txRepeats?c.txRepeats:1;_preambleBits=c.preambleBits;_calUs=c.calUs;_minToneSepHz=c.minToneSepHz;resetReceiver();return true;}
bool RH_ESP8266FSK::setBitRate(uint16_t bps){if(_mode==RHModeTx||!bps)return false;uint32_t us=((_fourFsk?2000000UL:1000000UL)+(bps/2))/bps;if(us<100||us>10000)return false;setBitUs(us);return true;}
void RH_ESP8266FSK::setTones(uint16_t z,uint16_t o){_toneZero=z;_toneOne=o;} void RH_ESP8266FSK::setPower(uint8_t a,uint8_t s){_apwr=a;_ask=s;} void RH_ESP8266FSK::setBitUs(uint32_t b){if(_mode!=RHModeTx&&b>=100&&b<=10000){_bitUs=b;resetReceiver();}} void RH_ESP8266FSK::setFrameGap(uint16_t m){_gapMs=m;} void RH_ESP8266FSK::setTxRepeats(uint8_t r){_txRepeats=r?r:1;} void RH_ESP8266FSK::setPollBudgetUs(uint16_t us){_pollBudgetUs=us?us:500;} void RH_ESP8266FSK::setMaxCorrections(uint8_t c){_maxCorrections=c;} void RH_ESP8266FSK::setTxFillUntil(uint32_t deadlineUs){_txFillUntil=deadlineUs;} uint32_t RH_ESP8266FSK::bitUs() const{return _bitUs;} uint16_t RH_ESP8266FSK::bitRate() const{return _bitUs?(_fourFsk?2000000UL:1000000UL)/_bitUs:0;} RH_ESP8266FSK::Calibration RH_ESP8266FSK::lastCalibration() const{return _lastCal;} uint8_t RH_ESP8266FSK::maxCorrections() const{return _maxCorrections;} uint8_t RH_ESP8266FSK::maxMessageLength(){return RH_ESP8266FSK_MAX_MESSAGE_LEN;}
bool RH_ESP8266FSK::init(){RHGenericDriver::init(); randomSeed(ESP.getCycleCount()); _radioReady=initRadioBase() && initRxPath();return _radioReady;}
bool pbusWriteFSK(uint8_t sel,uint8_t bank,uint16_t value){uint32_t cmd=rd32(PBUS_CMD);cmd&=0xFFFF0001u;cmd|=(uint32_t)(bank&3u)<<14;cmd|=(uint32_t)(value&0x1FFu)<<5;cmd|=(uint32_t)(sel&7u)<<2;cmd|=2u;wr32(PBUS_CMD,cmd);memw_fsk();uint32_t t0=micros();while(rd32(PBUS_STAT)&0x80000000u){if((uint32_t)(micros()-t0)>2500u){uint32_t r=rd32(PBUS_CMD);r&=~2u;wr32(PBUS_CMD,r);return false;} delay(0);}uint32_t r=rd32(PBUS_CMD);r&=~2u;wr32(PBUS_CMD,r);memw_fsk();return true;}
bool RH_ESP8266FSK::initRadioBase(){wifi_station_set_auto_connect(0); if(!wifi_set_opmode_current(STATION_MODE))return false; delay(60); wifi_station_disconnect(); if(!wifi_set_sleep_type(NONE_SLEEP_T))return false; if(!wifi_set_channel(_channel))return false; delay(20); return true;}
bool RH_ESP8266FSK::initTxPath(bool gate){
  if(!txSaved){
    for(uint8_t i=0;i<4;i++)savedPbus[i]=pbusRead(txSelectors[i],1);
    txSaved=true;
  }
  uint32_t r=rd32(RX_CTRL);r|=RX_STOP;wr32(RX_CTRL,r);memw_fsk();r=rd32(PBUS_CMD);r|=1u;wr32(PBUS_CMD,r);memw_fsk(); if(!(pbusWriteFSK(2,1,1)&&pbusWriteFSK(7,1,95)&&pbusWriteFSK(1,1,127)&&pbusWriteFSK(6,1,127)))return false; set_ana_scale(_apwr); set_txclk(1); r=rd32(TONE1);r&=~(K_MASK|SCALE_MASK|GATE_MASK);r|=_toneZero&K_MASK;r|=(uint32_t)askCode(_ask)<<SCALE_SHIFT;if(gate)r|=GATE_MASK;wr32(TONE1,r); r=rd32(TONE2);r&=~GATE_MASK;wr32(TONE2,r); r=rd32(TONE3);r&=~GATE_MASK;wr32(TONE3,r);memw_fsk(); return true;}
RH_ESP8266FSK::RfState RH_ESP8266FSK::rfState() const {
  return {(uint16_t)pbusRead(2,1),(uint16_t)pbusRead(3,1),(uint16_t)pbusRead(3,2),
    bool(rd32(PBUS_CMD)&1),bool(rd32(RX_CTRL)&RX_STOP),_apwr,_ask,_lastTxAskField};
}
void RH_ESP8266FSK::applyReceiveGain(){
  if(_rxGain<0){pbusWorkMode();return;}
  static const uint8_t words[7]={0x00,0x40,0x60,0x70,0x78,0x7c,0x7f};
  pbusDebugMode();
  // The saved Wi-Fi gain state may have powered down LNA/mixer stages.
  pbusForce(2,1,0x1fe);
  pbusForce(3,1,words[_rxGain]);
  pbusForce(3,2,(pbusRead(3,2)&0x1c7u)|uint32_t(_rxBbGain)<<3);
}
bool RH_ESP8266FSK::setReceiveGain(int8_t rf,uint8_t bb){
  if(_mode==RHModeTx || rf < -1 || rf>6 || bb>7)return false;
  _rxGain=rf;_rxBbGain=bb;applyReceiveGain();resetReceiver();return true;
}
bool RH_ESP8266FSK::setIqMode(uint8_t mode){
  if(_mode==RHModeTx || mode>1)return false;
  _iqMode=mode;restartReceive(true);return true;
}
bool RH_ESP8266FSK::initRxPath(){
  wr32(TONE1,rd32(TONE1)&~GATE_MASK);
  wr32(TONE2,rd32(TONE2)&~GATE_MASK);
  wr32(TONE3,rd32(TONE3)&~GATE_MASK);
  memw_fsk();
  set_txclk(0);
  if(txSaved){
    for(uint8_t i=0;i<4;i++)pbusWriteFSK(txSelectors[i],1,savedPbus[i]);
    txSaved=false;
  }
  if(!pbusWriteFSK(2,1,0x1fe))return false;
  pbusWorkMode();
  applyReceiveGain();
  cfgKeep=rd32(IQ_CTRL)&IQ_KEEP_MASK;
  return true;
}
void RH_ESP8266FSK::txTone(uint16_t tone){uint32_t r=rd32(TONE1);r=(r&~K_MASK)|(tone&K_MASK);wr32(TONE1,r);memw_fsk();}
uint8_t RH_ESP8266FSK::ham74Encode(uint8_t value){return ESP8266FSKCodec::encode(value);}
uint8_t RH_ESP8266FSK::ham74Decode(uint8_t code,uint8_t& corrected){return ESP8266FSKCodec::decode(code,corrected);}
uint16_t RH_ESP8266FSK::crc16(const uint8_t* data,uint8_t len){return ESP8266FSKCodec::crc16(data,len);}
// Legacy diagnostics do not provide addressed, CRC-checked acknowledgements.
bool RH_ESP8266FSK::supportsAckBeacon(){return false;}
bool RH_ESP8266FSK::sendAckBeacon(uint8_t,uint16_t){return false;}
bool RH_ESP8266FSK::waitAckBeacon(uint16_t){return false;}
void RH_ESP8266FSK::appendBit(bool bit){
  if(bit)_wire[_wireBits>>3]|=1u<<(7-(_wireBits&7));
  _wireBits++;
}
bool RH_ESP8266FSK::send(const uint8_t* data,uint8_t len){
  if(_txHeld)return false;
  return startFrame(data,len,false,false);
}
bool RH_ESP8266FSK::sendBurstFrame(const uint8_t* data,uint8_t len,bool last){
  return startFrame(data,len,_txHeld,!last);
}
void RH_ESP8266FSK::endTxBurst(){
  if(!_txHeld)return;
  _txHeld=false;_holdTx=false;
  initRxPath();resetReceiver();
  _nextSample=micros()+(uint32_t)_gapMs*1000;
}
bool RH_ESP8266FSK::startFrame(const uint8_t* data,uint8_t len,bool continuation,bool holdTx){
  if(!_radioReady||_mode==RHModeTx||(!data&&len)||len>maxMessageLength())return false;
  resetReceiver();
  uint8_t headers[4]={_txHeaderTo,_txHeaderFrom,_txHeaderId,_txHeaderFlags};
  _wireBits=FskFrame::encode(data,len,headers,_preambleBits,_wire,sizeof(_wire));
  if(!_wireBits)return false;
  if(!continuation && !initTxPath()){_txFillUntil=0;initRxPath();return false;}
  if(!continuation && _fourFsk)txTone(Fsk4::TONES[0]);
  _lastTxAskField=(rd32(TONE1)&SCALE_MASK)>>SCALE_SHIFT;
  _holdTx=holdTx;_txHeld=false;
  _mode=RHModeTx;_txSucceeded=false;_txStage=continuation?1:0;_txRepeatIndex=0;
  _wireIndex=0;_pilotIndex=0;_txDeadline=micros()+(continuation?0:_calUs);
  return true;
}
void RH_ESP8266FSK::serviceTx(){
  if(_mode!=RHModeTx)return;
  uint32_t now=micros();
  if((int32_t)(now-_txDeadline)<0)return;
  if(_txStage==0){
    if(_fourFsk){
      _pilotIndex++;
      if(_pilotIndex<4){txTone(Fsk4::TONES[_pilotIndex]);_txDeadline=now+_calUs;return;}
      _txStage=1;
    }else {txTone(_toneOne);_txStage=1;_txDeadline=now+_calUs;return;}
  }
  if(_txStage==3){
    if(!initTxPath()){_txFillUntil=0;initRxPath();_mode=RHModeIdle;_stats.txAborts++;return;}
    if(_fourFsk)txTone(Fsk4::TONES[0]);
    _txStage=0;_pilotIndex=0;_wireIndex=0;_txDeadline=micros()+_calUs;return;
  }
  if(_txStage==1){_txStage=2;_txDeadline=now;}
  if(!transmitBits()){
    _txFillUntil=0;initRxPath();_mode=RHModeIdle;_stats.txAborts++;return;
  }
  _txFillUntil=0;
  if(_holdTx){
    _txHeld=true;_txGood++;_txSucceeded=true;_mode=RHModeIdle;
    return;
  }
  initRxPath();
  if(++_txRepeatIndex<_txRepeats){_txStage=3;_txDeadline=micros()+(uint32_t)_gapMs*1000;return;}
  _txGood++;_txSucceeded=true;_mode=RHModeIdle;
  resetReceiver();
  _nextSample=micros()+(uint32_t)_gapMs*1000;
}
bool IRAM_ATTR RH_ESP8266FSK::transmitBits(){
  uint32_t savedPS;
  bool ok=true;
  __asm__ volatile("rsil %0, 15" : "=a"(savedPS) :: "memory");
  volatile uint32_t* tone=reinterpret_cast<volatile uint32_t*>(TONE1);
  uint32_t base=*tone&~K_MASK;
  for(;;){
    uint32_t now;
    do {now=micros();}while(int32_t(now-_txDeadline)<0);
    uint32_t late=now-_txDeadline;
    if(late>_stats.txLateUs)_stats.txLateUs=late;
    if(late>_bitUs/2){ok=false;break;}
    uint16_t selectedTone;
    if(_wireIndex<_wireBits){
      if(_fourFsk){
        uint8_t dibit=(_wire[_wireIndex>>3]>>(6-(_wireIndex&7)))&3;
        uint8_t index=dibit^(dibit>>1);
        // Immediate constants keep the interrupt-disabled loop out of flash.
        selectedTone=index==0?1012:index==1?1020:index==2?4:12;
        _wireIndex+=2;
      }else {
        bool bit=(_wire[_wireIndex>>3]>>(7-(_wireIndex&7)))&1;_wireIndex++;
        selectedTone=bit?_toneOne:_toneZero;
      }
    }else if(_txFillUntil && int32_t(_txDeadline-_txFillUntil)<0){
      _txFillBit=!_txFillBit;_stats.fillBits+=_fourFsk?2:1;
      selectedTone=_fourFsk?(_txFillBit?12:1012):(_txFillBit?_toneOne:_toneZero);
    }else break;
    *tone=base|selectedTone;
    __asm__ volatile("memw" ::: "memory");
    _txDeadline+=_bitUs;
  }
  __asm__ volatile("memw; wsr %0, ps; rsync" :: "a"(savedPS) : "memory");
  return ok;
}
void RH_ESP8266FSK::setMode(RHMode requested){
  // Only send() may enter TX: an enum change alone cannot start/stop RF.
  if(requested==RHModeTx)return;
  endTxBurst();
  if(_mode==RHModeTx){
    initRxPath();_txSucceeded=false;_stats.txAborts++;
    _txFillUntil=0;resetReceiver();_nextSample=micros()+(uint32_t)_gapMs*1000;
  }
  _mode=requested==RHModeRx?RHModeRx:RHModeIdle;
}
bool RH_ESP8266FSK::sleep(){
  setMode(RHModeIdle);
  return false; // PHY power-down is not implemented; never report fake sleep.
}
void RH_ESP8266FSK::poll(){if(_mode==RHModeTx)serviceTx();else available();}
bool RH_ESP8266FSK::waitPacketSent(){
  while(_mode==RHModeTx){serviceTx();yield();}
  return _txSucceeded;
}
bool RH_ESP8266FSK::waitPacketSent(uint16_t timeout){
  uint32_t started=millis();
  while(_mode==RHModeTx){
    if((uint32_t)(millis()-started)>=timeout){
      _txFillUntil=0;initRxPath();_mode=RHModeIdle;_txSucceeded=false;_stats.txAborts++;return false;
    }
    serviceTx();yield();
  }
  return _txSucceeded;
}
struct IqSample {uint32_t t;int32_t i,q;uint32_t e;};
static bool IRAM_ATTR __attribute__((noinline)) sampleIq(IqSample* s,uint8_t count,uint32_t cpuMHz,uint8_t mode){
  volatile uint32_t* ctrl=reinterpret_cast<volatile uint32_t*>(IQ_CTRL);
  uint32_t cfg=cfgKeep|((uint32_t)4<<2)|(mode?IQ_MODE:0),savedPS;
  // A short fixed-cadence burst keeps aliased tones independent of flash layout
  // and ISR latency. Restore the caller's interrupt state on every exit.
  __asm__ volatile("rsil %0, 15" : "=a"(savedPS) :: "memory");
  uint32_t next=ESP.getCycleCount();
  bool ok=true;
  for(uint8_t k=0;k<count;k++){
    *ctrl=cfg;*ctrl=cfg|IQ_ENABLE;
    __asm__ volatile("memw" ::: "memory");
    while(int32_t(ESP.getCycleCount()-next)<0){}
    s[k].t=ESP.getCycleCount();
    *ctrl=cfg|IQ_ENABLE|IQ_START;
    __asm__ volatile("memw" ::: "memory");
    while(!(*ctrl&IQ_DONE31)){
      if(uint32_t(ESP.getCycleCount()-s[k].t)>100*cpuMHz){ok=false;break;}
    }
    if(!ok)break;
    s[k].i=*reinterpret_cast<volatile int32_t*>(IQ_DCI);
    s[k].q=*reinterpret_cast<volatile int32_t*>(IQ_DCQ);
    s[k].e=*reinterpret_cast<volatile uint32_t*>(IQ_E4);
    *ctrl=cfg|IQ_ENABLE;*ctrl=cfg;
    __asm__ volatile("memw" ::: "memory");
    next=s[k].t+3*cpuMHz;
  }
  *ctrl=cfg;
  __asm__ volatile("memw; wsr %0, ps; rsync" :: "a"(savedPS) : "memory");
  return ok;
}
bool RH_ESP8266FSK::acquireFreq(FreqEst& out){
  static const uint8_t N=4;
  IqSample s[N];uint32_t cpuMHz=ESP.getCpuFreqMHz();
  if(!sampleIq(s,N,cpuMHz,_iqMode)){out.valid=false;return false;}
  uint32_t sampledAt=micros();
  int32_t ss=0,cc=0;uint32_t dtSum=0,dtMin=UINT32_MAX,dtMax=0;uint8_t pairs=0;uint64_t energy=0;
  int64_t sumI=0,sumQ=0;
  for(uint8_t k=0;k<N;k++){
    energy+=s[k].e;sumI+=s[k].i;sumQ+=s[k].q;
    s[k].i-=int32_t(_dcI>>8);s[k].q-=int32_t(_dcQ>>8);
  }
  // Remove the receiver's DC offset without following either FSK tone.
  _dcI+=(sumI*64-_dcI)>>7;_dcQ+=(sumQ*64-_dcQ)>>7;
  for(uint8_t k=1;k<N;k++){
    uint32_t dt=s[k].t-s[k-1].t;
    if(!dt || dt>100*cpuMHz)continue;
    if(dt<dtMin)dtMin=dt;
    if(dt>dtMax)dtMax=dt;
    int32_t cross,dot;
    FskPhase::correlate(s[k-1].i,s[k-1].q,s[k].i,s[k].q,cross,dot);
    if(!cross && !dot)continue;
    ss+=cross;cc+=dot;dtSum+=dt;pairs++;
  }
  if(pairs<2 || !dtSum){out.valid=false;return false;}
  out.t=sampledAt-(s[N-1].t-s[0].t)/(2*cpuMHz);out.e4=energy/N;
  out.hz=FskPhase::angle(ss,cc)*int32_t(15625*cpuMHz*pairs/dtSum)/1024;
  _stats.sampleStepNs=dtSum*1000/(cpuMHz*pairs);
  _stats.sampleJitterNs=(dtMax-dtMin)*1000/cpuMHz;
  _stats.lastEnergy=out.e4;_stats.lastHz=out.hz;
  _stats.iqSamples+=N;
  out.valid=true;return true;
}
void RH_ESP8266FSK::resetReceiver(){
  if(_candidateLen)_stats.candidateDrops++;
  _candidateLen=0;
  memset(_decoders,0,sizeof(_decoders));
  memset(_scores,0,sizeof(_scores));
  memset(_counts,0,sizeof(_counts));
  memset(_votes4,0,sizeof(_votes4));
  _training4N=0;_training4Pos=0;_nextTraining=micros();
  _trainingN=0;_trainingPos=0;_trained=false;
  _epoch=0;_bin=0;_nextSample=_nextTraining;
  if(_calMemoryMs && _lastCal.valid && (uint32_t)(millis()-_lastCal.atMs)<_calMemoryMs){
    _low=min(_lastCal.zeroHz,_lastCal.oneHz);
    _high=max(_lastCal.zeroHz,_lastCal.oneHz);
    _trained=true;_lockAt=micros();
  }
}
bool RH_ESP8266FSK::train(int32_t hz){
  if(_fourFsk)return trainFour(hz);
  _training[_trainingPos++]=hz;
  _trainingPos%=16;
  if(_trainingN<16)_trainingN++;
  if(_trainingN<16||(_trainingPos%4))return false;
  int32_t sorted[16];memcpy(sorted,_training,sizeof(sorted));
  for(uint8_t i=1;i<16;i++){int32_t v=sorted[i];int8_t j=i-1;while(j>=0&&sorted[j]>v){sorted[j+1]=sorted[j];j--;}sorted[j+1]=v;}
  int32_t lo=(sorted[3]+sorted[4])/2,hi=(sorted[11]+sorted[12])/2;
  int32_t sep=hi-lo;
  int32_t spread=sep/3;if(spread>20000)spread=20000;
  if(sep<_minToneSepHz||sorted[6]-sorted[1]>spread||sorted[14]-sorted[9]>spread)return false;
  bool firstLock=!_trained;
  if(!_trained || labs(lo-_low)>sep/4 || labs(hi-_high)>sep/4){
    memset(_decoders,0,sizeof(_decoders));
    memset(_scores,0,sizeof(_scores));memset(_counts,0,sizeof(_counts));
    _epoch=0;_bin=0;
  }
  _low=lo;_high=hi;_trained=true;if(firstLock)_lockAt=micros();
  return true;
}
bool RH_ESP8266FSK::trainFour(int32_t hz){
  _training4[_training4Pos++]=hz;_training4Pos%=Fsk4::TRAINING_SIZE;
  if(_training4N<Fsk4::TRAINING_SIZE)_training4N++;
  if(_training4N<32 || (_training4Pos%4))return false;
  int32_t centers[4];
  if(!Fsk4::calibrate(_training4,_training4N,_minToneSepHz,centers))return false;
  bool reset=!_trained;
  for(uint8_t k=0;k<4;k++)if(labs(centers[k]-_centers4[k])>_minToneSepHz/2)reset=true;
  if(reset){
    memset(_decoders,0,sizeof(_decoders));memset(_votes4,0,sizeof(_votes4));
    _epoch=0;_bin=0;
  }
  memcpy(_centers4,centers,sizeof(centers));
  _low=centers[0];_high=centers[3];
  if(!_trained)_lockAt=micros();
  _trained=true;return true;
}
bool RH_ESP8266FSK::decodeBit(Decoder& d,bool bit,uint8_t* raw,uint8_t* rawLen,bool firstInSymbol){
  Decoder::Result result=d.feed(bit,firstInSymbol,_fourFsk,_maxCorrections);
  if(result==Decoder::SYNC){_stats.syncs++;_lockAt=micros();}
  if(result==Decoder::CORRECTION_REJECT)_stats.correctionRejects++;
  if(result==Decoder::CRC_REJECT)_rxBad++;
  if(result!=Decoder::FRAME)return false;
  _stats.decodedCandidates++;
  memcpy(raw,d.raw,d.count);*rawLen=d.count;
  _lastCal={true,d.inverted?_high:_low,d.inverted?_low:_high,_high-_low,d.inverted,millis()};
  _lastCorrections=d.corrections;
  _lastReceiveUs=micros();
  return true;
}
bool RH_ESP8266FSK::receiveRaw(uint8_t* raw,uint8_t* rawLen){
  uint32_t now=micros();
  if(_candidateLen && uint32_t(now-_candidateAt)>=_bitUs)return finishCandidate(raw,rawLen);
  if((int32_t)(now-_nextSample)<0)return false;
  uint32_t period=27;
  _nextSample=now+period;
  FreqEst f;
  _stats.samples++;
  if(!acquireFreq(f)||!f.valid)return false;
  bool active=false;
  for(uint8_t i=0;i<8;i++)active|=_decoders[i].active;
  // A CRC-valid candidate must survive until its timing-phase comparison ends.
  if(!_candidateLen&&!active&&_trained&&(uint32_t)(micros()-_lockAt)>600000){resetReceiver();return false;}
  if(!active && !_candidateLen && int32_t(now-_nextTraining)>=0){
    _nextTraining=now+(_fourFsk?1000:2000);train(f.hz);
  }
  if(!_trained)return false;
  if(!_candidateLen&&(uint32_t)(micros()-_lockAt)>(uint32_t)(90+56*((RH_ESP8266FSK_MAX_MESSAGE_LEN+9)/4))*_bitUs){resetReceiver();return false;}
  if(!_epoch){_epoch=f.t;_bin=0;}
  uint32_t bin=(uint32_t)(f.t-_epoch)*8/_bitUs;
  if(bin-_bin>24){_stats.slowPolls++;resetReceiver();return false;}
  while(_bin<bin){
    Decoder& d=_decoders[_bin&7];
    bool decoded=false;
    if(_fourFsk){
      int8_t winner=Fsk4::decide(_votes4,_bin);
      if(winner>=0){
        uint8_t dibit=Fsk4::gray(uint8_t(winner));
        decoded=decodeBit(d,(dibit>>1)&1,raw,rawLen,true);
        if(!decoded)decoded=decodeBit(d,dibit&1,raw,rawLen,false);
      }else {d.active=false;d.shift=0;}
    }else {
      int16_t score=0;uint8_t count=0;
      for(uint8_t k=0;k<5;k++){uint8_t idx=(_bin-k)&7;score+=_scores[idx];count+=_counts[idx];}
      if(count)decoded=decodeBit(d,score>0,raw,rawLen);
      else {d.active=false;d.shift=0;}
    }
    if(decoded){
      // Compare adjacent timing phases before using FEC corrections as quality.
      if(!_candidateLen || _lastCorrections<_candidateCorrections){
        if(!_candidateLen)_candidateAt=_lastReceiveUs;
        _candidateLen=*rawLen;memcpy(_candidate,raw,*rawLen);
        _candidateCorrections=_lastCorrections;_candidateCal=_lastCal;
      }
      if(!_candidateCorrections)return finishCandidate(raw,rawLen);
    }
    _bin++;
    _scores[_bin&7]=0;_counts[_bin&7]=0;
    memset(_votes4[_bin&7],0,sizeof(_votes4[0]));
  }
  if(_fourFsk){
    int8_t tone=Fsk4::classify(f.hz,_centers4);
    if(tone>=0 && _votes4[_bin&7][tone]<255)_votes4[_bin&7][tone]++;
    return false;
  }
  int32_t sep=_high-_low,mid=_low+sep/2;
  if(f.hz>=_low-sep/3 && f.hz<=_high+sep/3){
    int32_t confidence=(f.hz-mid)*256/sep;
    if(confidence>128)confidence=128;
    if(confidence<-128)confidence=-128;
    _scores[_bin&7]+=(int16_t)confidence;
    _counts[_bin&7]++;
  }
  return false;
}
bool RH_ESP8266FSK::finishCandidate(uint8_t* raw,uint8_t* rawLen){
  *rawLen=_candidateLen;memcpy(raw,_candidate,_candidateLen);
  _lastCorrections=_candidateCorrections;_lastCal=_candidateCal;_lastReceiveUs=_candidateAt;
  _candidateLen=0;
  resetReceiver();
  return true;
}
bool RH_ESP8266FSK::available(){
  if(_txHeld)return false;
  if(_mode==RHModeTx){serviceTx();return false;}
  if(_bufValid)return true;
  _mode=RHModeRx;
  uint8_t raw[1+RH_ESP8266FSK_HEADER_LEN+RH_ESP8266FSK_MAX_MESSAGE_LEN+2],rawLen=0;
  uint32_t started=micros();bool received=false;
  // Keep acquisition and decoding together; the caller services serial in guards.
  uint32_t savedPS;
  __asm__ volatile("rsil %0, 15" : "=a"(savedPS) :: "memory");
  do {
    received=receiveRaw(raw,&rawLen);
  }while(!received && uint32_t(micros()-started)<900000
    && (uint32_t(micros()-started)<1000 || receivingFrame() || _candidateLen)
    && (!_receiveDeadline || int32_t(micros()-_receiveDeadline)<0));
  __asm__ volatile("memw; wsr %0, ps; rsync" :: "a"(savedPS) : "memory");
  uint32_t spent=micros()-started;_stats.polls++;_stats.workUs+=spent;
  if(spent>_stats.maxPollUs)_stats.maxPollUs=spent;
  if(!received){_mode=RHModeIdle;return false;}
  uint8_t payloadLen=raw[0];
  _rxHeaderTo=raw[1];_rxHeaderFrom=raw[2];_rxHeaderId=raw[3];_rxHeaderFlags=raw[4];
  _stats.crcFrames++;_stats.lastTo=_rxHeaderTo;_stats.lastFrom=_rxHeaderFrom;_stats.lastLength=payloadLen;
  if(!_promiscuous&&_rxHeaderTo!=_thisAddress&&_rxHeaderTo!=RH_BROADCAST_ADDRESS){
    _stats.addressRejects++;_mode=RHModeIdle;return false;
  }
  _bufLen=payloadLen-RH_ESP8266FSK_HEADER_LEN;memcpy(_buf,raw+5,_bufLen);
  _bufValid=true;_rxGood++;_mode=RHModeIdle;return true;
}
bool RH_ESP8266FSK::recv(uint8_t* buf,uint8_t* len){
  if(!available())return false;
  if(buf&&len){
    if(*len>_bufLen)*len=_bufLen;
    memcpy(buf,_buf,*len);
  }
  _bufValid=false;
  return true;
}


// ===== ModemSignal.cpp =====
#line 1 "ModemSignal.cpp"
#include <math.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
namespace AirModem { namespace Phy {
static void whiten(uint8_t* bytes) {
  unsigned lfsr = 0x1ff;
  for (unsigned n = 0; n < PACKET_BYTES; ++n) for (unsigned b = 0; b < 8; ++b) {
    bytes[n] ^= (lfsr & 1) << b;
    lfsr = (lfsr >> 1) | (((lfsr ^ (lfsr >> 5)) & 1) << 8);
  }
}
void makeFrame(const Packet& packet, Frame& frame) {
  frame.mode=AIRMODEM_DATA_FSK_MODE;
  if (AIRMODEM_DATA_FSK_MODE == 16) {
    encodeSixteen(packet,frame.sixteen);
    return;
  }
  if (AIRMODEM_DATA_FSK_MODE == 8) {
    encodeEight(packet,frame.eight);
    return;
  }
  if (AIRMODEM_DATA_FSK_MODE == 2) {
    memset(frame.bytes,0,sizeof(frame.bytes));
    uint8_t compact[TWO_FSK_BYTES]={};
    if (!encodeTwo(packet,compact)) return;
    for (unsigned n=0;n<320;++n) {
      const unsigned sym=n<160?n:n+24;
      const bool bit=(compact[n/8]>>(7-(n%8)))&1;
      const uint8_t dibit=bit?1:3; // outer tones +3 / -3
      frame.bytes[sym/4] |= dibit << (6-2*(sym%4));
    }
    return;
  }
  uint8_t raw[PACKET_BYTES] = {};
  encode(packet, raw); whiten(raw);
  memcpy(frame.bytes, raw, 40);
  for (unsigned n = 0; n < 6; ++n) frame.bytes[40 + n] = uint8_t(SYNC >> (40 - 8 * n));
  memcpy(frame.bytes + 46, raw + 40, 40);
}
bool readFrame(const Frame& frame, Packet& packet, bool& dataValid) {
  uint8_t raw[PACKET_BYTES]; memcpy(raw, frame.bytes, 40); memcpy(raw + 40, frame.bytes + 46, 40);
  whiten(raw);
  if (!decodeControl(raw, packet)) { dataValid = false; return false; }
  dataValid = (get32(raw + 76) == crc32(raw, 76));
  if (dataValid && !decode(raw, packet)) return false;
  return true; // A CRC16-validated control frame; dataValid distinguishes payload CRC32.
}
static const int16_t phaseTable[257] = {
  0,41,81,122,163,204,244,285,326,367,407,448,489,529,570,610,
  651,692,732,773,813,854,894,935,975,1015,1056,1096,1136,1177,1217,1257,
  1297,1337,1377,1417,1457,1497,1537,1577,1617,1656,1696,1736,1775,1815,1854,1894,
  1933,1973,2012,2051,2090,2129,2168,2207,2246,2285,2324,2363,2401,2440,2478,2517,
  2555,2594,2632,2670,2708,2746,2784,2822,2860,2897,2935,2973,3010,3047,3085,3122,
  3159,3196,3233,3270,3307,3344,3380,3417,3453,3490,3526,3562,3599,3635,3670,3706,
  3742,3778,3813,3849,3884,3920,3955,3990,4025,4060,4095,4129,4164,4199,4233,4267,
  4302,4336,4370,4404,4438,4471,4505,4539,4572,4605,4639,4672,4705,4738,4771,4803,
  4836,4869,4901,4933,4966,4998,5030,5062,5094,5125,5157,5188,5220,5251,5282,5313,
  5344,5375,5406,5437,5467,5498,5528,5559,5589,5619,5649,5679,5708,5738,5768,5797,
  5826,5856,5885,5914,5943,5972,6000,6029,6058,6086,6114,6142,6171,6199,6227,6254,
  6282,6310,6337,6365,6392,6419,6446,6473,6500,6527,6554,6580,6607,6633,6660,6686,
  6712,6738,6764,6790,6815,6841,6867,6892,6917,6943,6968,6993,7018,7043,7068,7092,
  7117,7141,7166,7190,7214,7238,7262,7286,7310,7334,7358,7381,7405,7428,7451,7475,
  7498,7521,7544,7566,7589,7612,7635,7657,7679,7702,7724,7746,7768,7790,7812,7834,
  7856,7877,7899,7920,7942,7963,7984,8005,8026,8047,8068,8089,8110,8131,8151,8172,
  8192
};
int32_t phaseHz(int32_t cross, int32_t dot) {
  uint32_t x = FskPhase::magnitude(dot), y = FskPhase::magnitude(cross);
  uint32_t high = x > y ? x : y, low = x > y ? y : x;
  if (!high) return 0;
  if (high > 65535) {
    unsigned shift = 16 - __builtin_clz(high);
    low >>= shift; high >>= shift;
  }
  uint32_t ratio = (low << 16) / high;
  unsigned index = ratio >> 8;
  int32_t angle = phaseTable[index];
  if (index < 256) angle += ((phaseTable[index+1] - angle) * int32_t(ratio & 255) + 128) / 256;
  if (y > x) angle = 16384 - angle;
  if (dot < 0) angle = 32768 - angle;
  if (cross < 0) angle = -angle;
  return angle * 75 / 128;
}
int32_t packIq(int16_t i, int16_t q) { return int32_t(uint16_t(i) | (uint32_t(uint16_t(q)) << 16)); }
int32_t demodulate(int32_t* samples, unsigned count) {
  if (!samples || !count || count > MAX_CAPTURE) return 0;
  int32_t si = 0, sq = 0;
  for (unsigned n = 0; n < count; ++n) {
    si += int16_t(samples[n] & 0xFFFF); sq += int16_t(uint32_t(samples[n]) >> 16);
  }
  int32_t dcI = si / int32_t(count), dcQ = sq / int32_t(count);
  int32_t lastI = int16_t(samples[0] & 0xFFFF) - dcI;
  int32_t lastQ = int16_t(uint32_t(samples[0]) >> 16) - dcQ;
  samples[0] = 0;
  int32_t sumCross = 0, sumDot = 0;
  for (unsigned n = 1; n < count; ++n) {
    int32_t i = int16_t(samples[n] & 0xFFFF) - dcI;
    int32_t q = int16_t(uint32_t(samples[n]) >> 16) - dcQ;
    int32_t cross, dot;
    FskPhase::correlate(lastI, lastQ, i, q, cross, dot);
    sumCross += cross; sumDot += dot;
    samples[n] = phaseHz(cross, dot);
    lastI = i; lastQ = q;
  }
  // Center circular phase differences before the linear matched filter. Without
  // this, CFO near Nyquist turns valid FSK into +/- Fs/2 discontinuities.
  int32_t offset = phaseHz(sumCross, sumDot);
  for (unsigned n = 1; n < count; ++n) {
    int32_t hz = samples[n] - offset;
    if (hz > int32_t(SAMPLE_RATE / 2)) hz -= SAMPLE_RATE;
    if (hz < -int32_t(SAMPLE_RATE / 2)) hz += SAMPLE_RATE;
    samples[n] = hz;
  }
  return offset;
}
Signal::Signal() {
  const double pi = 3.14159265358979323846, beta = 0.2;
  double sum = 0;
  double taps[RRC_TAPS];
  for (unsigned k = 0; k < RRC_TAPS; ++k) {
    double t = (int(k) - int(RRC_DELAY)) / double(SPS), v;
    if (fabs(t) < 1e-9) v = 1 + beta * (4 / pi - 1);
    else if (fabs(fabs(4 * beta * t) - 1) < 1e-9)
      v = beta / sqrt(2.0) * ((1 + 2 / pi) * sin(pi / (4 * beta)) + (1 - 2 / pi) * cos(pi / (4 * beta)));
    else v = (sin(pi * t * (1 - beta)) + 4 * beta * t * cos(pi * t * (1 + beta))) /
                 (pi * t * (1 - 16 * beta * beta * t * t));
    taps[k] = v; sum += v;
  }
  int32_t absoluteSum = 0;
  for (unsigned k = 0; k < RRC_TAPS; ++k) {
    rx[k] = int16_t(lround(taps[k] * 32768.0 / sum));
    tx[k] = lround(taps[k] * SPS / sum * 648.0 * 65536 * 1024 / 78125);
    // The 8-FSK data now use the ORIGINAL eight-tone frequency spacing:
    // -7,-5,-3,-1,+1,+3,+5,+7 in the same units as legacy 4-FSK.
    // No 7x compression of the frequencies or transmit coefficients.
    absoluteSum += rx[k] < 0 ? -rx[k] : rx[k];
  }
  // Together with bounded discriminator output this proves int32 FIR sums fit.
  assert(absoluteSum <= 65536);
}
void Signal::waveform(const Frame& frame, int32_t q16[SLOT_SAMPLES], int32_t offsetHz) const {
  int32_t offset = int64_t(offsetHz) * 65536 / 78125;
  if (frame.mode == 8 || frame.mode == 16) {
    // Preserve legacy 4-FSK frequency-unit spacing, as in the older Fsk8.h.
    // SYNC/CTRL continue to use -3,+3; payload uses eight tones -7..+7.
    // 8-FSK tone index = inverse Gray(data bits), not Gray(data bits).
    int8_t levels[SYMBOLS];
    for (unsigned s = 0; s < SYMBOLS; ++s) {
      if (s >= SYNC_AT && s < SYNC_AT + 24) {
        levels[s] = syncExtreme(s - SYNC_AT);
      } else {
        const unsigned logical = s < SYNC_AT ? s : s - 24;
        if (logical < 208) { // CRC16-protected 26-byte control, 2-FSK
          const unsigned bit = ((frame.mode==16 ? frame.sixteen[logical >> 3] : frame.eight[logical >> 3]) >> (7-(logical&7))) & 1u;
          levels[s] = int8_t(bit ? 3 : -3);
        } else {
          const unsigned bitpos = 208 + (logical-208)*(frame.mode==16 ? 4 : 3);
          if (frame.mode==16) {
            const uint8_t v = uint8_t((encodedBit8(frame.sixteen,bitpos)<<3) |
               (encodedBit8(frame.sixteen,bitpos+1)<<2) |
               (encodedBit8(frame.sixteen,bitpos+2)<<1) | encodedBit8(frame.sixteen,bitpos+3));
            levels[s]=int8_t(2*int(ungray16(v))-15);
          } else {
            const uint8_t v = uint8_t((encodedBit8(frame.eight,bitpos)<<2) |
               (encodedBit8(frame.eight,bitpos+1)<<1) | encodedBit8(frame.eight,bitpos+2));
            levels[s]=int8_t(2*int(ungray8(v))-7);
          }
        }
      }
    }
    for (unsigned n = 0; n < SLOT_SAMPLES; ++n) {
      int32_t acc = 0; // 15 * sum(abs(tx taps)) ~ 111M, safely within int32.
      int first = (int(n)-int(FIRST_CENTER)+int(RRC_DELAY)-int(RRC_TAPS)+1+int(SPS)-1)/int(SPS);
      if (first < 0) first = 0;
      for (unsigned s = unsigned(first); s < SYMBOLS; ++s) {
        int tap = int(n)-int(FIRST_CENTER)-int(s*SPS)+int(RRC_DELAY);
        if (tap < 0) break;
          if (tap < int(RRC_TAPS)) acc += int32_t(levels[s])*tx[tap];
      }
      // Bounded 32-bit FIR accumulation, preserving the fast TX path.
      q16[n] = offset + int32_t((acc + (acc < 0 ? -512 : 512)) / 1024);
    }
    return;
  }
  for (unsigned n = 0; n < SLOT_SAMPLES; ++n) {
    int32_t level = 0;
    int first = (int(n) - int(FIRST_CENTER) + int(RRC_DELAY) - int(RRC_TAPS) + 1 + int(SPS) - 1) / int(SPS);
    if (first < 0) first = 0;
    for (unsigned s = unsigned(first); s < SYMBOLS; ++s) {
      int tap = int(n) - int(FIRST_CENTER) - int(s * SPS) + int(RRC_DELAY);
      if (tap < 0) break;
      if (tap < int(RRC_TAPS)) {
        uint8_t d = (frame.bytes[s / 4] >> (6 - 2 * (s % 4))) & 3;
        const int8_t level4 = (s >= SYNC_AT && s < SYNC_AT + 24) ? syncExtreme(s - SYNC_AT) : symbol(d);
        level += level4 * tx[tap];
      }
    }
    q16[n] = offset + (level + (level < 0 ? -512 : 512)) / 1024;
  }
}
// Passive observation: same tone spacing as the historical eight-tone grid,
// {-7,-5,-3,-1,+1,+3,+5,+7} * unit. SYNC uses only +/-3 => indexes 2,5.
// The monitor never participates in decoding or calibration.
void Signal::inspectEightTones(unsigned start, int32_t offset, int32_t unit,
                               const int8_t* knownSync, unsigned count) {
  const uint32_t began = micros();
  const int32_t mag = unit < 0 ? -unit : unit;
  if (!mag) return;
  const int32_t dir = unit < 0 ? -1 : 1;
  uint32_t match = 0, ambiguous = 0;
  for (unsigned i = 0; i < count; ++i) {
    const int32_t value = (int32_t(filtered[start + (SYNC_AT + i) * SPS]) - offset) * dir;
    // Choose the nearest of eight physical tone centers by distance.
    uint32_t best = UINT32_MAX, second = UINT32_MAX;
    unsigned index = 0;
    for (unsigned t = 0; t < 8; ++t) {
      const int32_t center = (int32_t(t) * 2 - 7) * mag;
      const int32_t err = value - center;
      const uint32_t d = uint32_t(err < 0 ? -err : err);
      if (d < best) { second = best; best = d; index = t; }
      else if (d < second) second = d;
    }
    const unsigned expected = unsigned((int(knownSync[i]) + 7) / 2);
    if (index == expected) ++match;
    if (second - best < uint32_t(mag / 5)) ++ambiguous;
  }
  ++shadowFrames; shadowSymbols += count; shadowMatch += match;
  shadowMismatch += count - match; shadowAmbiguous += ambiguous;
  const uint32_t cost = micros() - began;
  if (cost > shadowTimeMaxUs) shadowTimeMaxUs = cost;
}
bool Signal::acquire(const int32_t* hz, unsigned count, Acquisition& result) {
  lastSyncCandidate = false;
  if (!hz || count > MAX_CAPTURE || count < (SYMBOLS - 1) * SPS + RRC_TAPS) return false;
  for (unsigned n = 0; n < count; ++n) if (hz[n] < -19200 || hz[n] > 19200) return false;
  for (unsigned n = 0; n < count; ++n) {
    int32_t sum = 0;
    unsigned taps = n + 1 < RRC_TAPS ? n + 1 : RRC_TAPS;
    for (unsigned k = 0; k < taps; ++k) sum += hz[n - k] * rx[k];
    sum /= 32768;
    filtered[n] = int16_t(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
  }
  int8_t syncLevels[24];
  int32_t sumX = 0, sumXX = 0;
  for (unsigned i = 0; i < 24; ++i) {
    syncLevels[i] = syncExtreme(i);
    sumX += syncLevels[i]; sumXX += syncLevels[i] * syncLevels[i];
  }
  const int32_t denominator = 24 * sumXX - sumX * sumX;
  uint32_t bestError = 160000;
  bool found = false; bool fullFound = false;
  unsigned shadowBestStart = 0; int32_t shadowBestOffset = 0, shadowBestUnit = 0;
  for (unsigned start = RRC_TAPS - 1; start + (SYMBOLS - 1) * SPS < count; ++start) {
    int32_t sumY = 0, sumXY = 0;
    for (unsigned i = 0; i < 24; ++i) {
      int32_t y = filtered[start + (SYNC_AT + i) * SPS];
      sumY += y; sumXY += y * syncLevels[i];
    }
    int32_t unit = (24 * sumXY - sumX * sumY) / denominator;
    int32_t magnitude = unit < 0 ? -unit : unit;
    if (magnitude < 400 || magnitude > 950) continue;
    int32_t offset = (sumY - unit * sumX) / 24;
    if (offset < -16000 || offset > 16000) continue;
    uint32_t sumSquared = 0;
    for (unsigned i = 0; i < 24; ++i) {
      int32_t error = filtered[start + (SYNC_AT + i) * SPS] - offset - unit * syncLevels[i];
      if (error < -2 * magnitude || error > 2 * magnitude) { sumSquared = UINT32_MAX; break; }
      sumSquared += error * error;
    }
    if (sumSquared >= uint32_t(unit * unit * 384 / 100)) continue;
    lastSyncCandidate = true;
    uint32_t mse = uint64_t(sumSquared) * 1000000 / uint32_t(24 * unit * unit);
    Frame frame;
    for (unsigned i = 0; i < SYMBOLS; ++i) {
      int32_t value = filtered[start + i * SPS] - offset;
      if (unit < 0) value = -value;
      uint8_t d = value >= 0 ? (value > 2 * magnitude ? 1 : 0) : (value < -2 * magnitude ? 3 : 2);
      frame.bytes[i / 4] |= d << (6 - 2 * (i % 4));
    }
    Packet packet; bool dataValid = false;
    uint8_t decodedMode = 4;
    bool controlValid=readFrame(frame,packet,dataValid);
    if (!controlValid) {
      uint8_t compact[TWO_FSK_BYTES]={};
      for (unsigned n=0;n<320;++n) {
        const unsigned sym=n<160?n:n+24;
        // Soft-limited decision on the two outer FSK tones; never allow inner tones.
        int32_t value=filtered[start+sym*SPS]-offset;
        if (unit<0) value=-value;
        if (value>=0) compact[n/8] |= uint8_t(1u << (7-(n%8)));
      }
      controlValid=decodeTwo(compact,packet,dataValid);
      if (controlValid) decodedMode = 2;
    }
    if (!controlValid) {
      uint8_t eight[EIGHT_FSK_BYTES]={};
      for (unsigned n=0;n<320;++n) {
        const unsigned sym=n<160?n:n+24;
        int32_t value=int32_t(filtered[start+sym*SPS])-offset;
        if (unit<0) value=-value;
        if (n<208) {
          if (value>=0) eight[n>>3] |= uint8_t(1u << (7-(n&7)));
        } else {
          // Same 2*unit tone spacing as the original 4-FSK. Center zero
          // is between tone 3(-1) and tone 4(+1). Round to nearest index.
          const int32_t denom = 2*magnitude;
          int32_t v = (value + 8*magnitude) / denom;
          if (v < 0) v = 0;
          if (v > 7) v = 7;
          const uint8_t bits = gray8(uint8_t(v));
          const unsigned bitpos=208+3*(n-208);
          setEncodedBit8(eight,bitpos,(bits>>2)&1);
          setEncodedBit8(eight,bitpos+1,(bits>>1)&1);
          setEncodedBit8(eight,bitpos+2,bits&1);
        }
      }
      controlValid=decodeEight(eight,packet,dataValid);
      if (controlValid) {
        decodedMode = 8;
        ++eightCtrlValid;
        if (dataValid) ++eightPayloadValid;
        else {
          ++eightPayloadFailed;
          // CTRL was validated with CRC16, so all 208 2-FSK header bits
          // are known. Re-estimate frequency shift/spacing from those bits
          // AND the 24 SYNC symbols (232 pilots) instead of just 24 SYNCs.
          // This lowers 8-FSK slicing bias without changing TDD timing.
          int64_t sx=0, sy=0, sxx=0, sxy=0;
          for (unsigned n=0; n<208+24; ++n) {
            const unsigned sym = n<208 ? (n<160?n:n+24) : SYNC_AT+(n-208);
            const int x = n<208 ? ((eight[n>>3]>>(7-(n&7)))&1 ? 3:-3)
                                : int(syncExtreme(n-208));
            const int32_t y = filtered[start + sym*SPS];
            sx += x; sy += y; sxx += x*x; sxy += int64_t(x)*y;
          }
          const int64_t ns=232, d=ns*sxx-sx*sx;
          if (d>0) {
            const int32_t fitUnit=int32_t((ns*sxy-sx*sy)/d);
            const int32_t fitOffset=int32_t((sy-int64_t(fitUnit)*sx)/ns);
            const int32_t unitDiff=absCal(fitUnit-unit);
            const int32_t offDiff=absCal(fitOffset-offset);
            if ((fitUnit<0)==(unit<0) && unitDiff<magnitude/2 && offDiff<1200) {
              uint8_t refined[EIGHT_FSK_BYTES]={};
              memcpy(refined,eight,26); // CRC16-checked CTRL is unchanged
              const int32_t m=absCal(fitUnit);
              for (unsigned n=208;n<320;++n) {
                const unsigned sym=n+24;
                int32_t value=int32_t(filtered[start+sym*SPS])-fitOffset;
                if (fitUnit<0) value=-value;
                int32_t v=(value+8*m)/(2*m);
                if (v<0) v=0;
                if (v>7) v=7;
                const uint8_t bits=gray8(uint8_t(v));
                const unsigned bitpos=208+3*(n-208);
                setEncodedBit8(refined,bitpos,(bits>>2)&1);
                setEncodedBit8(refined,bitpos+1,(bits>>1)&1);
                setEncodedBit8(refined,bitpos+2,bits&1);
              }
              Packet recovery; bool good=false;
              if (decodeEight(refined,recovery,good) && good) {
                ++eightRefinedRecovery;
                ++eightPayloadValid;
                packet=recovery; dataValid=true;
              }
              // Use improved frequency estimate for calibration only if
              // all the data are verified, never from speculative 8-FSK bits.
              if (dataValid) { offset=fitOffset; unit=fitUnit; }
            }
          }
        }
      }
    }
    if (!controlValid) {
      uint8_t sixteen[SIXTEEN_FSK_BYTES]={};
      for (unsigned n=0;n<320;++n) {
        const unsigned sym=n<160?n:n+24;
        int32_t value=int32_t(filtered[start+sym*SPS])-offset;
        if (unit<0) value=-value;
        if (n<208) {
          if (value>=0) sixteen[n>>3]|=uint8_t(1u<<(7-(n&7)));
        } else {
          // Centers -15,-13,...,-1,+1,...,+15 times signed FSK unit.
          int32_t v=(value+16*magnitude)/(2*magnitude);
          if(v<0)v=0; if(v>15)v=15;
          const uint8_t bits=gray16(uint8_t(v));
          const unsigned bp=208+4*(n-208);
          setEncodedBit8(sixteen,bp,(bits>>3)&1u);
          setEncodedBit8(sixteen,bp+1,(bits>>2)&1u);
          setEncodedBit8(sixteen,bp+2,(bits>>1)&1u);
          setEncodedBit8(sixteen,bp+3,bits&1u);
        }
      }
      controlValid=decodeSixteen(sixteen,packet,dataValid);
      if(controlValid) decodedMode=16;
    }
    if (!controlValid) continue;
    if (fullFound && !dataValid) continue; // Never replace a fully valid frame with control-only.
    if ((!dataValid || fullFound) && mse >= bestError) continue;
    result.packet = packet; result.dataValid = dataValid; result.mode = decodedMode;
    result.firstCenter = int32_t(start) - int32_t(RRC_DELAY);
    result.offsetHz = offset; result.unitHz = unit;
    shadowBestStart = start; shadowBestOffset = offset; shadowBestUnit = unit;
    bestError = mse; found = true; if (dataValid) fullFound = true;
  }
  if (found) inspectEightTones(shadowBestStart, shadowBestOffset, shadowBestUnit, syncLevels, 24);
  return found;
}
}}


// ===== ModemLink.cpp =====
#line 1 "ModemLink.cpp"
namespace AirModem {
void Link::clearStream() {
  tx.clear(); rx.clear(); pending = 0; attempts = 0; txSeq = rxSeq = 0; peerCredit = 0;
}
bool Link::dial(uint32_t session, uint32_t now, bool cable) {
  if (state_ != State::Idle || !session) return false;
  clearStream(); session_ = session; peerBoot = 0; started = lastPeer = now; cable_ = cable;
  reason_ = Reason::None; state_ = State::Dialing; return true;
}
bool Link::answer(uint32_t now) {
  if (state_ != State::Ringing) return false;
  state_ = State::Answering; started = lastPeer = now; return true;
}
void Link::close(Reason reason, uint32_t now) {
  if (state_ == State::Idle || state_ == State::Closing) return;
  clearStream(); reason_ = reason; started = now; state_ = State::Closing;
  events |= Disconnect; ++stats.disconnects;
}
void Link::tick(uint32_t now) {
  if (state_ == State::Closing && uint32_t(now - started) >= 2000) {
    state_ = State::Idle; session_ = 0; peerBoot = 0;
  }
  if (state_ == State::Connected && uint32_t(now - lastPeer) >= lossTimeout) close(Reason::Timeout, now);
  if ((state_ == State::Dialing || state_ == State::Answering || state_ == State::Ringing) &&
      uint32_t(now - started) >= connectTimeout) close(Reason::Timeout, now);
  if (state_ == State::Ringing && uint32_t(now - lastRing) >= 2000) { events |= Ring; lastRing = now; }
}
Packet Link::outgoing(uint32_t now) {
  tick(now);
  if (state_ == State::Connected && pending && peerCredit >= pending && attempts >= 64) close(Reason::Retries, now);
  Packet p; p.role = role_; p.boot = boot_; p.session = session_;
  p.seq = txSeq; p.ack = rxSeq; p.credit = uint16_t(rx.free());
  switch (state_) {
    case State::Idle: p.type = Type::Idle; p.session = 0; break;
    case State::Dialing: p.type = cable_ ? Type::CableOpen : Type::Open; break;
    case State::Ringing: p.type = Type::Ring; break;
    case State::Answering: p.type = Type::Accept; break;
    case State::Closing: p.type = Type::Hangup; break;
    case State::Connected:
      p.type = Type::Data;
      if (!pending && tx.size() && peerCredit) {
        unsigned n = tx.size(); if (n > LOCAL_TX_PAYLOAD) n = LOCAL_TX_PAYLOAD; if (n > peerCredit) n = peerCredit;
        pending = uint8_t(n); attempts = 0;
      }
      if (pending && peerCredit >= pending) {
        p.length = pending;
        for (unsigned n = 0; n < pending; ++n) p.data[n] = tx.peek(n);
        if (attempts++) ++stats.retries;
        ++stats.dataTx;
      } else if (pending || tx.size()) ++stats.busy;
      break;
  }
  return p;
}
bool Link::incoming(const Packet& p, uint32_t now) {
  if (!valid(p) || p.role == role_) { ++stats.rejected; return false; }
  ++stats.frames;
  // Simultaneous cable requests converge on role A's session. AT calls retain
  // their existing answer policy; an active session is never displaced.
  bool yieldCable = p.type == Type::CableOpen && cable_ && state_ == State::Dialing && role_ == 2;
  if (((p.type == Type::Open || p.type == Type::CableOpen) && state_ == State::Idle) || yieldCable) {
    clearStream(); session_ = p.session; peerBoot = p.boot; started = lastPeer = lastRing = now;
    cable_ = p.type == Type::CableOpen;
    state_ = State::Ringing; reason_ = Reason::None; events |= Ring; return true;
  }
  if (p.type == Type::Idle) {
    if (state_ == State::Connected || state_ == State::Answering)
      close(p.boot == peerBoot ? Reason::Remote : Reason::Restart, now);
    return true;
  }
  if (p.session != session_ || state_ == State::Idle || state_ == State::Closing) { ++stats.rejected; return false; }
  if (peerBoot && p.boot != peerBoot) { close(Reason::Restart, now); return false; }
  if (!peerBoot) {
    if (state_ != State::Dialing || (p.type != Type::Ring && p.type != Type::Accept && p.type != Type::Busy)) return false;
    peerBoot = p.boot;
  }
  lastPeer = now;
  if (p.type == Type::Hangup || p.type == Type::Busy) {
    close(p.type == Type::Busy ? Reason::Busy : Reason::Remote, now); return true;
  }
  if ((state_ == State::Dialing && p.type == Type::Accept) ||
      (state_ == State::Answering && p.type == Type::Data)) {
    state_ = State::Connected; peerCredit = p.credit; events |= Connect;
  }
  if (state_ != State::Connected || p.type != Type::Data) return true;
  peerCredit = p.credit;
  if (pending && attempts && p.ack == uint16_t(txSeq + 1)) {
    tx.drop(pending); stats.txBytes += pending; pending = 0; attempts = 0; ++txSeq;
  }
  if (!p.length) return true;
  if (p.seq == uint16_t(rxSeq - 1)) { ++stats.duplicates; return true; }
  if (p.seq != rxSeq || rx.free() < p.length) { ++stats.rejected; return false; }
  for (unsigned n = 0; n < p.length; ++n) rx.push(p.data[n]);
  stats.rxBytes += p.length; ++rxSeq; return true;
}
}


// ===== AirModem.cpp =====
#line 1 "AirModem.cpp"
namespace AirModem {
uint32_t Modem::nonce() {
  random_ ^= random_ << 13; random_ ^= random_ >> 17; random_ ^= random_ << 5;
  if (!random_) random_ = 1;
  return random_;
}
void Modem::updateFlow(uint32_t now) {
  unsigned queued = link.queued() + cablePending.size();
  if (queued > stats.maxQueued) stats.maxQueued = queued;
  bool stop = inputStopped;
  // Reserve half the offline buffer for bytes already in the UART/USB path.
  // An absent peer alone must not block a short PC cable handshake.
  if (scheduleStopped || queued >= 512) stop = true;
  else if (queued <= 256) stop = false;
  if (stop != inputStopped) {
    inputStopped = stop;
    flowPending = stop ? 0x13 : 0x11;
    if (stop) ++stats.xoff; else ++stats.xon;
  }
  if (uint32_t(now - lastFlow) >= 1000 && flowPending < 0)
    flowPending = inputStopped ? 0x13 : 0x11;
}
void Modem::tick(uint32_t now) {
  link.tick(now);
  uint8_t events = link.takeEvents();
  if (events & Ring) {
    if (link.cable()) link.answer(now);
    else link.close(Reason::Busy, now);
  }
  if (events & Connect) sessionEstablished = true;
  if (events & Disconnect) {
    // Keep preconnection bytes across dial timeouts, but never replay an old
    // established session into a new one after the radio stream was aborted.
    if (sessionEstablished) {
      stats.cableDropped += cablePending.size();
      cablePending.clear();
    }
    sessionEstablished = false;
  }
  if (link.state() == State::Idle) link.dial(nonce(), now, true);
  while (cablePending.size() && link.write(cablePending.peek(0))) cablePending.drop(1);
  updateFlow(now);
}
void Modem::input(uint8_t b, uint32_t now) {
  tick(now);
  if (b == 0x13) { if (!hostStopped) ++stats.hostPauses; hostStopped = true; return; }
  if (b == 0x11) { hostStopped = false; return; }
  ++stats.inputBytes;
  stats.inputCrc = crcByte(stats.inputCrc, b);
  if (!cablePending.push(b)) {
    ++stats.overflows;
    stats.cableDropped += cablePending.size() + 1;
    cablePending.clear();
    link.close(Reason::Overflow, now);
  }
  tick(now);
}
int Modem::output(uint32_t now) {
  tick(now);
  // Local flow controls must still pass when the PC has paused its payload.
  if (flowPending >= 0) { int b = flowPending; flowPending = -1; lastFlow = now; return b; }
  if (hostStopped) return -1;
  int b = online() ? link.read() : -1;
  if (b >= 0) {
    ++stats.outputBytes;
    stats.outputCrc = crcByte(stats.outputCrc, uint8_t(b));
  }
  return b;
}
void Modem::uartFailure(uint32_t now) {
  ++stats.uartErrors;
  stats.cableDropped += cablePending.size();
  cablePending.clear();
  link.close(Reason::Uart, now);
  tick(now);
}
}


// ===== ModemRadio.cpp =====
#line 1 "ModemRadio.cpp"
#include <math.h>

namespace AirModem { namespace Phy {
namespace {
constexpr uint32_t TONE = 0x600005B8u, IQCTRL = 0x6000057Cu;
constexpr uint32_t IREG = 0x600005DCu, QREG = 0x600005E0u;
constexpr uint32_t GATE = 0x40000u, FIELDS = 0x7FFFFu;
inline void fence() { __asm__ volatile("memw" ::: "memory"); }
inline volatile uint32_t& reg(uint32_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
inline int16_t shrink(int32_t value) {
  value /= 1024;
  return value > 32767 ? 32767 : value < -32768 ? -32768 : int16_t(value);
}
}
bool Radio::begin() {
  if (ready || ESP.getCpuFreqMHz() != 160) return false;
  setTones(0, 1);
  ready = init();
  return ready;
}
bool Radio::prepare(const Frame& frame, int32_t offsetHz) {
  if (!ready || offsetHz < -12000 || offsetHz > 12000) return false;
  uint32_t began = micros();
  signal.waveform(frame, buffer, offsetHz);
  uint32_t spent = micros() - began;
  if (spent > stats.prepareUs) stats.prepareUs = spent;
  prepared = true;
  return true;
}
bool Radio::transmitAt(uint32_t atUs) {
  if (!ready || !prepared) return false;
  prepared = false;
  uint32_t began = micros();
  bool ok = initTxPath(false);
  uint32_t spent = micros() - began;
  if (spent > stats.setupUs) stats.setupUs = spent;
  if (ok) ok = transmitSamples(atUs);
  reg(TONE) &= ~GATE; fence();
  if (!initRxPath()) { ready = false; ok = false; }
  if (ok) ++stats.tx; else ++stats.txAbort;
  return ok;
}
bool IRAM_ATTR Radio::transmitSamples(uint32_t atUs) {
  int32_t lateStart = int32_t(micros() - atUs);
  if (lateStart > 0 && uint32_t(lateStart) > stats.startLateUs) stats.startLateUs = lateStart;
  if (lateStart > 100) return false;
  uint32_t ps;
  __asm__ volatile("rsil %0, 15" : "=a"(ps) :: "memory");
  while (int32_t(micros() - atUs) < 0) {}
  volatile uint32_t* tone = reinterpret_cast<volatile uint32_t*>(TONE);
  uint32_t base = *tone & ~FIELDS;
  uint32_t deadline = ESP.getCycleCount(), fraction = 0, last = deadline;
  int32_t code = 0;
  int32_t error = 0;
  bool ok = true;
  // Fractional tone generation adapted from SP8ESA's MIT-licensed ditherBurst.
  // The integrated frequency error is kept across all shaped waveform samples.
  for (unsigned n = 0; n < SLOT_SAMPLES; ++n) {
    deadline += 4166; fraction += 25600;
    if (fraction >= 38400) { fraction -= 38400; ++deadline; }
    const int32_t q = buffer[n];
    int32_t nearest = (q + 32768) >> 16;
    bool enabled = n >= 12 && n < SLOT_SAMPLES - 12;
    unsigned edge = n < SLOT_SAMPLES / 2 ? n : SLOT_SAMPLES - 1 - n;
    unsigned ask = edge <= 12 ? 127 : edge < 44 ? (44 - edge) * 4 : 0;
    uint32_t amplitude = enabled ? GATE | (uint32_t(uint8_t(0u - ask)) << 10) : 0;
    do {
      uint32_t now = ESP.getCycleCount(), dt = now - last;
      if (dt > 8192) { ok = false; break; }
      int32_t errNow = error + (q - code * 65536) * int32_t(dt);
      if (errNow > (20 << 16) || errNow < -(20 << 16)) {
        bool up = errNow > 0;
        int32_t residual = q - nearest * 65536;
        int32_t rate = up ? 65536 - residual : 65536 + residual;
        int32_t length = (up ? errNow : -errNow) / rate;
        if (length > 4000) length = 4000;
        int32_t remaining = int32_t(deadline - now);
        if (length > remaining) length = remaining;
        if (length > 0) {
          *tone = base | (uint32_t(nearest + (up ? 1 : -1)) & 1023u) | amplitude;
          fence();
          uint32_t pulseAt = ESP.getCycleCount();
          dt = pulseAt - last;
          if (dt > 8192) { ok = false; break; }
          error += (q - code * 65536) * int32_t(dt);
          code = nearest + (up ? 1 : -1);
          while (int32_t(ESP.getCycleCount() - pulseAt) < length) {}
          last = pulseAt;
        }
      }
      *tone = base | (uint32_t(nearest) & 1023u) | amplitude;
      fence();
      uint32_t writtenAt = ESP.getCycleCount();
      dt = writtenAt - last;
      if (dt > 8192) { ok = false; break; }
      error += (q - code * 65536) * int32_t(dt);
      if (error > (1 << 29)) error = 1 << 29;
      if (error < -(1 << 29)) error = -(1 << 29);
      code = nearest;
      last = writtenAt;
    } while (int32_t(ESP.getCycleCount() - deadline) < 0);
    uint32_t late = ESP.getCycleCount() - deadline;
    if (late > stats.sampleLateCycles) stats.sampleLateCycles = late;
    if (!ok || late > 1600) { ok = false; break; }
  }
  *tone &= ~GATE; fence();
  __asm__ volatile("wsr %0, ps; rsync" :: "a"(ps) : "memory");
  return ok;
}
bool IRAM_ATTR Radio::capture(unsigned count, uint32_t& atUs) {
  volatile uint32_t* ctrl = reinterpret_cast<volatile uint32_t*>(IQCTRL);
  const uint32_t cfg = (*ctrl & 0x7FF80000u) | (127u << 2) | 0x40000u;
  uint32_t ps;
  __asm__ volatile("rsil %0, 15" : "=a"(ps) :: "memory");
  atUs = micros();
  uint32_t next = ESP.getCycleCount(), fraction = 0;
  bool ok = true;
  for (unsigned n = 0; n < count; ++n) {
    *ctrl = cfg; *ctrl = cfg | 1; fence();
    while (int32_t(ESP.getCycleCount() - next) < 0) {}
    uint32_t started = ESP.getCycleCount(), late = started - next;
    if (late > stats.sampleLateCycles) stats.sampleLateCycles = late;
    if (late > 800) { ok = false; break; }
    *ctrl = cfg | 3; fence();
    while (!(*ctrl & 0x80000000u)) {
      if (ESP.getCycleCount() - started > 3200) { ok = false; break; }
    }
    if (!ok) break;
    uint16_t i = uint16_t(shrink(int32_t(reg(IREG))));
    uint16_t q = uint16_t(shrink(int32_t(reg(QREG))));
    buffer[n] = int32_t(i | (uint32_t(q) << 16));
    *ctrl = cfg; fence();
    next += 4166; fraction += 25600;
    if (fraction >= 38400) { fraction -= 38400; ++next; }
  }
  *ctrl = cfg; fence();
  __asm__ volatile("wsr %0, ps; rsync" :: "a"(ps) : "memory");
  return ok;
}
bool Radio::testCarrier(uint16_t code) {
  if (!ready || (code != 0 && code != 4)) return false;
  prepared = false;
  bool ok = initTxPath(false);
  if (ok) {
    reg(TONE) = (reg(TONE) & ~FIELDS) | GATE | code;
    fence();
    delay(2000);
  }
  reg(TONE) &= ~GATE; fence();
  if (!initRxPath()) { ready = false; ok = false; }
  return ok;
}
bool Radio::captureIQ(int32_t* samples, unsigned count) {
  prepared = false;
  if (!ready || !samples || !count || count > MAX_CAPTURE) return false;
  uint32_t atUs;
  if (!capture(count, atUs)) { ++stats.captureAbort; return false; }
  memcpy(samples, buffer, count * sizeof(*samples));
  return true;
}
bool Radio::receive(unsigned samples, Acquisition& out, uint32_t& capturedAtUs) {
  prepared = false;
  if (!ready || samples > MAX_CAPTURE || samples < 1200) return false;
  if (!capture(samples, capturedAtUs)) { ++stats.captureAbort; return false; }
  uint32_t began = micros();
  int32_t coarseHz = demodulate(buffer, samples);
  uint32_t spent = micros() - began;
  if (spent > stats.demodUs) stats.demodUs = spent;
  began = micros();
  bool got = signal.acquire(buffer, samples, out);
  if (!got && signal.lastSyncCandidate) ++stats.syncCandidateOnly;
  if (got) {
    out.offsetHz += coarseHz;
    if (out.offsetHz > int32_t(SAMPLE_RATE / 2)) out.offsetHz -= SAMPLE_RATE;
    if (out.offsetHz < -int32_t(SAMPLE_RATE / 2)) out.offsetHz += SAMPLE_RATE;
  }
  stats.eightCtrlValid = signal.eightCtrlValid;
  stats.eightPayloadValid = signal.eightPayloadValid;
  stats.eightPayloadFailed = signal.eightPayloadFailed;
  stats.eightRefinedRecovery = signal.eightRefinedRecovery;
  stats.shadowFrames = signal.shadowFrames;
  stats.shadowSymbols = signal.shadowSymbols;
  stats.shadowMatch = signal.shadowMatch;
  stats.shadowMismatch = signal.shadowMismatch;
  stats.shadowAmbiguous = signal.shadowAmbiguous;
  stats.shadowTimeMaxUs = signal.shadowTimeMaxUs;
  spent = micros() - began;
  if (spent > stats.acquireUs) stats.acquireUs = spent;
  return got;
}
}}


// ===== ModemFirmware.cpp =====
#line 1 "ModemFirmware.cpp"
#include <new>


namespace AirModem {
void Firmware::begin() {
  Serial.setRxBufferSize(2048);
  Serial.begin(AIRMODEM_UART_BAUD, SERIAL_8N1);
  Serial.setDebugOutput(false);
  system_set_os_print(0);
  modem.~Modem(); new (&modem) Modem(role, os_random());
  ready = radio.begin();
  // At startup allow a full preparation lead before the first A TX.
  // Preserve the proven 2/4-FSK startup offset.
  next = micros() + (AIRMODEM_DATA_FSK_MODE >= 8 ? PREPARE + 6000 : 20000);
#if AIRMODEM_DIAGNOSTICS
  char boot[96];
  snprintf(boot, sizeof(boot), "[BOOT][ROLE=%c][PHY=%dFSK-4800][SYNC=2FSK-EXTREMES][CTRL=2FSK-CRC16][UART=19200][XON-XOFF=ON]\r\n", role == 1 ? 'A' : 'B', AIRMODEM_DATA_FSK_MODE);
  diagnosticQueue(boot);
  // External reset / watchdog / power-on information is reported at next safe UART window.
  // ESP.getResetReason() is explanatory, not a reliable DTR/RTS detector.
  String reason = ESP.getResetReason();
  char resetMsg[160];
  snprintf(resetMsg, sizeof(resetMsg), "[RESET-REASON=%s]\r\n", reason.c_str());
  diagnosticQueue(resetMsg);
#endif
}
uint32_t Firmware::nextRadioBoundary() const {
  if (role == 1) {
    if (phase == 0) return next - PREPARE;
    return next;
  }
  if (!locked) return micros(); // searching for sync has radio priority
  return phase == 1 ? next - PREPARE : next;
}
void Firmware::serial(uint32_t deadlineUs, bool permitted) {
  if (!permitted) return;
  // The UART must have enough time to finish every queued character BEFORE RF.
  if (int32_t(deadlineUs - micros()) <= int32_t(UART_TX_DRAIN_US + UART_SAFETY_US)) return;
  bool uartError = Serial.hasOverrun(); uartError |= Serial.hasRxError();
  if (uartError) { modem.uartFailure(millis()); diagnosticEvent("UART-ERROR"); }
  uint32_t began = micros();
  for (unsigned n = 0; n < 12 && Serial.available(); ++n) {
    if (int32_t(deadlineUs - micros()) <= int32_t(UART_TX_DRAIN_US + UART_SAFETY_US)) break;
    modem.input(uint8_t(Serial.read()), millis());
    if (uint32_t(micros() - began) > 1400) break;
  }
  modem.tick(millis());
  // 8 chars maximum per service, leaving guaranteed drain time before RF.
  unsigned sent = 0;
  while (sent < 8 && Serial.availableForWrite() >= 120) {
    if (int32_t(deadlineUs - micros()) <= int32_t(UART_TX_DRAIN_US + UART_SAFETY_US)) break;
    int b = modem.output(millis());
    if (b < 0) break;
    Serial.write(uint8_t(b));
    lastUartWriteUs = micros();
    ++sent;
  }
  // Diagnostics are low-priority and must fit the same deterministic window.
  if (int32_t(deadlineUs - micros()) > int32_t(UART_TX_DRAIN_US + UART_SAFETY_US))
    diagnosticDrain();
  modem.radioStats.tx = radio.stats.tx;
  modem.radioStats.aborts = radio.stats.txAbort + radio.stats.captureAbort;
}
bool Firmware::beforeTxQuiet(uint32_t txAt) {
  // UART TX must be fully idle before the RF clock takes over.
  // No Serial.flush(): the TDD slot is never held up by a PC terminal.
  if (Serial.availableForWrite() < 128 ||
      int32_t(micros() - lastUartWriteUs) < int32_t(AIRMODEM_UART_CHAR_US + 1000)) {
    ++txNotDrained; ++txGuardUart;
    diagnosticEvent("TX-GUARD-UART");
    return false;
  }
  if (int32_t(txAt - micros()) < int32_t(MIN_TX_LEAD_US)) {
    ++txGuardTime;
    diagnosticEvent("TX-GUARD-TIME");
    return false;
  }
  return true;
}
bool Firmware::transmit(uint32_t at) {
  ++txAttempts;
  const uint32_t began = micros();
  Packet packet = modem.link.outgoing(millis());
  packet.txHz = txHz; packet.tuneHz = tuneHz;
  Phy::Frame frame; Phy::makeFrame(packet, frame);
  if (!radio.prepare(frame, txHz)) { ++txFailed; diagnosticEvent("TX-PREP-FAILED"); return false; }
  txPrepLastUs = micros() - began;
  txBudgetLastUs = int32_t(at - began) > 0 ? at - began : 0; // initial TX lead
  // RF setup and modulation get exclusive use of the critical timing window.
  if (!beforeTxQuiet(at)) { ++txFailed; return false; }
  delayMicroseconds(2200);
  bool ok = radio.transmitAt(at);
  if (!ok) { ++txFailed; diagnosticEvent("TX-RADIO-FAILED"); }
  return ok;
}
bool Firmware::receive(unsigned samples, uint32_t& start) {
  ++rxAttempts;
  Phy::Acquisition got; uint32_t capturedAt;
  if (!radio.receive(samples, got, capturedAt)) { ++rxRejected; return false; }
  if (got.packet.role == role) { ++rxWrongRole; return false; }
  start = capturedAt + int32_t(int64_t(got.firstCenter - int32_t(Phy::FIRST_CENTER)) * 1000000 / Phy::SAMPLE_RATE);
  modem.radioStats.offset = got.offsetHz; modem.radioStats.unit = got.unitHz;
  if (got.dataValid) {
    // Calibration and ARQ/data delivery only from a FULLY CRC32-validated packet.
    // A complete CRC32-valid packet authorizes RF feedback.  CTRL-only
    // receptions may synchronize time, but never adjust RF frequency.
    lastCalOffset = got.offsetHz;
    lastCalUnit = got.unitHz;
    if (validCalMeasurement(got.offsetHz, got.unitHz)) {
      ++calAccepted;
      const int32_t target = got.offsetHz < 0 ? -CAL_TARGET_ABS_HZ : CAL_TARGET_ABS_HZ;
      lastCalError = got.offsetHz - target;
      tuneHz = tuneRequest(got.packet.txHz, got.offsetHz, got.unitHz);
      if (absCal(lastCalError) <= CAL_DEADBAND_HZ) ++calCentered;
    } else {
      ++calRejected;
      lastCalError = 0;
      tuneHz = NO_TUNE; // Never feed back an uncertain phase/frequency reading.
    }
    lastRemoteRequest = got.packet.tuneHz;
    if (got.packet.tuneHz != NO_TUNE) {
      const int16_t corrected = boundedTxCorrection(txHz, got.packet.tuneHz);
      if (corrected != got.packet.tuneHz) ++calSlewed;
      txHz = corrected;
    }
    modem.radioStats.shift = txHz;
    ++modem.radioStats.rx;
    modem.link.incoming(got.packet, millis()); modem.tick(millis());
  } else {
    // The 8-FSK DATA CRC32 failed, but the 2-FSK CTRL CRC16 was validated.
    // Use ONLY frequency/timing fields, never ACK/seq/credit or payload.
    ++ctrlOnly;
    if (got.mode == 8 && validCalMeasurement(got.offsetHz,got.unitHz)) {
      const uint32_t nowMs=millis();
      const bool consistent = consistentControlOnly != 0 &&
          uint32_t(nowMs-lastControlOnlyAtMs) < 3000 &&
          lastControlOnlyBoot == got.packet.boot &&
          absCal(lastControlOnlyOffset-got.offsetHz) < 2400;
      consistentControlOnly = consistent ? 2 : 1;
      lastControlOnlyAtMs=nowMs;
      lastControlOnlyBoot=got.packet.boot;
      lastControlOnlyOffset=got.offsetHz;
      if (consistentControlOnly >= 2) {
        // Smaller step than for a fully validated frame: extra safety
        // against an isolated accidental CRC16 control acceptance.
        lastCalOffset=got.offsetHz;
        lastCalUnit=got.unitHz;
        const int32_t target=got.offsetHz<0?-CAL_TARGET_ABS_HZ:CAL_TARGET_ABS_HZ;
        lastCalError=got.offsetHz-target;
        tuneHz=tuneRequest(got.packet.txHz,got.offsetHz,got.unitHz);
        if (got.packet.tuneHz!=NO_TUNE) {
          int32_t delta=int32_t(got.packet.tuneHz)-txHz;
          delta=clampCal(delta,300);
          txHz=int16_t(clampCal(int32_t(txHz)+delta,12000));
        }
        lastRemoteRequest=got.packet.tuneHz;
        modem.radioStats.shift=txHz;
        ++ctrlOnlyCalApplied;
      }
    } else if (got.mode == 8) ++ctrlOnlyCalRejected;
  }
  consecutiveRxMisses = 0;
  if (diagnosticMisses) {
    diagnosticEvent("SYNC-RETROUVE");
    diagnosticMisses = 0;
  }
  return true;
}
void Firmware::poll() {
  if (!ready) { delay(1); return; }
  uint32_t now = micros();
  // Decide RF events FIRST, before any serial processing.
  uint32_t boundary = nextRadioBoundary();
  bool guard = (role == 2 && !locked) ||
               int32_t(boundary - now) <= int32_t(UART_PRE_GUARD_US);
  modem.schedulePause((role == 2 && !locked) ||
      int32_t(boundary - now) <= int32_t(UART_XOFF_LEAD_US), millis());
  if (role == 1) {
    if (phase == 0 && int32_t(now - (next - PREPARE)) >= 0) {
      // A is the time master. Never move the reference epoch because of
      // slow 8-FSK modulation or UART activity. Skip an expired slot and
      // resume on the original 280-ms lattice, never retry in 28 ms.
      while (int32_t(now - next) >= 0) {
        next += CYCLE;
        ++txSlotMissed;
      }
      epoch = next;
      if (int32_t(epoch - now) >= int32_t(MIN_TX_LEAD_US)) transmit(epoch);
      else { ++txSlotMissed; diagnosticEvent("TX-DEADLINE-MISSED"); }
      phase = 1; next = epoch + HALF - 4000;
    } else if (phase == 1 && int32_t(now - next) >= 0) {
      uint32_t start; receive(3200, start); phase = 0; next = epoch + CYCLE;
    }
  } else if (!locked) {
    // SLAVE ACQUISITION: RX-only, until a full CRC-validated A frame is detected.
    // This avoids unsynchronised beacon TX and samples every possible A phase.
    uint32_t start;
    if (receive(Phy::MAX_CAPTURE, start)) {
      epoch = start; locked = true; misses = 0;
      if (int32_t(epoch + HALF - micros()) >= int32_t(PREPARE)) {
        phase = 1; next = epoch + HALF;
      } else {
        // Too late for this B slot: listen at the NEXT A slot.
        phase = 0; next = epoch + CYCLE - 4000;
      }
    }
  } else if (phase == 1 && int32_t(now - (next - PREPARE)) >= 0) {
    if (int32_t(next - now) >= int32_t(MIN_TX_LEAD_US)) transmit(next);
    else { ++txSlotMissed; diagnosticEvent("TX-DEADLINE-MISSED"); }
    phase = 0; next = epoch + CYCLE - 4000;
  } else if (phase == 0 && int32_t(now - next) >= 0) {
    uint32_t start;
    if (receive(3200, start)) {
      epoch = start; misses = 0; phase = 1; next = epoch + HALF;
    } else {
      epoch += CYCLE; next = epoch - 4000; // next A TX, not a second cycle later
      ++diagnosticMisses;
      if (misses == 0) diagnosticEvent("PERTE-SYNC");
      if (++misses >= 6) {
        locked = false; misses = 0; phase = 0;
        diagnosticEvent("TDD-UNLOCK");
        // Re-enter RX-only acquisition. A is never slowed or paused by this.
      }
    }
  }
  // Only AFTER RF processing do UART, diagnostics, and housekeeping get time.
  now = micros(); boundary = nextRadioBoundary();
  guard = (role == 2 && !locked) ||
          int32_t(boundary - now) <= int32_t(UART_PRE_GUARD_US);
  modem.schedulePause((role == 2 && !locked) ||
      int32_t(boundary - now) <= int32_t(UART_XOFF_LEAD_US), millis());
  if (!guard) {
    uint32_t start = micros();
    serial(boundary, true);
    if (uint32_t(micros() - start) > 2500) ++uartWindowOverruns;
  }
  diagnosticTick();
  yield();
}
void Firmware::diagnosticQueue(const char* msg) {
#if AIRMODEM_DIAGNOSTICS
  // Nonblocking: discard a complete report if the diagnostic FIFO is full.
  unsigned n = strlen(msg);
  unsigned free = (diagTail + DIAG_QUEUE_SIZE - diagHead - 1) % DIAG_QUEUE_SIZE;
  if (n > free) { ++diagDrops; return; }
  for (unsigned i = 0; i < n; ++i) {
    diagBuf[diagHead] = msg[i];
    diagHead = (diagHead + 1) % DIAG_QUEUE_SIZE;
  }
#else
  (void)msg;
#endif
}
void Firmware::diagnosticDrain() {
#if AIRMODEM_DIAGNOSTICS
  // Bound traffic per poll and leave headroom for flow-control/data bytes.
  // Do not block on the 19200-baud UART (~521 us per 10-bit character).
  unsigned n = 0;
  while (diagTail != diagHead && n < 4 && Serial.availableForWrite() >= 124) {
    Serial.write(uint8_t(diagBuf[diagTail]));
    diagTail = (diagTail + 1) % DIAG_QUEUE_SIZE;
    ++n;
  }
#endif
}
void Firmware::diagnosticEvent(const char* name) {
#if AIRMODEM_DIAGNOSTICS
  char line[100];
  snprintf(line, sizeof(line), "[%lums][%c][%s]\r\n",
           (unsigned long)millis(), role == 1 ? 'A' : 'B', name);
  diagnosticQueue(line);
#else
  (void)name;
#endif
}
void Firmware::diagnosticTick() {
#if AIRMODEM_DIAGNOSTICS
  char line[240];
  State s = modem.link.state();
  if (s != lastState) {
    snprintf(line, sizeof(line), "[LINK-STATE=%u][REASON=%u]\r\n",
             unsigned(s), unsigned(modem.link.reason()));
    diagnosticQueue(line);
    if (s == State::Idle && lastState != State::Idle) diagnosticEvent("DECONNEXION");
    lastState = s;
  }
  if (locked != lastLocked && role == 2) {
    diagnosticEvent(locked ? "TDD-LOCK" : "TDD-UNLOCK");
    lastLocked = locked;
  }
  if (tuneHz != lastReportedTune) {
    if (lastReportedTune != NO_TUNE &&
        (int32_t(tuneHz) > int32_t(lastReportedTune) + 200 ||
         int32_t(tuneHz) < int32_t(lastReportedTune) - 200)) {
      snprintf(line, sizeof(line), "[CALIBRATION-HZ=%d]\r\n", int(tuneHz));
      diagnosticQueue(line);
    }
    lastReportedTune = tuneHz;
  }
  uint32_t now = millis();
  if (uint32_t(now - lastDiagnostic) >= 5000) {
    lastDiagnostic = now;
    snprintf(line, sizeof(line),
      "[DIAG][PHY=%uFSK-4800][VITESSE-MAX=%u][RX=%lu][TX=%lu][ABORTS=%lu][SYNC-MISS=%lu][LOCK=%u][OFFSET-HZ=%ld][TUNE-HZ=%d][UART-ERRORS=%lu][XOFF=%lu][XON=%lu][TX-QUEUE=%u][RX-QUEUE=%u][DIAG-DROPS=%lu]\r\n",
      unsigned(AIRMODEM_DATA_FSK_MODE),
      unsigned(AIRMODEM_DATA_FSK_MODE == 16 ? 19200 : AIRMODEM_DATA_FSK_MODE == 8 ? 14400 : AIRMODEM_DATA_FSK_MODE == 4 ? 9600 : 4800),
      (unsigned long)modem.radioStats.rx, (unsigned long)modem.radioStats.tx,
      (unsigned long)modem.radioStats.aborts, (unsigned long)diagnosticMisses,
      unsigned(role == 1 || locked), (long)modem.radioStats.offset,
      int(tuneHz), (unsigned long)modem.stats.uartErrors,
      (unsigned long)modem.stats.xoff, (unsigned long)modem.stats.xon,
      unsigned(modem.link.queued()), unsigned(modem.link.available()),
      (unsigned long)diagDrops);
    diagnosticQueue(line);
    // A separate bounded report avoids changing the established DIAG layout.
    // It is queued (nonblocking), and may be dropped if UART diagnostics are busy.
    snprintf(line, sizeof(line),
      "[CAL][TX-APPLIED=%d][REMOTE-REQ=%d][LOCAL-REQ=%d][RAW-OFFSET=%ld][UNIT=%ld][ERR-HZ=%ld][ACCEPT=%lu][REJECT=%lu][SLEW=%lu][CENTERED=%lu]\r\n",
      int(txHz), int(lastRemoteRequest), int(tuneHz), (long)lastCalOffset, (long)lastCalUnit,
      (long)lastCalError, (unsigned long)calAccepted, (unsigned long)calRejected,
      (unsigned long)calSlewed, (unsigned long)calCentered);
    diagnosticQueue(line);
    // Alternating detail/timing reports to avoid filling the 384-byte diagnostic FIFO.
    diagnosticTimingTurn = !diagnosticTimingTurn;
    // Extra measurements sampled only every 5s, outside the RF critical section.
    if (diagnosticTimingTurn) {
    // TX-FAIL includes prepare/guard failures, not only the radio hardware aborts.
    snprintf(line, sizeof(line),
      "[RF-DETAIL][TX-TRY=%lu][TX-FAIL=%lu][TX-ABORT=%lu][RX-TRY=%lu][RX-REJECT=%lu][RX-WRONG-ROLE=%lu][RX-CAP-ABORT=%lu][SYNC-CANDIDATE-ONLY=%lu][CTRL-ONLY=%lu][TX-SLOT-MISSED=%lu]\r\n",
      (unsigned long)txAttempts, (unsigned long)txFailed,
      (unsigned long)radio.stats.txAbort, (unsigned long)rxAttempts,
      (unsigned long)rxRejected, (unsigned long)rxWrongRole,
      (unsigned long)radio.stats.captureAbort,
      (unsigned long)radio.stats.syncCandidateOnly, (unsigned long)ctrlOnly,
      (unsigned long)txSlotMissed);
    diagnosticQueue(line);
    } else {
    snprintf(line, sizeof(line),
      "[RF-TIMING-US][PREP-MAX=%lu][SETUP-MAX=%lu][TX-LATE-MAX=%lu][SAMPLE-LATE-CYC=%lu][DEMOD-MAX=%lu][ACQUIRE-MAX=%lu][UART-OVERRUN=%lu][TX-NOT-DRAINED=%lu]\r\n",
      (unsigned long)radio.stats.prepareUs, (unsigned long)radio.stats.setupUs,
      (unsigned long)radio.stats.startLateUs, (unsigned long)radio.stats.sampleLateCycles,
      (unsigned long)radio.stats.demodUs, (unsigned long)radio.stats.acquireUs,
      (unsigned long)uartWindowOverruns, (unsigned long)txNotDrained);
    diagnosticQueue(line);
    snprintf(line, sizeof(line),
      "[TX-SCHED][GUARD-UART=%lu][GUARD-TIME=%lu][PREP-LAST-US=%lu][LEAD-US=%lu]\r\n",
      (unsigned long)txGuardUart, (unsigned long)txGuardTime,
      (unsigned long)txPrepLastUs, (unsigned long)txBudgetLastUs);
    diagnosticQueue(line);
    snprintf(line, sizeof(line),
      "[R8][CTRL-OK=%lu][DATA-OK=%lu][DATA-FAIL=%lu][FINE-RECOVER=%lu][CAL-CTRL=%lu][CAL-REJECT=%lu]\r\n",
      (unsigned long)radio.stats.eightCtrlValid,
      (unsigned long)radio.stats.eightPayloadValid,
      (unsigned long)radio.stats.eightPayloadFailed,
      (unsigned long)radio.stats.eightRefinedRecovery,
      (unsigned long)ctrlOnlyCalApplied, (unsigned long)ctrlOnlyCalRejected);
    diagnosticQueue(line);
    }
  }
#endif
}

}


#line 1 "modemB.ino"
AirModem::Firmware modem(2);
void setup() { modem.begin(); }
void loop() { modem.poll(); }

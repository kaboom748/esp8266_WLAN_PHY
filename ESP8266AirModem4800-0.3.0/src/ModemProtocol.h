#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace AirModem {
constexpr unsigned PAYLOAD = 52, PACKET_BYTES = 80, BUFFER_BYTES = 2048;
constexpr int16_t NO_TUNE = 32767;
inline int16_t tuneRequest(int16_t transmitted, int32_t received, int32_t unit) {
  int32_t magnitude = received < 0 ? -received : received;
  if (magnitude >= 5000 && magnitude <= 13000) return transmitted;
  int32_t target = received < 0 ? -9000 : 9000;
  int32_t next = transmitted + (target - received) * (unit < 0 ? -1 : 1);
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
  out[0] = 0xa4; out[1] = 0x80; out[2] = 2; out[3] = p.role;
  put32(out + 4, p.boot); put32(out + 8, p.session);
  out[12] = uint8_t(p.type); out[13] = p.length;
  put16(out + 14, p.seq); put16(out + 16, p.ack); put16(out + 18, p.credit);
  put16(out + 20, uint16_t(p.txHz)); put16(out + 22, uint16_t(p.tuneHz));
  memcpy(out + 24, p.data, p.length); put32(out + 76, crc32(out, 76));
  return true;
}
inline bool decode(const uint8_t in[PACKET_BYTES], Packet& out) {
  if (in[0] != 0xa4 || in[1] != 0x80 || in[2] != 2 || get32(in + 76) != crc32(in, 76)) return false;
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

#include <cstdint>
#include <cstring>
#include <cstdlib>
namespace AirModem {
constexpr unsigned PAYLOAD = 68, PACKET_BYTES = 80, BUFFER_BYTES = 2048;
constexpr unsigned TWO_FSK_BYTES = 40, TWO_FSK_PAYLOAD = 10;
constexpr unsigned EIGHT_FSK_BYTES = 98; // 26B CTRL + 68B DATA + 4B CRC32
constexpr unsigned EIGHT_FSK_PAYLOAD = 68; // 208 2-FSK + 192 8-FSK symbols
constexpr unsigned SIXTEEN_FSK_BYTES = 82; // 26B CTRL + 56B (52 data + 4 CRC32)
constexpr unsigned SIXTEEN_FSK_PAYLOAD = 50; // legacy 16-FSK ceiling, inactive

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
constexpr int32_t CAL_MAX_OBS_HZ = 35000; // too near Nyquist => risk of alias
constexpr int32_t CAL_MIN_UNIT_HZ = 100;
constexpr int32_t CAL_MAX_UNIT_HZ = 9000;
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
  uint8_t role = 0, length = 0, subslot = 0;
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
  if (!valid(p) || p.length > 50) return false;
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

namespace Rs14 {
// Shortened Reed-Solomon RS(18,14), two correctable bytes, GF(256)/0x11d.
static uint8_t expT[512], logT[256];
static bool initialized=false;
inline void init() {
  if (initialized) return;
  unsigned x=1;
  for(unsigned i=0;i<255;++i) {
    expT[i]=uint8_t(x); logT[x]=uint8_t(i);
    x<<=1; if(x&256u) x^=0x11du;
  }
  for(unsigned i=255;i<512;++i) expT[i]=expT[i-255];
  initialized=true;
}
inline uint8_t mul(uint8_t a,uint8_t b) {
  return a&&b?expT[unsigned(logT[a])+logT[b]]:0;
}
inline uint8_t div(uint8_t a,uint8_t b) {
  if(!a) return 0;
  return expT[unsigned(logT[a])+255u-logT[b]];
}
inline void encode(const uint8_t msg[14],uint8_t parity[4]) {
  init();
  uint8_t g[5]={1,0,0,0,0};
  for (unsigned root=0;root<4;++root) {
    uint8_t next[5]={};
    for (unsigned j=0;j<=root;++j) {
      next[j]^=g[j]; next[j+1]^=mul(g[j],expT[root]);
    }
    memcpy(g,next,sizeof(g));
  }
  memset(parity,0,4);
  for (unsigned i=0;i<14;++i) {
    const uint8_t feed=uint8_t(msg[i]^parity[0]);
    for (unsigned j=0;j<3;++j)
      parity[j]=uint8_t(parity[j+1]^mul(feed,g[j+1]));
    parity[3]=mul(feed,g[4]);
  }
}
inline void syndrome(const uint8_t w[18],uint8_t s[4]) {
  init();
  for (unsigned j=0;j<4;++j) {
    uint8_t v=0;
    for(unsigned k=0;k<18;++k) v=uint8_t(mul(v,expT[j])^w[k]);
    s[j]=v;
  }
}
inline bool repair(uint8_t word[18],uint8_t& corrected) {
  corrected=0;
  uint8_t s[4]; syndrome(word,s);
  if(!(s[0]|s[1]|s[2]|s[3])) return true;
  for(unsigned a=0;a<18;++a) {
    const uint8_t x=expT[17-a],e=s[0];
    if(s[1]==mul(e,x) && s[2]==mul(s[1],x) && s[3]==mul(s[2],x)) {
      word[a]^=e; corrected=1; return true;
    }
  }
  for(unsigned a=0;a<18;++a)for(unsigned b=a+1;b<18;++b) {
    const uint8_t x=expT[17-a],y=expT[17-b];
    const uint8_t e=div(uint8_t(s[1]^mul(s[0],y)),uint8_t(x^y));
    const uint8_t f=uint8_t(s[0]^e);
    if(!e||!f)continue;
    const uint8_t x2=mul(x,x),y2=mul(y,y);
    if(s[2]!=uint8_t(mul(e,x2)^mul(f,y2)))continue;
    if(s[3]!=uint8_t(mul(e,mul(x2,x))^mul(f,mul(y2,y))))continue;
    word[a]^=e; word[b]^=f; corrected=2; return true;
  }
  return false;
}
}
// 36-byte CRC input with reserved RS parity fields and CRC16 masked to zero.
// This allows CRC32 to be computed before the RS parity itself is generated.
inline uint32_t twoDataCrc32(const uint8_t w[TWO_FSK_BYTES]) {
  uint8_t tmp[36]; memcpy(tmp,w,36);
  memset(tmp+16,0,4);tmp[24]=tmp[25]=0;
  return crc32(tmp,36);
}
// REV6 format: 24-byte control, CRC16(24), 10-byte payload, CRC32(36).
// Byte positions 16..19 are FEC parity, NOT ARQ ack/credit (both disabled).
// Parity is RS(18,14) over payload bytes26..35 plus CRC32 bytes36..39.
// 40 bytes and 344 RF symbols remain unchanged (not wire-compatible REV5).
inline bool encodeTwo(const Packet& p, uint8_t out[TWO_FSK_BYTES]) {
  if (!valid(p) || p.length > TWO_FSK_PAYLOAD) return false;
  memset(out,0,TWO_FSK_BYTES);
  out[0]=0xa4;out[1]=0x80;out[2]=uint8_t(5 | (p.subslot?0x80:0));out[3]=p.role;
  put32(out+4,p.boot);put32(out+8,p.session);
  out[12]=uint8_t(p.type);out[13]=p.length;
  put16(out+14,p.seq);
  // [16..19] Reed-Solomon parity instead of ACK/credit.
  put16(out+20,uint16_t(p.txHz));put16(out+22,uint16_t(p.tuneHz));
  memcpy(out+26,p.data,p.length);
  put32(out+36,twoDataCrc32(out));
  uint8_t fec[4];Rs14::encode(out+26,fec);memcpy(out+16,fec,4);
  put16(out+24,ctrlCrc16(out,24));
  return true;
}
inline bool decodeTwo(const uint8_t in[TWO_FSK_BYTES],Packet& p,
                      bool& dataValid,uint8_t* correctedBytes=nullptr) {
  dataValid=false;
  if(correctedBytes)*correctedBytes=0;
  // A weak header might contain an erroneous parity byte; RS can correct it.
  // Require magic/version/role before attempting the more expensive decoder.
  if(in[0]!=0xa4 || in[1]!=0x80 || (in[2]&0x7f)!=5 ||
     (in[3]!=1 && in[3]!=2) || in[13]>TWO_FSK_PAYLOAD) return false;
  uint8_t repaired[TWO_FSK_BYTES];memcpy(repaired,in,TWO_FSK_BYTES);
  uint8_t w[18];memcpy(w,repaired+26,14);memcpy(w+14,repaired+16,4);
  uint8_t fecFixed=0;
  const bool fecOk=Rs14::repair(w,fecFixed);
  if(fecOk) {
    memcpy(repaired+26,w,14);memcpy(repaired+16,w+14,4);
  }
  if(get16(repaired+24)!=ctrlCrc16(repaired,24))return false;
  uint8_t canonical[PACKET_BYTES]={};
  memcpy(canonical,repaired,24);canonical[2]=3;
  // ACK and credit removed from the wire; local link does not use them.
  put16(canonical+16,0);
  put16(canonical+18,BUFFER_BYTES);
  memcpy(canonical+24,repaired+26,TWO_FSK_PAYLOAD);
  put16(canonical+74,ctrlCrc16(canonical,24));
  if(!decodeControl(canonical,p))return false;
  p.subslot=uint8_t((in[2]>>7)&1);
  dataValid=fecOk && get32(repaired+36)==twoDataCrc32(repaired);
  if(dataValid) {
    memcpy(p.data,repaired+26,p.length);
    if(correctedBytes)*correctedBytes=fecFixed;
  }
  return true;
}

}


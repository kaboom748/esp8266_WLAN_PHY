#include "AirModem.h"
#include <assert.h>
#include <stdio.h>
#include <random>
#include <string>
using namespace AirModem;
static uint32_t now = 1000;
struct Flow { unsigned xon = 0, xoff = 0; bool paused = false; };
static void send(Modem& m, const std::string& s) { for (uint8_t b : s) m.input(b, now); }
static std::string read(Modem& m, Flow* flow = nullptr, unsigned limit = 10000) {
  std::string out;
  for (unsigned i = 0; i < limit;) {
    int b = m.output(now);
    if (b < 0) break;
    if (b == 0x11 || b == 0x13) {
      if (flow) { flow->paused = b == 0x13; if (flow->paused) ++flow->xoff; else ++flow->xon; }
    } else { out += char(b); ++i; }
  }
  return out;
}
static void move(Modem& a, Modem& b, unsigned fault = 0) {
  Packet p = a.link.outgoing(now), q; uint8_t bytes[PACKET_BYTES];
  assert(encode(p, bytes));
  if (fault == 1) return;
  if (fault == 2) bytes[30] ^= 1;
  if (decode(bytes, q)) { b.link.incoming(q, now); if (fault == 3) b.link.incoming(q, now); }
  b.tick(now);
}
static void cycle(Modem& a, Modem& b) {
  now += 280; a.tick(now); b.tick(now); move(a, b); move(b, a);
}
static void connect(Modem& a, Modem& b) {
  for (unsigned i = 0; i < 40 && (!a.online() || !b.online()); ++i) cycle(a, b);
  assert(a.online() && b.online() && a.cableMode() && b.cableMode());
  assert(a.link.session() == b.link.session());
}
static std::string deliver(Modem& from, Modem& to, const std::string& data) {
  send(from, data); std::string received;
  for (unsigned i = 0; i < 40 && received.size() < data.size(); ++i) {
    cycle(from, to); received += read(to);
  }
  return received;
}
static void bootAndLiteral() {
  Modem a(1, 1), b(2, 2);
  assert(a.cableMode() && b.cableMode());
  connect(a, b);
  assert(read(a).empty() && read(b).empty());
  std::string data = "AT\rATZ\rATH\rATDT2\r+++CLIENTCLIENTSERVER\r\n";
  data += std::string("\0\xff\x7e", 3);
  assert(deliver(a, b, data) == data);
  assert(read(a).empty());
  assert(deliver(b, a, data) == data);
  assert(read(b).empty());
  puts("PASS boot-transparent, automatic radio, AT/+++ literal, no local responses");
}
static void handshake(bool reverse) {
  Modem a(1, 10), b(2, 20);
  Modem& caller = reverse ? b : a;
  Modem& server = reverse ? a : b;
  send(caller, "CLIENT");
  Flow flow; assert(read(caller, &flow).empty() && !flow.paused);
  connect(a, b);
  std::string request;
  for (unsigned i = 0; i < 20 && request.size() < 6; ++i) {
    cycle(a, b); request += read(server); assert(read(caller).empty());
  }
  assert(request == "CLIENT");
  assert(deliver(server, caller, "CLIENTSERVER") == "CLIENTSERVER");
  assert(read(server).empty());
}
static void repeatedCalls() {
  Modem a(1, 30), b(2, 31); connect(a, b); read(a); read(b);
  uint32_t session = a.link.session();
  for (unsigned n = 0; n < 12; ++n) {
    // The PC cable profile sends XON at each serial-session initialization.
    a.input(0x11, now); b.input(0x11, now);
    assert(deliver(a, b, "CLIENT") == "CLIENT");
    assert(read(a).empty());
    assert(deliver(b, a, "CLIENTSERVER") == "CLIENTSERVER");
    assert(deliver(a, b, "\x7ePPP close frame\x7e") == "\x7ePPP close frame\x7e");
    a.input(0x13, now); b.input(0x13, now);
    cycle(a, b);
    assert(a.hostPaused() && b.hostPaused());
    assert(a.link.session() == session && b.link.session() == session);
  }
  puts("PASS 12 cable handshakes on one radio session; PC-open XON clears old pause");
}
static void handshakeControls() {
  for (uint8_t ch : std::string("CLIENTCLIENTSERVER")) assert(ch != 0x11 && ch != 0x13);
  Modem a(1, 40), b(2, 41); connect(a, b); read(a); read(b);
  send(a, std::string("CL") + char(0x13) + "IE" + char(0x11) + "NT");
  cycle(a, b); assert(read(b) == "CLIENT");
  assert(!a.hostPaused() && a.stats.inputBytes == 6);
  send(b, "CLIENTSERVER"); cycle(a, b);
  std::string reply = read(a, nullptr, 4); assert(reply == "CLIE");
  a.input(0x13, now);
  Flow controls;
  for (unsigned n = 0; n < 100; ++n) { cycle(a, b); assert(read(a, &controls).empty()); read(b); }
  assert(a.hostPaused() && controls.xon > 0);
  a.input(0x11, now); reply += read(a);
  assert(reply == "CLIENTSERVER");
  assert(a.stats.outputBytes == 12 && b.stats.outputBytes == 6);
  a.input(0x13, now); b.input(0x13, now);
  send(a, "CLIENT"); send(b, "CLIENTSERVER"); cycle(a, b);
  assert(read(a).empty() && read(b).empty());
  a.input(0x11, now); b.input(0x11, now);
  assert(read(a) == "CLIENTSERVER" && read(b) == "CLIENT");
  puts("PASS XON/XOFF interleaved with CLIENT, reply paused mid-word, simultaneous pauses");
}
static void latePeerAndThresholds() {
  Modem a(1, 50); send(a, "CLIENT"); Flow flow;
  for (unsigned n = 0; n < 150; ++n) {
    now += 1000; a.tick(now); assert(read(a, &flow).empty()); assert(!flow.paused);
  }
  assert(a.stats.cableDropped == 0 && a.stats.overflows == 0);
  Modem b(2, 51); connect(a, b);
  std::string request;
  for (unsigned n = 0; n < 20; ++n) { cycle(a, b); request += read(b); assert(read(a).empty()); }
  assert(request == "CLIENT");
  Modem c(1, 52); Flow pressure; send(c, std::string(512, 'q'));
  assert(read(c, &pressure).empty() && pressure.paused && pressure.xoff);
  send(c, std::string(128, 'r'));
  assert(!c.stats.overflows);
  Modem d(2, 53); connect(c, d); std::string received;
  for (unsigned n = 0; n < 60; ++n) {
    cycle(c, d); received += read(d); assert(read(c, &pressure).empty());
  }
  assert(received == std::string(512, 'q') + std::string(128, 'r'));
  assert(!pressure.paused && pressure.xon && !c.stats.overflows);
  puts("PASS peer absent 150s: CLIENT preserved, no premature XOFF, bounded offline buffering");
}
static std::string payload(unsigned n, unsigned seed) {
  std::mt19937 rng(seed); std::string out;
  for (unsigned i = 0; i < n; ++i) {
    uint8_t b = i < 256 ? uint8_t(i) : uint8_t(rng());
    if (b == 0x11 || b == 0x13) { out += char(0x7d); b ^= 0x20; }
    out += char(b);
  }
  return out;
}
static void stress() {
  Modem a(1, 60), b(2, 61); connect(a, b); read(a); read(b);
  std::string ab = payload(20000, 101), ba = payload(16000, 102), gotA, gotB;
  unsigned ia = 0, ib = 0; Flow fa, fb; std::mt19937 rng(555);
  uint32_t start = now; b.input(0x13, now);
  for (unsigned i = 0; i < 6000; ++i) {
    now += 280;
    if (uint32_t(now - start) >= 25000) b.input(0x11, now);
    for (unsigned n = 0; n < 120 && !fa.paused && ia < ab.size(); ++n) a.input(uint8_t(ab[ia++]), now);
    for (unsigned n = 0; n < 120 && !fb.paused && ib < ba.size(); ++n) b.input(uint8_t(ba[ib++]), now);
    move(a, b, rng() % 10 < 3 ? 1 + rng() % 3 : 0);
    move(b, a, rng() % 10 < 3 ? 1 + rng() % 3 : 0);
    gotA += read(a, &fa); gotB += read(b, &fb);
    assert(a.online() && b.online() && !a.stats.overflows && !b.stats.overflows);
    if (gotA.size() == ba.size() && gotB.size() == ab.size()) break;
  }
  assert(gotA == ba && gotB == ab);
  assert(a.stats.inputCrc == b.stats.outputCrc && b.stats.inputCrc == a.stats.outputCrc);
  assert(fa.xoff && fb.xoff && a.link.stats.retries && b.link.stats.retries);
  printf("PASS loss/corruption/duplicates/25s pause: %zu/%zu escaped bytes exact\n", ab.size(), ba.size());
}
static void faults() {
  Modem a(1, 70), b(2, 71); connect(a, b); read(a); read(b);
  a.input(0x13, now);
  now += 16000; a.tick(now); b.tick(now);
  assert(!a.online() && !b.online() && a.hostPaused());
  assert(read(a).empty() && read(b).empty());
  connect(a, b);
  send(b, "CLIENT"); cycle(a, b); assert(read(a).empty());
  a.input(0x11, now); assert(read(a) == "CLIENT");
  Modem reset(2, 72);
  for (unsigned i = 0; i < 100; ++i) cycle(a, reset);
  assert(a.online() && reset.online());
  read(a); read(reset);
  assert(deliver(a, reset, "CLIENT") == "CLIENT");
  a.uartFailure(now); assert(!a.online() && a.stats.uartErrors == 1);
  assert(read(a).empty());
  Modem full(1, 73); send(full, std::string(4000, 'x'));
  assert(full.stats.overflows && full.stats.cableDropped && read(full).empty());
  Modem fresh(2, 74); Link legacy(1, 75);
  assert(legacy.dial(123, now, false));
  fresh.link.incoming(legacy.outgoing(now), now); fresh.tick(now);
  assert(!fresh.online() && fresh.link.reason() == Reason::Busy && read(fresh).empty());
  puts("PASS loss/reconnect/reset, retained host pause, UART/overflow, legacy AT call rejected");
}
static void protocol() {
  assert(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xcbf43926u);
  Packet p, q; p.role = 1; p.boot = 1; p.session = 2; p.type = Type::Data;
  p.length = PAYLOAD; p.credit = BUFFER_BYTES;
  for (unsigned i = 0; i < PAYLOAD; ++i) p.data[i] = uint8_t(i);
  uint8_t bytes[PACKET_BYTES]; assert(encode(p, bytes) && decode(bytes, q));
  for (unsigned i = 0; i < PACKET_BYTES * 8; ++i) {
    bytes[i / 8] ^= 1u << (i % 8); assert(!decode(bytes, q)); bytes[i / 8] ^= 1u << (i % 8);
  }
  Modem a(1, 80), b(2, 81); connect(a, b); read(a); read(b);
  for (unsigned i = 0; i < 65540; ++i) {
    assert(a.link.write(uint8_t(i))); now += 1;
    Packet packet = a.link.outgoing(now); assert(packet.seq == uint16_t(i));
    b.link.incoming(packet, now); b.link.incoming(packet, now);
    assert(b.link.read() == int(uint8_t(i)) && b.link.read() == -1);
    move(b, a); assert(a.link.queued() == 0);
  }
  now = 0xfffff000u;
  Modem c(1, 82), d(2, 83); connect(c, d);
  now += 16000; c.tick(now); assert(!c.online());
  puts("PASS CRC bit faults, sequence wrap, timer wrap");
}
int main() {
  bootAndLiteral(); handshake(false); handshake(true); repeatedCalls();
  handshakeControls(); latePeerAndThresholds(); stress(); faults(); protocol();
  puts("PASS transparent core 0.3.0");
}

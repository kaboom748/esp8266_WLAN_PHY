#pragma once
#include "ModemProtocol.h"

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

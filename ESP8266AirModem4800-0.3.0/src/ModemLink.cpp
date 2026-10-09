#include "ModemLink.h"

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
        unsigned n = tx.size(); if (n > PAYLOAD) n = PAYLOAD; if (n > peerCredit) n = peerCredit;
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

#include "AirModem.h"

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
  if (queued >= 512) stop = true;
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

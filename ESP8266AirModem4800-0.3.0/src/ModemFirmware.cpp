#include "ModemFirmware.h"
#include <new>
extern "C" {
#include "user_interface.h"
}

namespace AirModem {
void Firmware::begin() {
  Serial.setRxBufferSize(2048);
  Serial.begin(4800, SERIAL_8N1);
  Serial.setDebugOutput(false);
  system_set_os_print(0);
  modem.~Modem(); new (&modem) Modem(role, os_random());
  ready = radio.begin();
  next = micros() + 20000;
}
void Firmware::serial() {
  bool uartError = Serial.hasOverrun(); uartError |= Serial.hasRxError();
  if (uartError) modem.uartFailure(millis());
  for (unsigned n = 0; n < 256 && Serial.available(); ++n) modem.input(uint8_t(Serial.read()), millis());
  modem.tick(millis());
  // Drain UART TX before switching the undocumented RF clock path. In RX
  // captures the UART can run autonomously, but no byte may straddle TX setup.
  unsigned budget = 32;
  if (role == 1 || locked) {
    uint32_t switchAt = role == 1 ? (phase == 0 ? next : epoch + CYCLE) - PREPARE :
      (phase == 1 ? next : epoch + CYCLE + HALF) - PREPARE;
    int32_t room = int32_t(switchAt - micros()) - 3000;
    budget = room <= 0 ? 0 : unsigned(room) / 2084;
    if (budget > 32) budget = 32;
  }
  while (Serial.availableForWrite() > int(128 - budget)) {
    int b = modem.output(millis()); if (b < 0) break; Serial.write(uint8_t(b));
  }
  modem.radioStats.tx = radio.stats.tx;
  modem.radioStats.aborts = radio.stats.txAbort + radio.stats.captureAbort;
}
bool Firmware::transmit(uint32_t at) {
  Packet packet = modem.link.outgoing(millis());
  packet.txHz = txHz; packet.tuneHz = tuneHz;
  Phy::Frame frame; Phy::makeFrame(packet, frame);
  if (!radio.prepare(frame, txHz)) return false;
  serial();
  Serial.flush();
  delayMicroseconds(2200);
  return radio.transmitAt(at);
}
bool Firmware::receive(unsigned samples, uint32_t& start) {
  Phy::Acquisition got; uint32_t capturedAt;
  if (!radio.receive(samples, got, capturedAt) || got.packet.role == role) return false;
  start = capturedAt + int32_t(int64_t(got.firstCenter - int32_t(Phy::FIRST_CENTER)) * 1000000 / Phy::SAMPLE_RATE);
  modem.radioStats.offset = got.offsetHz; modem.radioStats.unit = got.unitHz;
  // Absolute requests reference the actual TX offset in the CRC-checked frame,
  // so repeated or lost replies cannot integrate a correction twice.
  tuneHz = tuneRequest(got.packet.txHz, got.offsetHz, got.unitHz);
  if (got.packet.tuneHz != NO_TUNE) txHz = got.packet.tuneHz;
  modem.radioStats.shift = txHz;
  ++modem.radioStats.rx;
  modem.link.incoming(got.packet, millis()); modem.tick(millis()); return true;
}
void Firmware::poll() {
  serial();
  if (!ready) { delay(1); return; }
  uint32_t now = micros();
  if (role == 1) {
    if (phase == 0 && int32_t(now - (next - PREPARE)) >= 0) {
      if (int32_t(now - (next - PREPARE + 1000)) >= 0) next = now + PREPARE;
      epoch = next; transmit(epoch); phase = 1; next = epoch + HALF - 4000;
    } else if (phase == 1 && int32_t(now - next) >= 0) {
      uint32_t start; receive(3200, start); phase = 0; next = epoch + CYCLE;
    }
  } else if (!locked) {
    uint32_t start;
    if (receive(Phy::MAX_CAPTURE, start)) {
      epoch = start; locked = true; misses = 0;
      if (int32_t(epoch + HALF - micros()) >= int32_t(PREPARE)) { phase = 1; next = epoch + HALF; }
      else { phase = 0; next = epoch + CYCLE - 4000; }
    }
  } else if (phase == 1 && int32_t(now - (next - PREPARE)) >= 0) {
    if (int32_t(next - now) >= int32_t(PREPARE - 1000)) transmit(next);
    phase = 0; next = epoch + CYCLE - 4000;
  } else if (phase == 0 && int32_t(now - next) >= 0) {
    uint32_t start;
    if (receive(3200, start)) {
      epoch = start; misses = 0; phase = 1; next = epoch + HALF;
    } else {
      epoch += CYCLE; next = epoch + CYCLE - 4000;
      if (++misses >= 6) locked = false;
    }
  }
  serial(); yield();
}
}

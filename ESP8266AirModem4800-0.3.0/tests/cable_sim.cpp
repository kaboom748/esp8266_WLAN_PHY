// Host-only transport for real pppd tests. No USB device or RF hardware is used.
#include "AirModem.h"
#include <assert.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
using namespace AirModem;
static volatile sig_atomic_t running = 1;
static void stop(int) { running = 0; }
static void radio(Modem& a, Modem& b, uint32_t now, unsigned cycle) {
  Packet p = a.link.outgoing(now), q; uint8_t bytes[PACKET_BYTES];
  assert(encode(p, bytes));
  if (cycle % 19 == 0) return;
  if (cycle % 31 == 0) bytes[30] ^= 1;
  if (decode(bytes, q)) {
    b.link.incoming(q, now);
    if (cycle % 23 == 0) b.link.incoming(q, now);
  }
  b.tick(now);
}
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  signal(SIGTERM, stop); signal(SIGINT, stop);
  int fds[2] = {atoi(argv[1]), atoi(argv[2])};
  for (int fd : fds) assert(fcntl(fd, F_SETFL, O_NONBLOCK) == 0);
  Modem a(1, 100), b(2, 200); Modem* ends[2] = {&a, &b};
  std::deque<uint8_t> output[2];
  uint32_t nextRadio = 0, nextSerial = 0; unsigned cycles = 0;
  auto start = std::chrono::steady_clock::now();
  while (running) {
    uint32_t now = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count());
    for (unsigned i = 0; i < 2; ++i) {
      uint8_t bytes[256]; ssize_t n = read(fds[i], bytes, sizeof(bytes));
      for (ssize_t k = 0; k < n; ++k) ends[i]->input(bytes[k], now);
      ends[i]->tick(now);
    }
    if (now >= nextRadio) {
      radio(a, b, now, ++cycles); radio(b, a, now, cycles + 3); nextRadio = now + 280;
    }
    if (now >= nextSerial) {
      for (unsigned i = 0; i < 2; ++i) {
        for (unsigned n = 0; n < 4 && output[i].size() < 128; ++n) {
          int ch = ends[i]->output(now); if (ch < 0) break; output[i].push_back(uint8_t(ch));
        }
        for (unsigned n = 0; n < 4 && !output[i].empty(); ++n) {
          uint8_t ch = output[i].front();
          if (write(fds[i], &ch, 1) != 1) break;
          output[i].pop_front();
        }
      }
      nextSerial = now + 9;
    }
    usleep(1000);
  }
  fprintf(stderr, "SIMULATION cable=%d/%d retries=%u/%u overflow=%u/%u\n", a.cableMode(), b.cableMode(),
          a.link.stats.retries, b.link.stats.retries, a.stats.overflows, b.stats.overflows);
  return a.cableMode() && b.cableMode() && !a.stats.overflows && !b.stats.overflows ? 0 : 1;
}

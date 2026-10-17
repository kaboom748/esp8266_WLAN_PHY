#ifndef MODEMFAST_CODEC_H
#define MODEMFAST_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef MF_PAYLOAD
#define MF_PAYLOAD 192
#endif
#ifndef MF_PARITY
#define MF_PARITY 32
#endif

namespace BlockCodec {

enum {
  PAYLOAD = 192,
  PARITY = 32,
  FAST_PAYLOAD = 208,
  FAST_PARITY = 16,
  MAX_PAYLOAD = FAST_PAYLOAD,
  HEADER = 16,
  DATA = HEADER + PAYLOAD + 4,
  WIRE = DATA + PARITY,
  FLAG_DATA = 1,
  FLAG_ACK = 2,
  FLAG_COMPRESSED = 4,
  FLAG_FAST_FEC = 8,
  FLAG_FRAGMENT = 16,
  VERSION = 0x20
};
static_assert(MF_PAYLOAD == PAYLOAD && MF_PARITY == PARITY,
              "v2 uses fixed 192/32 reference and 208/16 fast profiles");
static_assert(WIRE == 244 && HEADER + FAST_PAYLOAD + 4 + FAST_PARITY == WIRE,
              "Both v2 profiles must preserve the reference RF frame length");

inline unsigned packetCapacity(uint8_t flags) {
  return flags & FLAG_FAST_FEC ? FAST_PAYLOAD : PAYLOAD;
}
inline unsigned packetParity(uint8_t flags) {
  return flags & FLAG_FAST_FEC ? FAST_PARITY : PARITY;
}

struct Packet {
  uint8_t role, flags, length, credit;
  uint32_t session;
  uint16_t seq, ack;
  uint32_t ackSession;
  uint8_t payload[MAX_PAYLOAD];
};

namespace Detail {

// Systematic shortened RS over GF(256), primitive polynomial 0x11d,
// generator roots alpha^0 through alpha^(PARITY-1), alpha = 2.
// Leading zero symbols of the full (255,255-PARITY) code are omitted.
struct Tables {
  uint8_t exp[512];
  uint8_t log[256];
  uint8_t generator[PARITY + 1]; // Descending polynomial powers.
  uint8_t generatorLog[PARITY + 1];
  uint8_t fastGenerator[FAST_PARITY + 1];
  uint8_t fastGeneratorLog[FAST_PARITY + 1];

  uint8_t mul(uint8_t a, uint8_t b) const {
    return a && b ? exp[unsigned(log[a]) + log[b]] : 0;
  }
  uint8_t div(uint8_t a, uint8_t b) const {
    return a ? exp[unsigned(log[a]) + 255u - log[b]] : 0;
  }
  uint8_t inverse(uint8_t a) const { return exp[255u - log[a]]; }

  Tables() {
    memset(log, 0, sizeof(log));
    unsigned value = 1;
    for (unsigned i = 0; i < 255; ++i) {
      exp[i] = uint8_t(value);
      log[value] = uint8_t(i);
      value <<= 1;
      if (value & 0x100) value ^= 0x11d;
    }
    for (unsigned i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    buildGenerator(generator, generatorLog, PARITY);
    buildGenerator(fastGenerator, fastGeneratorLog, FAST_PARITY);
  }

  void buildGenerator(uint8_t* coefficients, uint8_t* logarithms, unsigned parity) {
    memset(coefficients, 0, parity + 1);
    coefficients[0] = 1;
    for (unsigned root = 0; root < parity; ++root) {
      for (unsigned j = root + 1; j > 0; --j)
        coefficients[j] ^= mul(coefficients[j - 1], exp[root]);
    }
    // All coefficients are nonzero for the supported PARITY 16 and 32.
    // Host tests check these generators and every optimized multiplication.
    for (unsigned j = 0; j <= parity; ++j) logarithms[j] = log[coefficients[j]];
  }
};

inline const Tables& tables() {
  // Fixed storage; first call initializes the tables, with no heap allocation.
  static const Tables instance;
  return instance;
}

inline void put16(uint8_t* out, uint16_t value) {
  out[0] = uint8_t(value); out[1] = uint8_t(value >> 8);
}
inline void put32(uint8_t* out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) out[i] = uint8_t(value >> (8 * i));
}
inline uint16_t get16(const uint8_t* in) {
  return uint16_t(uint16_t(in[0]) | (uint16_t(in[1]) << 8));
}
inline uint32_t get32(const uint8_t* in) {
  return uint32_t(in[0]) | (uint32_t(in[1]) << 8) |
         (uint32_t(in[2]) << 16) | (uint32_t(in[3]) << 24);
}

inline uint32_t crc32(const uint8_t* bytes, unsigned length) {
  uint32_t crc = 0xffffffffu;
  for (unsigned i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xffffffffu;
}

inline void rsEncode(uint8_t* wire, unsigned parityCount = PARITY) {
  const Tables& gf = tables();
  const unsigned data = WIRE - parityCount;
  const uint8_t* generatorLog = parityCount == FAST_PARITY ? gf.fastGeneratorLog : gf.generatorLog;
  uint8_t* parity = wire + data;
  memset(parity, 0, parityCount);
  for (unsigned i = 0; i < data; ++i) {
    const uint8_t feedback = wire[i] ^ parity[0];
    if (feedback) {
      const unsigned feedbackLog = gf.log[feedback];
      for (unsigned j = 0; j + 1 < parityCount; ++j)
        parity[j] = parity[j + 1] ^ gf.exp[feedbackLog + generatorLog[j + 1]];
      parity[parityCount - 1] = gf.exp[feedbackLog + generatorLog[parityCount]];
    } else {
      for (unsigned j = 0; j + 1 < parityCount; ++j) parity[j] = parity[j + 1];
      parity[parityCount - 1] = 0;
    }
  }
}

// Both profiles evaluate the same 244 received symbols at the same roots.
// Compute an initial prefix, then only the additional roots if the alternate
// profile needs more parity. The already computed prefix is never modified.
inline void syndromeRange(const uint8_t* wire, uint8_t* out, const Tables& gf,
                          unsigned first, unsigned limit) {
  if (!first && limit) {
    uint8_t sumZero = 0;
    for (unsigned j = 0; j < WIRE; ++j) sumZero ^= wire[j];
    out[0] = sumZero; // Evaluation at alpha^0 = 1 needs no GF lookup.
    first = 1;
  }
  for (unsigned i = first; i < limit; ++i) {
    uint8_t sum = 0;
    for (unsigned j = 0; j < WIRE; ++j)
      sum = (sum ? gf.exp[unsigned(gf.log[sum]) + i] : 0) ^ wire[j];
    out[i] = sum;
  }
}

inline bool syndromes(const uint8_t* wire, uint8_t* out, const Tables& gf,
                      unsigned parityCount = PARITY) {
  syndromeRange(wire, out, gf, 0, parityCount);
  uint8_t nonzero = 0;
  for (unsigned i = 0; i < parityCount; ++i) nonzero |= out[i];
  return nonzero != 0;
}

inline bool rsCorrectWithSyndromes(uint8_t* wire, uint8_t& corrected,
                                   const uint8_t* syndrome, unsigned parityCount) {
  const Tables& gf = tables();
  corrected = 0;
  uint8_t nonzero = 0;
  for (unsigned i = 0; i < parityCount; ++i) nonzero |= syndrome[i];
  if (!nonzero) return true;

  // Berlekamp-Massey: locator coefficients are in ascending powers.
  uint8_t locator[PARITY + 1] = {1};
  uint8_t previous[PARITY + 1] = {1};
  uint8_t saved[PARITY + 1];
  unsigned degree = 0, shift = 1;
  uint8_t lastDiscrepancy = 1;
  for (unsigned n = 0; n < parityCount; ++n) {
    uint8_t discrepancy = syndrome[n];
    for (unsigned i = 1; i <= degree; ++i)
      discrepancy ^= gf.mul(locator[i], syndrome[n - i]);
    if (!discrepancy) { ++shift; continue; }
    memcpy(saved, locator, sizeof(locator));
    const uint8_t scale = gf.div(discrepancy, lastDiscrepancy);
    for (unsigned i = 0; i + shift <= parityCount; ++i)
      locator[i + shift] ^= gf.mul(scale, previous[i]);
    if (2 * degree <= n) {
      degree = n + 1 - degree;
      memcpy(previous, saved, sizeof(previous));
      lastDiscrepancy = discrepancy;
      shift = 1;
    } else {
      ++shift;
    }
  }
  if (!degree || degree > parityCount / 2) return false;

  // Chien search over transmitted positions only. Errors that would be in
  // the omitted leading zeros make this shortened block undecodable.
  uint8_t positions[PARITY / 2], locations[PARITY / 2];
  unsigned count = 0;
  for (unsigned pos = 0; pos < WIRE; ++pos) {
    const unsigned power = WIRE - 1 - pos;
    const uint8_t inverseLocation = gf.exp[255 - power];
    uint8_t evaluation = locator[degree];
    for (unsigned i = degree; i > 0; --i)
      evaluation = gf.mul(evaluation, inverseLocation) ^ locator[i - 1];
    if (!evaluation) {
      if (count == degree) return false;
      positions[count] = uint8_t(pos);
      locations[count] = gf.exp[power];
      ++count;
    }
  }
  if (count != degree) return false;

  // Solve the bounded Vandermonde system S_i = sum(e_j * X_j^i).
  // At most 16x17 bytes, avoiding a fragile Forney convention dependency.
  uint8_t matrix[PARITY / 2][PARITY / 2 + 1];
  for (unsigned j = 0; j < count; ++j) {
    matrix[0][j] = 1;
    for (unsigned i = 1; i < count; ++i)
      matrix[i][j] = gf.mul(matrix[i - 1][j], locations[j]);
  }
  for (unsigned i = 0; i < count; ++i) matrix[i][count] = syndrome[i];
  for (unsigned col = 0; col < count; ++col) {
    unsigned pivot = col;
    while (pivot < count && !matrix[pivot][col]) ++pivot;
    if (pivot == count) return false;
    if (pivot != col) {
      for (unsigned j = col; j <= count; ++j) {
        const uint8_t temp = matrix[pivot][j];
        matrix[pivot][j] = matrix[col][j]; matrix[col][j] = temp;
      }
    }
    const uint8_t inversePivot = gf.inverse(matrix[col][col]);
    for (unsigned j = col; j <= count; ++j)
      matrix[col][j] = gf.mul(matrix[col][j], inversePivot);
    for (unsigned row = 0; row < count; ++row) {
      if (row == col) continue;
      const uint8_t factor = matrix[row][col];
      if (!factor) continue;
      for (unsigned j = col; j <= count; ++j)
        matrix[row][j] ^= gf.mul(factor, matrix[col][j]);
    }
  }
  for (unsigned i = 0; i < count; ++i) {
    if (!matrix[i][count]) return false;
    wire[positions[i]] ^= matrix[i][count];
  }
  // The caller may reuse the original syndromes for its other independent
  // profile attempt. Reuse the now-dead locator backup for post-correction
  // verification instead of overwriting that immutable initial prefix.
  if (syndromes(wire, saved, gf, parityCount)) return false;
  corrected = uint8_t(count);
  return true;
}

inline bool rsCorrect(uint8_t* wire, uint8_t& corrected, unsigned parityCount = PARITY) {
  uint8_t syndrome[PARITY];
  syndromeRange(wire, syndrome, tables(), 0, parityCount);
  return rsCorrectWithSyndromes(wire, corrected, syndrome, parityCount);
}

inline bool validMetadata(uint8_t role, uint8_t flags, uint8_t length,
                          uint32_t session, uint32_t ackSession) {
  return (role == 1 || role == 2) && !(flags & ~uint8_t(31)) &&
         length <= packetCapacity(flags) && ((flags & FLAG_DATA) || length == 0) &&
         (!(flags & FLAG_COMPRESSED) || (flags & FLAG_DATA)) &&
         (!(flags & FLAG_FRAGMENT) ||
          ((flags & FLAG_DATA) && !(flags & FLAG_FAST_FEC) && length > 6)) &&
         session != 0 && (!(flags & FLAG_ACK) || ackSession != 0);
}

} // namespace Detail

inline bool encode(const Packet& packet, uint8_t wire[WIRE]) {
  if (!Detail::validMetadata(packet.role, packet.flags, packet.length,
                             packet.session, packet.ackSession)) return false;
  memset(wire, 0, WIRE);
  wire[0] = 0xd6;
  wire[1] = uint8_t(VERSION | packet.role);
  wire[2] = packet.flags;
  wire[3] = packet.length;
  Detail::put32(wire + 4, packet.session);
  Detail::put16(wire + 8, packet.seq);
  Detail::put16(wire + 10, packet.ack);
  Detail::put32(wire + 12, packet.ackSession);
  memcpy(wire + HEADER, packet.payload, packet.length);
  const unsigned capacity = packetCapacity(packet.flags);
  Detail::put32(wire + HEADER + capacity, Detail::crc32(wire, HEADER + capacity));
  Detail::rsEncode(wire, packetParity(packet.flags));
  return true;
}

namespace Detail {

// The profile bit is checked AFTER correction and is covered by the CRC.
// A raw header hint may reorder attempts but never decides admission.
inline bool decodeProfile(uint8_t wire[WIRE], Packet& packet, uint8_t& corrected,
                          bool fast, const uint8_t* initialSyndromes = nullptr) {
  const unsigned capacity = fast ? FAST_PAYLOAD : PAYLOAD;
  const unsigned parityCount = fast ? FAST_PARITY : PARITY;
  uint8_t repairs = 0;
  if (initialSyndromes) {
    if (!rsCorrectWithSyndromes(wire, repairs, initialSyndromes, parityCount)) return false;
  } else if (!rsCorrect(wire, repairs, parityCount)) return false;
  if (get32(wire + HEADER + capacity) != crc32(wire, HEADER + capacity)) return false;
  if (wire[0] != 0xd6 || (wire[1] & 0xf0) != VERSION ||
      bool(wire[2] & FLAG_FAST_FEC) != fast) return false;
  const uint8_t role = wire[1] & 0x0f;
  const uint32_t session = get32(wire + 4);
  const uint32_t ackSession = get32(wire + 12);
  if (!validMetadata(role, wire[2], wire[3], session, ackSession)) return false;
  for (unsigned i = wire[3]; i < capacity; ++i)
    if (wire[HEADER + i]) return false;
  packet.role = role; packet.flags = wire[2]; packet.length = wire[3];
  packet.credit = 0; packet.session = session;
  packet.seq = get16(wire + 8); packet.ack = get16(wire + 10);
  packet.ackSession = ackSession;
  memset(packet.payload, 0, MAX_PAYLOAD);
  memcpy(packet.payload, wire + HEADER, capacity);
  corrected = repairs;
  return true;
}

} // namespace Detail

// Two bounded, independent RS attempts. No shared mutable profile state.
// Failure leaves both the caller's wire and Packet untouched. Successful
// correction copies the restored codeword back, preserving the original API.
inline bool decode(uint8_t wire[WIRE], Packet& packet, uint8_t& corrected) {
  corrected = 0;
  const bool suggestedFast = (wire[2] & FLAG_FAST_FEC) != 0;
  uint8_t candidate[WIRE];
  uint8_t syndrome[PARITY];
  unsigned computed = 0;
  for (unsigned attempt = 0; attempt < 2; ++attempt) {
    memcpy(candidate, wire, WIRE);
    const bool fast = attempt == 0 ? suggestedFast : !suggestedFast;
    const unsigned needed = fast ? FAST_PARITY : PARITY;
    if (needed > computed) {
      Detail::syndromeRange(wire, syndrome, Detail::tables(), computed, needed);
      computed = needed;
    }
    if (Detail::decodeProfile(candidate, packet, corrected, fast, syndrome)) {
      memcpy(wire, candidate, WIRE);
      return true;
    }
  }
  return false;
}

// Reference PN9: initial state 0x1ff, output bit 0, feedback taps 0 xor 5,
// output bytes least-significant-bit first. Call once before RF TX and once
// after RF RX, outside encode/decode. Every block starts from the same seed.
inline void scramble(uint8_t* bytes, unsigned length) {
  uint16_t state = 0x1ff;
  for (unsigned i = 0; i < length; ++i) {
    uint8_t mask = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
      mask |= uint8_t((state & 1u) << bit);
      const uint16_t feedback = (state ^ (state >> 5)) & 1u;
      state = uint16_t((state >> 1) | (feedback << 8));
    }
    bytes[i] ^= mask;
  }
}

} // namespace BlockCodec

#endif

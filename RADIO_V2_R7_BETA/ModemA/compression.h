#ifndef MODEMRADIO_COMPRESSION_H
#define MODEMRADIO_COMPRESSION_H

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "vendor/heatshrink/heatshrink_encoder.h"
#include "vendor/heatshrink/heatshrink_decoder.h"
}

#if HEATSHRINK_DYNAMIC_ALLOC || HEATSHRINK_USE_INDEX || \
    HEATSHRINK_STATIC_WINDOW_BITS != 8 || HEATSHRINK_STATIC_LOOKAHEAD_BITS != 4 || \
    HEATSHRINK_STATIC_INPUT_BUFFER_SIZE != 32
#error "RadioCompression requires static heatshrink W8/L4, input32, no index"
#endif

namespace RadioCompression {

enum : size_t { MAX_RAW = 512, HEADER = 8, MAX_ENCODED = HEADER + (9 * MAX_RAW + 7) / 8 };
enum : uint8_t { CODEC_HEATSHRINK = 1, PROFILE_W8_L4 = 0x84 };
enum : size_t {
  ENCODER_STATE_BYTES = sizeof(heatshrink_encoder),
  DECODER_STATE_BYTES = sizeof(heatshrink_decoder)
};

namespace Detail {
inline heatshrink_encoder& encoder() { static heatshrink_encoder state; return state; }
inline heatshrink_decoder& decoder() { static heatshrink_decoder state; return state; }

inline bool overlaps(const uint8_t* a, size_t na, const uint8_t* b, size_t nb) {
  const uintptr_t aa = reinterpret_cast<uintptr_t>(a), bb = reinterpret_cast<uintptr_t>(b);
  return aa <= bb ? bb - aa < na : aa - bb < nb;
}
inline uint32_t crc32(const uint8_t* bytes, size_t n) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xffffffffu;
}
inline uint32_t read32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline unsigned bits(const uint8_t* p, size_t& at, unsigned n) {
  unsigned value = 0;
  while (n--) { value = (value << 1) | ((p[at / 8] >> (7 - at % 8)) & 1u); ++at; }
  return value;
}

// heatshrink's finish API permits partial final tokens as padding. Validate
// the exact W8/L4 token boundary, raw length and zero padding separately so
// truncation, excess output and extra whole bytes cannot be accepted.
// Backreferences into the initially zero-filled window are legal upstream.
inline bool validStream(const uint8_t* p, size_t n, size_t raw) {
  const size_t end = n * 8;
  size_t at = 0, produced = 0;
  while (produced < raw) {
    if (at == end) return false;
    if (bits(p, at, 1)) {
      if (end - at < 8) return false;
      at += 8; ++produced;
    } else {
      if (end - at < 12) return false;
      at += 8;
      const size_t count = bits(p, at, 4) + 1;
      if (count > raw - produced) return false;
      produced += count;
    }
  }
  if (end - at >= 8) return false;
  while (at < end) if (bits(p, at, 1)) return false;
  return true;
}

inline bool pollEncoder(heatshrink_encoder& state, uint8_t* out, size_t capacity,
                        size_t& written) {
  for (;;) {
    uint8_t probe = 0;
    size_t count = 0;
    const bool full = written == capacity;
    const HSE_poll_res result = heatshrink_encoder_poll(&state,
      full ? &probe : out + written, full ? 1 : capacity - written, &count);
    if (result < 0 || (full && count)) return false;
    written += count;
    if (result == HSER_POLL_EMPTY) return true;
    if (!count) return false;
  }
}
} // namespace Detail

// Independent blocks; serialized foreground use only (not ISR/reentrant).
// There is no heap allocation. The input is never modified and overlapping
// buffers are rejected. A failure sets *encodedLength=0; output is then
// unspecified. Capacity includes HEADER. The transport owns raw fallback,
// chooses whether compression saves a slot, and caches a successful payload
// unchanged until ACK. A complete block call is not a real-time time slice.
inline bool encode(const uint8_t* src, size_t n, uint8_t* out, size_t capacity,
                   size_t* encodedLength) {
  if (!encodedLength) return false;
  *encodedLength = 0;
  if (!src || !out || !n || n > MAX_RAW || capacity <= HEADER ||
      Detail::overlaps(src, n, out, capacity)) return false;
  const size_t bodyCapacity = capacity - HEADER;
  heatshrink_encoder& state = Detail::encoder();
  heatshrink_encoder_reset(&state);
  size_t consumed = 0, written = 0;
  while (consumed < n) {
    size_t count = 0;
    // Upstream's sink takes a mutable pointer but only copies the input.
    if (heatshrink_encoder_sink(&state, const_cast<uint8_t*>(src + consumed),
                               n - consumed, &count) < 0 || !count) return false;
    consumed += count;
    if (!Detail::pollEncoder(state, out + HEADER, bodyCapacity, written)) return false;
  }
  for (;;) {
    const HSE_finish_res result = heatshrink_encoder_finish(&state);
    if (result < 0) return false;
    if (result == HSER_FINISH_DONE) break;
    if (!Detail::pollEncoder(state, out + HEADER, bodyCapacity, written)) return false;
  }
  if (!written || written > MAX_ENCODED - HEADER) return false;
  out[0] = CODEC_HEATSHRINK; out[1] = PROFILE_W8_L4;
  out[2] = uint8_t(n); out[3] = uint8_t(n >> 8);
  const uint32_t crc = Detail::crc32(src, n);
  for (unsigned i = 0; i < 4; ++i) out[4 + i] = uint8_t(crc >> (8 * i));
  *encodedLength = HEADER + written;
  return true;
}

// Call only after outer RS/CRC and session/sequence admission. Output may be
// modified on failure and must never be published/ACKed unless true is
// returned. Exact original length and CRC are checked before success.
inline bool decode(const uint8_t* src, size_t encodedLen, uint8_t* out,
                   size_t capacity, size_t* rawLength) {
  if (!rawLength) return false;
  *rawLength = 0;
  if (!src || !out || encodedLen <= HEADER || encodedLen > MAX_ENCODED ||
      Detail::overlaps(src, encodedLen, out, capacity)) return false;
  if (src[0] != CODEC_HEATSHRINK || src[1] != PROFILE_W8_L4) return false;
  const size_t expected = size_t(src[2]) | (size_t(src[3]) << 8);
  if (!expected || expected > MAX_RAW || expected > capacity ||
      !Detail::validStream(src + HEADER, encodedLen - HEADER, expected)) return false;
  heatshrink_decoder& state = Detail::decoder();
  heatshrink_decoder_reset(&state);
  size_t consumed = HEADER, produced = 0;
  for (;;) {
    size_t accepted = 0;
    if (consumed < encodedLen) {
      if (heatshrink_decoder_sink(&state, const_cast<uint8_t*>(src + consumed),
                                 encodedLen - consumed, &accepted) < 0) return false;
      consumed += accepted;
    }
    size_t thisPass = 0;
    for (;;) {
      uint8_t probe = 0;
      size_t count = 0;
      const bool full = produced == expected;
      const HSD_poll_res result = heatshrink_decoder_poll(&state,
        full ? &probe : out + produced, full ? 1 : expected - produced, &count);
      if (result < 0 || (full && count)) return false;
      produced += count; thisPass += count;
      if (result == HSDR_POLL_EMPTY) break;
      if (!count) return false;
    }
    if (consumed == encodedLen) {
      if (heatshrink_decoder_finish(&state) != HSDR_FINISH_DONE) return false;
      break;
    }
    if (!accepted && !thisPass) return false;
  }
  if (produced != expected || Detail::crc32(out, produced) != Detail::read32(src + 4))
    return false;
  *rawLength = produced;
  return true;
}
} // namespace RadioCompression
#endif

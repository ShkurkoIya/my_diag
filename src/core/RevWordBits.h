/// @file RevWordBits.h
/// @brief scat/dia_vldos RevWordBits: reverse LE words → BE bitstream, MSB-first index.
///
/// Used by Qualcomm ML1 measurement cells (e.g. LTE 0xB193 subpacket 0x19) where
/// RSRP/RSRQ/RSSI live in a word-reversed bit window, not in native LE bitfields.
#pragma once

#include <cstddef>
#include <cstdint>

#include <observer/model/Utils.h>

namespace QCom::Utils {

struct RevWordBits {
  static constexpr int kMaxWords = 12;

  uint8_t be[kMaxWords * 4]{};
  int nbytes{0};

  RevWordBits() = default;

  /// @param le_words little-endian u32 words as on the wire
  /// @param nwords number of u32 words (clamped to kMaxWords)
  RevWordBits(const uint8_t* le_words, int nwords) {
    if (nwords > kMaxWords) nwords = kMaxWords;
    if (nwords < 0) nwords = 0;
    nbytes = nwords * 4;
    for (int i = 0; i < nwords; ++i) {
      const uint32_t w =
          Converter::read_le<uint32_t>(le_words, static_cast<size_t>(nwords - 1 - i) * 4);
      be[i * 4 + 0] = static_cast<uint8_t>((w >> 24) & 0xFF);
      be[i * 4 + 1] = static_cast<uint8_t>((w >> 16) & 0xFF);
      be[i * 4 + 2] = static_cast<uint8_t>((w >> 8) & 0xFF);
      be[i * 4 + 3] = static_cast<uint8_t>(w & 0xFF);
    }
  }

  /// Half-open bit range `[a, b)`, MSB-first in the reversed bitstream.
  [[nodiscard]] uint32_t slice(int a, int b) const {
    uint32_t v = 0;
    for (int i = a; i < b; ++i) {
      const int by = i >> 3;
      const int bit = 7 - (i & 7);
      if (by >= nbytes) break;
      v = (v << 1) | static_cast<uint32_t>((be[by] >> bit) & 1u);
    }
    return v;
  }
};

/// Inclusive-style half-open bit range for RevWordBits tables.
struct BitRange {
  int begin{};
  int end{};  // exclusive

  [[nodiscard]] constexpr int width() const noexcept { return end - begin; }
};

}  // namespace QCom::Utils

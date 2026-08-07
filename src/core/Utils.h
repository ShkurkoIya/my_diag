/// @file Utils.h
/// @brief Binary parsing utilities: LE readers, bitfield extraction, validity checks.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace QCom::Utils {

class Converter {
public:
  template <typename T>
  [[nodiscard]] static constexpr T read_le(std::string_view data, size_t offset) noexcept {
    if (offset + sizeof(T) > data.size()) return T{};
    T result{};
    std::memcpy(&result, data.data() + offset, sizeof(T));
    return result;
  }

  template <typename T>
  [[nodiscard]] static constexpr T read_le(std::span<const uint8_t> data, size_t offset) noexcept {
    if (offset + sizeof(T) > data.size()) return T{};
    T result{};
    std::memcpy(&result, data.data() + offset, sizeof(T));
    return result;
  }

  /// @pre offset + sizeof(T) must be within valid readable range.
  template <typename T>
  [[nodiscard]] static T read_le(const uint8_t* data, size_t offset) noexcept {
    T result{};
    std::memcpy(&result, data + offset, sizeof(T));
    return result;
  }

  template <typename Container>
  [[nodiscard]] static constexpr uint16_t digits_to_number(const Container& digits) noexcept {
    uint16_t result = 0;
    for (size_t i = 0; i < digits.size(); ++i) {
      assert(digits[i] <= 9);
      result = result * 10 + digits[i];
    }
    return result;
  }
};

/// Extract `nbits` starting at bit position `lsb` from a 32-bit word.
[[nodiscard]] inline constexpr uint32_t bits(uint32_t word, unsigned lsb, unsigned nbits) noexcept {
  if (nbits == 0) return 0;
  if (nbits >= 32) return word >> lsb;
  return (word >> lsb) & ((1u << nbits) - 1);
}

// ============================================================================
// ML1 conversion formulas (from scat, verified against Qualcomm QXDM)
// ============================================================================

/// LTE/NR RSRP: raw * 0.0625 - 180.0 (dBm)
[[nodiscard]] inline constexpr float ml1_rsrp(uint32_t raw) noexcept {
  return static_cast<float>(raw) * 0.0625f - 180.0f;
}

/// LTE/NR RSRQ: raw * 0.0625 - 30.0 (dB)
[[nodiscard]] inline constexpr float ml1_rsrq(uint32_t raw) noexcept {
  return static_cast<float>(raw) * 0.0625f - 30.0f;
}

/// LTE RSSI: raw * 0.0625 - 110.0 (dBm)
[[nodiscard]] inline constexpr float ml1_rssi(uint32_t raw) noexcept {
  return static_cast<float>(raw) * 0.0625f - 110.0f;
}

/// NR SINR: raw * 0.0625 - 20.0 (dB)
[[nodiscard]] inline constexpr float ml1_nr_sinr(uint32_t raw) noexcept {
  return static_cast<float>(raw) * 0.0625f - 20.0f;
}

// ============================================================================
// Validity guards — fail-closed: reject garbage values
// ============================================================================

[[nodiscard]] inline constexpr bool valid_lte_earfcn(uint32_t v) noexcept {
  // 3GPP TS 36.101 EARFCN; reject 0 / 0xFFFFFFFF modem padding.
  return v > 0 && v <= 262143;
}
[[nodiscard]] inline constexpr bool valid_lte_pci(uint16_t v) noexcept { return v <= 503; }
/// 28-bit E-UTRAN Cell Identity (reject 0xFFFFFFFF padding).
[[nodiscard]] inline constexpr bool valid_lte_eci(uint64_t v) noexcept {
  return v > 0 && v <= 0x0FFFFFFFull;
}
/// 16-bit TAC (0xFFFF is reserved / unused).
[[nodiscard]] inline constexpr bool valid_lte_tac(uint32_t v) noexcept {
  return v > 0 && v < 0xFFFFu;
}

/// PCI packed in ML1 meas words (0xB17F / 0xB193 cells).
/// SIM8300 + MobileInsight: low 9 bits. scat bitstring MSB[0:9] uses >>7 — fallback when lo==0.
[[nodiscard]] inline constexpr uint16_t lte_pci_from_meas_word(uint16_t w) noexcept {
  const uint16_t lo = static_cast<uint16_t>(w & 0x1FFu);
  if (lo != 0 && lo <= 503) return lo;
  return static_cast<uint16_t>((w >> 7) & 0x1FFu);
}
[[nodiscard]] inline constexpr bool valid_lte_rsrp(float v) noexcept {
  // Survey-grade: reject nonsense extremes (−30 from bad decode, <−150 noise floor).
  return v >= -150.0f && v <= -40.0f;
}
[[nodiscard]] inline constexpr bool valid_nr_arfcn(uint32_t v) noexcept { return v <= 3279165; }
[[nodiscard]] inline constexpr bool valid_nr_pci(uint16_t v) noexcept { return v <= 1007; }

}  // namespace QCom::Utils

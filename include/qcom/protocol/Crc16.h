/// @file Crc16.h
/// @brief Compile-time parametric CRC16 engine.
///
/// Usage: QualcommCrc::calculate(data, len) for Qualcomm DIAG CRC.
/// Poly 0x1021, Init 0xFFFF, reflect both, XOR 0xFFFF (CRC-16/X.25 inverted).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace QCom {

template <uint16_t Poly, uint16_t Init, bool ReflectIn, bool ReflectOut, uint16_t XorOut>
class Crc16 {
public:
  [[nodiscard]] static uint16_t calculate(const uint8_t* data, size_t len) noexcept {
    static const auto table = generate_table();

    uint16_t crc = Init;
    for (size_t i = 0; i < len; ++i) {
      uint8_t byte = data[i];
      if constexpr (ReflectIn) byte = reflect8(byte);
      crc = (crc << 8) ^ table[((crc >> 8) ^ byte) & 0xFF];
    }
    if constexpr (ReflectOut) crc = reflect16(crc);
    return crc ^ XorOut;
  }

  [[nodiscard]] static uint16_t calculate(std::span<const uint8_t> data) noexcept {
    return calculate(data.data(), data.size());
  }

private:
  static constexpr uint8_t reflect8(uint8_t val) noexcept {
    val = ((val & 0xF0) >> 4) | ((val & 0x0F) << 4);
    val = ((val & 0xCC) >> 2) | ((val & 0x33) << 2);
    val = ((val & 0xAA) >> 1) | ((val & 0x55) << 1);
    return val;
  }

  static constexpr uint16_t reflect16(uint16_t val) noexcept {
    uint16_t res = 0;
    for (int i = 0; i < 16; ++i) {
      if (val & (1 << i)) res |= (1 << (15 - i));
    }
    return res;
  }

  static constexpr std::array<uint16_t, 256> generate_table() noexcept {
    std::array<uint16_t, 256> table{};
    for (int byte = 0; byte < 256; ++byte) {
      uint16_t crc = static_cast<uint16_t>(byte << 8);
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ Poly) : (crc << 1);
      }
      table[byte] = crc;
    }
    return table;
  }
};

/// Qualcomm DIAG CRC-16 (inverted CCITT / X.25)
using QualcommCrc = Crc16<0x1021, 0xFFFF, true, true, 0xFFFF>;

}  // namespace QCom
